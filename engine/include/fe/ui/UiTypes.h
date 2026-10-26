// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Math.h"
#include "fe/core/Types.h"

namespace fe::ui {

// The value types in-game UI is built from: rectangles, alignment, sizing, and
// spacing. They are plain data with no engine dependency beyond math, so the
// layout solver can be a pure function of them and a test can assert on a
// rectangle with no scene, no GPU, and no window.
//
// Coordinate space. UI lives in *screen space*: the origin is the viewport's
// top-left, +X is right, and +Y is *down*, matching the pixel space the platform
// reports mouse and touch positions in. That is deliberately not the world
// space the 2D camera uses (origin at the camera, +Y up): a HUD must not move
// when the camera pans, and a click must be testable against the same numbers
// the platform handed us, with no conversion in between. The two spaces meet
// once, in the renderer, where the UI pass is drawn with its own projection
// ([ADR-0086](../../docs/adr/0086-in-game-ui-layout-and-extraction.md)).

// A screen-space rectangle: a top-left position and a size, both in pixels.
//
// Kept as position + size rather than min/max because layout computes a size and
// then places it; storing the result as a corner and an extent is what makes
// "grow this box" a single addition rather than two coordinated edits.
struct Rect {
    Vec2 position{};
    Vec2 size{};

    [[nodiscard]] constexpr f32 left() const noexcept { return position.x; }
    [[nodiscard]] constexpr f32 top() const noexcept { return position.y; }
    [[nodiscard]] constexpr f32 right() const noexcept { return position.x + size.x; }
    [[nodiscard]] constexpr f32 bottom() const noexcept { return position.y + size.y; }
    [[nodiscard]] constexpr Vec2 center() const noexcept {
        return Vec2{position.x + size.x * 0.5f, position.y + size.y * 0.5f};
    }

    // Whether `point` lies inside the rectangle. Half-open on the far edges
    // (right/bottom are outside), so two abutting rectangles never both claim a
    // boundary pixel and a hit test is unambiguous.
    [[nodiscard]] constexpr bool contains(const Vec2& point) const noexcept {
        return point.x >= position.x && point.x < right() && point.y >= position.y && point.y < bottom();
    }

    // Whether the rectangle has no area. A zero-size element is skipped by
    // extraction, so this is the "nothing to draw or hit" test.
    [[nodiscard]] constexpr bool empty() const noexcept { return size.x <= 0.0f || size.y <= 0.0f; }

    [[nodiscard]] constexpr bool operator==(const Rect&) const noexcept = default;
};

// Per-edge spacing, in pixels. Used for both an element's outer margin and a
// container's inner padding, so one type describes both and an author reads the
// same four numbers either way.
struct EdgeInsets {
    f32 left = 0.0f;
    f32 top = 0.0f;
    f32 right = 0.0f;
    f32 bottom = 0.0f;

    [[nodiscard]] static constexpr EdgeInsets all(f32 value) noexcept { return EdgeInsets{value, value, value, value}; }
    [[nodiscard]] static constexpr EdgeInsets symmetric(f32 horizontal, f32 vertical) noexcept {
        return EdgeInsets{horizontal, vertical, horizontal, vertical};
    }

    [[nodiscard]] constexpr f32 horizontal() const noexcept { return left + right; }
    [[nodiscard]] constexpr f32 vertical() const noexcept { return top + bottom; }

    // Shrinks `rect` by this spacing. A padding larger than the rect collapses it
    // to zero rather than going negative, so a child of an over-padded container
    // has no area instead of an inside-out one.
    [[nodiscard]] constexpr Rect inset(const Rect& rect) const noexcept {
        const f32 width = rect.size.x - horizontal();
        const f32 height = rect.size.y - vertical();
        return Rect{Vec2{rect.position.x + left, rect.position.y + top},
                    Vec2{width > 0.0f ? width : 0.0f, height > 0.0f ? height : 0.0f}};
    }

    [[nodiscard]] constexpr bool operator==(const EdgeInsets&) const noexcept = default;
};

// How an element's width or height is decided. This is the whole sizing model:
// four cases, each with one meaning, so an author can predict a box's size
// without running the solver in their head.
enum class UiSizeMode : u8 {
    // `value` pixels in the canvas's design space.
    Fixed = 0,
    // `value` as a fraction (0..1) of the parent's corresponding extent.
    Percent = 1,
    // From the element's own children: their total extent along the flow axis,
    // or the largest along the cross axis. An element with no children is zero.
    Content = 2,
    // The parent's extent minus its padding. What a panel that fills its parent
    // asks for.
    Fill = 3,
};

// One axis of an element's size.
struct UiSize {
    UiSizeMode mode = UiSizeMode::Fixed;
    f32 value = 0.0f;

    [[nodiscard]] static constexpr UiSize fixed(f32 pixels) noexcept { return UiSize{UiSizeMode::Fixed, pixels}; }
    [[nodiscard]] static constexpr UiSize percent(f32 fraction) noexcept {
        return UiSize{UiSizeMode::Percent, fraction};
    }
    [[nodiscard]] static constexpr UiSize content() noexcept { return UiSize{UiSizeMode::Content, 0.0f}; }
    [[nodiscard]] static constexpr UiSize fill() noexcept { return UiSize{UiSizeMode::Fill, 0.0f}; }

    [[nodiscard]] constexpr bool operator==(const UiSize&) const noexcept = default;
};

// Where an element sits inside its parent when the parent is *not* a flow
// container (`UiDirection::None`). The anchor names the point of the parent's
// content box the element is placed against, and `offset` nudges it from there.
// A score pinned to `TopLeft` and a lives count pinned to `TopRight` are the two
// cases that matter, and both are a corner of the parent.
enum class UiAnchor : u8 {
    TopLeft = 0,
    TopCenter,
    TopRight,
    MiddleLeft,
    MiddleCenter,
    MiddleRight,
    BottomLeft,
    BottomCenter,
    BottomRight,
};

// How a container arranges its children. `None` leaves each child where its own
// anchor and offset put it, which is what a HUD wants: a score in one corner and
// a lives count in the other, with nothing in between. `Row` and `Column` lay
// children out in sequence with a gap, which is what a menu or a settings list
// wants. One component covers both because a HUD and a menu differ only in this
// field, not in kind.
enum class UiDirection : u8 {
    None = 0,
    Row,
    Column,
};

// Cross-axis placement of a flow container's children: where they sit across the
// line they flow along.
enum class UiAlign : u8 {
    Start = 0,
    Center,
    End,
    // The child's cross-axis size is stretched to the container's, so a full-width
    // menu button needs no explicit width.
    Stretch,
};

// Main-axis placement of a flow container's children: how the line as a whole is
// distributed along the axis it flows.
enum class UiJustify : u8 {
    Start = 0,
    Center,
    End,
    // Free space is distributed *between* children, which is what a row of
    // buttons spread across a bar wants.
    SpaceBetween,
};

// How the canvas's design resolution maps onto the real viewport. A game is
// authored against one resolution and must look right in a browser window of any
// size, so the canvas scales the whole UI by one factor rather than letting every
// element fight the aspect ratio.
enum class UiScaleMode : u8 {
    // Uniform scale to fit *inside* the viewport, centred: the whole design is
    // visible and nothing is cut off, at the cost of margins on one axis. The
    // right default for a game HUD, and what matches the 2D camera's reference
    // box so UI and world content line up.
    Fit = 0,
    // Non-uniform scale that fills the viewport exactly. No margins, but a circle
    // becomes an ellipse; correct only when the design tolerates it.
    Stretch,
    // Uniform scale by width alone, letting the vertical overflow. Good for a UI
    // that must touch both side edges.
    MatchWidth,
    // Uniform scale by height alone, letting the horizontal overflow.
    MatchHeight,
};

// What happened to an element this frame. Edge-triggered states last exactly one
// frame, matching InputState's convention, so a game polls `clicked` the same way
// it polls a key press.
struct UiInteraction {
    // The pointer is over the element and it is the topmost element there.
    bool hovered = false;
    // The pointer went down on the element and has not come up.
    bool pressed = false;
    // The pointer went down on the element this frame.
    bool pressed_this_frame = false;
    // The pointer came up over the element this frame, after pressing on it. This
    // is a click: press and release on the same element, so a drag off the element
    // before releasing does not fire it.
    bool clicked = false;
    // The pointer came up over the element this frame regardless of where it went
    // down.
    bool released = false;

    [[nodiscard]] constexpr bool operator==(const UiInteraction&) const noexcept = default;
};

} // namespace fe::ui
