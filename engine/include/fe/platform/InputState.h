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

// A touch point's lifecycle phase, translated from the backend's native event.
// A touch is keyed by an opaque id the backend assigns; ids are reused after a
// touch ends, so only the events between Began and Ended for one id describe
// one contact.
enum class TouchPhase : u8 {
    Began = 0,
    Moved,
    Ended,
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

    // The maximum number of simultaneous touch contacts tracked. A backend
    // reports more than this only by reusing an id, exactly as it would when a
    // contact ends and a new one begins.
    static constexpr usize kMaxTouchPoints = 10;

    // Advances the frame: current -> previous, edges and per-frame deltas
    // cleared. Call once at the start of each frame, before polling events.
    void begin_frame() noexcept;

    void on_key(Key key, bool down) noexcept;
    void on_mouse_button(MouseButton button, bool down) noexcept;
    void on_mouse_move(f32 x, f32 y) noexcept;
    void on_scroll(f32 dx, f32 dy) noexcept;

    // Feeds one touch event. `id` is the backend's contact id, stable across
    // Began/Moved/Ended for one contact. The position is in the same window
    // space as mouse_position(). The touch path mirrors the mouse path: down
    // state persists until the matching Ended, and the begin/end edges are
    // cleared by begin_frame().
    void on_touch(TouchPhase phase, u32 id, f32 x, f32 y) noexcept;

    [[nodiscard]] bool is_key_down(Key key) const noexcept;
    [[nodiscard]] bool was_key_pressed(Key key) const noexcept;
    [[nodiscard]] bool was_key_released(Key key) const noexcept;

    [[nodiscard]] bool is_mouse_down(MouseButton button) const noexcept;
    [[nodiscard]] bool was_mouse_pressed(MouseButton button) const noexcept;
    [[nodiscard]] bool was_mouse_released(MouseButton button) const noexcept;

    [[nodiscard]] Vec2 mouse_position() const noexcept { return mouse_position_; }
    [[nodiscard]] Vec2 mouse_delta() const noexcept { return mouse_delta_; }
    [[nodiscard]] Vec2 scroll_delta() const noexcept { return scroll_delta_; }

    // Touch queries, keyed by the backend's contact id. `is_touch_down` is true
    // from the Began event until the matching Ended event; `was_touch_began`
    // and `was_touch_ended` are edges that live for exactly one frame.
    // `touch_position` is the id's last known position (zero for an id that is
    // not down). `touch_delta` is the movement accumulated during the current
    // frame, cleared by begin_frame() like mouse_delta().
    [[nodiscard]] bool is_touch_down(u32 id) const noexcept;
    [[nodiscard]] bool was_touch_began(u32 id) const noexcept;
    [[nodiscard]] bool was_touch_ended(u32 id) const noexcept;
    [[nodiscard]] Vec2 touch_position(u32 id) const noexcept;
    [[nodiscard]] Vec2 touch_delta(u32 id) const noexcept;

    // Releases every key/button/touch without clearing edge history. Used when
    // the window loses focus so that a held key does not stick down.
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
    // Touch ids are arbitrary u32 values, so a fixed slot array tracks them:
    // `find_touch` returns the slot index of a tracked id, or kMaxTouchPoints
    // when the id is unknown.
    [[nodiscard]] usize find_touch(u32 id) const noexcept;

    std::array<bool, kKeyCount> keys_down_{};
    std::array<bool, kKeyCount> keys_pressed_{};
    std::array<bool, kKeyCount> keys_released_{};

    std::array<bool, kMouseButtonCount> buttons_down_{};
    std::array<bool, kMouseButtonCount> buttons_pressed_{};
    std::array<bool, kMouseButtonCount> buttons_released_{};

    // One slot per tracked contact. `active` means the slot is occupied by a
    // known id (it stays occupied for the frame in which the contact ends, so
    // the end edge can be read, and is freed at the next begin_frame()); `down`
    // means the contact is currently touching.
    struct TouchPoint {
        u32 id{};
        bool active{};
        bool down{};
        bool began{};
        bool ended{};
        Vec2 position{};
        Vec2 delta{};
    };
    std::array<TouchPoint, kMaxTouchPoints> touches_{};

    Vec2 mouse_position_{};
    Vec2 mouse_delta_{};
    Vec2 scroll_delta_{};
};

} // namespace fe
