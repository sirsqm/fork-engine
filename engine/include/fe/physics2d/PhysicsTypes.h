// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

namespace fe::reflect {

// Registers the physics2d component types (Collider2D, RigidBody2D,
// DistanceJoint2D, RevoluteJoint2D) with the reflection registry.
//
// Kept separate from register_builtin_types() so that the reflection layer does
// not have to depend on the physics module. The engine calls both during
// startup; a tool or test that uses physics without an Engine calls this
// directly. Idempotent, like the built-in registration.
void register_physics_types();

} // namespace fe::reflect
