// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Error.h"
#include "fe/pack/PackFile.h"
#include "fe/resources/AssetDatabase.h"

#include <string_view>

namespace fe::pack {

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
    // Returns an Error when the asset root is unset, a file cannot be read, or
    // the scene key collides with an asset path.
    [[nodiscard]] static Result<PackFile> build(const AssetDatabase& assets, std::string_view startup_scene_path,
                                                std::string_view scene_key);

    // build() plus writing the pack to `pack_path`, creating parent directories.
    [[nodiscard]] static Error write(std::string_view pack_path, const AssetDatabase& assets,
                                     std::string_view startup_scene_path, std::string_view scene_key);
};

} // namespace fe::pack
