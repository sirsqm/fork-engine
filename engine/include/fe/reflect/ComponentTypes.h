// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

namespace fe::reflect {

// Registers the engine's built-in component types with the reflection registry.
//
// The scene serializer and, next milestone, the property inspector work purely
// through the registry, so this is where the engine's own component set becomes
// visible to them. Calling it more than once is harmless: each component's
// registration replaces the previous one, which keeps test binaries that share a
// process well defined.
//
// The engine calls this during startup, before any scene is loaded. Code that
// loads scenes without an Engine (for example a test) must call it explicitly.
void register_builtin_types();

} // namespace fe::reflect
