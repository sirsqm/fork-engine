// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Error.h"
#include "fe/core/Types.h"
#include "fe/resources/AssetDatabase.h"
#include "fe/resources/ImportCache.h"
#include "fe/resources/ImportSettings.h"
#include "fe/resources/Resource.h"
#include "fe/resources/ResourceSource.h"

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
//
// The manager is itself the default ResourceSource: it reads asset bytes from
// the filesystem. Mounting another source (`mount`) makes every load read from
// it instead, which is how a shipped build reads from a pack
// (docs/adr/0030-asset-pack-format.md) without any loader knowing.
class ResourceManager final : public ResourceSource {
public:
    ResourceManager() = default;
    ~ResourceManager() = default;

    ResourceManager(const ResourceManager&) = delete;
    ResourceManager& operator=(const ResourceManager&) = delete;

    // Registers a loader. The manager takes ownership. Loaders are consulted in
    // registration order; register the most specific first.
    void register_loader(std::unique_ptr<ResourceLoader> loader);

    // Installs a source to read asset bytes from, replacing the filesystem. The
    // source is consulted by read_binary()/read_text()/exists() below, so the
    // loaders that read through them serve a mounted pack transparently. A null
    // source restores the filesystem default. GUID references resolve through
    // the source when it knows the id, and through the asset database
    // otherwise, so a pack is self-describing.
    void mount(std::shared_ptr<ResourceSource> source);
    [[nodiscard]] const std::shared_ptr<ResourceSource>& source() const noexcept { return source_; }

    // ResourceSource: the filesystem by default, the mounted source when one is
    // installed. These are what a loader calls to read its input.
    [[nodiscard]] Result<std::vector<u8>> read_binary(std::string_view path) const override;
    [[nodiscard]] Result<std::string> read_text(std::string_view path) const override;
    [[nodiscard]] bool exists(std::string_view path) const override;
    [[nodiscard]] const AssetLocation* resolve_asset(const AssetId& id) const override;

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

    // --- Hot reload --------------------------------------------------------

    // The references of cached resources whose source file changed on disk
    // since it was loaded, and updates each one's recorded time so a change is
    // reported once. Empty when nothing changed. This is the editor's "asset
    // changed, refresh what uses it" step, headless and poll-driven: there is no
    // OS watch here, so a caller (the editor's frame loop, a test) decides how
    // often to ask.
    //
    // It detects but does not itself reload. A cache entry holds a weak
    // reference (see the ownership note above), so an instance swapped in with
    // no caller to adopt it would be released on the spot; detection and
    // adoption are therefore two steps. The caller takes each returned reference
    // to reload() and re-wires whatever holds it.
    //
    // Only live resources are considered: an entry whose resource has been
    // released is dropped and not reported. A resource whose file cannot be
    // stat-ed is left alone rather than treated as deleted, so a transient read
    // error does not tear down a loaded asset. A resource mounted from a pack
    // has no loose file and is never reported.
    [[nodiscard]] std::vector<std::string> poll_changed();

    // Loads `reference` afresh, replaces its cache entry, and returns the new
    // instance for the caller to hold (and to re-resolve into a component's
    // property - that re-wiring is the editor's job; this refreshes the cache).
    // The previous shared_ptr stays valid to callers still holding it, so
    // nothing observes a dangling pointer.
    //
    // Returns an Error when the reference is not cached (nothing to refresh) or
    // the reload fails; on failure the existing entry is left untouched, so a
    // broken file does not tear down a working resource.
    [[nodiscard]] Result<std::shared_ptr<Resource>> reload(std::string_view reference);

    [[nodiscard]] usize cached_count() const noexcept { return cache_.size(); }
    [[nodiscard]] usize loaded_count() const noexcept;

private:
    struct CacheEntry {
        std::weak_ptr<Resource> resource;
        // The absolute path whose bytes produced this resource, or "" when the
        // resource has no loose file (loaded from a mounted pack, or from a
        // source that is not the filesystem). poll_reloads() compares this
        // file's write time against the recorded stamp. See ADR-0046.
        std::string reload_path;
        // The file's last-write time at load, in the opaque unit fs::last_write_time_ns
        // returns. Compared for inequality only.
        u64 reload_stamp = 0;
    };

    // Records a freshly cached entry: remembers the file to watch and its
    // current write time, so poll_reloads() can detect a later change. Called
    // from both the path and GUID load paths, after the resource is loaded.
    void remember_for_reload(const std::string& key, const std::shared_ptr<Resource>& resource,
                             std::string_view file_path);

    // The body of load() without the cache lookup or the cache store: resolves
    // `key` and loads it through the loaders, reporting the absolute file it
    // read in `*watched_path` (or clearing it when the load read no loose file,
    // e.g. from a mounted pack). load() and reload() share it so a reload
    // re-runs exactly the resolution a first load did.
    [[nodiscard]] Result<std::shared_ptr<Resource>> load_uncached(const std::string& key, std::string* watched_path);

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
    // The mounted byte source, or null to read from the filesystem. Set by
    // mount(); consulted by read_binary()/read_text()/exists()/resolve_asset().
    std::shared_ptr<ResourceSource> source_;
    // The settings for the load currently running, or nullptr. Set and cleared
    // around each loader call in load_file(); see current_import_settings().
    const ImportSettings* current_settings_ = nullptr;
};

} // namespace fe
