// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Error.h"
#include "fe/core/Types.h"
#include "fe/resources/AssetDatabase.h"
#include "fe/resources/ImportCache.h"
#include "fe/resources/ImportSettings.h"
#include "fe/resources/Resource.h"

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace fe {

// Loads, caches, and releases assets.
//
// Responsibilities:
//   * Map a path (including its scheme) to a single shared instance.
//   * Dispatch to the right ResourceLoader by scheme and file extension.
//   * Keep a resource alive only while something references it, so that
//     reloading a scene does not leak assets.
//
// Ownership model: the manager holds a weak reference in its cache. A resource
// stays loaded as long as a caller holds a shared_ptr. This gives reliable
// "reference or reload" behaviour across scene reloads and asset reimports
// without a manual release call, and avoids the classic bug where the manager
// pins every asset for the lifetime of the process.
class ResourceManager {
public:
    ResourceManager() = default;
    ~ResourceManager() = default;

    ResourceManager(const ResourceManager&) = delete;
    ResourceManager& operator=(const ResourceManager&) = delete;

    // Registers a loader. The manager takes ownership. Loaders are consulted in
    // registration order; register the most specific first.
    void register_loader(std::unique_ptr<ResourceLoader> loader);

    // Loads a resource, or returns the already-cached instance.
    //
    // `reference` is one of:
    //   * "guid:<uuid>" — a stable asset identity, resolved through the asset
    //     database to the asset's current path. This is what survives a file
    //     move. Fails clearly when there is no database or the id is unknown.
    //   * "<scheme>:<path>" or "<path>" — a direct path load, resolved by the
    //     loader registered for the scheme and extension.
    //
    // A resource resolved through the database is tagged with its AssetId, so a
    // reference taken from it persists as a GUID.
    [[nodiscard]] Result<std::shared_ptr<Resource>> load(std::string_view reference);

    // Resolves a bare path through the asset database to a stable GUID
    // reference ("guid:<uuid>"), or returns the input unchanged when there is no
    // database or the path is not a known asset. This is what a component's
    // reference_key falls back to when it only has a resource path.
    [[nodiscard]] std::string resolve_reference(std::string_view path) const;

    // The asset database that GUID references resolve through. It is empty by
    // default (no root), which leaves path references working unchanged.
    [[nodiscard]] AssetDatabase& assets() noexcept { return assets_; }
    [[nodiscard]] const AssetDatabase& assets() const noexcept { return assets_; }

    // The content-addressed import cache loaders may reuse. It is inert by
    // default (no root), so a load behaves exactly as it did before the cache
    // existed; a caller opts in by setting a root. See
    // docs/adr/0029-import-cache.md.
    [[nodiscard]] ImportCache& import_cache() noexcept { return import_cache_; }
    [[nodiscard]] const ImportCache& import_cache() const noexcept { return import_cache_; }

    // The import settings in effect for the load currently running on this
    // thread, or nullptr when the load was not initiated by load() (a direct
    // loader call) or the asset has none.
    //
    // A loader reads this to honour its asset's per-type settings without a
    // settings parameter on `ResourceLoader::load`: the manager scopes the
    // settings for the duration of the load and a loader that cares asks for
    // them. This keeps the loader contract unchanged, so a loader that has no
    // settings keeps compiling and behaving exactly as before.
    [[nodiscard]] const ImportSettings* current_import_settings() const noexcept { return current_settings_; }

    // Typed convenience: returns nullptr (with an Error) when the resource is
    // missing or is not a `T`.
    template <typename T>
    [[nodiscard]] Result<std::shared_ptr<T>> load_as(std::string_view reference) {
        auto result = load(reference);
        if (!result) {
            return result.error();
        }
        auto typed = std::dynamic_pointer_cast<T>(result.value());
        if (!typed) {
            return Error("resource '" + std::string(reference) + "' is not a " + std::string(T::kTypeName));
        }
        return typed;
    }

    // Drops cache entries whose resource is no longer referenced. Safe to call
    // at any time; typically called between scene loads.
    void collect_garbage();

    [[nodiscard]] usize cached_count() const noexcept { return cache_.size(); }
    [[nodiscard]] usize loaded_count() const noexcept;

private:
    struct CacheEntry {
        std::weak_ptr<Resource> resource;
    };

    // Loads a concrete file path (already scheme-stripped) through the loaders.
    // Shared by the path and GUID load paths. `settings`, when non-null, is the
    // import settings of the asset being loaded; it is scoped for the duration
    // of the loader call so a loader can read it through
    // current_import_settings().
    [[nodiscard]] Result<std::shared_ptr<Resource>> load_file(std::string_view scheme, std::string_view file_path,
                                                              const ImportSettings* settings = nullptr);

    std::vector<std::unique_ptr<ResourceLoader>> loaders_;
    std::unordered_map<std::string, CacheEntry> cache_;
    AssetDatabase assets_;
    ImportCache import_cache_;
    // The settings for the load currently running, or nullptr. Set and cleared
    // around each loader call in load_file(); see current_import_settings().
    const ImportSettings* current_settings_ = nullptr;
};

} // namespace fe
