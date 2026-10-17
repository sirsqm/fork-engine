// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

namespace fe::reflect {

// Registers the scene module's tilemap component type (Tilemap) with the
// reflection registry.
//
// Kept separate from register_builtin_types() for the same reason
// register_anim_types() is: a component whose data is a variable-length list
// registers its own properties and its own JSON accessors, and the engine calls
// every module's registration during startup. A tool or test that uses the
// tilemap without an Engine calls this directly. Idempotent, like the built-in
// registration.
void register_tilemap_types();

} // namespace fe::reflect
