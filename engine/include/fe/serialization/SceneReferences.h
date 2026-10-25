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

// The resource references a scene document names, split by how certain they are.
//
// The split is the point, not a convenience: a component's `ResourceRef` property
// is a certainty (the reflection registry says the field *is* a resource), while
// a string inside a script's `"properties"` object is only a candidate (a
// behaviour's property kinds are unknown until its module is loaded). A caller
// must therefore treat the two differently: a missing `required` reference is a
// broken scene and a hard error, while a missing `candidate` is simply a string
// that was not an asset - a plain label like "Player" - and is discarded
// (docs/adr/0065-pack-reachability-pruning.md). Treating a candidate as required
// makes every scene that carries any string script property (a follow camera's
// `target`, a door's `opens_with`) fail to pack, which is the bug this split
// fixes.
struct SceneReferences {
    // References from reflected ResourceRef properties: guaranteed to be
    // resources. A caller must resolve each one or fail.
    std::vector<std::string> required;
    // Every string inside a script's properties object. The caller resolves each
    // against its asset database and keeps the ones that name a real asset.
    std::vector<std::string> candidates;
};

// Collects the resource references a scene document names, so a packer can ship
// only what the scene uses rather than the whole asset tree
// (docs/adr/0065-pack-reachability-pruning.md).
//
// A reference is found two ways:
//
//   * A component property whose reflected kind is ResourceRef, located through
//     the reflection registry. The serializer already persists these, so this
//     needs no per-component knowledge: a new resource property is traced the
//     moment it is registered, exactly as it is persisted. These are `required`.
//   * Every string inside a script's "properties" object. A behaviour's property
//     kinds are known only once its module is loaded, which a packer does not do,
//     so a script string is a `candidate` rather than a certainty.
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
[[nodiscard]] Result<SceneReferences> collect_resource_references(std::string_view scene_json);

// Maps one reference to a root-relative asset path, or an empty string when it
// names nothing the database knows. A "guid:<uuid>" id is looked up for its
// current path; a "<scheme>:<path>" or bare path has its scheme stripped. The
// scheme is informational here: the pack keys an asset by its path, which is what
// a loader asks for once it is mounted (docs/adr/0030-asset-pack-format.md).
[[nodiscard]] std::string reference_to_asset_path(std::string_view reference, const AssetDatabase& assets);

// Resolves a traced scene's references to root-relative asset paths, applying the
// required/candidate policy in one place so `fork_pack` and `fork_export` cannot
// drift (they had, and a script string property made both refuse to pack a scene
// whose every asset was present).
//
//   * Every `required` reference must resolve to an indexed asset or to a file
//     present under `pack_root`; one that resolves to neither is a hard error,
//     because shipping a build whose scene names an asset it does not carry
//     produces a game that runs until it draws.
//   * A `candidate` is kept only when it resolves; the rest (a plain label such
//     as a follow camera's `target`) are discarded, which is what makes tracing
//     script strings safe (docs/adr/0065-pack-reachability-pruning.md).
//
// The returned paths are the ones to hand `PackOptions::include`. Duplicates are
// removed, because a scene may name the same texture from many nodes.
[[nodiscard]] Result<std::vector<std::string>>
resolve_scene_asset_paths(const SceneReferences& references, std::string_view pack_root, const AssetDatabase& assets);

} // namespace serialization::scene
} // namespace fe
