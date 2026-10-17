// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Error.h"
#include "fe/pack/PackFile.h"
#include "fe/resources/AssetDatabase.h"

#include <string_view>
#include <vector>

namespace fe::pack {

// How much of the asset tree to pack.
//
// The default packs everything, which is the behaviour every existing caller
// had. Pruning ships only what the startup scene references (plus anything the
// caller lists), so a build does not carry the whole project's art for one level
// (docs/adr/0065-pack-reachability-pruning.md).
//
// The writer stays scene-agnostic: it does not parse the scene. The caller
// resolves the scene's references (through
// `serialization::scene::collect_resource_references`) and any asset a game
// references only from code, and passes them here.
struct PackOptions {
    // When false (default), pack every file under the asset root, exactly as
    // before pruning existed. When true, pack only the startup scene and the
    // paths listed in `include`.
    bool prune = false;

    // Root-relative paths to pack when pruning. A path is matched against the
    // tree's own relative paths, so a caller passes what it read from a scene
    // ("assets/hero.png") without re-deriving the join. Ignored when `prune` is
    // false.
    std::vector<std::string> include;
};

// Turns a project's asset tree and startup scene into one pack.
//
// The writer gathers bytes; it does not parse the project file and it does not
// know what a project is. The caller (the `fork_pack` tool, or a future export
// step) resolves the project's asset root and startup scene and passes them in,
// so the engine stays project-agnostic exactly as it is today
// (docs/adr/0025-project-file.md, docs/adr/0030-asset-pack-format.md).
//
// The pack holds the startup scene plus every asset the database has indexed,
// keyed by the same relative path a loader is given ("assets/hero.png") and
// carrying each asset's scheme and stable id. Unindexed files are not packed;
// importing an asset is already what gives it a stable reference
// (docs/adr/0020-resource-identity-guid-sidecar.md).
class PackWriter {
public:
    // Builds a pack from the database's rooted asset tree and one scene file.
    //
    // `assets` must have a root set (its indexed paths are read from there).
    // `startup_scene_path` is the scene file on disk; `scene_key` is the path it
    // is stored under in the pack (for example "scenes/main.fescene"), which is
    // the key the runtime asks for when it loads the scene from the pack.
    //
    // `options` selects whether to pack the whole tree or only what the scene
    // references.
    //
    // Returns an Error when the asset root is unset, a file cannot be read, or
    // the scene key collides with an asset path.
    [[nodiscard]] static Result<PackFile> build(const AssetDatabase& assets, std::string_view startup_scene_path,
                                                std::string_view scene_key, const PackOptions& options = {});

    // build() plus writing the pack to `pack_path`, creating parent directories.
    [[nodiscard]] static Error write(std::string_view pack_path, const AssetDatabase& assets,
                                     std::string_view startup_scene_path, std::string_view scene_key,
                                     const PackOptions& options = {});
};

} // namespace fe::pack
