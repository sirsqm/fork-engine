// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "breakout_state.h"

#include "fe/core/Types.h"
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

} // namespace breakout
