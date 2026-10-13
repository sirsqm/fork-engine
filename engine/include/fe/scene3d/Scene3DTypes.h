// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

namespace fe::reflect {

// Registers the 3D scene component types (Camera3D, MeshRenderer3D,
// DirectionalLight3D) with the reflection registry.
//
// Kept separate from register_builtin_types() so that the reflection layer does
// not have to depend on the 3D scene and mesh modules, mirroring how
// register_physics_types() keeps physics out of it. The engine calls this
// during startup; a tool or test that builds 3D scenes without an Engine calls
// it directly. Idempotent.
void register_scene3d_types();

} // namespace fe::reflect
