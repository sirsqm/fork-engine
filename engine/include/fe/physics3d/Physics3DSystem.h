// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Error.h"
#include "fe/core/Math.h"
#include "fe/core/Types.h"
#include "fe/core/Uuid.h"
#include "fe/physics/IslandSolver.h"
#include "fe/physics3d/BroadPhase3D.h"
#include "fe/physics3d/JointSolver3D.h"
#include "fe/systems/System.h"

#include <string_view>
#include <unordered_map>
#include <vector>

namespace fe {

class Node;
class Scene;

namespace physics3d {

class Collider3D;
class RigidBody3D;
class DistanceJoint3D;

// Whether a contact event reports a contact that has just appeared or one that
// has just disappeared.
enum class ContactPhase : u8 {
    Begin,
    End,
};

// A contact between two colliders that began or ended during the last update.
//
// `a` and `b` are the two nodes involved, in a stable order (the order the pair
// is keyed by, not the order they were gathered), so a gameplay listener can tell
// the same pair apart across frames. The event carries no contact point or
// normal: it is a signal to react to ("I just landed", "I left the ground"), and
// a listener that needs the geometry can run a query.
struct ContactEvent {
    ContactPhase phase = ContactPhase::Begin;
    Node* a = nullptr;
    Node* b = nullptr;
};

// Advances the 3D physics world by one frame: integration, then collision
// resolution, then contact-event reconciliation.
//
// This is the 3D counterpart of Physics2DSystem and shares its shape: it is a
// single-threaded, fixed-iteration sequential-impulse solver with gravity,
// restitution, friction, and -- since ADR-0043 -- angular dynamics. It is the
// first cut, so it is deliberately narrower than the 2D module:
//
//   * **Angular dynamics, but an axis-aligned collider.** A body carries an
//     angular velocity, a torque, and a shape-derived (isotropic) inertia; the
//     solver integrates the orientation as a quaternion and applies angular
//     impulses from contacts and joints, so a box tumbles and a ball rolls. The
//     collider shape is not re-oriented, though: a rotated body is drawn rotated
//     and collided as its unrotated box or sphere, exactly the limit the 2D
//     collider had before oriented boxes (ADR-0037). Oriented shapes are later
//     work.
//   * **Box and sphere shapes.** The 3D analogues of box and circle. Capsules and
//     convex hulls are later work.
//   * **Sleeping and islands.** A body at rest long enough sleeps, and sleeping
//     is grouped by island exactly as in 2D (ADR-0026, ADR-0111): a body sleeps
//     only when every dynamic body in its contact-and-joint island is ready, and
//     waking one wakes the group. The island graph is the shared, pure
//     `fe::physics::IslandBuilder`.
//
// Shape and motion are separate components. A node with a Collider3D alone is a
// static obstacle; a node with both a Collider3D and a dynamic RigidBody3D is
// moved by the solver. Candidate pairs are found once per step by a uniform-grid
// broad phase and the iteration loop walks only that set, so a step costs
// O(candidates * iterations) rather than O(bodies^2 * iterations). Sharing a grid
// cell is necessary but not sufficient: the solver still runs the `bounds_overlap`
// prefilter and then the narrow phase on every candidate.
//
// One step runs, in order:
//
//   1. gather      -- flatten the scene into bodies and joints
//   2. islands     -- group the bodies by contacts and joints
//   3. wake checks -- wake an island touched by an awake dynamic body, then
//                     propagate that wake across the whole island
//   4. integrate   -- gravity, torque, damping, position, orientation; awake only
//   5. broad phase -- find the candidate contact pairs once for the step
//   6. solve       -- sequential impulses over the candidate pairs; awake only
//   7. joints      -- solve every joint once, after the contacts
//   8. write back  -- transforms, velocities, and orientation
//   9. sleep checks-- advance each awake island's rest timers, then sleep a whole
//                     island once every body in it has been still long enough
//  10. reconcile   -- diff the touching pairs against last frame for events
class Physics3DSystem final : public System {
public:
    [[nodiscard]] std::string_view name() const noexcept override { return "Physics3DSystem"; }

    void update(const FrameContext& context, Scene& scene) override;

    // Gravity in world units per second squared. Points down by default, which
    // matches the renderer's y-up convention.
    Vec3 gravity{0.0f, -9.81f, 0.0f};

    // Sequential-impulse iterations per frame. More iterations resolve stacked
    // bodies more stably at a linear cost. Four is enough for the shapes this cut
    // supports and is cheap; it is a tunable, not a constant.
    u32 iterations = 4;

    // Contacts found in the most recent update. Exposed for tests and the
    // editor's debug overlay.
    [[nodiscard]] usize contact_count() const noexcept { return contacts_; }

    // Candidate pairs the broad phase returned in the most recent update, before
    // the narrow phase. This is the number the spatial hash is meant to shrink;
    // exposed for tests and the editor's debug overlay. A brute-force double loop
    // would produce bodies_ * (bodies_ - 1) / 2 candidates.
    [[nodiscard]] usize candidate_count() const noexcept { return broad_phase_.candidate_count(); }

    // The contacts that began or ended during the most recent update.
    //
    // Events are collected on the system and read after the step rather than
    // dispatched mid-solve, so a listener sees a stable world and cannot mutate it
    // while the solver is iterating. A gameplay system that runs after physics in
    // the schedule reads them here; the buffer is cleared at the start of every
    // update, so it always describes exactly one step.
    //
    // Order within a step is deterministic: all Begin events first, then all End
    // events, each sorted by the pair key.
    [[nodiscard]] const std::vector<ContactEvent>& contact_events() const noexcept { return events_; }
    [[nodiscard]] usize contact_event_count() const noexcept { return events_.size(); }

    // Joints solved in the most recent update: distance joints that were applied.
    // A joint that could not be solved -- a nil or missing body reference, two
    // immovable ends, or a coincident pair -- is not counted. Exposed for tests
    // and the editor's debug overlay, mirroring Physics2DSystem::joint_count.
    [[nodiscard]] usize joint_count() const noexcept { return joints_solved_; }

    // Islands found in the most recent update: the connected components of the
    // contact-and-joint graph over the dynamic bodies (ADR-0111).
    [[nodiscard]] usize island_count() const noexcept { return islands_.island_count(); }

    // True when the last update put the two bodies (by node id) in one island.
    // False when either is not a simulated dynamic body, or is not part of an
    // island. Used by tests to prove a jointed pair is one island and that two
    // bodies resting on separate parts of a floor are not.
    [[nodiscard]] bool same_island(const Uuid& a, const Uuid& b) const;

    // Sleeping dynamic bodies in the most recent update. A sleeping body is
    // skipped by integration and by the solver, so this is the count of bodies
    // that cost nothing this step.
    [[nodiscard]] usize sleeping_body_count() const noexcept { return sleeping_bodies_; }

private:
    // One simulated node, flattened out of the scene for the duration of the
    // update so the solver works on a contiguous array.
    struct Body {
        Collider3D* collider;
        RigidBody3D* body; // null for a static collider
        Node* node;
        Vec3 position;
        Vec3 velocity;
        Quat orientation; // the node's world rotation, integrated in place
        Vec3 angular_velocity;
        f32 inv_mass;
        f32 inv_inertia;
        f32 restitution;
        f32 friction;
        bool fixed_rotation; // the solver must not change this body's orientation
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

    // Emits Begin/End events for the difference between the pairs that touch this
    // frame and those that touched last frame, then rolls `previous_pairs_`
    // forward. Both vectors are kept sorted.
    void reconcile_contacts(Scene& scene);
    void append_event(ContactPhase phase, const BodyPair& pair, Scene& scene);

    // One joint flattened out of the scene for the duration of the update. Both
    // ends are resolved after the whole scene is indexed, so a joint may point
    // forward to a node that appears later.
    struct Joint {
        DistanceJoint3D* distance = nullptr;
        Node* a = nullptr;
        Node* b = nullptr;
    };

    // Solves every joint in the scene once, after the contact pass. Joints run
    // after contacts so a joint correction is the last word on position -- which
    // is what keeps a pinned body pinned even while it also rests on a collider.
    // A joint end may be a bare body with no collider, so this runs even when
    // `bodies_` is empty.
    void solve_joints(Scene& scene, f32 delta_time);

    // Builds the island graph: one link per contact pair and per solved joint,
    // over the dynamic bodies only (a link to a static body is dropped, so it
    // does not conduct motion between the bodies resting on it).
    void build_islands();

    // Wakes islands that an awake dynamic body is touching or joined to, then
    // propagates that wake across each island. Runs before integration, so a
    // moving body wakes a sleeper in the same step it reaches it.
    void apply_wake_checks();

    // Advances each awake island's rest timers and sleeps an island once every
    // body in it has been still long enough and can_sleep.
    void apply_sleep_checks(f32 delta_time);

    // Marks every body of island `index` awake, in `asleep_` and on the bodies
    // themselves, and restarts each newly woken body's rest timer.
    void wake_island(usize index);

    std::vector<Body> bodies_;
    usize contacts_ = 0;

    // Joints, rebuilt each update from the scene.
    std::vector<Joint> joints_;
    usize joints_solved_ = 0;
    // Maps a node id to its node so a joint can resolve either end in O(1),
    // including a bare pivot that is not simulated as a body.
    std::unordered_map<Uuid, Node*> node_index_;

    // Maps a node id to its index in `bodies_`, for the simulated bodies only.
    // The island graph needs it to turn a joint's two node ids into the two body
    // vertices it links; a joint end that is not a simulated dynamic body is
    // absent, so it contributes no link.
    std::unordered_map<Uuid, usize> body_index_;

    // Broad phase, rebuilt once per step. `broad_phase_bounds_` holds the world
    // bounds of the bodies that have a collider; the grid returns index pairs into
    // it. `bounds_owner_` maps each bounds index back to its `bodies_` index,
    // because `bodies_` also holds colliderless masses the grid never sees.
    BroadPhase3D broad_phase_;
    std::vector<Aabb3D> broad_phase_bounds_;
    std::vector<usize> bounds_owner_;

    // Scratch, reused across frames so a steady-state step allocates nothing.
    std::vector<BodyPair> active_pairs_;
    std::vector<BodyPair> previous_pairs_;
    std::vector<ContactEvent> events_;

    // Sleeping. `islands_` groups the dynamic bodies by contact and joint. The
    // island graph has only dynamic bodies as vertices, so `island_vertex_body_`
    // maps an island vertex back to its index in `bodies_`, and `body_vertex_`
    // maps the other way (kNoVertex for a static body, which is never part of an
    // island). `island_awake_` holds one flag per island for the wake and sleep
    // passes; `asleep_` is one flag per body, rebuilt each step from
    // `RigidBody3D::is_sleeping` and written back after the sleep checks.
    // `sleeping_bodies_` is the count reported by sleeping_body_count().
    static constexpr usize kNoVertex = static_cast<usize>(-1);
    physics::IslandBuilder islands_;
    std::vector<physics::IslandLink> island_links_;
    std::vector<usize> island_vertex_body_;
    std::vector<usize> body_vertex_;
    std::vector<bool> asleep_;
    std::vector<bool> island_awake_;
    usize sleeping_bodies_ = 0;
};

} // namespace physics3d
} // namespace fe
