// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

namespace fe::reflect {

// Registers the anim module's component types (AnimationStateMachine) with the
// reflection registry.
//
// Kept separate from register_builtin_types() for the same reason
// register_physics_types() is: the reflection layer must not depend on a module
// above it, so each module registers its own types and the engine calls them
// all during startup. A tool or test that uses the anim module without an
// Engine calls this directly. Idempotent, like the built-in registration.
void register_anim_types();

} // namespace fe::reflect
