// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Error.h"

#include <nlohmann/json.hpp>

namespace fe {

class Node;
class Scene;

namespace serialization {

// The seam through which the scene serializer persists gameplay scripts.
//
// Scripts are not components: they are code, they are owned by a `ScriptHost`,
// and they are looked up by behaviour name in a `ScriptRegistry` (ADR-0044).
// The serialization module must not depend on the script module to reach them —
// that would invert the layering — so it declares this small interface and the
// script module implements it (ADR-0063). A caller that wants script bindings
// persisted passes an implementation to `scene::to_json`/`from_json`; a caller
// that does not passes nothing, and the document is exactly what it was before
// scripts existed.
//
// The reader is called in three phases, because a binding cannot be made until
// every node exists:
//
//   reset()                  once, before any node is read
//   read(node, node_json)    once per node, in document order
//   finish(scene)            once, after every node is in the scene
//
// `finish` is where a script is actually constructed and attached, so it runs
// after the scene's id index is rebuilt and every node is addressable by the id
// its binding names.
class ScriptBinding {
public:
    virtual ~ScriptBinding() = default;

    // Writes the scripts bound to `node` into `node_json` (an "scripts" array),
    // or leaves it untouched when the node has none.
    virtual void write(const Node& node, nlohmann::json& node_json) const = 0;

    // Begins a new document. Clears any bindings recorded from a previous read.
    virtual void reset() = 0;

    // Records the scripts `node_json` describes for `node`. Nothing is attached
    // yet: the node exists, but the scene is still being built.
    virtual void read(const Node& node, const nlohmann::json& node_json) = 0;

    // Constructs and attaches every recorded script now that the scene is
    // complete. Returns an Error only for a failure that should stop the load;
    // an unknown behaviour or a malformed property is reported and skipped, the
    // same policy the reader applies to an unknown component type.
    [[nodiscard]] virtual Error finish(Scene& scene) = 0;
};

} // namespace serialization
} // namespace fe
