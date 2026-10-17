// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/scene/Tilemap.h"

#include <nlohmann/json.hpp>

namespace fe {

i32 Tilemap::at(u32 x, u32 y) const noexcept {
    if (!in_bounds(x, y)) {
        return kEmpty;
    }
    const usize index = static_cast<usize>(y) * width + x;
    if (index >= cells.size()) {
        return kEmpty;
    }
    return cells[index];
}

void Tilemap::set(u32 x, u32 y, i32 tile) noexcept {
    if (!in_bounds(x, y)) {
        return;
    }
    const usize index = static_cast<usize>(y) * width + x;
    if (index >= cells.size()) {
        return;
    }
    cells[index] = tile;
}

void Tilemap::resize(u32 new_width, u32 new_height, i32 fill) {
    width = new_width;
    height = new_height;
    cells.assign(cell_count(), fill);
}

void Tilemap::clear(i32 fill) noexcept {
    for (i32& cell : cells) {
        cell = fill;
    }
}

Vec4 Tilemap::tile_uv(i32 tile) const noexcept {
    // A zero-dimension tileset cannot address a cell; the whole texture is the
    // defined fallback, matching SpriteAnimator::uv().
    if (tile < 0 || columns == 0 || rows == 0) {
        return Vec4{0.0f, 0.0f, 1.0f, 1.0f};
    }
    const u32 index = static_cast<u32>(tile);
    if (index >= columns * rows) {
        return Vec4{0.0f, 0.0f, 1.0f, 1.0f};
    }
    const f32 cell_w = 1.0f / static_cast<f32>(columns);
    const f32 cell_h = 1.0f / static_cast<f32>(rows);
    const u32 col = index % columns;
    const u32 row = index / columns;
    const f32 u0 = static_cast<f32>(col) * cell_w;
    const f32 v0 = static_cast<f32>(row) * cell_h;
    return Vec4{u0, v0, u0 + cell_w, v0 + cell_h};
}

std::string Tilemap::to_json() const {
    nlohmann::json document;
    document["width"] = width;
    document["height"] = height;
    document["cells"] = cells;
    return document.dump();
}

bool Tilemap::from_json(std::string_view text) {
    // Parse into temporaries so a malformed document leaves the map unchanged
    // rather than half-applied. The whole body is guarded, not just the parse:
    // a field of the wrong JSON type (a string where a number belongs, an object
    // where an array belongs) throws from the accessors too, and this runs in a
    // reflection setter, where throwing would escape the scene deserializer.
    u32 new_width = 0;
    u32 new_height = 0;
    std::vector<i32> new_cells;
    try {
        const nlohmann::json document = nlohmann::json::parse(text);
        if (!document.is_object()) {
            return false;
        }
        new_width = document.value("width", 0u);
        new_height = document.value("height", 0u);
        if (document.contains("cells") && document.at("cells").is_array()) {
            new_cells = document.at("cells").get<std::vector<i32>>();
        }
    } catch (const nlohmann::json::exception&) {
        return false;
    }

    // The dimensions and the cell count are one invariant; a document that
    // disagrees with itself is rejected rather than loaded as a partial grid.
    if (new_cells.size() != static_cast<usize>(new_width) * new_height) {
        return false;
    }

    width = new_width;
    height = new_height;
    cells = std::move(new_cells);
    return true;
}

} // namespace fe
