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

// Version 1 stored resource references as path-derived keys ("tex:assets/hero.png").
// Version 2 stores them as stable asset GUIDs ("guid:<uuid>"), which survive a
// file move. Version 3 gives every component a stable `Uuid` in the document
// (`"id"`), the counterpart of the node id that has been written since version
// 1; older documents load with ids assigned at creation. The reader migrates
// version-1 documents at load time. See
// docs/adr/0020-resource-identity-guid-sidecar.md and
// docs/adr/0045-component-identity.md.
inline constexpr i32 kFormatVersion = 3;

// Serialises `scene` to a JSON string.
//
// `resources` may be null. When it is provided and its asset database knows a
// referenced asset, the reference is written as a stable "guid:" key; otherwise
// it is written as the loader's path-derived key. A null manager therefore
// produces the version-1 reference shape, which still loads.
[[nodiscard]] Result<std::string> to_json(const Scene& scene, const ResourceManager* resources = nullptr);

// Deserialises a scene from a JSON string. On success `out_scene` is replaced.
// `resources` may be null; when provided, resource references are resolved
// through it so a loaded scene comes back fully formed.
//
// A version-1 document (path-derived references) is migrated: each reference is
// looked up in the manager's asset database and rewritten to a "guid:" key. A
// version-1 document with a reference that has no asset entry is reported as an
// error rather than loaded with a silently broken reference.
[[nodiscard]] Error from_json(std::string_view text, Scene& out_scene, ResourceManager* resources = nullptr);

[[nodiscard]] Error save(const Scene& scene, std::string_view path, const ResourceManager* resources = nullptr);
[[nodiscard]] Error load(std::string_view path, Scene& out_scene, ResourceManager* resources = nullptr);

} // namespace scene
} // namespace serialization
} // namespace fe
