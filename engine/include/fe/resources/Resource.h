// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Error.h"
#include "fe/core/Types.h"
#include "fe/resources/AssetId.h"

#include <memory>
#include <string>
#include <string_view>

namespace fe {

// Base class for every loaded asset.
//
// A Resource owns its decoded data and knows its source path. Lifetime is
// managed by the ResourceManager through shared_ptr; callers hold a
// std::shared_ptr<Resource> (or a typed alias) for as long as they need the
// data. When the last reference goes away the manager drops it from its cache,
// which is how assets are released.
//
// A resource may also carry a stable AssetId. It is set when the resource is
// resolved through the AssetDatabase (a "guid:" reference), and it is what lets
// a component persist a reference that survives a file move: the reference is
// the id, not the path. A resource loaded from a bare path has no id, and
// references to it fall back to a path-derived key.
class Resource {
public:
    virtual ~Resource() = default;

    Resource(const Resource&) = delete;
    Resource& operator=(const Resource&) = delete;

    [[nodiscard]] const std::string& path() const noexcept { return path_; }

    // The stable identity of the asset, or a nil id when the resource was loaded
    // by path and is not known to the asset database.
    [[nodiscard]] const AssetId& asset_id() const noexcept { return asset_id_; }
    [[nodiscard]] bool has_asset_id() const noexcept { return !asset_id_.is_nil(); }

    // Records the identity. Called by the ResourceManager when a resource is
    // resolved through the asset database; not part of the load contract.
    void set_asset_id(AssetId id) noexcept { asset_id_ = id; }

    // The name of the concrete type ("Texture2D", "Image", ...). Used by the
    // serializer and, later, by the editor's asset browser.
    [[nodiscard]] virtual const char* type_name() const noexcept = 0;

    // An estimate of GPU/CPU bytes held, for diagnostics and budget tracking.
    [[nodiscard]] virtual usize memory_usage() const noexcept { return 0; }

protected:
    explicit Resource(std::string path) : path_(std::move(path)) {}

private:
    std::string path_;
    AssetId asset_id_{};
};

// Loads one or more resource types from files.
//
// Loaders are registered with a ResourceManager and matched by scheme and file
// extension. A loader is stateless with respect to the cache; it only knows how
// to turn bytes on disk into a Resource.
//
// Schemes disambiguate loaders that handle the same file type but produce
// different resources. For example "assets/hero.png" loads a CPU Image, while
// "tex:assets/hero.png" loads a GPU Texture2D from the same file. The default
// scheme is the empty string.
class ResourceLoader {
public:
    virtual ~ResourceLoader() = default;

    // The scheme this loader handles, without the trailing colon. "" means the
    // default (unprefixed) scheme.
    [[nodiscard]] virtual std::string_view scheme() const noexcept { return {}; }

    // Returns true when this loader handles the given lower-case extension
    // (including the dot, e.g. ".png").
    [[nodiscard]] virtual bool supports(std::string_view extension) const noexcept = 0;

    // Loads `path` (with the scheme already stripped). The manager is provided
    // so a loader can request other resources; for example, the GPU texture
    // loader first loads the decoded CPU image.
    [[nodiscard]] virtual Result<std::shared_ptr<Resource>> load(std::string_view path) = 0;
};

} // namespace fe
