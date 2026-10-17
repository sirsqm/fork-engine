// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Math.h"
#include "fe/core/Types.h"
#include "fe/resources/Resource.h"
#include "fe/scene/Component.h"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace fe {

// A grid of tiles drawn from one tileset texture.
//
// A sprite is one quad; a level is thousands of them. Building a level out of
// individual Sprite2D nodes works but is the wrong shape: the level is a *grid*,
// not a tree of nodes, and an artist edits it as a grid. This component is that
// grid. It holds the cell values and the tileset they index; the renderer turns
// the non-empty cells into the same quads a Sprite2D produces, so a tilemap
// costs no new draw path.
//
// Layout. The tileset texture is a `columns` x `rows` grid of equal tiles, read
// left-to-right and top-to-bottom, the same convention SpriteAnimator uses for
// a sprite sheet. A cell's value is a tile index into that grid; kEmpty draws
// nothing, which is what makes a sparse map cheap to author.
//
// Coordinates. Cell (0, 0) is the *top-left* of the map, +X goes right and +Y
// goes down: the convention every tile editor uses. In the node's local space
// that means the map occupies x in [0, width * tile_size.x] and y in
// [-height * tile_size.y, 0], so the node's origin is the map's top-left corner.
// The renderer composes the node's world matrix with each cell's local offset,
// so moving, rotating, or scaling the node moves the whole map.
//
// Serialization. The grid is a variable-length list of ints, which the
// reflection model (ADR-0005, ADR-0006) deliberately does not express as a
// property: its Value set is closed and scalar or fixed-group. Rather than widen
// that model for one component, the grid is exposed as a single String property
// holding the component's own compact JSON, which the scene serializer
// round-trips like any other string and the component parses in its setter. This
// is the same trade AnimationStateMachine made for its graph (ADR-0059); a
// dedicated tile editor is a follow-up. See ADR-0062.
class Tilemap final : public Component {
public:
    static constexpr const char* kTypeName = "Tilemap";

    // A cell with no tile. Signed, so "empty" is distinct from tile index 0.
    static constexpr i32 kEmpty = -1;

    [[nodiscard]] const char* type_name() const noexcept override { return kTypeName; }

    // The tileset texture. Holding the shared_ptr is what keeps it loaded (see
    // ResourceManager). Null is allowed and draws nothing, so a map can be
    // authored before its tileset exists.
    std::shared_ptr<Resource> texture;

    // The tileset grid, in tiles. A zero in either axis makes every tile index
    // out of range, which tile_uv resolves to the full texture rather than a
    // degenerate one.
    u32 columns = 1;
    u32 rows = 1;

    // The size of one tile in world units. Every tile is the same size; a
    // tileset with mixed tile sizes is not this cut.
    Vec2 tile_size{16.0f, 16.0f};

    // Tint multiplied with every tile.
    Vec4 color{1.0f, 1.0f, 1.0f, 1.0f};

    // Render order within the 2D pass, on the same scale as Sprite2D::layer, so
    // a map and the sprites over it are ordered together.
    i32 layer = 0;

    // The map grid: `height` rows of `width` cells, row-major from the top-left.
    // Kept consistent with `cells.size() == width * height` by resize(); a
    // hand-built component that breaks that invariant reads as empty rather than
    // reading out of bounds.
    u32 width = 0;
    u32 height = 0;
    std::vector<i32> cells;

    [[nodiscard]] bool has_texture() const noexcept { return texture != nullptr; }

    // Total cells the grid describes.
    [[nodiscard]] usize cell_count() const noexcept { return static_cast<usize>(width) * height; }

    // True when the component has nothing to draw: no texture, or an empty grid.
    [[nodiscard]] bool empty() const noexcept { return !has_texture() || cell_count() == 0; }

    [[nodiscard]] bool in_bounds(u32 x, u32 y) const noexcept { return x < width && y < height; }

    // The tile index at (x, y), or kEmpty when the coordinate is out of bounds.
    // Reading outside the grid is an authoring query, not a bug, so it is
    // defined rather than asserted.
    [[nodiscard]] i32 at(u32 x, u32 y) const noexcept;

    // Writes the tile index at (x, y). Out-of-bounds writes are ignored, so a
    // tool that walks a rectangle past the edge cannot corrupt the grid.
    void set(u32 x, u32 y, i32 tile) noexcept;

    // Resizes the grid, filling every cell with `fill`. This discards existing
    // cells: resizing a map is a structural edit, not a crop.
    void resize(u32 new_width, u32 new_height, i32 fill = kEmpty);

    // Fills every existing cell with `fill`, keeping the dimensions.
    void clear(i32 fill = kEmpty) noexcept;

    // The UV sub-rectangle (u0, v0, u1, v1) of `tile` within the tileset, in the
    // layout Sprite2D::uv expects. An out-of-range index yields the full texture,
    // so a misconfigured map draws the whole tileset rather than a degenerate
    // cell.
    [[nodiscard]] Vec4 tile_uv(i32 tile) const noexcept;

    // The grid as compact JSON: {"width":W,"height":H,"cells":[...]}. Read by the
    // "grid" reflection property.
    [[nodiscard]] std::string to_json() const;

    // Replaces the grid from the JSON to_json() produces. Returns false and
    // leaves the component unchanged when the document is malformed or its cell
    // count disagrees with its dimensions, so a hand-edited scene keeps a valid
    // map rather than loading a half-built one.
    bool from_json(std::string_view text);
};

} // namespace fe
