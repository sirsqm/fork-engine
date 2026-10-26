// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/systems/System.h"
#include "fe/ui/UiLayout.h"

#include <string_view>

namespace fe::ui {

// Turns the pointer into per-element hover, press, and click state once per
// frame.
//
// This is the input half of the UI seam, and it exists for one reason: a button
// must respond exactly where it draws. It therefore does not compute any geometry
// of its own -- it calls `ui::layout`, the same function `render::extract_ui`
// calls, and hit tests against that result. A second geometry implementation
// would be a second chance to disagree, and a UI where the click and the pixels
// disagree is worse than no UI at all ([ADR-0086](../../docs/adr/0086-in-game-ui-layout-and-extraction.md)).
//
// What it does *not* do: decide what a click means. It writes `UiInteraction`
// onto each element and stops. A menu system reads `clicked` and changes state,
// exactly as a gameplay system reads an action rather than a key, which is what
// keeps a button from being engine code.
//
// Ordering: add it before the systems that read `UiInteraction` and after nothing
// in particular; it depends only on the input snapshot and the scene's layout, so
// it can run first in the schedule.
//
// Input: it reads `FrameContext::input`, so it needs a window's snapshot. A
// headless test drives it by constructing an `InputState` and calling the `on_*`
// methods, which is the same seam every other input consumer is tested through.
// It uses the mouse position and left button, and the first touch contact when
// there is no mouse, so one code path serves a desktop and a touchscreen -- the
// same property `ActionMap` gives gameplay.
class UiInputSystem final : public System {
public:
    UiInputSystem() = default;

    [[nodiscard]] std::string_view name() const noexcept override { return "ui/input"; }

    void update(const FrameContext& context, Scene& scene) override;

    // The layout the most recent update() produced. Exposed so a game (or a test)
    // can hit test a point of its own -- a world click that must not also fire
    // when the pointer is over a menu -- without laying the UI out a second time.
    [[nodiscard]] const UiLayoutResult& layout() const noexcept { return layout_; }

    // The viewport the last update() laid out for. Zero before the first update,
    // or when the frame had no input and the system did nothing.
    [[nodiscard]] u32 viewport_width() const noexcept { return viewport_width_; }
    [[nodiscard]] u32 viewport_height() const noexcept { return viewport_height_; }

    // The element the pointer is over, or nullptr. Cleared every frame.
    [[nodiscard]] const UiResolved* hovered() const noexcept { return hovered_; }

    // True when the pointer is over any UI element this frame. A game uses it to
    // decide whether a click is the UI's or the world's.
    [[nodiscard]] bool pointer_over_ui() const noexcept { return hovered_ != nullptr; }

private:
    UiLayoutResult layout_;
    // The element the pointer went down on, so a release can tell a click (press
    // and release on the same element) from a release somewhere else. Held as a
    // pointer into `layout_`, which is rebuilt in place each frame; the element
    // component it points at is owned by the scene and outlives the frame, and the
    // comparison below is on that component, not on the array slot.
    const UiElement* pressed_element_ = nullptr;
    const UiResolved* hovered_ = nullptr;
    u32 viewport_width_ = 0;
    u32 viewport_height_ = 0;
};

} // namespace fe::ui
