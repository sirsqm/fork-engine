// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/player/BuiltinBehaviours.h"

#include "fe/core/Math.h"
#include "fe/platform/InputState.h"
#include "fe/reflect/PropertyBuilders.h"
#include "fe/scene/Node.h"
#include "fe/systems/FrameContext.h"

#include <algorithm>
#include <cmath>

namespace fe::player {
namespace {

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

} // namespace fe::player
