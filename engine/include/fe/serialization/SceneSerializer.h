// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Error.h"
#include "fe/core/Types.h"

#include <memory>
#include <string>
#include <string_view>

namespace fe {

class Scene;
class ResourceManager;

namespace serialization {

// Reads and writes scenes as UTF-8 JSON.
//
// JSON was chosen for the first persistence format because it is diffable,
// human-readable, and trivially inspectable while the data model is still
// moving. A binary format can be added later behind the same string-based API;
// nothing outside this module knows which format is used.
//
// The serialised document is versioned (`format_version`). The reader rejects
// documents from a future version rather than silently misinterpreting them.
//
// Two entry points per direction:
//   * to_json / from_json work on strings and never touch the filesystem. They
//     are what the tests exercise and what the editor will use for undo/redo.
//   * save / load add file I/O on top.
namespace scene {

inline constexpr i32 kFormatVersion = 1;

// Serialises `scene` to a JSON string.
[[nodiscard]] Result<std::string> to_json(const Scene& scene);

// Deserialises a scene from a JSON string. On success `out_scene` is replaced.
// `resources` may be null; when provided, sprite textures are resolved through
// it so a loaded scene comes back fully formed.
[[nodiscard]] Error from_json(std::string_view text, Scene& out_scene, ResourceManager* resources = nullptr);

[[nodiscard]] Error save(const Scene& scene, std::string_view path);
[[nodiscard]] Error load(std::string_view path, Scene& out_scene, ResourceManager* resources = nullptr);

} // namespace scene
} // namespace serialization
} // namespace fe
