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
// This is the seam docs/SHIPPING_QUEUE.md's `touch-input-actions` item calls for. Touch
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

    // Removes the bindings of one kind for `action`, leaving the others alone.
    //
    // This is what lets a game *replace* a binding the engine declared rather
    // than only add to it: the built-in behaviours bind a conventional touch
    // layout (declare_builtin_actions), and a game that wants a different one
    // clears the region it is redefining before binding its own. Per kind,
    // because replacing a game's touch layout must not also silently drop its
    // keyboard bindings.
    void clear_key_bindings(Id action);
    void clear_mouse_bindings(Id action);
    void clear_touch_regions(Id action);

    // --- Per-frame evaluation ----------------------------------------------

    // Keeps interactive touch regions off the parts of the screen the OS owns: a
    // notch, a rounded corner, a home indicator. The four values are normalised
    // fractions of the viewport (0..1) reserved on each edge, so they are the
    // same units the regions are written in and survive a resize the way a region
    // does. They are measured from CSS `env(safe-area-inset-*)` by the web shell,
    // which is the only layer that can read them, and handed down as numbers
    // (docs/SHIPPING_QUEUE.md, `mobile-safe-area`).
    //
    // The insets *shrink the space a region is measured in* rather than being
    // subtracted from each region: a region written `[0, 0, 0.34, 1.0]` means "the
    // left third of the usable area", which is what an author means and what
    // keeps a layout valid on a notched and an unnotched phone alike. A touch in
    // the reserved band is outside every region, so a stray palm on the home
    // indicator fires nothing.
    //
    // All-zero (the default, and what every non-mobile backend reports) is the
    // identity mapping, so a desktop build is unaffected.
    void set_safe_area_insets(f32 top, f32 right, f32 bottom, f32 left);

    [[nodiscard]] f32 safe_area_top() const noexcept { return safe_top_; }
    [[nodiscard]] f32 safe_area_right() const noexcept { return safe_right_; }
    [[nodiscard]] f32 safe_area_bottom() const noexcept { return safe_bottom_; }
    [[nodiscard]] f32 safe_area_left() const noexcept { return safe_left_; }

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

    // Normalised fractions of the viewport reserved on each edge for the OS's own
    // chrome. Zero on every backend that is not a phone.
    f32 safe_top_ = 0.0f;
    f32 safe_right_ = 0.0f;
    f32 safe_bottom_ = 0.0f;
    f32 safe_left_ = 0.0f;

    // Indexed by Id - 1. Empty until the first update().
    std::vector<bool> down_;
    std::vector<bool> pressed_;
    std::vector<bool> released_;
};

} // namespace fe
