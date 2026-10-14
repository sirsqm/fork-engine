// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/resources/AssetDatabase.h"

#include "fe/core/FileSystem.h"
#include "fe/core/Log.h"

#include <nlohmann/json.hpp>

#include <string>
#include <utility>
#include <vector>

namespace fe {
namespace {

using json = nlohmann::json;

[[nodiscard]] bool has_meta_suffix(std::string_view path) noexcept {
    return path.size() > kMetaSuffix.size() &&
           path.compare(path.size() - kMetaSuffix.size(), kMetaSuffix.size(), kMetaSuffix) == 0;
}

// Strips the ".meta" suffix from a sidecar path to get the asset's path.
[[nodiscard]] std::string asset_path_of_sidecar(std::string_view sidecar) {
    return std::string(sidecar.substr(0, sidecar.size() - kMetaSuffix.size()));
}

} // namespace

Result<AssetMeta> read_asset_meta(std::string_view sidecar_path) {
    auto text = fs::read_text_file(sidecar_path);
    if (!text) {
        return text.error();
    }

    json document;
    try {
        document = json::parse(text.value());
    } catch (const json::parse_error& e) {
        return Error("asset metadata parse error in '" + std::string(sidecar_path) + "': " + e.what());
    }

    const std::string format = document.value("format", std::string{});
    if (format != "fork_asset") {
        return Error("not a fork asset metadata file (format='" + format + "') in '" + std::string(sidecar_path) + "'");
    }

    const i32 version = document.value("format_version", 0);
    if (version > AssetMeta::kFormatVersion) {
        return Error("asset metadata version " + std::to_string(version) + " is newer than supported version " +
                     std::to_string(AssetMeta::kFormatVersion) + " in '" + std::string(sidecar_path) + "'");
    }

    AssetMeta meta;
    const std::string guid_text = document.value("guid", std::string{});
    if (!AssetId::try_parse(guid_text, meta.id)) {
        return Error("asset metadata has an invalid guid '" + guid_text + "' in '" + std::string(sidecar_path) + "'");
    }
    meta.source = document.value("source", std::string{});
    meta.scheme = document.value("scheme", std::string{});

    // Import settings are optional: a sidecar written before they existed has
    // none, and a default value is the same as absent. A malformed one is an
    // error rather than a silent fall back to defaults, so a bad edit is not
    // mistaken for an intentional default.
    if (document.contains("settings")) {
        auto settings = import_settings_from_json(document.at("settings").dump());
        if (!settings) {
            return Error("asset metadata in '" + std::string(sidecar_path) + "': " + settings.error().message());
        }
        meta.settings = settings.value();
    }
    return meta;
}

Error write_asset_meta(std::string_view sidecar_path, const AssetMeta& meta) {
    json document;
    document["format"] = "fork_asset";
    document["format_version"] = AssetMeta::kFormatVersion;
    document["guid"] = meta.id.to_string();
    document["source"] = meta.source;
    document["scheme"] = meta.scheme;
    // A generic asset has no type-specific settings, so the block is omitted
    // entirely and its sidecar stays byte-identical to one written before import
    // settings existed. An asset that has a kind always records it, so the kind
    // (which the editor needs to draw the right settings) survives a reload even
    // when every value is a default.
    if (meta.settings.kind != ImportSettings::Kind::Generic) {
        document["settings"] = json::parse(import_settings_to_json(meta.settings));
    }
    return fs::write_text_file(sidecar_path, document.dump(2));
}

void AssetDatabase::set_root(std::string root) {
    root_ = fs::normalize(root);
    by_id_.clear();
    by_path_.clear();
    rescan();
}

std::string AssetDatabase::sidecar_path(std::string_view relative_path) const {
    // The suffix is appended to the name ("hero.png" -> "hero.png.meta"), not
    // joined as a path segment.
    return fs::join(root_, std::string(relative_path) + std::string(kMetaSuffix));
}

void AssetDatabase::rescan() {
    by_id_.clear();
    by_path_.clear();
    if (root_.empty()) {
        return;
    }

    for (const std::string& relative : fs::list_files_recursive(root_)) {
        if (!has_meta_suffix(relative)) {
            continue;
        }
        const std::string asset_path = asset_path_of_sidecar(relative);
        if (!fs::exists(fs::join(root_, asset_path))) {
            // A sidecar whose asset is gone is stale. It is dropped from the
            // index (not deleted) so a user can restore the file.
            FE_LOG_DEBUG("asset database: sidecar '%s' has no asset; skipping", relative.c_str());
            continue;
        }

        auto meta = read_asset_meta(fs::join(root_, relative));
        if (!meta) {
            FE_LOG_WARN("asset database: %s", meta.error().message().c_str());
            continue;
        }

        Asset asset;
        asset.id = meta.value().id;
        asset.path = asset_path;
        asset.scheme = meta.value().scheme;
        asset.settings = meta.value().settings;
        by_path_[asset.path] = asset.id;
        by_id_[asset.id] = std::move(asset);
    }
    FE_LOG_DEBUG("asset database: indexed %zu asset(s) under '%s'", by_id_.size(), root_.c_str());
}

Result<AssetDatabase::Asset> AssetDatabase::import(std::string_view path, std::string_view scheme) {
    // Default settings for the scheme: a texture gets the texture importer's
    // defaults, anything else the generic (empty) settings.
    ImportSettings defaults;
    if (scheme == "tex") {
        defaults = ImportSettings::texture_defaults();
    }
    return import(path, scheme, defaults);
}

Result<AssetDatabase::Asset> AssetDatabase::import(std::string_view path, std::string_view scheme,
                                                   ImportSettings settings) {
    if (root_.empty()) {
        return Error("asset database has no root; call set_root() before import()");
    }

    std::string relative = fs::normalize(path);
    // Accept an absolute path that lives under the root by making it relative.
    if (!root_.empty() && relative.rfind(root_, 0) == 0) {
        relative = fs::normalize(relative.substr(root_.size()));
        while (!relative.empty() && relative.front() == '/') {
            relative.erase(relative.begin());
        }
    }

    const std::string sidecar = sidecar_path(relative);
    AssetId id;
    if (fs::exists(sidecar)) {
        auto existing = read_asset_meta(sidecar);
        if (!existing) {
            return existing.error();
        }
        // Reuse the identity: a re-import of a moved or edited asset must not
        // change what existing references point at.
        id = existing.value().id;
    } else {
        id = AssetId::generate();
    }

    AssetMeta meta;
    meta.id = id;
    meta.source = fs::filename(relative);
    meta.scheme = std::string(scheme);
    meta.settings = settings;
    if (Error error = write_asset_meta(sidecar, meta); error) {
        return error;
    }

    Asset asset;
    asset.id = id;
    asset.path = relative;
    asset.scheme = meta.scheme;
    asset.settings = meta.settings;
    by_path_[asset.path] = asset.id;
    by_id_[asset.id] = asset;
    return asset;
}

const AssetDatabase::Asset* AssetDatabase::find(const AssetId& id) const {
    const auto it = by_id_.find(id);
    return it == by_id_.end() ? nullptr : &it->second;
}

const AssetDatabase::Asset* AssetDatabase::find_by_path(std::string_view path) const {
    const auto it = by_path_.find(fs::normalize(path));
    return it == by_path_.end() ? nullptr : &by_id_.at(it->second);
}

std::string AssetDatabase::absolute_path(const AssetId& id) const {
    const Asset* asset = find(id);
    return asset == nullptr ? std::string{} : fs::join(root_, asset->path);
}

std::string AssetDatabase::absolute_path_by_path(std::string_view path) const {
    const Asset* asset = find_by_path(path);
    return asset == nullptr ? std::string{} : fs::join(root_, asset->path);
}

} // namespace fe
