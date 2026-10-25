// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Math.h"
#include "fe/core/Types.h"
#include "fe/platform/InputState.h"

#include <string>
#include <string_view>
#include <vector>

namespace fe {

// Gameplay actions: what a player *means* to do, separated from how they say it.
//
// A game asks whether "MoveLeft" is held, not whether Key::A or a finger on the
// left half of a phone screen is held. That indirection is what lets one piece of
// gameplay code serve a keyboard, a mouse and a touchscreen: each device is a
// binding, and the system that reads the action never learns which one fired.
//
// This is the seam docs/ROADMAP.md's `touch-input-actions` item calls for. Touch
// already reaches InputState — a backend translates native events into
// `on_touch` — but without an action layer it is present and unusable, because
// gameplay reads raw keys. Here a touch region produces the same action a key
// press does, so a phone drives the identical system code.
//
// The map is *derived* state: update() recomputes every action from this frame's
// InputState snapshot rather than accumulating its own, so it inherits
// InputState's per-frame edge semantics exactly and cannot drift from them.
class ActionMap {
public:
    // An action handle. 1-based, so kNone is never a valid action and a default
    // handle fails safe — is_down() answers false rather than aliasing the first
    // action.
    using Id = u32;
    static constexpr Id kNone = 0;

    // Declares an action and returns its handle. The name is for diagnostics and
    // for a future rebinding file; gameplay holds the handle.
    Id add(std::string_view name);

    [[nodiscard]] Id find(std::string_view name) const noexcept;

    // --- Bindings ----------------------------------------------------------

    // A key, a mouse button, or a rectangle of the viewport that a touch inside
    // of triggers. The rectangle is normalised (0..1, origin top-left, y
    // increasing downward), so a binding survives a resize and reads the same
    // whichever way a phone is held.
    void bind_key(Id action, Key key);
    void bind_mouse_button(Id action, MouseButton button);
    void bind_touch_region(Id action, f32 x0, f32 y0, f32 x1, f32 y1);

    // --- Per-frame evaluation ----------------------------------------------

    // Recomputes every action from `input`'s snapshot. `viewport` is the drawing
    // size in the same pixels as InputState's positions and is what a touch
    // position is normalised against. Call once per frame, after
    // InputState::begin_frame() and the backend's event delivery.
    void update(const InputState& input, Vec2 viewport);

    [[nodiscard]] bool is_down(Id action) const noexcept;
    [[nodiscard]] bool was_pressed(Id action) const noexcept;
    [[nodiscard]] bool was_released(Id action) const noexcept;

    [[nodiscard]] usize action_count() const noexcept { return names_.size(); }
    [[nodiscard]] std::string_view action_name(Id action) const noexcept;

private:
    struct KeyBinding {
        Id action;
        Key key;
    };
    struct ButtonBinding {
        Id action;
        MouseButton button;
    };
    struct TouchRegion {
        Id action;
        f32 x0;
        f32 y0;
        f32 x1;
        f32 y1;
    };

    [[nodiscard]] bool valid(Id action) const noexcept {
        return action != kNone && static_cast<usize>(action) <= names_.size();
    }

    std::vector<std::string> names_;
    std::vector<KeyBinding> keys_;
    std::vector<ButtonBinding> buttons_;
    std::vector<TouchRegion> touch_regions_;

    // Indexed by Id - 1. Empty until the first update().
    std::vector<bool> down_;
    std::vector<bool> pressed_;
    std::vector<bool> released_;
};

} // namespace fe
