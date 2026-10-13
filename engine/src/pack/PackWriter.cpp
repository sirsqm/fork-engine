// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/pack/PackWriter.h"

#include "fe/core/FileSystem.h"

#include <string>
#include <vector>

namespace fe::pack {

Result<PackFile> PackWriter::build(const AssetDatabase& assets, std::string_view startup_scene_path,
                                   std::string_view scene_key) {
    if (assets.root().empty()) {
        return Error("pack: the asset database has no root; nothing to pack");
    }

    PackFile pack;

    // Every file under the asset root, keyed by its path relative to the root -
    // exactly the string a loader is given. An indexed file carries its scheme
    // and id so a "guid:" reference resolves from the pack alone; an unindexed
    // file is still packed (under the default scheme, with no id) so a project
    // that references it by path runs from the pack.
    //
    // Sidecars (".meta") are metadata, not assets: the scheme and id they hold
    // are already on the entry, so they are not packed.
    for (const std::string& relative : fs::list_files_recursive(assets.root())) {
        if (fs::extension(relative) == ".meta") {
            continue;
        }
        auto bytes = fs::read_binary_file(fs::join(assets.root(), relative));
        if (!bytes) {
            return bytes.error();
        }
        if (const AssetDatabase::Asset* asset = assets.find_by_path(relative)) {
            pack.add_file(relative, asset->scheme, asset->id, std::move(bytes.value()));
        } else {
            pack.add_file(relative, std::string{}, Uuid{}, std::move(bytes.value()));
        }
    }

    // The startup scene, under the key the runtime will ask for. It has no guid:
    // it is not an imported asset, it is the entry point.
    auto scene_bytes = fs::read_binary_file(startup_scene_path);
    if (!scene_bytes) {
        return scene_bytes.error();
    }
    if (const PackEntry* existing = pack.find_path(scene_key)) {
        // The scene usually lives outside the asset root, but it may live inside
        // it, in which case the tree scan already packed it under this key. That
        // is the same file, not a collision; only different bytes are an error.
        if (existing->data != scene_bytes.value()) {
            return Error("pack: scene key '" + std::string(scene_key) + "' collides with an asset path");
        }
        return pack;
    }
    pack.add_file(std::string(scene_key), std::string{}, Uuid{}, std::move(scene_bytes.value()));

    return pack;
}

Error PackWriter::write(std::string_view pack_path, const AssetDatabase& assets, std::string_view startup_scene_path,
                        std::string_view scene_key) {
    auto built = build(assets, startup_scene_path, scene_key);
    if (!built) {
        return built.error();
    }
    auto bytes = built.value().write();
    if (!bytes) {
        return bytes.error();
    }
    return fs::write_binary_file(pack_path, bytes.value().data(), bytes.value().size());
}

} // namespace fe::pack
