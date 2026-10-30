// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

namespace fe::reflect {

// Registers the skeletal-animation module's component types (Animator,
// SkinnedMeshRenderer3D) with the reflection registry.
//
// Kept separate from `register_anim_types()` and the built-in registration for
// the same reason the other modules keep theirs separate: the reflection layer
// must not depend on a module above it, so each module registers its own types
// and the engine calls them all during startup. A tool or test that uses the
// skeletal module without an Engine calls this directly. Idempotent.
void register_skeletal_animation_types();

} // namespace fe::reflect
