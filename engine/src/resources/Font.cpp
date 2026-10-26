// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/resources/Font.h"

#include <nlohmann/json.hpp>

#include <utility>

namespace fe {

Font::Font(std::string path, std::shared_ptr<Resource> atlas, f32 line_height, std::vector<Glyph> glyphs)
    : Resource(std::move(path)), atlas_(std::move(atlas)), line_height_(line_height), glyphs_(std::move(glyphs)) {}

f32 Font::measure(std::string_view text) const noexcept {
    f32 width = 0.0f;
    for (const char raw : text) {
        width += advance(static_cast<u8>(raw));
    }
    return width;
}

std::shared_ptr<Font> Font::from_json(std::string_view path, std::string_view text, std::shared_ptr<Resource> atlas,
                                      std::string& error) {
    nlohmann::json document;
    try {
        document = nlohmann::json::parse(text);
    } catch (const nlohmann::json::exception& exception) {
        error = exception.what();
        return nullptr;
    }

    // `line_height` is required: a font without it cannot lay out a second line,
    // and a silent default would hide a broken atlas.
    if (!document.contains("line_height") || !document["line_height"].is_number()) {
        error = "font: missing numeric 'line_height'";
        return nullptr;
    }

    // `glyphs` is a dense array indexed by byte value, so the table's size is
    // the highest byte it can address plus one. A sparse object keyed by
    // character would need a map and would hide a hole; an array cannot have one.
    if (!document.contains("glyphs") || !document["glyphs"].is_array()) {
        error = "font: missing 'glyphs' array";
        return nullptr;
    }

    std::vector<Glyph> glyphs;
    glyphs.reserve(document["glyphs"].size());
    for (const auto& entry : document["glyphs"]) {
        // A null entry is a byte with no glyph, which is the honest encoding of
        // "not in the atlas": it keeps the array dense and indexed by byte
        // without inventing metrics for a character that was never rasterized.
        if (!entry.is_null() && !entry.is_object()) {
            error = "font: a glyph entry is neither an object nor null";
            return nullptr;
        }
        Glyph glyph;
        try {
            if (entry.is_object()) {
                glyph.uv = Vec4{entry.at("u0").get<f32>(), entry.at("v0").get<f32>(), entry.at("u1").get<f32>(),
                                entry.at("v1").get<f32>()};
                glyph.advance = entry.at("advance").get<f32>();
                glyph.bearing = Vec2{entry.at("bearing_x").get<f32>(), entry.at("bearing_y").get<f32>()};
                glyph.size = Vec2{entry.at("width").get<f32>(), entry.at("height").get<f32>()};
            }
        } catch (const nlohmann::json::exception& exception) {
            error = exception.what();
            return nullptr;
        }
        glyphs.push_back(glyph);
    }

    return std::make_shared<Font>(std::string(path), std::move(atlas), document["line_height"].get<f32>(),
                                  std::move(glyphs));
}

} // namespace fe
