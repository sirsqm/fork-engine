// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/resources/FontLoader.h"

#include "fe/resources/Font.h"
#include "fe/resources/Texture2D.h"
#include "fe/resources/Texture2DLoader.h"

#include <nlohmann/json.hpp>

#include <string>

namespace fe {

bool FontLoader::supports(std::string_view extension) const noexcept {
    return extension == ".fefont";
}

std::string FontLoader::reference_key(std::string_view file_path) {
    return "font:" + std::string(file_path);
}

Result<std::shared_ptr<Resource>> FontLoader::load(std::string_view path) {
    auto text = read_text(path);
    if (!text) {
        return text.error();
    }

    // The metrics name the atlas. Resolving it here, through the manager's
    // texture scheme, is what makes the atlas one shared Texture2D rather than a
    // private copy the font owns. A font whose atlas cannot load is an error
    // rather than a font that draws nothing: a missing atlas is a broken asset,
    // not an empty label.
    std::string atlas_reference;
    try {
        const nlohmann::json document = nlohmann::json::parse(text.value());
        if (!document.contains("atlas") || !document["atlas"].is_string()) {
            return Error{"font: missing string 'atlas'"};
        }
        atlas_reference = Texture2DLoader::reference_key(document["atlas"].get<std::string>());
    } catch (const nlohmann::json::exception& exception) {
        return Error{std::string("font: ") + exception.what()};
    }

    auto atlas = manager_->load(atlas_reference);
    if (!atlas) {
        return atlas.error();
    }
    if (dynamic_cast<Texture2D*>(atlas.value().get()) == nullptr) {
        return Error{"font: the atlas did not load as a Texture2D"};
    }

    std::string error;
    auto font = Font::from_json(path, text.value(), atlas.value(), error);
    if (font == nullptr) {
        return Error{"font: " + error};
    }
    return std::shared_ptr<Resource>{std::move(font)};
}

} // namespace fe
