// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/resources/ImportSettings.h"

#include <nlohmann/json.hpp>

#include <string>

namespace fe {
namespace {

using json = nlohmann::json;

[[nodiscard]] std::string_view filter_name(rhi::TextureFilter filter) noexcept {
    return filter == rhi::TextureFilter::Nearest ? "nearest" : "linear";
}

[[nodiscard]] std::string_view wrap_name(rhi::TextureWrap wrap) noexcept {
    return wrap == rhi::TextureWrap::Repeat ? "repeat" : "clamp";
}

[[nodiscard]] Result<rhi::TextureFilter> filter_from_name(std::string_view name) {
    if (name == "nearest") {
        return rhi::TextureFilter::Nearest;
    }
    if (name == "linear") {
        return rhi::TextureFilter::Linear;
    }
    return Error("unknown texture filter '" + std::string(name) + "'");
}

[[nodiscard]] Result<rhi::TextureWrap> wrap_from_name(std::string_view name) {
    if (name == "repeat") {
        return rhi::TextureWrap::Repeat;
    }
    if (name == "clamp") {
        return rhi::TextureWrap::ClampToEdge;
    }
    return Error("unknown texture wrap '" + std::string(name) + "'");
}

} // namespace

bool ImportSettings::is_default() const noexcept {
    if (kind == Kind::Generic) {
        return true;
    }
    // Texture defaults are the pre-import-settings behaviour.
    return filter == rhi::TextureFilter::Linear && wrap == rhi::TextureWrap::ClampToEdge && !mipmaps;
}

const char* to_string(ImportSettings::Kind kind) noexcept {
    switch (kind) {
    case ImportSettings::Kind::Texture:
        return "texture";
    case ImportSettings::Kind::Generic:
    default:
        return "generic";
    }
}

Result<ImportSettings::Kind> import_kind_from_string(std::string_view name) {
    if (name == "generic") {
        return ImportSettings::Kind::Generic;
    }
    if (name == "texture") {
        return ImportSettings::Kind::Texture;
    }
    return Error("unknown import settings kind '" + std::string(name) + "'");
}

std::string import_settings_to_json(const ImportSettings& settings) {
    json document;
    document["kind"] = to_string(settings.kind);
    if (settings.kind == ImportSettings::Kind::Texture) {
        document["filter"] = filter_name(settings.filter);
        document["wrap"] = wrap_name(settings.wrap);
        document["mipmaps"] = settings.mipmaps;
    }
    return document.dump(2);
}

Result<ImportSettings> import_settings_from_json(std::string_view json_text) {
    json document;
    try {
        document = json::parse(json_text);
    } catch (const json::parse_error& e) {
        return Error("import settings parse error: " + std::string(e.what()));
    }
    if (!document.is_object()) {
        return Error("import settings must be a JSON object");
    }

    auto kind = import_kind_from_string(document.value("kind", std::string{"generic"}));
    if (!kind) {
        return kind.error();
    }

    ImportSettings settings;
    settings.kind = kind.value();
    if (settings.kind == ImportSettings::Kind::Texture) {
        auto filter = filter_from_name(document.value("filter", std::string{"linear"}));
        if (!filter) {
            return filter.error();
        }
        auto wrap = wrap_from_name(document.value("wrap", std::string{"clamp"}));
        if (!wrap) {
            return wrap.error();
        }
        settings.filter = filter.value();
        settings.wrap = wrap.value();
        settings.mipmaps = document.value("mipmaps", false);
    }
    return settings;
}

} // namespace fe
