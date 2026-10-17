// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Uuid.h"
#include "fe/reflect/Property.h"
#include "fe/reflect/Value.h"

#include <optional>
#include <string>
#include <vector>

namespace fe::serialization {

// The seam through which the operation model reads and writes a scene's live
// gameplay scripts.
//
// A script is not a component: it is owned by a `ScriptHost` in the `script`
// module, which sits above `serialization` (ADR-0044). The operation model must
// therefore not name `ScriptHost` or `ScriptRegistry` directly, or it would
// invert the layering. It declares this small interface instead, and the script
// module implements it over a host and a registry — exactly the arrangement
// `ScriptBinding` uses for the document (ADR-0063), applied here to live edits.
//
// The editor implements this over its `ScriptCatalog`, so a script property edit
// becomes an ordinary serializable operation that the undo stack, and later the
// collaborative log (ADR-0019), can carry.
class ScriptTarget {
public:
    virtual ~ScriptTarget() = default;

    // The behaviour bound to `node_id`, or "" when the node has no script. This
    // is recorded in the operation and checked on apply, so an operation does not
    // write a property to a behaviour the node no longer runs.
    [[nodiscard]] virtual std::string behaviour_of(const Uuid& node_id) const = 0;

    // The current value of the property at `path` on the node's script, or an
    // empty optional when it does not resolve. This is what an inverse reads.
    [[nodiscard]] virtual std::optional<reflect::Value> read(const Uuid& node_id,
                                                             const std::vector<std::string>& path) const = 0;

    // Writes `value` to the property at `path` on the node's script. Returns
    // false when the path does not resolve or the property has no setter.
    [[nodiscard]] virtual bool write(const Uuid& node_id, const std::vector<std::string>& path,
                                     const reflect::Value& value) = 0;
};

} // namespace fe::serialization
