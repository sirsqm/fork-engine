// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Error.h"
#include "fe/core/Types.h"
#include "fe/resources/AssetId.h"

#include <string>
#include <string_view>
#include <vector>

namespace fe {

// Where the resource system reads an asset's bytes from.
//
// Today an asset is a file on disk, but a shipping build reads its assets from a
// pack (docs/adr/0018-multi-platform-shipping.md, docs/adr/0030-asset-pack-format.md),
// and a browser or a mobile app does not give a game an ordinary filesystem. The
// loaders and the scene reader must therefore not call the filesystem directly;
// they read through this interface, and the manager decides which implementation
// is in use.
//
// `ResourceManager` is itself a ResourceSource that reads from the filesystem
// (the default). Mounting a different source (`ResourceManager::mount`) changes
// where every load reads from, and nothing above the manager needs to know.
//
// The paths passed here are the same relative, forward-slashed asset paths the
// loaders already receive ("assets/hero.png"); a source maps them to its own
// storage however it likes.
class ResourceSource {
public:
    virtual ~ResourceSource() = default;

    // Reads the whole file at `path`. Returns an Error when it does not exist or
    // cannot be read; an empty file is a successful read of size 0.
    [[nodiscard]] virtual Result<std::vector<u8>> read_binary(std::string_view path) const = 0;

    // Reads the whole file at `path` as text. The bytes are returned verbatim.
    [[nodiscard]] virtual Result<std::string> read_text(std::string_view path) const = 0;

    [[nodiscard]] virtual bool exists(std::string_view path) const = 0;

    // Resolves a stable asset id to its current path and import scheme, or
    // nullptr when the source does not know the id. A filesystem source with an
    // asset database answers from it; a pack answers from its own index, so a
    // pack carries the identity mapping and does not need the loose sidecars.
    struct AssetLocation {
        std::string path;
        std::string scheme;
    };
    [[nodiscard]] virtual const AssetLocation* resolve_asset(const AssetId& id) const {
        (void)id;
        return nullptr;
    }
};

} // namespace fe
