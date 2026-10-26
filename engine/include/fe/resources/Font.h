// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Math.h"
#include "fe/core/Types.h"
#include "fe/resources/Resource.h"

#include <array>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace fe {

// A bitmap font: one texture atlas of glyphs plus the metrics needed to lay a
// string out ([ADR-0083](../../docs/adr/0083-in-engine-text-rendering.md)).
//
// This is the resource half of in-engine text. The atlas is an ordinary
// `Texture2D` the resource system already loads; the metrics are a small JSON
// document that travels beside it. A glyph is addressed by its byte value, so a
// lookup is an array index rather than a map probe, and an unknown byte draws
// nothing rather than the wrong character.
//
// The atlas is generated deterministically by `tools/gen_font_atlas.py` and
// committed, so the build needs no font file, no rasterizer, and no network, and
// a test that asserts a glyph's UV is asserting a reproducible value.
//
// What this is deliberately not: there is no wrapping, no kerning, no shaping,
// and no bidirectional text. A font lays out one line of left-to-right glyphs,
// which is what a HUD needs. A real rasterizer (stb_truetype) is the follow-on
// that would change this resource, not the component that consumes it.
class Font final : public Resource {
public:
    static constexpr const char* kTypeName = "Font";

    // One glyph's placement. `uv` is the atlas cell in the (u0, v0, u1, v1)
    // convention Sprite2D::uv uses, so a glyph quad reaches the draw step exactly
    // like any other sprite.
    //
    // The metrics are in em units, not atlas pixels: one em is 1.0, so an
    // `advance` of 0.6 means "six tenths of an em". Normalizing here is what
    // gives the layout a single scale factor -- multiply every metric by the
    // label's `size` to get world units -- rather than one for the atlas and one
    // for the label, which is the pair that drifts.
    struct Glyph {
        // Atlas cell. A zero-area cell (u0 == u1) means "no glyph", which is how
        // an unlisted byte draws nothing.
        Vec4 uv{0.0f, 0.0f, 0.0f, 0.0f};
        // How far the pen advances after this glyph, in em units. Zero for a
        // blank.
        f32 advance = 0.0f;
        // Offset from the pen position to the quad's top-left, in em units, with
        // +Y up. Lets a glyph sit on the baseline without the layout knowing its
        // shape.
        Vec2 bearing{0.0f, 0.0f};
        // The quad's size in em units. Independent of the atlas cell so a glyph
        // can have padding around it.
        Vec2 size{0.0f, 0.0f};
    };

    Font(std::string path, std::shared_ptr<Resource> atlas, f32 line_height, std::vector<Glyph> glyphs);

    [[nodiscard]] const char* type_name() const noexcept override { return kTypeName; }

    // The glyph atlas. Holding the shared_ptr is what keeps the texture loaded,
    // for the same reason Sprite2D holds its texture.
    [[nodiscard]] const std::shared_ptr<Resource>& atlas() const noexcept { return atlas_; }
    [[nodiscard]] bool has_atlas() const noexcept { return atlas_ != nullptr; }

    // Distance from one baseline to the next, in em units.
    [[nodiscard]] f32 line_height() const noexcept { return line_height_; }

    // The glyph for a byte, or nullptr when the atlas has none. Bytes at or above
    // the table's size have no glyph, so a UTF-8 continuation byte draws nothing
    // rather than indexing out of bounds.
    [[nodiscard]] const Glyph* glyph(u8 code) const noexcept {
        return code < glyphs_.size() ? &glyphs_[code] : nullptr;
    }

    // The pen advance for a byte: the glyph's advance, or zero when there is no
    // glyph. The layout uses this so a missing glyph still leaves the string
    // measurable.
    [[nodiscard]] f32 advance(u8 code) const noexcept {
        const Glyph* g = glyph(code);
        return g != nullptr ? g->advance : 0.0f;
    }

    // The width a string occupies in em units: the sum of its glyphs'
    // advances. Multiply by the label's size for world units.
    [[nodiscard]] f32 measure(std::string_view text) const noexcept;

    // The number of glyphs the table holds (one past the highest byte it can
    // address).
    [[nodiscard]] usize glyph_count() const noexcept { return glyphs_.size(); }

    [[nodiscard]] usize memory_usage() const noexcept override { return glyphs_.size() * sizeof(Glyph); }

    // Builds a font from the metrics document the generator writes. `atlas` is
    // the texture the metrics refer to, already loaded. Returns nullptr and fills
    // `error` when the document is malformed or disagrees with itself, so a
    // hand-edited font is rejected rather than loaded half-built.
    [[nodiscard]] static std::shared_ptr<Font> from_json(std::string_view path, std::string_view text,
                                                         std::shared_ptr<Resource> atlas, std::string& error);

private:
    std::shared_ptr<Resource> atlas_;
    f32 line_height_ = 0.0f;
    // Indexed by byte value, so `glyph()` is a bounds check and an index.
    std::vector<Glyph> glyphs_;
};

} // namespace fe
