// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/resources/ResourceManager.h"

#include "fe/core/FileSystem.h"
#include "fe/core/Log.h"

#include <string>
#include <string_view>
#include <vector>

namespace fe {

void ResourceManager::register_loader(std::unique_ptr<ResourceLoader> loader) {
    if (loader) {
        loaders_.push_back(std::move(loader));
    }
}

void ResourceManager::mount(std::shared_ptr<ResourceSource> source) {
    source_ = std::move(source);
}

Result<std::vector<u8>> ResourceManager::read_binary(std::string_view path) const {
    if (source_) {
        return source_->read_binary(path);
    }
    return fs::read_binary_file(path);
}

Result<std::string> ResourceManager::read_text(std::string_view path) const {
    if (source_) {
        return source_->read_text(path);
    }
    return fs::read_text_file(path);
}

bool ResourceManager::exists(std::string_view path) const {
    if (source_) {
        return source_->exists(path);
    }
    return fs::exists(path);
}

const ResourceSource::AssetLocation* ResourceManager::resolve_asset(const AssetId& id) const {
    if (source_) {
        return source_->resolve_asset(id);
    }
    return nullptr;
}

Result<std::vector<u8>> ResourceLoader::read_bytes(std::string_view path) const {
    if (source_ != nullptr) {
        return source_->read_binary(path);
    }
    return fs::read_binary_file(path);
}

Result<std::string> ResourceLoader::read_text(std::string_view path) const {
    if (source_ != nullptr) {
        return source_->read_text(path);
    }
    return fs::read_text_file(path);
}

Result<std::shared_ptr<Resource>> ResourceManager::load(std::string_view reference) {
    const std::string key = fs::normalize(reference);

    const auto it = cache_.find(key);
    if (it != cache_.end()) {
        if (auto existing = it->second.resource.lock()) {
            return existing;
        }
        // The entry is stale (the resource was released). Fall through and
        // reload, replacing the dead weak reference.
    }

    std::string watched_path;
    auto result = load_uncached(key, &watched_path);
    if (!result) {
        return result.error();
    }
    remember_for_reload(key, result.value(), watched_path);
    return result.value();
}

void ResourceManager::remember_for_reload(const std::string& key, const std::shared_ptr<Resource>& resource,
                                          std::string_view file_path) {
    CacheEntry entry;
    entry.resource = resource;
    if (!source_ && !file_path.empty()) {
        entry.reload_path = fs::normalize(file_path);
        entry.reload_stamp = fs::last_write_time_ns(entry.reload_path);
    }
    cache_[key] = std::move(entry);
}

Result<std::shared_ptr<Resource>> ResourceManager::load_uncached(const std::string& key, std::string* watched_path) {
    if (watched_path != nullptr) {
        watched_path->clear();
    }

    // A GUID reference names an identity, not a file. Resolve it to the asset's
    // current path and import scheme; this is what lets a reference survive a
    // file move. A mounted source (a pack) carries the mapping itself; otherwise
    // the asset database answers.
    AssetId guid;
    if (parse_reference(key, guid)) {
        std::string scheme;
        std::string path;
        // A mounted source has no loose import settings; the filesystem path
        // takes them from the asset database.
        const ImportSettings* settings = nullptr;
        if (source_) {
            const AssetLocation* location = source_->resolve_asset(guid);
            if (location == nullptr) {
                return Error("asset " + guid.to_string() + " is not in the mounted pack");
            }
            scheme = location->scheme;
            path = location->path;
        } else {
            const AssetDatabase::Asset* asset = assets_.find(guid);
            if (asset == nullptr) {
                return Error("asset " + guid.to_string() + " is not in the asset database (root '" + assets_.root() +
                             "')");
            }
            scheme = asset->scheme;
            path = asset->path;
            settings = &asset->settings;
        }
        auto result = load_file(scheme, path, settings);
        if (!result) {
            return result.error();
        }
        result.value()->set_asset_id(guid);
        if (watched_path != nullptr && !source_) {
            *watched_path = assets_.absolute_path(guid);
        }
        FE_LOG_DEBUG("resolved guid asset '%s' to '%s'", guid.to_string().c_str(), path.c_str());
        return result.value();
    }

    // A malformed "guid:" reference is a broken reference, not a path; report it
    // rather than letting it fall through to a confusing "no loader" error.
    if (key.rfind(std::string(kGuidScheme) + ":", 0) == 0) {
        return Error("malformed guid reference '" + key + "'");
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

    // A path load still honours the asset's import settings when the source
    // knows the file, so a direct "tex:assets/hero.png" reference and a "guid:"
    // reference to the same asset produce the same resource. A mounted pack has
    // no loose settings, so a path load from it takes the loader defaults.
    const ImportSettings* settings = nullptr;
    if (!source_) {
        if (const AssetDatabase::Asset* asset = assets_.find_by_path(file_path)) {
            settings = &asset->settings;
        }
    }

    auto result = load_file(scheme, file_path, settings);
    if (!result) {
        return result.error();
    }
    if (watched_path != nullptr && !source_) {
        // Resolve the file the loader actually read to an absolute path so a
        // change check does not depend on the process working directory, which
        // a project load can change.
        const std::string absolute = assets_.absolute_path_by_path(file_path);
        *watched_path = absolute.empty() ? fs::normalize(std::string(file_path)) : absolute;
    }
    return result.value();
}

Result<std::shared_ptr<Resource>> ResourceManager::load_file(std::string_view scheme, std::string_view file_path,
                                                             const ImportSettings* settings) {
    const std::string extension = fs::extension(file_path);
    for (const auto& loader : loaders_) {
        if (loader->scheme() != scheme || !loader->supports(extension)) {
            continue;
        }
        // Scope the settings for the loader call. load() is not re-entrant
        // through this path in a way that would clobber it (the texture loader
        // fetches the CPU image under the default scheme, which has no asset
        // settings), and the engine is single-threaded per ADR-0004. The source
        // is set for the same reason: a loader reads its bytes through it, so a
        // mounted pack is served.
        const ImportSettings* previous = current_settings_;
        current_settings_ = settings;
        loader->set_source(source_.get());
        auto result = loader->load(file_path);
        loader->set_source(nullptr);
        current_settings_ = previous;
        if (!result) {
            return result.error();
        }
        FE_LOG_DEBUG("loaded %s resource '%s'", result.value()->type_name(), std::string(file_path).c_str());
        return result.value();
    }

    if (!scheme.empty()) {
        return Error("no loader registered for scheme '" + std::string(scheme) + "' with extension '" + extension +
                     "' (resource '" + std::string(file_path) + "')");
    }
    return Error("no loader registered for extension '" + extension + "' (resource '" + std::string(file_path) + "')");
}

std::string ResourceManager::resolve_reference(std::string_view path) const {
    const std::string normalized = fs::normalize(path);

    // Already a GUID reference (or a malformed one): leave it for load() to
    // interpret, so this helper never silently rewrites a reference.
    AssetId existing;
    if (parse_reference(normalized, existing) || normalized.rfind(std::string(kGuidScheme) + ":", 0) == 0) {
        return normalized;
    }

    // An explicit scheme ("tex:assets/hero.png") names an import; strip it and
    // look the path up, then re-apply the scheme to the resolved GUID.
    std::string_view scheme;
    std::string_view file_path = normalized;
    const std::size_t colon = normalized.find(':');
    if (colon != std::string::npos && colon > 1) {
        scheme = std::string_view(normalized).substr(0, colon);
        file_path = std::string_view(normalized).substr(colon + 1);
    }

    if (const AssetDatabase::Asset* asset = assets_.find_by_path(file_path)) {
        (void)scheme; // The asset's own scheme wins; the reference's is informational.
        return to_reference(asset->id);
    }

    // No database or unknown path: fall back to the input unchanged so callers
    // that only have a path (and no project) keep working.
    return normalized;
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

std::vector<std::string> ResourceManager::poll_changed() {
    // A mounted pack has no loose files to watch; the pack is the source of
    // truth and is replaced wholesale, not edited in place.
    if (source_) {
        return {};
    }

    std::vector<std::string> changed;
    for (auto& [key, entry] : cache_) {
        if (entry.resource.expired() || entry.reload_path.empty()) {
            continue;
        }
        const u64 stamp = fs::last_write_time_ns(entry.reload_path);
        // Zero means the file could not be read; treat that as "leave it alone"
        // rather than "it was deleted", so a transient error does not tear down
        // a loaded asset.
        if (stamp != 0 && stamp != entry.reload_stamp) {
            // Record the new time now, so an unchanged poll after this does not
            // report the same change twice; the caller still has to adopt the
            // reload it was told about.
            entry.reload_stamp = stamp;
            changed.push_back(key);
        }
    }
    return changed;
}

Result<std::shared_ptr<Resource>> ResourceManager::reload(std::string_view reference) {
    const std::string key = fs::normalize(reference);

    const auto it = cache_.find(key);
    if (it == cache_.end() || it->second.resource.expired()) {
        // Not cached, or cached but released: there is no live instance to
        // refresh, and reloading would resurrect a resource nothing asked for.
        return Error("resource '" + key + "' is not loaded");
    }

    std::string watched_path;
    auto result = load_uncached(key, &watched_path);
    if (!result) {
        // A broken file is not a reason to drop a working resource; keep the
        // existing instance and report the failure.
        FE_LOG_DEBUG("reload of '%s' failed: %s", key.c_str(), result.error().message().c_str());
        return result.error();
    }

    remember_for_reload(key, result.value(), watched_path);
    FE_LOG_DEBUG("reloaded resource '%s'", key.c_str());
    return result.value();
}

} // namespace fe
