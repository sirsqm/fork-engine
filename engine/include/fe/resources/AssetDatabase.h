// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Error.h"
#include "fe/core/Types.h"
#include "fe/resources/AssetId.h"

#include <string>
#include <string_view>
#include <unordered_map>

namespace fe {

// The persisted metadata for one asset, stored in a sidecar file next to it
// ("hero.png.meta"). The GUID is the asset's stable identity; `source` and
// `scheme` describe the file it names and how it is imported.
struct AssetMeta {
    static constexpr i32 kFormatVersion = 1;

    AssetId id;
    // The asset file's own name (informational; the file's location is the
    // directory the sidecar is found in).
    std::string source;
    // The loader scheme the asset is imported under ("tex", "obj", or "" for the
    // default scheme). This is what a GUID reference resolves to: the scheme
    // plus the asset's current path. It is stored rather than derived because
    // several loaders can share a file extension (a ".png" is both a CPU Image
    // and a GPU Texture2D), so the scheme is a genuine import setting, not a
    // function of the path.
    std::string scheme;
};

// Reads a sidecar file. Fails with a clear error on a missing file, malformed
// JSON, a bad format marker, or an unparseable GUID.
[[nodiscard]] Result<AssetMeta> read_asset_meta(std::string_view sidecar_path);

// Writes a sidecar file, creating parent directories as needed.
[[nodiscard]] Error write_asset_meta(std::string_view sidecar_path, const AssetMeta& meta);

// Maps stable asset GUIDs to their current file paths.
//
// This is the piece that makes a GUID reference resolve after a file move. A
// scene stores "guid:<uuid>"; the database turns that id into the asset's
// *current* path, and the ResourceManager then loads it through the ordinary
// loader path. Because the id is not derived from the path, the reference
// survives a rename or a move.
//
// Source of truth vs. cache: the GUIDs live in the per-asset sidecars, which
// travel with the files. The in-memory index is a cache rebuilt by scanning the
// asset root for sidecars (rescan()), so deleting the index is a performance
// event, never a data-loss event. See
// docs/adr/0020-resource-identity-guid-sidecar.md.
//
// A database with no root set holds nothing and resolves nothing; it is valid
// (so a headless test can construct one) but inert.
class AssetDatabase {
public:
    struct Asset {
        AssetId id;
        // Path relative to the root, normalized with forward slashes.
        std::string path;
        // Loader scheme the asset is imported under ("tex", "obj", or "").
        std::string scheme;
    };

    AssetDatabase() = default;

    AssetDatabase(const AssetDatabase&) = delete;
    AssetDatabase& operator=(const AssetDatabase&) = delete;

    // Sets the asset root and rebuilds the index from its sidecars. The root is
    // normalized; an empty root clears the database. Rescanning is idempotent.
    void set_root(std::string root);
    [[nodiscard]] const std::string& root() const noexcept { return root_; }

    // Rebuilds the index by scanning the root for "*.meta" sidecars. Entries
    // whose asset file no longer exists are dropped. Does nothing when no root
    // is set.
    void rescan();

    // Registers the asset at `path` (relative to the root) under loader
    // `scheme`. If a sidecar already exists its GUID is reused, so re-importing
    // a moved or edited asset keeps its identity; otherwise a new GUID is
    // minted. The sidecar is written and the index updated. Returns the
    // resulting asset.
    [[nodiscard]] Result<Asset> import(std::string_view path, std::string_view scheme);

    [[nodiscard]] const Asset* find(const AssetId& id) const;
    [[nodiscard]] const Asset* find_by_path(std::string_view path) const;

    // The absolute filesystem path of an asset, or "" when the id is unknown.
    [[nodiscard]] std::string absolute_path(const AssetId& id) const;

    [[nodiscard]] usize asset_count() const noexcept { return by_id_.size(); }

    // Every indexed asset. The order is unspecified; a caller that needs a
    // stable order (the asset browser sorts by path) must sort it. Exposed as
    // the map rather than a copied vector so listing assets for a UI costs no
    // allocation beyond what the caller does itself.
    [[nodiscard]] const std::unordered_map<AssetId, Asset>& entries() const noexcept { return by_id_; }

private:
    [[nodiscard]] std::string sidecar_path(std::string_view relative_path) const;

    std::string root_;
    std::unordered_map<AssetId, Asset> by_id_;
    std::unordered_map<std::string, AssetId> by_path_;
};

} // namespace fe
