// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/pack/AssetClosure.h"

#include "fe/core/FileSystem.h"

#include <unordered_set>

namespace fe::pack {

std::vector<std::string> expand_asset_dependencies(std::vector<std::string> paths, std::string_view pack_root,
                                                   const AssetDependencyReader& reader) {
    const std::string root(pack_root);
    std::unordered_set<std::string> seen;
    seen.reserve(paths.size() * 2 + 1);
    for (const std::string& path : paths) {
        seen.insert(fs::normalize(path));
    }

    // A worklist rather than a nested loop: a dependency can itself be a
    // container, so expansion continues until the set stops growing.
    for (std::size_t i = 0; i < paths.size(); ++i) {
        auto bytes = fs::read_binary_file(fs::join(root, paths[i]));
        if (!bytes) {
            continue;
        }
        const std::string_view content(reinterpret_cast<const char*>(bytes.value().data()), bytes.value().size());
        for (std::string& dependency : reader(paths[i], content)) {
            if (seen.insert(fs::normalize(dependency)).second) {
                paths.push_back(std::move(dependency));
            }
        }
    }

    return paths;
}

} // namespace fe::pack
