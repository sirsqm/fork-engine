// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Error.h"
#include "fe/core/Types.h"
#include "fe/reflect/Property.h"

#include <nlohmann/json.hpp>

#include <vector>

namespace fe {

class ResourceManager;

namespace serialization {

// The mapping between a reflected property and its JSON representation.
//
// The document shape is a stable part of the file format, so it lives here
// rather than in the reflection layer:
//
//   Bool            -> boolean
//   Int, Float      -> number
//   String          -> string
//   Uuid            -> string
//   ResourceRef     -> string (a stable "guid:<uuid>" key when the asset is
//                             known to the asset database, otherwise the
//                             loader's path key "<scheme>:<path>")
//   Group (array)   -> array of the members, e.g. [x, y]
//   Group (struct)  -> object keyed by member name, e.g. {"x":..,"y":..}
//
// These helpers are what let one description drive more than one document. The
// scene serializer reads and writes components through them, and the script
// bindings (ADR-0063) read and write a behaviour's properties through the same
// functions, so a property is spelled the same way wherever it is persisted and
// a new property kind is one edit here rather than one per document.

// Writes one property of `object` to JSON. `object` is the owning object the
// property's accessors expect (a Component or a Script).
[[nodiscard]] nlohmann::json property_to_json(const reflect::Property& property, const void* object,
                                              const ResourceManager* resources);

// Applies one JSON value to a property of `object`. A malformed value is logged
// and skipped rather than fatal: a hand-edited document should still load, and
// the property simply keeps its default.
//
// `source_version` selects how a ResourceRef is interpreted, so a document that
// predates the stable "guid:" keys can be migrated at load time.
[[nodiscard]] Error apply_property(const reflect::Property& property, void* object, const nlohmann::json& source,
                                   ResourceManager* resources, i32 source_version);

// The same two operations over a whole property list, keyed by property name.
// A property absent from `source` is left at its default.
[[nodiscard]] nlohmann::json properties_to_json(const std::vector<reflect::Property>& properties, const void* object,
                                                const ResourceManager* resources);

[[nodiscard]] Error apply_properties(const std::vector<reflect::Property>& properties, void* object,
                                     const nlohmann::json& source, ResourceManager* resources, i32 source_version);

} // namespace serialization
} // namespace fe
