// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "breakout_state.h"

#include "fe/core/Types.h"
#include "fe/platform/InputActions.h"
#include "fe/resources/Resource.h"

#include <memory>

namespace fe {
class Scene;
class SceneSchedule;
namespace physics2d {
class Physics2DSystem;
}
} // namespace fe

namespace breakout {

// The textures the field draws with. All optional: a headless test builds the
// same field with no assets, which is why the builder takes them as data rather
// than loading them itself. The colliders and gameplay are identical either way;
// only what is drawn changes.
struct SceneAssets {
    std::shared_ptr<fe::Resource> wall;
    std::shared_ptr<fe::Resource> paddle;
    std::shared_ptr<fe::Resource> ball;
    std::shared_ptr<fe::Resource> brick;
    // The HUD font: the glyph atlas and metrics behind the score and lives text.
    // Missing is not fatal -- the field is built anyway and the text simply does
    // not draw, which is what a headless run wants.
    std::shared_ptr<fe::Resource> font;
};

// Builds the canonical breakout field into `scene`: camera, three walls, paddle,
// ball, the 5x11 brick grid, and the brick-break audio node. It does not clear
// `scene` first, so the caller decides whether to append or replace.
//
// This is the single source of truth for the layout. The authored scene file
// (scenes/breakout.fescene) is a serialization of exactly this, and the tests
// build it in memory; that is what keeps the file path, the fallback path, and
// the tested path from drifting apart.
void build_breakout_scene(fe::Scene& scene, const SceneAssets& assets = {});

// The gameplay actions breakout declares, resolved once so the systems and the
// application agree on the handles. `bind` installs the default device bindings:
// the arrow keys and A/D for moving, Space and Enter for launching, and — the
// point of the layer — touch regions so a phone can play the same game.
//
// The regions are the two lower thirds of the screen plus a central launch
// strip, which is what a two-thumbed player expects: hold left or right to
// steer, tap the middle to serve. They are normalised, so they survive a resize
// and a change of orientation.
struct BreakoutActions {
    fe::ActionMap::Id move_left = fe::ActionMap::kNone;
    fe::ActionMap::Id move_right = fe::ActionMap::kNone;
    fe::ActionMap::Id launch = fe::ActionMap::kNone;
};

// Declares breakout's actions on `actions` and installs their default bindings.
// Separate from install_breakout_systems() so a caller can add its own bindings
// (a rebinding screen, a different control scheme) between the two steps.
[[nodiscard]] BreakoutActions declare_breakout_actions(fe::ActionMap& actions);

// Installs the game's systems on `schedule` in the order the game runs them
// (ADR-0008): paddle control, ball launch, physics2d, brick breaking, game
// state. `state` is borrowed and must outlive the schedule.
//
// The physics system is created here with gravity off and returned so the caller
// can read its contacts or tune it. The audio system is deliberately not added:
// the caller owns the device and the engine's audio system, so it adopts and
// appends that itself (last, so a cue fired this frame is mixed this frame).
[[nodiscard]] fe::physics2d::Physics2DSystem* install_breakout_systems(fe::SceneSchedule& schedule,
                                                                       BreakoutState& state, bool autoplay);

// As above, but wires the systems to `actions` and `bindings` so input arrives
// through the action layer. Passing the bindings from declare_breakout_actions()
// is what makes a touch play the game; the three-argument overload is the
// no-actions path a headless tool and an existing test use.
[[nodiscard]] fe::physics2d::Physics2DSystem* install_breakout_systems(fe::SceneSchedule& schedule,
                                                                       BreakoutState& state, bool autoplay,
                                                                       const BreakoutActions& bindings);

} // namespace breakout
