// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/resources/ResourceManager.h"

#include "fe/core/FileSystem.h"
#include "fe/core/Log.h"

#include <string_view>

namespace fe {

void ResourceManager::register_loader(std::unique_ptr<ResourceLoader> loader) {
    if (loader) {
        loaders_.push_back(std::move(loader));
    }
}

Result<std::shared_ptr<Resource>> ResourceManager::load(std::string_view path) {
    const std::string key = fs::normalize(path);

    const auto it = cache_.find(key);
    if (it != cache_.end()) {
        if (auto existing = it->second.resource.lock()) {
            return existing;
        }
        // The entry is stale (the resource was released). Fall through and
        // reload, replacing the dead weak reference.
    }

    // Split an optional "scheme:" prefix from the file path. A Windows drive
    // letter ("C:\...") is not a scheme, so a single-character prefix is left
    // alone.
    std::string_view scheme;
    std::string_view file_path = key;
    const std::size_t colon = key.find(':');
    if (colon != std::string::npos && colon > 1) {
        scheme = std::string_view(key).substr(0, colon);
        file_path = std::string_view(key).substr(colon + 1);
    }

    const std::string extension = fs::extension(file_path);
    for (const auto& loader : loaders_) {
        if (loader->scheme() != scheme || !loader->supports(extension)) {
            continue;
        }
        auto result = loader->load(file_path);
        if (!result) {
            return result.error();
        }
        cache_[key] = CacheEntry{result.value()};
        FE_LOG_DEBUG("loaded %s resource '%s'", result.value()->type_name(), key.c_str());
        return result.value();
    }

    if (!scheme.empty()) {
        return Error("no loader registered for scheme '" + std::string(scheme) + "' with extension '" + extension +
                     "' (resource '" + key + "')");
    }
    return Error("no loader registered for extension '" + extension + "' (resource '" + key + "')");
}

void ResourceManager::collect_garbage() {
    for (auto it = cache_.begin(); it != cache_.end();) {
        if (it->second.resource.expired()) {
            it = cache_.erase(it);
        } else {
            ++it;
        }
    }
}

usize ResourceManager::loaded_count() const noexcept {
    usize count = 0;
    for (const auto& [key, entry] : cache_) {
        (void)key;
        if (!entry.resource.expired()) {
            ++count;
        }
    }
    return count;
}

} // namespace fe
