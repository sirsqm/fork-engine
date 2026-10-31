// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/player/BuiltinBehaviours.h"

#include "fe/core/Math.h"
#include "fe/physics3d/Collider3D.h"
#include "fe/physics3d/Physics3DQueries.h"
#include "fe/physics3d/RigidBody3D.h"
#include "fe/platform/InputState.h"
#include "fe/reflect/PropertyBuilders.h"
#include "fe/scene/Node.h"
#include "fe/scene/Scene.h"
#include "fe/systems/FrameContext.h"

#include <algorithm>
#include <cmath>

namespace fe::player {
namespace {

// The gap left between a probe's origin and the collider it belongs to, so a
// query can never return the character's own shape as the thing it hit. The
// geometry tests treat a ray starting exactly on a surface as a hit, so the
// clearance must be strictly positive.
constexpr f32 kProbeClearance = 0.02f;

// Just inside a right angle, so the view can approach straight up or down but
// never reach the pole, where the up vector would be parallel to the view
// direction and the basis would degenerate (the horizon would flip).
constexpr f32 kMaxPitch = FlyCamera3D::kMaxPitchRadians;

// One frame's worth of a held action, as a signed -1/0/+1 axis. Kept here so
// every behaviour reduces its actions to a direction the same way and a
// key-vs-touch difference cannot leak into the movement maths.
[[nodiscard]] f32 axis(bool negative, bool positive) noexcept {
    return (positive ? 1.0f : 0.0f) - (negative ? 1.0f : 0.0f);
}

// Whether an action is held, treating a missing action map as "not held". A
// headless tool or a test may run the schedule with no actions; a behaviour must
// then simply not move rather than dereference a null.
[[nodiscard]] bool held(const FrameContext& context, ActionMap::Id action) noexcept {
    return context.actions != nullptr && context.actions->is_down(action);
}

// The number of degrees the axis-angle rotation should advance this frame.
[[nodiscard]] f32 spin_step(f32 degrees_per_second, f32 delta_time) noexcept {
    return degrees_per_second * delta_time;
}

// How far the node's own collider extends below its centre, so a ground probe can
// start below the character rather than inside it. A sphere uses its radius, a box
// its half-height; a node with no collider gets a small default so a probe still
// leaves the centre instead of starting inside the character.
[[nodiscard]] f32 collider_half_height(const Node& node) noexcept {
    const auto* collider = node.get_component<physics3d::Collider3D>();
    if (collider == nullptr) {
        return 0.5f;
    }
    if (collider->shape == physics3d::ShapeType3D::Sphere) {
        return collider->radius;
    }
    return collider->half_extent.y;
}

// How far the node's own collider reaches from its centre along a horizontal
// direction: the support function of the collider's box, or the radius of a
// sphere. A horizontal probe must start beyond this, or it hits the character
// itself and reports the character as the wall it just walked into.
[[nodiscard]] f32 collider_horizontal_reach(const Node& node, const Vec3& direction) noexcept {
    const auto* collider = node.get_component<physics3d::Collider3D>();
    if (collider == nullptr) {
        return 0.5f;
    }
    if (collider->shape == physics3d::ShapeType3D::Sphere) {
        return collider->radius;
    }
    return std::fabs(direction.x) * collider->half_extent.x + std::fabs(direction.z) * collider->half_extent.z;
}

// Builds the action handles the behaviours read, resolving the standard names in
// one place. A name that was never declared resolves to kNone, which reads as
// "never held", so a game that declared only some of the actions still runs.
struct ActionIds {
    ActionMap::Id forward = ActionMap::kNone;
    ActionMap::Id back = ActionMap::kNone;
    ActionMap::Id left = ActionMap::kNone;
    ActionMap::Id right = ActionMap::kNone;
    ActionMap::Id up = ActionMap::kNone;
    ActionMap::Id down = ActionMap::kNone;
    ActionMap::Id look_up = ActionMap::kNone;
    ActionMap::Id look_down = ActionMap::kNone;
    ActionMap::Id look_left = ActionMap::kNone;
    ActionMap::Id look_right = ActionMap::kNone;
    ActionMap::Id run = ActionMap::kNone;
    ActionMap::Id jump = ActionMap::kNone;

    // Resolved once per update from the frame's map. A null map (a headless tool
    // or a test that runs the schedule with no input) yields every action
    // unbound, so the behaviour simply does not move rather than dereferencing
    // it. Resolving per frame is cheap — a handful of string compares over a tiny
    // table — and keeps the behaviour from caching a handle that belongs to a
    // different schedule's map.
    [[nodiscard]] static ActionIds resolve(const ActionMap* map) noexcept {
        ActionIds ids;
        if (map == nullptr) {
            return ids;
        }
        ids.forward = map->find(actions::kMoveForward);
        ids.back = map->find(actions::kMoveBack);
        ids.left = map->find(actions::kMoveLeft);
        ids.right = map->find(actions::kMoveRight);
        ids.up = map->find(actions::kMoveUp);
        ids.down = map->find(actions::kMoveDown);
        ids.look_up = map->find(actions::kLookUp);
        ids.look_down = map->find(actions::kLookDown);
        ids.look_left = map->find(actions::kLookLeft);
        ids.look_right = map->find(actions::kLookRight);
        ids.run = map->find(actions::kRun);
        ids.jump = map->find(actions::kJump);
        return ids;
    }
};

// Adds the reflected properties every built-in behaviour exposes, so the
// inspector can edit them and the scene serializer persists them (ADR-0063).
// Each behaviour's own registration calls the ones it has.
template <typename T>
[[nodiscard]] std::vector<reflect::Property> speed_properties(const T& defaults) {
    return {
        reflect::make_scalar<T, f32>("speed", &T::speed, reflect::ValueKind::Float, &defaults),
        reflect::make_scalar<T, f32>("run_multiplier", &T::run_multiplier, reflect::ValueKind::Float, &defaults),
    };
}

} // namespace

void declare_builtin_actions(ActionMap& map) {
    const ActionMap::Id forward = map.add(actions::kMoveForward);
    const ActionMap::Id back = map.add(actions::kMoveBack);
    const ActionMap::Id left = map.add(actions::kMoveLeft);
    const ActionMap::Id right = map.add(actions::kMoveRight);
    const ActionMap::Id up = map.add(actions::kMoveUp);
    const ActionMap::Id down = map.add(actions::kMoveDown);
    const ActionMap::Id look_up = map.add(actions::kLookUp);
    const ActionMap::Id look_down = map.add(actions::kLookDown);
    const ActionMap::Id look_left = map.add(actions::kLookLeft);
    const ActionMap::Id look_right = map.add(actions::kLookRight);
    const ActionMap::Id run = map.add(actions::kRun);
    const ActionMap::Id jump = map.add(actions::kJump);

    // Keyboard. WASD and the arrow keys both move, which is the convention a
    // player already has in their fingers; the arrow keys are also what a
    // laptop without a numpad has for a second hand.
    map.bind_key(forward, Key::W);
    map.bind_key(forward, Key::Up);
    map.bind_key(back, Key::S);
    map.bind_key(back, Key::Down);
    map.bind_key(left, Key::A);
    map.bind_key(left, Key::Left);
    map.bind_key(right, Key::D);
    map.bind_key(right, Key::Right);
    map.bind_key(up, Key::Space);
    map.bind_key(down, Key::LeftShift);
    map.bind_key(run, Key::LeftControl);
    map.bind_key(jump, Key::Space);

    // Look, on the keyboard: the classic IJKL pair, so a keyboard-only player can
    // aim without a mouse.
    map.bind_key(look_up, Key::I);
    map.bind_key(look_down, Key::K);
    map.bind_key(look_left, Key::J);
    map.bind_key(look_right, Key::L);

    // A phone. The regions are the lower-left and lower-right eighths, which is
    // where a player's thumbs already rest, and they are normalised so they
    // follow a rotation or a resize (ADR-0077). Touch movement is in the
    // camera's own frame, exactly like the keys, so a phone and a keyboard drive
    // the identical code path (ADR-0075).
    map.bind_touch_region(forward, 0.0f, 0.5f, 0.25f, 0.75f);
    map.bind_touch_region(back, 0.0f, 0.75f, 0.25f, 1.0f);
    map.bind_touch_region(left, 0.0f, 0.25f, 0.25f, 0.5f);
    map.bind_touch_region(right, 0.0f, 0.0f, 0.25f, 0.25f);
    map.bind_touch_region(look_left, 0.75f, 0.0f, 1.0f, 0.5f);
    map.bind_touch_region(look_right, 0.75f, 0.5f, 1.0f, 1.0f);
    // Jump sits in the lower-right eighth, under the thumb that would otherwise
    // be idle while the left thumb is on the stick. It deliberately overlaps no
    // movement region, so a thumb on "jump" cannot also be read as a direction.
    map.bind_touch_region(jump, 0.75f, 0.75f, 1.0f, 1.0f);
}

void register_builtin_behaviours(ScriptRegistry& registry) {
    {
        FlyCamera3D defaults;
        ScriptDesc desc;
        desc.name = "FlyCamera3D";
        desc.factory = [] { return std::unique_ptr<Script>(new FlyCamera3D()); };
        desc.properties.push_back(reflect::make_scalar<FlyCamera3D, f32>("move_speed", &FlyCamera3D::move_speed,
                                                                         reflect::ValueKind::Float, &defaults));
        desc.properties.push_back(reflect::make_scalar<FlyCamera3D, f32>("run_multiplier", &FlyCamera3D::run_multiplier,
                                                                         reflect::ValueKind::Float, &defaults));
        desc.properties.push_back(reflect::make_scalar<FlyCamera3D, f32>("look_speed", &FlyCamera3D::look_speed,
                                                                         reflect::ValueKind::Float, &defaults));
        desc.properties.push_back(reflect::make_scalar<FlyCamera3D, f32>(
            "mouse_sensitivity", &FlyCamera3D::mouse_sensitivity, reflect::ValueKind::Float, &defaults));
        desc.properties.push_back(reflect::make_scalar<FlyCamera3D, bool>(
            "enable_mouse_look", &FlyCamera3D::enable_mouse_look, reflect::ValueKind::Bool, &defaults));
        registry.register_desc(std::move(desc));
    }
    {
        SpinNode3D defaults;
        ScriptDesc desc;
        desc.name = "SpinNode3D";
        desc.factory = [] { return std::unique_ptr<Script>(new SpinNode3D()); };
        desc.properties.push_back(
            reflect::make_scalar<SpinNode3D, f32>("axis_x", &SpinNode3D::axis_x, reflect::ValueKind::Float, &defaults));
        desc.properties.push_back(
            reflect::make_scalar<SpinNode3D, f32>("axis_y", &SpinNode3D::axis_y, reflect::ValueKind::Float, &defaults));
        desc.properties.push_back(
            reflect::make_scalar<SpinNode3D, f32>("axis_z", &SpinNode3D::axis_z, reflect::ValueKind::Float, &defaults));
        desc.properties.push_back(reflect::make_scalar<SpinNode3D, f32>(
            "degrees_per_second", &SpinNode3D::degrees_per_second, reflect::ValueKind::Float, &defaults));
        registry.register_desc(std::move(desc));
    }
    {
        MoveNode3D defaults;
        ScriptDesc desc;
        desc.name = "MoveNode3D";
        desc.factory = [] { return std::unique_ptr<Script>(new MoveNode3D()); };
        desc.properties = speed_properties(defaults);
        desc.properties.push_back(reflect::make_scalar<MoveNode3D, bool>("lock_vertical", &MoveNode3D::lock_vertical,
                                                                         reflect::ValueKind::Bool, &defaults));
        registry.register_desc(std::move(desc));
    }
    {
        CharacterController3D defaults;
        ScriptDesc desc;
        desc.name = "CharacterController3D";
        desc.factory = [] { return std::unique_ptr<Script>(new CharacterController3D()); };
        desc.properties.push_back(reflect::make_scalar<CharacterController3D, f32>(
            "speed", &CharacterController3D::speed, reflect::ValueKind::Float, &defaults));
        desc.properties.push_back(reflect::make_scalar<CharacterController3D, f32>(
            "run_multiplier", &CharacterController3D::run_multiplier, reflect::ValueKind::Float, &defaults));
        desc.properties.push_back(reflect::make_scalar<CharacterController3D, f32>(
            "turn_speed", &CharacterController3D::turn_speed, reflect::ValueKind::Float, &defaults));
        desc.properties.push_back(reflect::make_scalar<CharacterController3D, f32>(
            "jump_speed", &CharacterController3D::jump_speed, reflect::ValueKind::Float, &defaults));
        desc.properties.push_back(reflect::make_scalar<CharacterController3D, f32>(
            "gravity", &CharacterController3D::gravity, reflect::ValueKind::Float, &defaults));
        desc.properties.push_back(reflect::make_scalar<CharacterController3D, f32>(
            "ground_probe", &CharacterController3D::ground_probe, reflect::ValueKind::Float, &defaults));
        desc.properties.push_back(reflect::make_scalar<CharacterController3D, bool>(
            "slide_along_walls", &CharacterController3D::slide_along_walls, reflect::ValueKind::Bool, &defaults));
        registry.register_desc(std::move(desc));
    }
    {
        FollowCamera3D defaults;
        ScriptDesc desc;
        desc.name = "FollowCamera3D";
        desc.factory = [] { return std::unique_ptr<Script>(new FollowCamera3D()); };
        desc.properties.push_back(reflect::make_scalar<FollowCamera3D, std::string>(
            "target", &FollowCamera3D::target, reflect::ValueKind::String, &defaults));
        desc.properties.push_back(reflect::make_scalar<FollowCamera3D, f32>("distance", &FollowCamera3D::distance,
                                                                            reflect::ValueKind::Float, &defaults));
        desc.properties.push_back(reflect::make_scalar<FollowCamera3D, f32>("height", &FollowCamera3D::height,
                                                                            reflect::ValueKind::Float, &defaults));
        desc.properties.push_back(reflect::make_scalar<FollowCamera3D, f32>("look_ahead", &FollowCamera3D::look_ahead,
                                                                            reflect::ValueKind::Float, &defaults));
        desc.properties.push_back(reflect::make_scalar<FollowCamera3D, f32>("look_height", &FollowCamera3D::look_height,
                                                                            reflect::ValueKind::Float, &defaults));
        desc.properties.push_back(reflect::make_scalar<FollowCamera3D, f32>("smoothing", &FollowCamera3D::smoothing,
                                                                            reflect::ValueKind::Float, &defaults));
        registry.register_desc(std::move(desc));
    }
}

// --- FlyCamera3D -----------------------------------------------------------

Error FlyCamera3D::attach(Node& node) {
    // Start from the authored orientation. The node stores Euler angles in the
    // order rotation_euler() applies them (Rz * Ry * Rx), so X is pitch and Y is
    // yaw, which is the same convention the controller writes back.
    const Vec3& rotation = node.transform().rotation;
    pitch = rotation.x;
    yaw = rotation.y;
    return Error::none();
}

Error FlyCamera3D::update(const FrameContext& context) {
    Node* owner = node();
    if (owner == nullptr) {
        return Error::none();
    }

    const ActionIds ids = ActionIds::resolve(context.actions);
    const f32 delta = context.delta_time;

    // Look, from the actions first (a keyboard or a touch strip), then from a
    // drag (a mouse). Both are additive so a player can use either; the actions
    // are rate-based and the drag is displacement-based, so neither depends on
    // the frame rate for the same input.
    //
    // Yaw: the engine's Y rotation is counter-clockwise seen from above, so
    // turning *right* (which is what the player means by "look right") decreases
    // the yaw. Pitch: positive pitch looks up, and a drag upward is a negative
    // screen delta, so the drag term is negated.
    f32 yaw_step = axis(held(context, ids.look_right), held(context, ids.look_left)) * look_speed * delta;
    f32 pitch_step = axis(held(context, ids.look_down), held(context, ids.look_up)) * look_speed * delta;

    if (enable_mouse_look && context.input != nullptr && context.input->is_mouse_down(MouseButton::Left)) {
        const Vec2 drag = context.input->mouse_delta();
        yaw_step += -drag.x * mouse_sensitivity;
        pitch_step += -drag.y * mouse_sensitivity;
    }

    yaw += yaw_step;
    pitch = std::clamp(pitch + pitch_step, -kMaxPitch, kMaxPitch);

    // Keep yaw in [-pi, pi] so a long session cannot lose precision to a value
    // that grows without bound. The wrap is invisible to the view, which only
    // ever sees the sine and cosine of the angle.
    if (yaw > kPi) {
        yaw -= 2.0f * kPi;
    } else if (yaw < -kPi) {
        yaw += 2.0f * kPi;
    }

    Transform& transform = owner->transform();
    transform.rotation = Vec3{pitch, yaw, 0.0f};

    // Movement, in the camera's own frame: "forward" is into the screen, which is
    // what a player expects. The basis is rebuilt from the yaw and pitch just
    // computed, so the movement matches the view exactly on the frame the player
    // turns.
    const f32 move_x = axis(held(context, ids.left), held(context, ids.right));
    const f32 move_z = axis(held(context, ids.back), held(context, ids.forward));
    const f32 move_y = axis(held(context, ids.down), held(context, ids.up));

    if (move_x != 0.0f || move_y != 0.0f || move_z != 0.0f) {
        // The basis is taken from the same rotation matrix the camera uses to
        // derive its view direction, so "forward" here is provably the direction
        // the camera is looking — deriving it by hand from the Euler angles is
        // how the two drift apart on a sign.
        const Mat4 orientation = Mat4::rotation_euler(transform.rotation);
        const Vec3 forward = transform_direction(orientation, Vec3{0.0f, 0.0f, -1.0f}).normalized();
        const Vec3 world_up{0.0f, 1.0f, 0.0f};
        Vec3 right = forward.cross(world_up);
        // Looking straight up or down makes forward parallel to up; fall back to
        // a horizontal vector perpendicular to the view so the basis stays well
        // defined at the pole instead of collapsing.
        if (right.length_squared() < 1e-8f) {
            right = Vec3{-forward.z, 0.0f, forward.x};
        }
        right = right.normalized();

        Vec3 direction = forward * move_z + right * move_x + world_up * move_y;
        const f32 length = direction.length();
        if (length > 1e-6f) {
            // Normalise so moving diagonally is not faster than moving straight.
            direction = direction / length;
        }

        const f32 speed = move_speed * (held(context, ids.run) ? run_multiplier : 1.0f);
        transform.position = transform.position + direction * (speed * delta);
    }

    return Error::none();
}

// --- SpinNode3D ------------------------------------------------------------

Error SpinNode3D::update(const FrameContext& context) {
    Node* owner = node();
    if (owner == nullptr) {
        return Error::none();
    }

    const Vec3 axis_vector{axis_x, axis_y, axis_z};
    if (axis_vector.length_squared() < 1e-8f) {
        return Error::none();
    }

    // Rotate about the node's own axis by an angle proportional to the frame's
    // duration, so the spin rate is independent of the frame rate. The axis is
    // normalised, so its magnitude cannot scale the angle.
    const Quat delta =
        Quat::from_axis_angle(axis_vector.normalized(), radians(spin_step(degrees_per_second, context.delta_time)));
    const Quat current = Quat::from_euler(owner->transform().rotation);
    owner->transform().rotation = (delta * current).to_euler();
    return Error::none();
}

// --- MoveNode3D ------------------------------------------------------------

Error MoveNode3D::update(const FrameContext& context) {
    Node* owner = node();
    if (owner == nullptr) {
        return Error::none();
    }

    const ActionIds ids = ActionIds::resolve(context.actions);
    const f32 move_x = axis(held(context, ids.left), held(context, ids.right));
    const f32 move_z = axis(held(context, ids.back), held(context, ids.forward));
    const f32 move_y = lock_vertical ? 0.0f : axis(held(context, ids.down), held(context, ids.up));

    if (move_x == 0.0f && move_y == 0.0f && move_z == 0.0f) {
        return Error::none();
    }

    // The direction in the node's own frame, then rotated into the world by the
    // node's rotation, so a rotated object moves where it is pointing. The local
    // basis is the engine's: +X is right, +Y is up, and -Z is forward (the
    // convention Camera3D looks along), so "forward" is a negative local Z.
    const Vec3 local_direction{move_x, move_y, -move_z};
    const Mat4 rotation = Mat4::rotation_euler(owner->transform().rotation);
    Vec3 direction = transform_direction(rotation, local_direction);
    const f32 length = direction.length();
    if (length > 1e-6f) {
        direction = direction / length;
    }

    const f32 current_speed = speed * (held(context, ids.run) ? run_multiplier : 1.0f);
    owner->transform().position = owner->transform().position + direction * (current_speed * context.delta_time);
    return Error::none();
}

// --- CharacterController3D -------------------------------------------------

// The Y rotation that turns a local -Z forward by `yaw` about the world up axis.
// Kept in one place so the facing used to move and the facing written back to the
// node are provably the same angle.
[[nodiscard]] Vec3 facing_from_yaw(f32 yaw) noexcept {
    // The engine's Y rotation is counter-clockwise seen from above and its
    // forward is -Z, so yaw 0 faces -Z and a positive yaw swings towards -X.
    return Vec3{-std::sin(yaw), 0.0f, -std::cos(yaw)};
}

Error CharacterController3D::attach(Node& node) {
    // Start from the authored facing, so a character placed looking a particular
    // way in the editor starts that way in the game. Only the Y rotation is
    // taken: the controller keeps the character upright.
    yaw_ = node.transform().rotation.y;
    vertical_velocity_ = 0.0f;
    grounded_ = false;

    // The controller owns the vertical axis. A body left under the solver's
    // gravity would fight this script for Y, so it is turned off here rather than
    // left to the scene author to remember.
    if (auto* body = node.get_component<physics3d::RigidBody3D>(); body != nullptr) {
        body->affected_by_gravity = false;
        // The controller writes the body's velocity every frame, so the body must
        // never sleep: a sleeping body ignores that write and the character would
        // freeze after standing still long enough to settle.
        body->can_sleep = false;
    }
    return Error::none();
}

Error CharacterController3D::update(const FrameContext& context) {
    Node* owner = node();
    if (owner == nullptr) {
        return Error::none();
    }

    const f32 delta = context.delta_time;
    if (delta <= 0.0f) {
        return Error::none();
    }

    const ActionIds ids = ActionIds::resolve(context.actions);

    // Turn first, so this frame's movement is along the facing the player just
    // chose rather than the one they had last frame. A turn is rate-based, so it
    // is frame-rate independent.
    const f32 turn = axis(held(context, ids.right), held(context, ids.left));
    yaw_ += turn * radians(turn_speed) * delta;
    if (yaw_ > kPi) {
        yaw_ -= 2.0f * kPi;
    } else if (yaw_ < -kPi) {
        yaw_ += 2.0f * kPi;
    }

    // The character is kept upright: pitch and roll are always zero. That is what
    // a character wants, and it is also what keeps the axis-aligned collider
    // honest, since the solver collides the unrotated box.
    owner->transform().rotation = Vec3{0.0f, yaw_, 0.0f};

    const Vec3 facing = facing_from_yaw(yaw_);
    const f32 walk = axis(held(context, ids.back), held(context, ids.forward));
    const f32 move_speed = speed * (held(context, ids.run) ? run_multiplier : 1.0f);

    Vec3 desired = facing * (walk * move_speed);
    desired.y = 0.0f;

    // Ground probe: a ray straight down from just below the character's own
    // collider. Starting strictly below the collider's half-height is what stops
    // the probe from hitting the character itself, which would report it as
    // permanently grounded. The scene may be absent in a headless test, in which
    // case the character simply has no ground to find and falls.
    const Scene* scene = owner->scene();
    const f32 half_height = collider_half_height(*owner);
    const Vec3 position = owner->transform().position;
    const Vec3 probe_origin = position - Vec3{0.0f, half_height + kProbeClearance, 0.0f};
    const f32 probe_distance = ground_probe;

    grounded_ = false;
    if (scene != nullptr && probe_distance > 0.0f) {
        const physics3d::RaycastHit hit =
            physics3d::raycast(*scene, probe_origin, Vec3{0.0f, -1.0f, 0.0f}, probe_distance);
        grounded_ = hit.hit;
    }

    // Wall slide: if the desired move runs into something, keep only the part of
    // it that is not into the wall, so a character walking into a wall at an
    // angle slides along it rather than stopping dead. The probe is a short ray
    // from the character's centre along the move, at chest height so a low step
    // does not count as a wall.
    if (slide_along_walls && scene != nullptr && desired.length_squared() > 1e-8f) {
        const Vec3 move_dir = desired.normalized();
        // Start the probe beyond the character's own collider, or it would strike
        // the character itself and report a wall that is not there.
        const f32 own_reach = collider_horizontal_reach(*owner, move_dir);
        const f32 reach = desired.length() * delta + ground_probe;
        const Vec3 chest = position + Vec3{0.0f, half_height * 0.5f, 0.0f} + move_dir * (own_reach + kProbeClearance);
        const physics3d::RaycastHit wall = physics3d::raycast(*scene, chest, move_dir, reach);
        if (wall.hit) {
            // Remove the component into the surface; what is left is the slide.
            const Vec3 into_wall = wall.normal * desired.dot(wall.normal);
            desired = desired - into_wall;
        }
    }

    // Vertical motion. The script owns Y: a jump sets an upward velocity, gravity
    // pulls it down, and a grounded character is held at zero so it does not
    // accumulate a fall speed while standing still.
    if (grounded_ && vertical_velocity_ <= 0.0f) {
        vertical_velocity_ = 0.0f;
    }
    if (grounded_ && context.actions != nullptr && context.actions->was_pressed(ids.jump)) {
        vertical_velocity_ = jump_speed;
        grounded_ = false;
    }
    vertical_velocity_ -= gravity * delta;

    // Hand the motion to the physics body as a velocity, so the solver does the
    // moving and the character collides with the world like any other body. The
    // Y the script computed is written here; a body with no rigid body is moved
    // directly, which is what lets the controller run in a scene with no physics.
    if (auto* body = owner->get_component<physics3d::RigidBody3D>(); body != nullptr) {
        body->velocity = Vec3{desired.x, vertical_velocity_, desired.z};
    } else {
        owner->transform().position = position + Vec3{desired.x, vertical_velocity_, desired.z} * delta;
    }

    return Error::none();
}

// --- FollowCamera3D --------------------------------------------------------

Error FollowCamera3D::update(const FrameContext& context) {
    Node* owner = node();
    if (owner == nullptr || target.empty()) {
        return Error::none();
    }
    Scene* scene = owner->scene();
    if (scene == nullptr) {
        return Error::none();
    }

    // Resolve the target by name once, then keep the id. A node's id is stable
    // and the id lookup is a hash, so a follow does not walk the scene every
    // frame. The id is re-resolved if the node it named has gone away (a scene
    // reload), so the camera reconnects instead of holding a dangling id.
    Node* followed = resolved_ ? scene->find_node(target_id_) : nullptr;
    if (followed == nullptr) {
        followed = scene->find_node_by_name(target);
        resolved_ = followed != nullptr;
        target_id_ = followed != nullptr ? followed->id() : Uuid{};
    }
    if (followed == nullptr) {
        return Error::none();
    }

    const Transform& target_transform = followed->transform();
    const f32 target_yaw = target_transform.rotation.y;

    // The offset is expressed in the target's own frame and rotated by its yaw,
    // so the camera swings round behind the character as it turns. The engine's
    // forward is -Z, so "behind" is +Z.
    const Vec3 behind = Vec3{std::sin(target_yaw), 0.0f, std::cos(target_yaw)};
    const Vec3 desired = target_transform.position + behind * distance + Vec3{0.0f, height, 0.0f};

    Transform& transform = owner->transform();
    if (smoothing > 0.0f) {
        // Frame-rate-independent exponential smoothing: the same fraction of the
        // remaining gap is closed per second however long the frame was. A clamp
        // keeps the factor in [0, 1] so a long frame cannot overshoot.
        const f32 factor = std::clamp(1.0f - std::exp(-smoothing * context.delta_time), 0.0f, 1.0f);
        transform.position = transform.position + (desired - transform.position) * factor;
    } else {
        transform.position = desired;
    }

    // Aim at a point ahead of the target, so the character is not dead centre and
    // the player can see where they are going. The camera's own forward is its
    // local -Z, and the node is kept level (yaw only) so the horizon stays put.
    const Vec3 aim = target_transform.position + Vec3{0.0f, look_height, 0.0f} +
                     Vec3{-std::sin(target_yaw), 0.0f, -std::cos(target_yaw)} * look_ahead;
    const Vec3 to_aim = aim - transform.position;
    const f32 flat = std::sqrt(to_aim.x * to_aim.x + to_aim.z * to_aim.z);
    transform.rotation = Vec3{std::atan2(to_aim.y, flat), std::atan2(-to_aim.x, -to_aim.z), 0.0f};

    return Error::none();
}

} // namespace fe::player
