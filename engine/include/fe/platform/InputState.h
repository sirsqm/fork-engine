// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Math.h"
#include "fe/core/Types.h"

#include <array>

namespace fe {

// Physical key codes, independent of any windowing library. The values are
// stable within a major version and are the only key identity the rest of the
// engine sees; the platform backend translates native codes into these.
enum class Key : u16 {
    Unknown = 0,

    A,
    B,
    C,
    D,
    E,
    F,
    G,
    H,
    I,
    J,
    K,
    L,
    M,
    N,
    O,
    P,
    Q,
    R,
    S,
    T,
    U,
    V,
    W,
    X,
    Y,
    Z,

    Num0,
    Num1,
    Num2,
    Num3,
    Num4,
    Num5,
    Num6,
    Num7,
    Num8,
    Num9,

    Escape,
    Enter,
    Tab,
    Backspace,
    Insert,
    Delete,
    Right,
    Left,
    Down,
    Up,
    PageUp,
    PageDown,
    Home,
    End,
    Space,
    Minus,
    Equal,
    LeftBracket,
    RightBracket,
    Backslash,
    Semicolon,
    Apostrophe,
    Comma,
    Period,
    Slash,
    Grave,

    F1,
    F2,
    F3,
    F4,
    F5,
    F6,
    F7,
    F8,
    F9,
    F10,
    F11,
    F12,

    LeftShift,
    RightShift,
    LeftControl,
    RightControl,
    LeftAlt,
    RightAlt,

    Count,
};

enum class MouseButton : u8 {
    Left = 0,
    Right,
    Middle,
    Count,
};

// Per-frame input snapshot.
//
// The state is event-driven: the platform backend feeds raw events in, and the
// runtime calls begin_frame() once per iteration to advance the edge state.
// This keeps "was pressed this frame" queries cheap and deterministic, and
// keeps the whole subsystem testable without a window: tests call the on_*()
// methods directly.
class InputState {
public:
    static constexpr usize kKeyCount = static_cast<usize>(Key::Count);
    static constexpr usize kMouseButtonCount = static_cast<usize>(MouseButton::Count);

    // Advances the frame: current -> previous, edges and per-frame deltas
    // cleared. Call once at the start of each frame, before polling events.
    void begin_frame() noexcept;

    void on_key(Key key, bool down) noexcept;
    void on_mouse_button(MouseButton button, bool down) noexcept;
    void on_mouse_move(f32 x, f32 y) noexcept;
    void on_scroll(f32 dx, f32 dy) noexcept;

    [[nodiscard]] bool is_key_down(Key key) const noexcept;
    [[nodiscard]] bool was_key_pressed(Key key) const noexcept;
    [[nodiscard]] bool was_key_released(Key key) const noexcept;

    [[nodiscard]] bool is_mouse_down(MouseButton button) const noexcept;
    [[nodiscard]] bool was_mouse_pressed(MouseButton button) const noexcept;
    [[nodiscard]] bool was_mouse_released(MouseButton button) const noexcept;

    [[nodiscard]] Vec2 mouse_position() const noexcept { return mouse_position_; }
    [[nodiscard]] Vec2 mouse_delta() const noexcept { return mouse_delta_; }
    [[nodiscard]] Vec2 scroll_delta() const noexcept { return scroll_delta_; }

    // Releases every key/button without clearing edge history. Used when the
    // window loses focus so that a held key does not stick down.
    void release_all() noexcept;

private:
    [[nodiscard]] static usize index_of(Key key) noexcept {
        const auto value = static_cast<usize>(key);
        return value < kKeyCount ? value : 0;
    }
    [[nodiscard]] static usize index_of(MouseButton button) noexcept {
        const auto value = static_cast<usize>(button);
        return value < kMouseButtonCount ? value : 0;
    }

    std::array<bool, kKeyCount> keys_down_{};
    std::array<bool, kKeyCount> keys_pressed_{};
    std::array<bool, kKeyCount> keys_released_{};

    std::array<bool, kMouseButtonCount> buttons_down_{};
    std::array<bool, kMouseButtonCount> buttons_pressed_{};
    std::array<bool, kMouseButtonCount> buttons_released_{};

    Vec2 mouse_position_{};
    Vec2 mouse_delta_{};
    Vec2 scroll_delta_{};
};

} // namespace fe
