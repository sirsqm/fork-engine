// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

namespace fe::runtime {

// Registers every component type the engine ships with the reflection registry:
// the 2D builtins, tilemaps, animation, 2D and 3D physics, the 3D scene
// components, and audio. Calling it more than once is harmless, because each
// per-module entry point replaces a type's registration rather than appending.
//
// This is the single place that knows the full set. It lives in `runtime`
// because that is the one module allowed to depend on every component module;
// `reflect` cannot host it, as `reflect` must not depend on `scene3d`,
// `physics3d`, `anim`, or `audio` (they depend on `reflect`, so the edge would
// be a cycle).
//
// `Engine::initialize` calls it, and so must any code that loads or traces a
// scene without an Engine: the serializer, the inspector, and the pack and
// export tools. Registering only the 2D builtins is what makes a tool trace a
// 3D scene's mesh and texture references as zero and ship a pack with no assets
// in it (see docs/adr/0076-3d-in-the-browser.md).
void register_all_component_types();

} // namespace fe::runtime
