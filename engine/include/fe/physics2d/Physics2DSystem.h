// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Error.h"
#include "fe/core/Math.h"
#include "fe/core/Types.h"
#include "fe/core/Uuid.h"
#include "fe/physics2d/BroadPhase.h"
#include "fe/physics2d/IslandSolver.h"
#include "fe/physics2d/JointSolver.h"
#include "fe/systems/System.h"

#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace fe {

class Node;
class Scene;

namespace physics2d {

class Collider2D;
class DistanceJoint2D;
class RevoluteJoint2D;
class RigidBody2D;

// Whether a contact event reports a contact that has just appeared or one that
// has just disappeared.
enum class ContactPhase : u8 {
    Begin,
    End,
};

// A contact between two colliders that began or ended during the last update.
//
// `a` and `b` are the two nodes involved, in a stable order (the order the pair
// is keyed by, not the order they were gathered), so a gameplay listener can
// tell the same pair apart across frames. The event carries no contact point or
// normal: it is a signal to react to ("I just landed", "I left the ground"), and
// a listener that needs the geometry can run a query.
struct ContactEvent {
    ContactPhase phase = ContactPhase::Begin;
    Node* a = nullptr;
    Node* b = nullptr;
};

// Advances the 2D physics world by one frame: integration, then collision
// resolution, then contact-event reconciliation.
//
// This is the first system that actually changes the scene as it runs, which is
// what the systems layer (ADR-0008) was built for. It is a single-threaded,
// fixed-iteration solver, not a full physics engine: it has linear and angular
// motion, gravity, restitution, and friction. Angular dynamics were added in a
// later cut (ADR-0013); a collider stays axis-aligned, so a rotated body is
// drawn rotated but collided as its AABB.
//
// Shape and motion are separate components. A node with a Collider2D alone is a
// static obstacle; a node with both a Collider2D and a dynamic RigidBody2D is
// moved by the solver. That is the composition model doing the work: "static
// floor" and "falling crate" are the same two components in different
// configurations, not two classes.
//
// The solver is sequential-impulse with Baumgarte position correction, run for a
// fixed number of iterations. It reads the bodies into a flat array first, so
// the inner loop walks contiguous memory rather than the scene's pointer graph.
//
// Candidate pairs are found once per step by a uniform-grid broad phase
// (`BroadPhase.h`) and the iteration loop walks only that set, so a step costs
// O(candidates * iterations) rather than O(bodies^2 * iterations). Sharing a
// grid cell is necessary but not sufficient: the solver still runs the
// `bounds_overlap` prefilter and then the narrow phase on every candidate, so
// the broad phase is free to be conservative.
//
// --- Sleeping and the step order -------------------------------------------
//
// A body that has been nearly still for a sustained number of steps is put to
// sleep, and a sleeping body is skipped by integration and by the solver, so a
// pile that settled long ago stops costing solver time (ADR-0026). Sleeping is
// decided per *island*: the connected component of the contact-and-joint graph,
// with static geometry not joining islands. A body sleeps only when every
// dynamic body in its island is ready, and waking one body wakes the island.
//
// One step runs, in order:
//
//   1. gather        -- flatten the scene into bodies, joints, node index
//   2. broad phase   -- find the candidate contact pairs once for the step
//   3. islands       -- group the bodies by contacts and joints
//   4. wake checks   -- wake an island touched by an awake dynamic body, then
//                       propagate that wake across the whole island
//   5. integrate     -- gravity, torque, damping, position, rotation; awake only
//   6. solve         -- contacts and joints; awake only
//   7. sleep checks  -- advance each awake island's rest timers, then sleep a
//                       whole island once every body in it has been still long
//                       enough
//
// The wake checks run before integration so a moving body wakes a sleeper in the
// same step it reaches it, and the solver then resolves the pair; a body can
// never pass through a sleeping one because a touching pair is a link, and a link
// to a moving body wakes it before the solver runs. The sleep checks run last so
// the decision is made on the velocity the solver actually leaves behind.
class Physics2DSystem final : public System {
public:
    [[nodiscard]] std::string_view name() const noexcept override { return "Physics2DSystem"; }

    void update(const FrameContext& context, Scene& scene) override;

    // Gravity in world units per second squared. Points down by default, which
    // matches the renderer's y-up convention.
    Vec2 gravity{0.0f, -9.81f};

    // Sequential-impulse iterations per frame. More iterations resolve stacked
    // bodies more stably at a linear cost. Four is enough for the shapes this
    // cut supports and is cheap; it is a tunable, not a constant.
    u32 iterations = 4;

    // Contacts found in the most recent update. Exposed for tests and the
    // editor's debug overlay.
    [[nodiscard]] usize contact_count() const noexcept { return contacts_; }

    // Candidate pairs the broad phase returned in the most recent update, before
    // the narrow phase. This is the number the spatial hash is meant to shrink;
    // exposed for tests and the editor's debug overlay. A brute-force double
    // loop would produce bodies_ * (bodies_ - 1) / 2 candidates.
    [[nodiscard]] usize candidate_count() const noexcept { return broad_phase_.candidate_count(); }

    // The contacts that began or ended during the most recent update.
    //
    // Events are collected on the system and read after the step rather than
    // dispatched mid-solve, so a listener sees a stable world and cannot mutate
    // it while the solver is iterating. A gameplay system that runs after
    // physics in the schedule reads them here; the buffer is cleared at the
    // start of every update, so it always describes exactly one step.
    //
    // Order within a step is deterministic: all Begin events first, then all End
    // events, each sorted by the pair key. Determinism matters because a game
    // that reacts to two events in one frame must get the same order every run.
    [[nodiscard]] const std::vector<ContactEvent>& contact_events() const noexcept { return events_; }
    [[nodiscard]] usize contact_event_count() const noexcept { return events_.size(); }

    // --- Joints ------------------------------------------------------------

    // Number of joints solved in the most recent update: distance joints that
    // were applied plus revolute joints that were applied (each revolute joint
    // counts once, not once per row). A joint that could not be solved -- a nil
    // body reference, a body that is not in the scene, a body with no rigid
    // body, or a pair the solver cannot constrain -- is not counted and records
    // a diagnostic instead.
    [[nodiscard]] usize joint_count() const noexcept { return joints_solved_; }

    // A diagnostic for the most recent update: the joints that were skipped,
    // with the reason. Empty on a clean step. This is how a joint with a dead
    // or missing body surfaces to the caller instead of failing silently, and
    // how the editor can flag a broken constraint.
    struct JointDiagnostic {
        std::string_view type;
        Uuid body_a{};
        Uuid body_b{};
        Error error;
    };
    [[nodiscard]] const std::vector<JointDiagnostic>& joint_diagnostics() const noexcept { return joint_diagnostics_; }

    // --- Sleeping ----------------------------------------------------------

    // Islands found in the most recent update: the connected components of the
    // contact-and-joint graph over the dynamic bodies, static geometry excluded.
    // Exposed so a test or the editor can assert the grouping without reaching
    // into the solver.
    [[nodiscard]] usize island_count() const noexcept { return islands_.island_count(); }

    // True when the last update put the two bodies (by node id) in one island.
    // False when either node is not a simulated dynamic body, or is in no
    // island. Used by tests to prove a jointed pair is one island and that two
    // separate piles are not.
    [[nodiscard]] bool same_island(const Uuid& a, const Uuid& b) const;

    // Sleeping dynamic bodies in the most recent update. A sleeping body is
    // skipped by integration and by the solver, so this is the count that is not
    // costing solver time.
    [[nodiscard]] usize sleeping_body_count() const noexcept { return sleeping_bodies_; }

private:
    // One simulated node, flattened out of the scene for the duration of the
    // update so the solver works on a contiguous array.
    struct Body {
        Collider2D* collider;
        RigidBody2D* body; // null for a static collider
        Node* node;
        Vec2 position;
        Vec2 velocity;
        f32 rotation;
        f32 angular_velocity;
        f32 inv_mass;
        f32 inv_inertia;
        f32 restitution;
        f32 friction;
        bool fixed_rotation;
    };

    // A collider pair, keyed by node identity so it is stable across frames
    // regardless of scene order. Ordered so the active set can be sorted and
    // diffed against the previous frame deterministically.
    struct BodyPair {
        Uuid first;
        Uuid second;

        [[nodiscard]] bool operator==(const BodyPair& other) const noexcept {
            return first == other.first && second == other.second;
        }
        [[nodiscard]] bool operator<(const BodyPair& other) const noexcept {
            const auto& a = first.bytes();
            const auto& b = other.first.bytes();
            for (usize i = 0; i < a.size(); ++i) {
                if (a[i] != b[i]) {
                    return a[i] < b[i];
                }
            }
            const auto& c = second.bytes();
            const auto& d = other.second.bytes();
            for (usize i = 0; i < c.size(); ++i) {
                if (c[i] != d[i]) {
                    return c[i] < d[i];
                }
            }
            return false;
        }
    };

    // Emits Begin/End events for the difference between the pairs that touch
    // this frame and those that touched last frame, then rolls `previous_pairs_`
    // forward. Both vectors are kept sorted.
    void reconcile_contacts(Scene& scene);
    void append_event(ContactPhase phase, const BodyPair& pair, Scene& scene);

    // One joint flattened out of the scene for the duration of the update. Both
    // ends are resolved to nodes up front; a nil, missing, or self-referential
    // reference leaves one null and the joint is skipped with a diagnostic.
    struct Joint {
        DistanceJoint2D* distance = nullptr;
        RevoluteJoint2D* revolute = nullptr;
        Node* a = nullptr;
        Node* b = nullptr;
    };

    // Solves every joint in the scene once. Runs after the contact pass has
    // written back, so it reads each end's current transform and velocity fresh
    // and a joint correction is the last word on position -- which is what keeps
    // a pinned body pinned even while it also rests on a collider. A joint end
    // may be any node: it does not need a collider, so a pendulum can hang from
    // a bare pivot node.
    void solve_joints(Scene& scene, f32 delta_time);

    // Records a skipped joint with its reason, so a broken reference is visible
    // rather than silent. Never throws and never mutates the world.
    void note_joint_skipped(std::string_view type, const Uuid& body_a, const Uuid& body_b, Error error);

    // Builds the island graph: one link per contact pair and per solved joint,
    // with only dynamic bodies as vertices (a link to a static body is dropped).
    // Contacts are read from `active_pairs_` (the pairs that touch this step),
    // which the solve pass has already filled.
    void build_islands();

    // Wakes islands that an awake dynamic body is touching or joined to, then
    // propagates that wake across each island. Runs before integration, so a
    // moving body wakes a sleeper in the same step it reaches it.
    void apply_wake_checks();

    // Advances each awake island's rest timers and sleeps an island once every
    // dynamic body in it has been still for long enough. Runs last, on the
    // velocity the solver left behind, and writes the result back to the
    // RigidBody2D components.
    void apply_sleep_checks(f32 delta_time);

    // Marks every body of island `index` awake, in `asleep_` and on the bodies
    // themselves. Idempotent.
    void wake_island(usize index);

    std::vector<Body> bodies_;
    usize contacts_ = 0;

    // Broad phase, rebuilt once per step. `broad_phase_bounds_` holds the world
    // bounds of the bodies that have a collider; the grid returns index pairs
    // into it. `bounds_owner_` maps each bounds index back to its `bodies_`
    // index, because `bodies_` also holds colliderless (joint-only) masses that
    // the grid never sees.
    BroadPhase broad_phase_;
    std::vector<Aabb2D> broad_phase_bounds_;
    std::vector<usize> bounds_owner_;

    // Scratch, reused across frames so a steady-state step allocates nothing.
    std::vector<BodyPair> active_pairs_;
    std::vector<BodyPair> previous_pairs_;
    std::vector<ContactEvent> events_;

    // Sleeping. `islands_` groups the dynamic bodies by contact and joint. The
    // island graph has only dynamic bodies as vertices, so `island_vertex_body_`
    // maps an island vertex back to its index in `bodies_`, and `body_vertex_`
    // maps a body index to its vertex (or kNoVertex for a static body, which is
    // never part of an island). `island_awake_` holds one flag per island for
    // the wake and sleep passes; `asleep_` is one flag per body, rebuilt each
    // step from `RigidBody2D::is_sleeping` and written back after the sleep
    // checks. `sleeping_bodies_` is the count reported by sleeping_body_count().
    IslandBuilder islands_;
    std::vector<IslandLink> island_links_;
    std::vector<usize> island_vertex_body_;
    std::vector<usize> body_vertex_;
    std::vector<bool> asleep_;
    std::vector<bool> island_awake_;
    usize sleeping_bodies_ = 0;

    // Maps a node id to its index in `bodies_`, rebuilt each update, so a
    // contact link and the same_island() query can find a body in O(1).
    std::unordered_map<Uuid, usize> body_index_;

    // Joints, rebuilt each update from the scene.
    std::vector<Joint> joints_;
    usize joints_solved_ = 0;
    std::vector<JointDiagnostic> joint_diagnostics_;
    // Maps a node id to its node so a joint can resolve either end in O(1)
    // instead of scanning. Rebuilt each update.
    std::unordered_map<Uuid, Node*> node_index_;
};

} // namespace physics2d
} // namespace fe
