// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/platform/InputActions.h"
#include "fe/script/Script.h"
#include "fe/script/ScriptRegistry.h"

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

} // namespace player
} // namespace fe
