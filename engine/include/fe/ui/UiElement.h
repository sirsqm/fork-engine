// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Math.h"
#include "fe/core/Types.h"
#include "fe/resources/Resource.h"
#include "fe/scene/Component.h"
#include "fe/ui/UiTypes.h"

#include <memory>
#include <string>

namespace fe::ui {

// One node of an in-game UI, laid out in screen space.
//
// An element is deliberately *not* a widget with a behaviour. It says where a box
// goes and what fills it; what the box *does* is the game's, read from the
// `interaction` field this frame produced. That split is what keeps a button from
// being engine code: a menu system reads `clicked` and decides, exactly as a
// gameplay system reads an action rather than a key.
//
// The tree comes from the scene graph. A `UiCanvas` establishes the screen box;
// every descendant node carrying a `UiElement` is laid out relative to its
// nearest `UiElement` ancestor, or to the canvas when it has none. That is the
// whole authoring model, and it is why the editor's existing hierarchy, undo, and
// serialization work on a UI for free.
//
// Placement has two modes, chosen by the parent:
//   * The parent is a flow container (`UiDirection::Row`/`Column`): children are
//     placed in sequence along the axis with `gap` between them, and `align` /
//     `justify` distribute them. A menu or a settings list.
//   * The parent is not (`UiDirection::None`): each child is placed by its own
//     `anchor` and `offset` against the parent's content box. A HUD, where a
//     score sits in one corner and a lives count in the other.
//
// Appearance is three optional layers drawn in order: a solid `fill`, then a
// `texture`, then the `text`. Each is skipped when unset, so a spacer element is
// an element with no fill, no texture, and no text.
class UiElement final : public Component {
public:
    static constexpr const char* kTypeName = "UiElement";

    [[nodiscard]] const char* type_name() const noexcept override { return kTypeName; }

    // --- Sizing ------------------------------------------------------------
    // How this element's own box is sized. See UiSize for the four modes. A
    // canvas-sized panel is `width = fill()`, a fixed button is
    // `width = fixed(160.0f)`.
    UiSize width = UiSize::content();
    UiSize height = UiSize::content();

    // Space reserved around the element, outside its box. A margin pushes an
    // element away from its siblings in a flow container and is ignored by an
    // anchored one, whose `offset` already places it exactly.
    EdgeInsets margin{};

    // Space between the element's edge and its children. Only meaningful on a
    // container, and the first thing to check when a panel's contents touch its
    // border.
    EdgeInsets padding{};

    // --- Placement (used when the parent is not a flow container) ----------
    // Which point of the parent's content box this element is placed against.
    UiAnchor anchor = UiAnchor::TopLeft;

    // A nudge from the anchor, in pixels. `anchor = TopRight` with
    // `offset = {-24, 16}` pins a lives count 24px from the right edge.
    Vec2 offset{};

    // --- Flow (used when this element is a container) ----------------------
    // How this element arranges *its* children. `None` (the default) leaves them
    // to their own anchors, which is what a HUD wants.
    UiDirection direction = UiDirection::None;

    // Space between children along the flow axis, in pixels.
    f32 gap = 0.0f;

    // Cross-axis placement of this element's children.
    UiAlign align = UiAlign::Start;

    // Main-axis distribution of this element's children.
    UiJustify justify = UiJustify::Start;

    // --- Appearance: fill --------------------------------------------------
    // A solid colour quad behind everything else. Alpha 0 (the default) draws
    // nothing, so an element that only carries text or a texture is the norm.
    Vec4 fill{0.0f, 0.0f, 0.0f, 0.0f};

    // --- Appearance: texture ----------------------------------------------
    // An optional texture stretched over the element's box. Null draws nothing.
    // Holding the shared_ptr is what keeps it loaded, the same rule Sprite2D and
    // TextLabel follow.
    std::shared_ptr<Resource> texture;

    // The sub-rectangle of `texture` to sample, in the (u0, v0, u1, v1)
    // convention the rest of the 2D path uses.
    Vec4 texture_uv{0.0f, 0.0f, 1.0f, 1.0f};

    // Tint multiplied with the sampled texel.
    Vec4 texture_color{1.0f, 1.0f, 1.0f, 1.0f};

    // --- Appearance: text --------------------------------------------------
    // A single line of text drawn inside the element's box. Empty draws nothing.
    std::string text;

    // The font for `text`. Null falls back to the canvas's default font, so a HUD
    // sets its font once on the canvas rather than on every label.
    std::shared_ptr<Resource> font;

    // One em, in pixels. Zero means "use the canvas's `text_size`", so a HUD sets
    // its size once on the canvas rather than on every label. The atlas is a
    // bitmap, so a large size shows its pixels; that is honest for the generated
    // atlas and is what the follow-on rasterizer would change, not this field.
    f32 text_size = 0.0f;

    // Tint multiplied with the sampled glyph texel.
    Vec4 text_color{1.0f, 1.0f, 1.0f, 1.0f};

    // Where `text` sits inside the element's box. `Start`/`Center`/`End` read as
    // left/centre/right horizontally and top/middle/bottom vertically; `Stretch`
    // is meaningless for a line and reads as `Start`.
    UiAlign text_align = UiAlign::Center;
    UiAlign text_vertical = UiAlign::Center;

    // --- Order and state ---------------------------------------------------
    // An extra layer offset, added on top of the canvas's base layer and the
    // element's depth in the tree. Tree order already puts a parent behind its
    // children; this is the escape hatch for the rare case that needs to
    // override it.
    i32 layer = 0;

    // When false the element is skipped by layout, drawing, and hit testing, and
    // its children are skipped with it. An invisible element costs nothing, which
    // is what makes a hidden menu free rather than merely transparent.
    bool visible = true;

    // When false the element still draws but is not hit tested, so a click passes
    // through to what is behind it. A disabled button is the case.
    bool interactable = true;

    // --- Runtime output ----------------------------------------------------
    // What the input system observed this frame. Written by
    // `UiInputSystem`, read by the game. Not serialized: it is the result of an
    // interaction, not authored state.
    UiInteraction interaction{};
};

// Establishes a screen-space UI region and the design resolution it is authored
// against.
//
// A game is authored against one resolution and must look right in a browser
// window of any size, so the canvas scales the whole UI by a single factor rather
// than letting every element fight the aspect ratio. Layout runs in design space
// and the result is mapped to the real viewport, which is what keeps the numbers
// an author types stable across window sizes.
//
// One canvas per UI layer. A game HUD, a pause menu, and a settings screen are
// usually three canvases on different base layers, which is also what lets one be
// hidden without touching the others.
class UiCanvas final : public Component {
public:
    static constexpr const char* kTypeName = "UiCanvas";

    [[nodiscard]] const char* type_name() const noexcept override { return kTypeName; }

    // The resolution the UI is authored against. Layout is computed in this
    // space, so an element at (24, 16) is 24px from the left and 16px from the
    // top of the *design*, at any window size.
    f32 design_width = 1280.0f;
    f32 design_height = 720.0f;

    // How the design maps onto the real viewport.
    UiScaleMode scale_mode = UiScaleMode::Fit;

    // Space reserved between the viewport edge and the design box, in design
    // pixels. The one knob that moves every anchored element inward at once.
    EdgeInsets padding{};

    // The layer this canvas's content starts at, on the same scale as
    // Sprite2D::layer. A HUD sits above the game (a positive layer) and a menu
    // above the HUD.
    i32 layer = 100;

    // The font used by any element whose own `font` is null, so a HUD names its
    // font once. Null means an element without a font draws no text.
    std::shared_ptr<Resource> font;

    // The default text size for elements that do not set one.
    f32 text_size = 24.0f;

    // When false the whole canvas is skipped, children included. Hiding a menu is
    // one flag, not a walk of its tree.
    bool visible = true;
};

} // namespace fe::ui
