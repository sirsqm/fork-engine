// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

namespace fe::reflect {

// Registers the physics3d component types (Collider3D, RigidBody3D) with the
// reflection registry.
//
// Kept separate from register_builtin_types() so that the reflection layer does
// not have to depend on the physics module. The engine calls both during startup;
// a tool or test that uses physics without an Engine calls this directly.
// Idempotent, like the built-in registration.
void register_physics3d_types();

} // namespace fe::reflect
