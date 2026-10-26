// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/ui/UiLayout.h"

#include "fe/resources/Font.h"
#include "fe/scene/Node.h"
#include "fe/scene/Scene.h"
#include "fe/ui/UiElement.h"

#include <algorithm>
#include <cmath>

namespace fe::ui {
namespace {

// The fraction of the parent's box an anchor names, per axis: 0 is the start
// edge, 0.5 the centre, 1 the end edge. Turning the nine anchors into two
// fractions is what keeps placement one expression instead of a nine-case
// switch, and it is why a new anchor would be a table entry rather than new
// code.
[[nodiscard]] constexpr f32 anchor_fraction_x(UiAnchor anchor) noexcept {
    switch (anchor) {
    case UiAnchor::TopLeft:
    case UiAnchor::MiddleLeft:
    case UiAnchor::BottomLeft:
        return 0.0f;
    case UiAnchor::TopCenter:
    case UiAnchor::MiddleCenter:
    case UiAnchor::BottomCenter:
        return 0.5f;
    case UiAnchor::TopRight:
    case UiAnchor::MiddleRight:
    case UiAnchor::BottomRight:
        return 1.0f;
    }
    return 0.0f;
}

[[nodiscard]] constexpr f32 anchor_fraction_y(UiAnchor anchor) noexcept {
    switch (anchor) {
    case UiAnchor::TopLeft:
    case UiAnchor::TopCenter:
    case UiAnchor::TopRight:
        return 0.0f;
    case UiAnchor::MiddleLeft:
    case UiAnchor::MiddleCenter:
    case UiAnchor::MiddleRight:
        return 0.5f;
    case UiAnchor::BottomLeft:
    case UiAnchor::BottomCenter:
    case UiAnchor::BottomRight:
        return 1.0f;
    }
    return 0.0f;
}

// The font an element draws its text with: its own, else its canvas's. Returns
// nullptr when neither is set or neither resolves to a `Font`, in which case the
// element has no text extent -- an unset font is an authoring state, not an
// error, exactly as an unset sprite texture is.
[[nodiscard]] const Font* resolve_font(const UiElement& element, const UiCanvas& canvas) noexcept {
    const Resource* resource = element.font != nullptr ? element.font.get() : canvas.font.get();
    return dynamic_cast<const Font*>(resource);
}

// The size a single line of the element's text occupies, in design pixels. Zero
// when the element has no text or no font, which is what lets the caller take a
// max without a special case.
[[nodiscard]] Vec2 text_extent(const UiElement& element, const UiCanvas& canvas) noexcept {
    if (element.text.empty()) {
        return Vec2{};
    }
    const Font* font = resolve_font(element, canvas);
    if (font == nullptr) {
        return Vec2{};
    }
    const f32 size = element.text_size > 0.0f ? element.text_size : canvas.text_size;
    return Vec2{font->measure(element.text) * size, font->line_height() * size};
}

// The children of `node` that take part in layout: those carrying a visible
// `UiElement`. Collected into a fixed-size caller array by the callers below so
// the layout walk allocates nothing per element beyond the result array.
[[nodiscard]] bool is_layout_child(const Node& child) noexcept {
    const auto* element = child.get_component<UiElement>();
    return element != nullptr && element->visible;
}

// Counts the children that take part in layout, so a flow can compute its free
// space before placing anything.
[[nodiscard]] usize layout_child_count(const Node& node) noexcept {
    usize count = 0;
    for (const auto& child : node.children()) {
        if (is_layout_child(*child)) {
            ++count;
        }
    }
    return count;
}

// The element's own box size in design pixels, resolved against `available` --
// the content extent of the box it is being placed in.
//
// A `Content` axis measures the element's children and its own text and takes
// the larger, so a label with text and no children sizes to its text and a panel
// sizes to what it holds. A `Fill`/`Percent` axis resolves against `available`,
// which is the space the *parent* offers rather than the parent's eventual
// content size; see the note on layout() for why that is the documented rule.
[[nodiscard]] Vec2 element_size(const Node& node, const UiElement& element, const UiCanvas& canvas,
                                const Vec2& available) noexcept;

// The extent `element`'s children occupy along each axis, used by a `Content`
// size. `Row` sums widths and takes the largest height; `Column` is the
// transpose; `None` takes the largest of each, because anchored children are
// placed independently and have no sequence to sum. Gaps between children are
// part of the extent, so a row of three with an 8px gap is two gaps wider than
// its children.
[[nodiscard]] Vec2 children_extent(const Node& node, const UiElement& element, const UiCanvas& canvas,
                                   const Vec2& available) noexcept {
    Vec2 extent{};
    const UiDirection direction = element.direction;

    for (const auto& child_ptr : node.children()) {
        const Node& child = *child_ptr;
        if (!is_layout_child(child)) {
            continue;
        }
        const auto* child_element = child.get_component<UiElement>();
        const Vec2 size = element_size(child, *child_element, canvas, available);
        const Vec2 outer{size.x + child_element->margin.horizontal(), size.y + child_element->margin.vertical()};

        switch (direction) {
        case UiDirection::Row:
            extent.x += outer.x;
            extent.y = std::max(extent.y, outer.y);
            break;
        case UiDirection::Column:
            extent.y += outer.y;
            extent.x = std::max(extent.x, outer.x);
            break;
        case UiDirection::None:
            extent.x = std::max(extent.x, outer.x);
            extent.y = std::max(extent.y, outer.y);
            break;
        }
    }

    const usize count = layout_child_count(node);
    if (count > 1) {
        if (direction == UiDirection::Row) {
            extent.x += element.gap * static_cast<f32>(count - 1);
        } else if (direction == UiDirection::Column) {
            extent.y += element.gap * static_cast<f32>(count - 1);
        }
    }

    return extent;
}

[[nodiscard]] f32 resolve_axis(const UiSize& size, f32 available, f32 content_extent) noexcept {
    switch (size.mode) {
    case UiSizeMode::Fixed:
        return size.value > 0.0f ? size.value : 0.0f;
    case UiSizeMode::Percent:
        return available * size.value;
    case UiSizeMode::Fill:
        return available;
    case UiSizeMode::Content:
        return content_extent;
    }
    return 0.0f;
}

[[nodiscard]] Vec2 element_size(const Node& node, const UiElement& element, const UiCanvas& canvas,
                                const Vec2& available) noexcept {
    // Measure children once: a `Content` axis needs it and a fixed axis does not,
    // but the measurement is cheap and doing it unconditionally keeps the two
    // axes from disagreeing about what the children are.
    Vec2 content{};
    if (element.width.mode == UiSizeMode::Content || element.height.mode == UiSizeMode::Content) {
        content = children_extent(node, element, canvas, available);
    }

    // The element's own text is part of its content: a label with a string and no
    // children sizes to the string. Taking the max rather than the sum is what
    // lets an element carry both text and children without the two stacking.
    const Vec2 text = text_extent(element, canvas);
    content.x = std::max(content.x, text.x);
    content.y = std::max(content.y, text.y);

    const f32 width = resolve_axis(element.width, available.x, content.x);
    const f32 height = resolve_axis(element.height, available.y, content.y);
    return Vec2{std::max(width, 0.0f), std::max(height, 0.0f)};
}

// Places `element` at its anchor within `content_box` and returns its design-space
// rectangle. The anchor names the point of the parent's box the element is placed
// against, so the element's matching point is put there; `offset` then nudges it.
// The margin is not applied here: it separates flow siblings, and an anchored
// element's offset already places it exactly.
[[nodiscard]] Rect anchored_rect(const UiElement& element, const Vec2& size, const Rect& content_box) noexcept {
    const f32 fraction_x = anchor_fraction_x(element.anchor);
    const f32 fraction_y = anchor_fraction_y(element.anchor);
    const f32 x = content_box.position.x + fraction_x * (content_box.size.x - size.x) + element.offset.x;
    const f32 y = content_box.position.y + fraction_y * (content_box.size.y - size.y) + element.offset.y;
    return Rect{Vec2{x, y}, size};
}

// The along-axis cursor start and per-child extra spacing a `UiJustify` implies
// for a line of `total` extent inside `available`. `SpaceBetween` returns the
// start and reports the extra gap through `out_extra_gap`, so the caller's
// placement loop is one shape for all four modes.
[[nodiscard]] f32 justify_start(UiJustify justify, f32 available, f32 total, usize count, f32& out_extra_gap) noexcept {
    out_extra_gap = 0.0f;
    const f32 free = available - total;
    switch (justify) {
    case UiJustify::Start:
        return 0.0f;
    case UiJustify::Center:
        return free * 0.5f;
    case UiJustify::End:
        return free;
    case UiJustify::SpaceBetween:
        // Free space goes *between* the children, so a single child has nowhere
        // to put it and stays at the start.
        if (count > 1) {
            out_extra_gap = free / static_cast<f32>(count - 1);
        }
        return 0.0f;
    }
    return 0.0f;
}

// The cross-axis position of a child of the given size inside `content_box`.
[[nodiscard]] f32 cross_position(UiAlign align, f32 content_start, f32 content_extent, f32 child_extent) noexcept {
    switch (align) {
    case UiAlign::Start:
    case UiAlign::Stretch: // A stretch is handled by resizing; the origin is the start.
        return content_start;
    case UiAlign::Center:
        return content_start + (content_extent - child_extent) * 0.5f;
    case UiAlign::End:
        return content_start + (content_extent - child_extent);
    }
    return content_start;
}

// Writes one resolved element and lays out its children.
//
// The parent calls this with the element's final design-space rectangle, so
// positioning is decided by whoever knows the parent's flow mode and this
// function only has to place the children. Emitting the parent before recursing
// is what makes the result array's order a valid draw order: a parent is always
// behind its children, and siblings keep tree order.
void emit(const Node& node, const UiElement& element, const UiCanvas& canvas, const UiCanvasTransform& transform,
          const Rect& design_rect, i32 canvas_layer, u32 depth, UiLayoutResult& out) {
    out.push_back(UiResolved{&element, &canvas, &node, transform.to_viewport(design_rect), design_rect,
                             canvas_layer + element.layer, depth});

    // The children are laid out inside the padding, so a panel's border is never
    // covered by its contents.
    const Rect content_box = element.padding.inset(design_rect);
    const UiDirection direction = element.direction;

    if (direction == UiDirection::None) {
        for (const auto& child_ptr : node.children()) {
            const Node& child = *child_ptr;
            if (!is_layout_child(child)) {
                continue;
            }
            const auto* child_element = child.get_component<UiElement>();
            const Vec2 size = element_size(child, *child_element, canvas, content_box.size);
            emit(child, *child_element, canvas, transform, anchored_rect(*child_element, size, content_box),
                 canvas_layer, depth + 1, out);
        }
        return;
    }

    const usize count = layout_child_count(node);
    if (count == 0) {
        return;
    }

    // Measure every child first: the total decides the justify offset, and
    // `SpaceBetween` needs the count, so placement cannot be decided one child at
    // a time.
    struct Measured {
        const Node* node = nullptr;
        const UiElement* element = nullptr;
        Vec2 size{};
    };
    std::vector<Measured> measured;
    measured.reserve(count);
    f32 main_total = 0.0f;
    for (const auto& child_ptr : node.children()) {
        const Node& child = *child_ptr;
        if (!is_layout_child(child)) {
            continue;
        }
        const auto* child_element = child.get_component<UiElement>();
        Vec2 size = element_size(child, *child_element, canvas, content_box.size);
        const f32 outer = direction == UiDirection::Row ? size.x + child_element->margin.horizontal()
                                                        : size.y + child_element->margin.vertical();
        main_total += outer;
        measured.push_back(Measured{&child, child_element, size});
    }
    if (count > 1) {
        main_total += element.gap * static_cast<f32>(count - 1);
    }

    const bool is_row = direction == UiDirection::Row;
    const f32 available_main = is_row ? content_box.size.x : content_box.size.y;
    f32 extra_gap = 0.0f;
    const f32 start = is_row ? content_box.position.x : content_box.position.y;
    f32 cursor = start + justify_start(element.justify, available_main, main_total, count, extra_gap);

    for (Measured& entry : measured) {
        const UiElement& child_element = *entry.element;
        const EdgeInsets& margin = child_element.margin;

        if (is_row) {
            // A stretch resizes the child across the row's height; every other
            // alignment leaves the size alone and only moves it.
            if (element.align == UiAlign::Stretch) {
                entry.size.y = std::max(content_box.size.y - margin.vertical(), 0.0f);
            }
            const f32 x = cursor + margin.left;
            const f32 y = cross_position(element.align, content_box.position.y, content_box.size.y, entry.size.y);
            emit(*entry.node, child_element, canvas, transform, Rect{Vec2{x, y}, entry.size}, canvas_layer, depth + 1,
                 out);
            cursor += entry.size.x + margin.horizontal() + element.gap + extra_gap;
        } else {
            if (element.align == UiAlign::Stretch) {
                entry.size.x = std::max(content_box.size.x - margin.horizontal(), 0.0f);
            }
            const f32 y = cursor + margin.top;
            const f32 x = cross_position(element.align, content_box.position.x, content_box.size.x, entry.size.x);
            emit(*entry.node, child_element, canvas, transform, Rect{Vec2{x, y}, entry.size}, canvas_layer, depth + 1,
                 out);
            cursor += entry.size.y + margin.vertical() + element.gap + extra_gap;
        }
    }
}

// Lays out one canvas: its design box, then every child element inside it.
//
// The canvas node's own `UiElement`, when it has one, is laid out over the whole
// design box with its size ignored, so a canvas can carry a full-screen
// background fill or a title without a wrapper node. Its children then lay out
// inside its padding, exactly as any other element's would.
void layout_canvas(const Node& canvas_node, const UiCanvas& canvas, const UiCanvasTransform& transform,
                   UiLayoutResult& out) {
    const Rect design_box{Vec2{}, Vec2{canvas.design_width, canvas.design_height}};

    // A canvas node that carries its own `UiElement` lays that element over the
    // whole design box: a canvas is the frame, so its authored size is ignored
    // and a full-screen background or a title needs no wrapper node. Its children
    // then lay out inside its padding, exactly as any other element's would.
    const auto* canvas_element = canvas_node.get_component<UiElement>();
    if (canvas_element != nullptr && canvas_element->visible) {
        emit(canvas_node, *canvas_element, canvas, transform, design_box, canvas.layer, 0, out);
        return;
    }

    const Rect content_box = canvas.padding.inset(design_box);
    for (const auto& child_ptr : canvas_node.children()) {
        const Node& child = *child_ptr;
        if (!is_layout_child(child)) {
            continue;
        }
        const auto* child_element = child.get_component<UiElement>();
        const Vec2 size = element_size(child, *child_element, canvas, content_box.size);
        emit(child, *child_element, canvas, transform, anchored_rect(*child_element, size, content_box), canvas.layer,
             0, out);
    }
}

} // namespace

UiCanvasTransform canvas_transform(const UiCanvas& canvas, u32 viewport_width, u32 viewport_height) noexcept {
    const f32 design_width = canvas.design_width;
    const f32 design_height = canvas.design_height;
    const f32 viewport_w = static_cast<f32>(viewport_width);
    const f32 viewport_h = static_cast<f32>(viewport_height);

    // A degenerate design size or viewport has no meaningful mapping. Returning
    // the identity draws the design 1:1 from the viewport origin, which is
    // predictable and, more importantly, never divides by zero.
    if (design_width <= 0.0f || design_height <= 0.0f || viewport_w <= 0.0f || viewport_h <= 0.0f) {
        return UiCanvasTransform{};
    }

    const f32 available_w = std::max(viewport_w - canvas.padding.horizontal(), 0.0f);
    const f32 available_h = std::max(viewport_h - canvas.padding.vertical(), 0.0f);

    Vec2 scale{1.0f, 1.0f};
    switch (canvas.scale_mode) {
    case UiScaleMode::Fit: {
        const f32 uniform = std::min(available_w / design_width, available_h / design_height);
        scale = Vec2{uniform, uniform};
        break;
    }
    case UiScaleMode::Stretch:
        scale = Vec2{available_w / design_width, available_h / design_height};
        break;
    case UiScaleMode::MatchWidth: {
        const f32 uniform = available_w / design_width;
        scale = Vec2{uniform, uniform};
        break;
    }
    case UiScaleMode::MatchHeight: {
        const f32 uniform = available_h / design_height;
        scale = Vec2{uniform, uniform};
        break;
    }
    }

    // Centre the scaled design box in the available area, then shift by the
    // padding so the padding is the gap between the viewport edge and the design.
    const f32 box_w = design_width * scale.x;
    const f32 box_h = design_height * scale.y;
    const Vec2 offset{canvas.padding.left + (available_w - box_w) * 0.5f,
                      canvas.padding.top + (available_h - box_h) * 0.5f};
    return UiCanvasTransform{scale, offset};
}

void layout(const Scene& scene, u32 viewport_width, u32 viewport_height, UiLayoutResult& out) {
    out.clear();
    if (viewport_width == 0 || viewport_height == 0) {
        return;
    }

    // One walk of the scene. A node is a canvas when it carries a `UiCanvas`; the
    // canvases are laid out in the order the walk meets them, which is scene
    // order, so a canvas authored later in the tree draws over an earlier one at
    // the same layer.
    scene.for_each_node([&](Node& node) {
        const auto* canvas = node.get_component<UiCanvas>();
        if (canvas == nullptr || !canvas->visible) {
            return;
        }
        const UiCanvasTransform transform = canvas_transform(*canvas, viewport_width, viewport_height);
        layout_canvas(node, *canvas, transform, out);
    });
}

const UiResolved* hit_test(const UiLayoutResult& result, const Vec2& viewport_point) noexcept {
    // Backwards: the last element in draw order is the one the user sees, so it
    // wins a tie. That is the same rule the renderer's sort encodes, applied to
    // the array layout already put in draw order.
    for (auto it = result.rbegin(); it != result.rend(); ++it) {
        if (it->element == nullptr || !it->element->interactable) {
            continue;
        }
        if (it->rect.contains(viewport_point)) {
            return &*it;
        }
    }
    return nullptr;
}

bool pointer_over_ui(const UiLayoutResult& result, const Vec2& viewport_point) noexcept {
    return hit_test(result, viewport_point) != nullptr;
}

} // namespace fe::ui
