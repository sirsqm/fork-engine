// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/platform/InputState.h"

#include "fe/core/Assert.h"

namespace fe {

void InputState::begin_frame() noexcept {
    keys_pressed_.fill(false);
    keys_released_.fill(false);
    buttons_pressed_.fill(false);
    buttons_released_.fill(false);
    mouse_delta_ = Vec2{};
    scroll_delta_ = Vec2{};
}

void InputState::on_key(Key key, bool down) noexcept {
    FE_CHECK(key != Key::Unknown && key != Key::Count);
    const usize index = index_of(key);
    if (keys_down_[index] == down) {
        return;
    }
    keys_down_[index] = down;
    if (down) {
        keys_pressed_[index] = true;
    } else {
        keys_released_[index] = true;
    }
}

void InputState::on_mouse_button(MouseButton button, bool down) noexcept {
    FE_CHECK(button != MouseButton::Count);
    const usize index = index_of(button);
    if (buttons_down_[index] == down) {
        return;
    }
    buttons_down_[index] = down;
    if (down) {
        buttons_pressed_[index] = true;
    } else {
        buttons_released_[index] = true;
    }
}

void InputState::on_mouse_move(f32 x, f32 y) noexcept {
    const Vec2 previous = mouse_position_;
    mouse_position_ = Vec2{x, y};
    // The first motion event after a frame boundary contributes the delta from
    // the last known position; that is the value gameplay code expects.
    mouse_delta_ = mouse_position_ - previous;
}

void InputState::on_scroll(f32 dx, f32 dy) noexcept {
    scroll_delta_ = Vec2{scroll_delta_.x + dx, scroll_delta_.y + dy};
}

bool InputState::is_key_down(Key key) const noexcept { return keys_down_[index_of(key)]; }

bool InputState::was_key_pressed(Key key) const noexcept { return keys_pressed_[index_of(key)]; }

bool InputState::was_key_released(Key key) const noexcept { return keys_released_[index_of(key)]; }

bool InputState::is_mouse_down(MouseButton button) const noexcept { return buttons_down_[index_of(button)]; }

bool InputState::was_mouse_pressed(MouseButton button) const noexcept { return buttons_pressed_[index_of(button)]; }

bool InputState::was_mouse_released(MouseButton button) const noexcept { return buttons_released_[index_of(button)]; }

void InputState::release_all() noexcept {
    keys_down_.fill(false);
    buttons_down_.fill(false);
}

} // namespace fe
