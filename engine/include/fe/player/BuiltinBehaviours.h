// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Uuid.h"
#include "fe/platform/InputActions.h"
#include "fe/script/Script.h"
#include "fe/script/ScriptRegistry.h"

#include <string>
#include <string_view>

namespace fe {

class ActionMap;

// Engine-shipped gameplay behaviours, and the standard action set they read.
//
// Why this module exists
// ----------------------
// Gameplay logic has so far lived in C++ gameplay modules: shared libraries the
// engine loads at run time (ADR-0044, ADR-0061). A browser has no dynamic
// loader, so `fork_export --target web` refuses to record a module at all — a
// web build could render a scene but had no logic to run it. The renderer, the
// input backend and the action seam all worked; the thing that *consumes* input
// did not exist on that target.
//
// The fix is not "make dlopen work in a browser" (it cannot). It is to put a
// usable set of behaviours *in the engine*, compiled into every target including
// wasm, and registered in-process. A module is still how a game ships its own
// logic on desktop; these are the behaviours a game can ship everywhere,
// including where no module can be loaded.
//
// This is deliberately a separate module from `script`: `script` is the
// language-agnostic seam (what a Script *is*), and this is a library of
// behaviours built on it (what a Script can *do*). Keeping them apart means the
// seam stays small and a game that wants none of these pays nothing for them.
//
// The behaviours are ordinary Scripts. They register into a ScriptRegistry like
// any module's would, a scene binds them by name like any other behaviour, and a
// host drives them through the ordinary ScriptSystem — no new execution model,
// and nothing here is web-specific.
//
// Portability
// -----------
// Everything in this module compiles for every target. It reads input through
// FrameContext (the action map and the input snapshot), never from a platform
// API, so the identical code drives a keyboard on desktop and a touchscreen in a
// browser (ADR-0075).

namespace player {

// The standard gameplay actions. A game that wants the built-in behaviours to
// work declares this set once; the names are the contract between the
// declaration below and the behaviours that read them, so they live in one place
// rather than as string literals scattered across two files.
namespace actions {
inline constexpr std::string_view kMoveForward = "MoveForward";
inline constexpr std::string_view kMoveBack = "MoveBack";
inline constexpr std::string_view kMoveLeft = "MoveLeft";
inline constexpr std::string_view kMoveRight = "MoveRight";
inline constexpr std::string_view kMoveUp = "MoveUp";
inline constexpr std::string_view kMoveDown = "MoveDown";
inline constexpr std::string_view kLookUp = "LookUp";
inline constexpr std::string_view kLookDown = "LookDown";
inline constexpr std::string_view kLookLeft = "LookLeft";
inline constexpr std::string_view kLookRight = "LookRight";
inline constexpr std::string_view kRun = "Run";
inline constexpr std::string_view kJump = "Jump";
} // namespace actions

// Declares the standard actions on `map` and binds each to the devices a player
// is likely to have: a keyboard, and — for a phone — a touch region.
//
// This is the one place the standard bindings are written down, so a game gets a
// keyboard *and* a touchscreen for free and cannot drift between them. It is
// what makes the built-in behaviours playable on a phone without the game
// author writing any input code at all, which is the `touch-input-actions` and
// `touch-gesture-regions` seam applied to shipped behaviour.
//
// The layout is the conventional one: WASD and the arrow keys both move, Space
// and Shift move vertically, and a phone gets a virtual stick (the lower-left
// eighth of the screen) plus a look strip (the right half). Regions are
// normalised, so they survive a rotation or a resize (ADR-0077).
void declare_builtin_actions(ActionMap& map);

// Registers every built-in behaviour into `registry`, by the name a scene binds:
//
//   "FlyCamera3D"   a first-person camera: move with actions, look by dragging
//   "SpinNode3D"    rotates its node about an axis, at a configurable rate
//   "MoveNode3D"    translates its node with the movement actions
//
// A host calls this once, before it loads a scene, so the behaviours a scene
// binds by name resolve to live instances. The player calls it for every target,
// which is what gives a web build its gameplay; the editor calls it so a
// designer can attach these in the inspector.
void register_builtin_behaviours(ScriptRegistry& registry);

// --- The behaviours --------------------------------------------------------
//
// Exposed as concrete types (not only as registry entries) so a test can drive
// one directly, without a scene file, and assert what it does to a node.

// A first-person camera controller.
//
// Movement is relative to where the camera is looking, so "forward" is always
// into the screen, which is what a player expects and what makes the same code
// work for a free-fly camera and a walker. Yaw and pitch are kept as state and
// the node's rotation is written from them, rather than accumulating rotation
// onto the node, so the angles cannot drift out of range over a long session.
//
// The angle convention is the engine's: a rotation of (0, 0, 0) looks down -Z
// (the camera's own convention, ADR: see Camera3D), positive pitch looks up, and
// positive yaw turns right. Because the engine composes Euler angles as
// Rz * Ry * Rx, the yaw is recovered by `Quat::to_euler` in [-pi/2, pi/2] — so
// this controller drives the view directly from its own yaw and pitch rather
// than round-tripping them through the node, which would fold a 180-degree turn
// into a flipped view.
//
// It reads gameplay actions, never keys, so the identical behaviour serves a
// keyboard and a touchscreen (ADR-0075). The node is expected to be a root — a
// camera is normally a sibling of the world, not a child of it — so its local
// transform is treated as its world transform; a camera parented to a moving
// object would need the parent's transform folded in.
class FlyCamera3D final : public Script {
public:
    [[nodiscard]] std::string_view name() const noexcept override { return "FlyCamera3D"; }

    // The pitch limit, just inside a right angle. Public so a test (and a game)
    // can reason about the clamp without restating the number.
    static constexpr f32 kMaxPitchRadians = 1.55334303427f; // 89 degrees

    // Takes the node's authored orientation as the starting yaw and pitch, so a
    // camera aimed in the editor starts aimed the same way in the game.
    Error attach(Node& node) override;
    [[nodiscard]] Error update(const FrameContext& context) override;

    // Tuning. Reflected, so a designer can change it in the inspector and it
    // persists in the scene (ADR-0063).
    f32 move_speed = 4.0f;           // world units per second
    f32 run_multiplier = 3.0f;       // multiplier while the run action is held
    f32 look_speed = 2.2f;           // radians per second for action-driven look
    f32 mouse_sensitivity = 0.0025f; // radians per pixel of drag
    bool enable_mouse_look = true;

    // The accumulated orientation, in radians. Positive pitch looks up; positive
    // yaw turns left (the engine's Y rotation), so looking right decreases yaw.
    f32 yaw = 0.0f;
    f32 pitch = 0.0f;
};

// Rotates its node about a local axis at a constant rate. The simplest possible
// proof that a behaviour runs on a target: no input, no other system, just time.
class SpinNode3D final : public Script {
public:
    [[nodiscard]] std::string_view name() const noexcept override { return "SpinNode3D"; }

    [[nodiscard]] Error update(const FrameContext& context) override;

    // The axis is normalised on use, so a non-unit axis is not a silent scaling
    // bug. Degrees per second, because that is what a designer types.
    f32 axis_x = 0.0f;
    f32 axis_y = 1.0f;
    f32 axis_z = 0.0f;
    f32 degrees_per_second = 90.0f;
};

// Translates its node with the movement actions, in the node's own frame.
//
// Separate from FlyCamera3D because the concerns are: this moves *a thing* (a
// player, a crate), the other aims *a view*. A game that wants a third-person
// camera has one MoveNode3D on the character and a camera that follows it, not a
// camera controller that also happens to move the world.
class MoveNode3D final : public Script {
public:
    [[nodiscard]] std::string_view name() const noexcept override { return "MoveNode3D"; }

    [[nodiscard]] Error update(const FrameContext& context) override;

    f32 speed = 3.0f;
    f32 run_multiplier = 2.5f;
    // Keeps the node's Y fixed, so a "walking" object does not drift off the
    // floor when the up/down actions are pressed by a player exploring the keys.
    bool lock_vertical = true;
};

// A third-person character controller: the "player controller" a game ships.
//
// Why this is engine-shipped
// --------------------------
// A character controller is the one behaviour every game needs and no game
// should have to write from scratch. It is here rather than in a gameplay module
// for the reason ADR-0078 gives: a browser has no dynamic loader, so a module is
// exactly what an exported web game cannot load. A controller that lived only in
// a module would make "a playable 3D game in the browser" impossible to ship.
//
// The model
// ---------
// This is a *kinematic* controller: it decides the velocity the character should
// have and hands that to the physics step, rather than applying forces and hoping
// the solver lands the character where the player pointed. That is the standard
// model for a platformer or a third-person action game, and it is what makes the
// movement feel the same on every machine: the character moves at `speed`, not at
// "whatever the impulse integrates to".
//
// It is written to drive a node that carries a `RigidBody3D` and a `Collider3D`,
// with the body's gravity left *off* (`affected_by_gravity = false`): the script
// owns the vertical velocity, so it can decide what "grounded" means and how a
// jump feels, and so the fall is frame-rate independent and testable. A body left
// under the solver's gravity would fight the script for the Y axis.
//
// Ground and walls are found with the read-only `physics3d` queries against the
// scene the node belongs to, so the controller needs no contact callback and
// works whether or not a `Physics3DSystem` ever ran. The scene comes from
// `Node::scene()`, so the behaviour needs nothing beyond its own node.
//
// Orientation
// -----------
// The character stays upright: its yaw is kept as state and written back as a
// pure Y rotation, and pitch and roll are never touched. That is both what a
// character wants (it should not tip over) and what keeps the axis-aligned
// collider honest, since the solver collides the unrotated box (ADR-0043).
//
// Turning is by action, not by mouse: `MoveLeft`/`MoveRight` yaw the character,
// `MoveForward`/`MoveBack` walk along its facing. A third-person game is played
// with the camera behind the character, so this is the layout that needs no mouse
// capture — which a browser only grants after a user gesture, and which a phone
// does not have at all. A mouse-look variant is a later behaviour, not a change
// to this one.
//
// It reads gameplay actions, never keys, so the identical behaviour serves a
// keyboard and a touchscreen (ADR-0075).
class CharacterController3D final : public Script {
public:
    [[nodiscard]] std::string_view name() const noexcept override { return "CharacterController3D"; }

    [[nodiscard]] Error attach(Node& node) override;
    [[nodiscard]] Error update(const FrameContext& context) override;

    // Walk speed, in world units per second.
    f32 speed = 4.5f;
    // Multiplier applied while the run action is held.
    f32 run_multiplier = 2.0f;
    // Turn rate, in degrees per second, for the left/right actions.
    f32 turn_speed = 150.0f;
    // Upward velocity given at the start of a jump, in world units per second.
    // A larger value jumps higher; the height reached is v^2 / (2 g).
    f32 jump_speed = 6.0f;
    // Downward acceleration, in world units per second squared. Positive here is
    // a downward pull, so the sign is written the way a designer says it.
    f32 gravity = 20.0f;
    // How far below the character's centre the ground probe looks, beyond the
    // collider's own half-height. A small margin lets the probe find the floor
    // across a frame's fall without reaching through it.
    f32 ground_probe = 0.1f;
    // Whether the character may step over a low obstacle by itself. When true a
    // blocked move slides along the obstacle instead of stopping dead, which is
    // what keeps a character from sticking to a wall it walks into at an angle.
    bool slide_along_walls = true;

    // Whether the character was on the ground at the end of the last update. Read
    // by a test and by a follow camera that wants to know when to steady itself.
    [[nodiscard]] bool grounded() const noexcept { return grounded_; }

    // The character's facing, in radians about Y. Positive turns left, matching
    // the engine's Y rotation (FlyCamera3D's convention).
    [[nodiscard]] f32 yaw() const noexcept { return yaw_; }

private:
    bool grounded_ = false;
    f32 yaw_ = 0.0f;
    // The fall speed the script owns, kept between frames so a jump arcs rather
    // than teleporting. It is separate from the body's velocity, which the solver
    // also writes; the script's value is authoritative for Y.
    f32 vertical_velocity_ = 0.0f;
};

// A camera that trails a named node, looking at it from behind and above.
//
// The companion to CharacterController3D: a third-person game needs a camera
// that follows the character, and the follow logic (a smoothed offset behind the
// target's facing) is the same in every game, so it ships here too.
//
// The target is named rather than referenced by a stored pointer, so a scene
// records `target = "Player"` and the behaviour resolves it by name. It resolves
// lazily on the first update and re-resolves if the node goes away, so the order
// of nodes in the scene file does not matter and a reloaded scene reconnects
// without the camera holding a stale pointer.
//
// The offset is expressed in the target's own frame (behind it and above it) and
// rotated by the target's yaw, so the camera swings round behind the character as
// it turns. The camera's position is smoothed towards the desired one, so a turn
// or a jump does not snap the view.
class FollowCamera3D final : public Script {
public:
    [[nodiscard]] std::string_view name() const noexcept override { return "FollowCamera3D"; }

    [[nodiscard]] Error update(const FrameContext& context) override;

    // The node to follow, by name. A scene sets this to the character's node
    // name. Empty means "follow nothing", and the behaviour is inert.
    std::string target = "Player";
    // How far behind the target the camera sits, in world units. Positive is
    // behind (the engine's forward is -Z, so "behind" is +Z in the target's
    // frame).
    f32 distance = 7.0f;
    // How far above the target the camera sits.
    f32 height = 3.2f;
    // How far ahead of the target the camera aims, so the character is not dead
    // centre and the player can see where they are going.
    f32 look_ahead = 1.5f;
    // How far above the target's own position the camera aims.
    f32 look_height = 1.0f;
    // How quickly the camera catches up to its desired position, per second. A
    // larger value tracks more tightly; 0 disables smoothing (the camera snaps).
    f32 smoothing = 8.0f;

private:
    // The resolved target's id. A name is resolved once and then kept as an id,
    // because ids are stable across a reload and the id lookup is a hash rather
    // than a walk of the scene.
    Uuid target_id_{};
    bool resolved_ = false;
};

} // namespace player
} // namespace fe
