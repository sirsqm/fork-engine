// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Types.h"
#include "fe/ui/UiTypes.h"

#include <vector>

namespace fe {

class Node;
class Scene;

namespace ui {

class UiCanvas;
class UiElement;

// One laid-out UI element: the box it occupies on screen, and enough context to
// draw it and to hit test it.
//
// This is the flat array the whole UI seam meets at, exactly as
// `render::SpriteInstance` is for the 2D world (ADR-0017, ADR-0086). Layout is a
// pure function of the scene and the viewport that produces one of these per
// visible element, in draw order; extraction turns it into quads, hit testing
// turns it into clicks, and the editor will turn it into selection outlines.
// Because it is a plain value, a test asserts on a rectangle with no GPU, no
// window, and no frame.
//
// Draw order is the array order: an element later in the array draws over one
// earlier. Layout emits a parent before its children and siblings in tree order,
// so "later in the tree draws on top" holds without any sort.
struct UiResolved {
    // The component, kept for provenance. The draw reads the resolved fields.
    const UiElement* element = nullptr;
    // The canvas the element belongs to, which supplies the default font and text
    // size for an element that does not set its own.
    const UiCanvas* canvas = nullptr;
    // The node, for a caller that wants to act on the element (the editor's
    // selection, a game looking up the node behind a click).
    const Node* node = nullptr;
    // The element's box in viewport pixels, top-left origin, +Y down.
    Rect rect{};
    // The same box in the canvas's design space, before the canvas scale. Kept
    // so a test can assert on the numbers an author typed rather than on the
    // scaled result, and so a debug overlay can label an element in design units.
    Rect design_rect{};
    // The absolute layer: the canvas's layer plus the element's own.
    i32 layer = 0;
    // The element's depth in the tree, from the canvas root. Part of the sort
    // key so a nested element is never reordered behind its parent.
    u32 depth = 0;
};

// The design-space-to-viewport mapping a canvas produces. One uniform scale and
// one offset, so the whole UI is authored once and drawn correctly at any window
// size ([ADR-0086](../../docs/adr/0086-in-game-ui-layout-and-extraction.md)).
//
// Exposed because three callers need the same answer and must not compute it
// differently: layout uses it to place elements, extraction uses it to size
// glyphs, and a debug overlay uses it to draw a canvas's bounds.
struct UiCanvasTransform {
    // Viewport pixels per design pixel. Uniform under every scale mode but
    // `Stretch`, which allows the two axes to differ.
    Vec2 scale{1.0f, 1.0f};
    // The viewport position of the design origin (0, 0).
    Vec2 offset{};

    // Maps a point from design space to viewport pixels.
    [[nodiscard]] constexpr Vec2 to_viewport(const Vec2& design_point) const noexcept {
        return Vec2{design_point.x * scale.x + offset.x, design_point.y * scale.y + offset.y};
    }

    // Maps a rectangle from design space to viewport pixels. A rect with a
    // non-uniform scale is still a rect, so this is exact, not an approximation.
    [[nodiscard]] constexpr Rect to_viewport(const Rect& design_rect) const noexcept {
        return Rect{to_viewport(design_rect.position),
                    Vec2{design_rect.size.x * scale.x, design_rect.size.y * scale.y}};
    }

    // Maps a point from viewport pixels back into design space, so a pointer
    // position can be hit tested against design-space rectangles directly.
    [[nodiscard]] constexpr Vec2 to_design(const Vec2& viewport_point) const noexcept {
        return Vec2{scale.x != 0.0f ? (viewport_point.x - offset.x) / scale.x : 0.0f,
                    scale.y != 0.0f ? (viewport_point.y - offset.y) / scale.y : 0.0f};
    }

    [[nodiscard]] constexpr bool operator==(const UiCanvasTransform&) const noexcept = default;
};

// The mapping `canvas` produces for a viewport of the given pixel size. A
// degenerate design size or viewport yields the identity rather than a division
// by zero, so a half-configured canvas draws something predictable.
[[nodiscard]] UiCanvasTransform canvas_transform(const UiCanvas& canvas, u32 viewport_width,
                                                 u32 viewport_height) noexcept;

// The result of a layout pass: the resolved elements in draw order.
using UiLayoutResult = std::vector<UiResolved>;

// Lays out every visible canvas in `scene` for a viewport of the given pixel
// size, writing the result into `out` (cleared first) in draw order.
//
// Pure: it reads the scene and writes nothing to it. That is what lets the
// renderer and the input system call it independently and be certain they agree
// on where every element is -- the one property a UI cannot get wrong, because a
// button that draws in one place and responds in another is worse than no button
// at all. It also lets a test assert on a rectangle with no GPU.
//
// Layout runs in the canvas's design space and the result is mapped to the
// viewport, so the numbers an author types are stable across window sizes.
//
// A node is laid out when it carries a `UiElement` and its nearest ancestor
// carrying a `UiElement` is under a `UiCanvas`. An element with `visible` false
// and its whole subtree are skipped, so a hidden menu costs nothing.
//
// Sizing is one pass, not a constraint solver. An element's own size is resolved
// against its parent's content box; a `Content` axis then measures its children
// against that box. The consequence, stated because it is the one thing an
// author can be surprised by: a `Fill` or `Percent` child of a `Content`-sized
// parent resolves against the space available to the parent, not against the
// parent's eventual content size. Every HUD and menu layout in the shipped
// examples fits this rule; a layout that does not need a real constraint pass,
// which is the follow-on a second consumer would justify.
void layout(const Scene& scene, u32 viewport_width, u32 viewport_height, UiLayoutResult& out);

// The topmost element containing `viewport_point`, or nullptr.
//
// "Topmost" is the last in draw order, so it is the element the user sees. Only
// `interactable` elements are considered, and an element whose box is empty is
// never hit, so a spacer or a disabled button does not swallow a click. The
// rectangle test is half-open on the far edges, so two abutting elements never
// both claim a boundary pixel.
[[nodiscard]] const UiResolved* hit_test(const UiLayoutResult& result, const Vec2& viewport_point) noexcept;

// Whether any element in `result` contains `viewport_point`. A cheap way for a
// game to ask "is the pointer over the UI?" before acting on a world click, which
// is what stops a click on a menu from also firing a shot.
[[nodiscard]] bool pointer_over_ui(const UiLayoutResult& result, const Vec2& viewport_point) noexcept;

} // namespace ui
} // namespace fe
