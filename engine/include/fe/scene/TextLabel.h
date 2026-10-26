// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Math.h"
#include "fe/core/Types.h"
#include "fe/resources/Resource.h"
#include "fe/scene/Component.h"

#include <memory>
#include <string>

namespace fe {

// One line of text, drawn as a glyph quad per character.
//
// A string is many small quads, exactly as a level is many tiles, so text reuses
// the path a sprite takes rather than adding a second one: extraction turns the
// string into `render::SpriteInstance`s, the renderer batches them with the
// sprites and tiles around them, and nothing below the seam knows a character
// exists ([ADR-0083](../../docs/adr/0083-in-engine-text-rendering.md)). That is
// what makes text cost no new shader, no new pipeline state, and no renderer
// special-casing.
//
// The font is a `Font` resource: a glyph atlas plus per-byte metrics. Holding
// the shared_ptr is what keeps it loaded, the same rule Sprite2D follows.
//
// Coordinates. The label's origin is its pen start; the text runs along +X and
// the baseline is the origin's Y, so a glyph sits above it by its bearing. The
// node's world matrix composes with each glyph's local offset, so moving,
// rotating, or scaling the node transforms the whole line.
//
// Deliberately not here: wrapping, kerning, shaping, and bidirectional text. One
// line, left-to-right, in the glyphs the atlas holds. Naming that is what keeps
// the seam honest.
class TextLabel final : public Component {
public:
    static constexpr const char* kTypeName = "TextLabel";

    // Where the origin sits relative to the laid-out line, so a score can be
    // pinned to a corner without the author computing its width.
    enum class Align : i32 {
        Left = 0,
        Center = 1,
        Right = 2,
    };

    [[nodiscard]] const char* type_name() const noexcept override { return kTypeName; }

    // The glyph atlas and metrics. Null is allowed and draws nothing, so a label
    // can be authored before its font exists.
    std::shared_ptr<Resource> font;

    // The string to draw. A byte with no glyph in the font draws nothing but
    // still advances by zero, so an unsupported character is a gap rather than a
    // wrong glyph or a broken layout.
    std::string text;

    // The size one em is drawn at, in world units. The atlas is laid out at a
    // nominal pixel size, so this scales the glyph quads; text scaled far up
    // shows the atlas's pixels, which is honest for a bitmap font.
    f32 size = 32.0f;

    // Tint multiplied with the sampled glyph texel.
    Vec4 color{1.0f, 1.0f, 1.0f, 1.0f};

    // Horizontal placement of the line about the node origin.
    Align align = Align::Left;

    // Render order within the 2D pass, on the same scale as Sprite2D::layer, so
    // a HUD draws over the game and under a menu.
    i32 layer = 0;

    [[nodiscard]] bool has_font() const noexcept { return font != nullptr; }

    // True when the label has nothing to draw: no font, or no text.
    [[nodiscard]] bool empty() const noexcept { return !has_font() || text.empty(); }

    // The width of the line in world units at `size`, or 0 with no font. Used by
    // the layout and by a test that asserts the advance.
    [[nodiscard]] f32 measured_width() const noexcept;
};

} // namespace fe
