// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Error.h"
#include "fe/core/Math.h"
#include "fe/core/Types.h"
#include "fe/core/Uuid.h"
#include "fe/physics2d/JointSolver.h"
#include "fe/systems/System.h"

#include <string_view>
#include <unordered_map>
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

    std::vector<Body> bodies_;
    usize contacts_ = 0;

    // Scratch, reused across frames so a steady-state step allocates nothing.
    std::vector<BodyPair> active_pairs_;
    std::vector<BodyPair> previous_pairs_;
    std::vector<ContactEvent> events_;

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
