// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Error.h"
#include "fe/core/Types.h"

#include <string>
#include <string_view>
#include <vector>

namespace fe {

class AssetDatabase;

namespace serialization::scene {

// Collects the resource references a scene document names, so a packer can ship
// only what the scene uses rather than the whole asset tree
// (docs/adr/0065-pack-reachability-pruning.md).
//
// A reference is found two ways:
//
//   * A component property whose reflected kind is ResourceRef, located through
//     the reflection registry. The serializer already persists these, so this
//     needs no per-component knowledge: a new resource property is traced the
//     moment it is registered, exactly as it is persisted.
//   * Every string inside a script's "properties" object. A behaviour's property
//     kinds are known only once its module is loaded, which a packer does not do,
//     so a script string is a candidate rather than a certainty. The caller
//     resolves candidates against its asset database and keeps the ones that name
//     a real file, so a false candidate costs nothing and a script resource
//     reference is not missed.
//
// References are returned as they are written in the document: a stable
// "guid:<uuid>" key or a loader key ("tex:assets/hero.png"). The caller resolves
// them; this function reads no files and needs no manager.
//
// The caller must have registered the component types it wants traced. This
// registers the builtin 2D types (as the scene reader does) so a plain scene is
// covered without setup; a scene using a 3D or audio component is traced once the
// caller registers that module's types, which the engine's own startup already
// does.
[[nodiscard]] Result<std::vector<std::string>> collect_resource_references(std::string_view scene_json);

// Maps one reference to a root-relative asset path, or an empty string when it
// names nothing the database knows. A "guid:<uuid>" id is looked up for its
// current path; a "<scheme>:<path>" or bare path has its scheme stripped. The
// scheme is informational here: the pack keys an asset by its path, which is what
// a loader asks for once it is mounted (docs/adr/0030-asset-pack-format.md).
[[nodiscard]] std::string reference_to_asset_path(std::string_view reference, const AssetDatabase& assets);

} // namespace serialization::scene
} // namespace fe
