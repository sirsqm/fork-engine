// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include <doctest/doctest.h>

#include "fe/platform/InputState.h"

using namespace fe;

TEST_CASE("key down state persists until released") {
    InputState input;
    input.on_key(Key::W, true);

    CHECK(input.is_key_down(Key::W));
    CHECK(input.was_key_pressed(Key::W));
    CHECK_FALSE(input.was_key_released(Key::W));

    input.begin_frame();
    // The key is still held, but the press edge belongs to the previous frame.
    CHECK(input.is_key_down(Key::W));
    CHECK_FALSE(input.was_key_pressed(Key::W));
}

TEST_CASE("release produces a release edge for exactly one frame") {
    InputState input;
    input.on_key(Key::A, true);
    input.begin_frame();

    input.on_key(Key::A, false);
    CHECK(input.was_key_released(Key::A));
    CHECK_FALSE(input.is_key_down(Key::A));

    input.begin_frame();
    CHECK_FALSE(input.was_key_released(Key::A));
}

TEST_CASE("mouse buttons report down and press edges") {
    InputState input;
    input.on_mouse_button(MouseButton::Left, true);

    CHECK(input.is_mouse_down(MouseButton::Left));
    CHECK(input.was_mouse_pressed(MouseButton::Left));

    input.begin_frame();
    input.on_mouse_button(MouseButton::Left, false);
    CHECK(input.was_mouse_released(MouseButton::Left));
}

TEST_CASE("mouse movement accumulates a per-frame delta") {
    InputState input;
    input.on_mouse_move(10.0f, 20.0f);
    CHECK(input.mouse_position() == Vec2{10.0f, 20.0f});
    CHECK(input.mouse_delta() == Vec2{10.0f, 20.0f});

    input.begin_frame();
    CHECK(input.mouse_delta() == Vec2{0.0f, 0.0f});

    input.on_mouse_move(15.0f, 25.0f);
    CHECK(input.mouse_delta() == Vec2{5.0f, 5.0f});
}

TEST_CASE("scroll delta is reported per frame and then cleared") {
    InputState input;
    input.on_scroll(0.0f, 1.5f);
    CHECK(input.scroll_delta() == Vec2{0.0f, 1.5f});

    input.begin_frame();
    CHECK(input.scroll_delta() == Vec2{0.0f, 0.0f});
}

TEST_CASE("release_all clears held keys but preserves edge history") {
    InputState input;
    input.on_key(Key::D, true);
    CHECK(input.was_key_pressed(Key::D));

    input.release_all();
    CHECK_FALSE(input.is_key_down(Key::D));
    // The press edge from this frame is still observable, which is what lets
    // gameplay code notice a key that was pressed and the window then lost
    // focus.
    CHECK(input.was_key_pressed(Key::D));
}

TEST_CASE("a touch reports begin, update, and end keyed by id") {
    InputState input;
    input.on_touch(TouchPhase::Began, 7, 10.0f, 20.0f);
    CHECK(input.is_touch_down(7));
    CHECK(input.was_touch_began(7));
    CHECK_FALSE(input.was_touch_ended(7));
    CHECK(input.touch_position(7) == Vec2{10.0f, 20.0f});

    input.begin_frame();
    // The contact is still down, but the begin edge belongs to the previous
    // frame.
    CHECK(input.is_touch_down(7));
    CHECK_FALSE(input.was_touch_began(7));
    CHECK(input.touch_delta(7) == Vec2{0.0f, 0.0f});

    input.on_touch(TouchPhase::Moved, 7, 15.0f, 25.0f);
    CHECK(input.is_touch_down(7));
    CHECK(input.touch_position(7) == Vec2{15.0f, 25.0f});
    CHECK(input.touch_delta(7) == Vec2{5.0f, 5.0f});

    input.on_touch(TouchPhase::Ended, 7, 15.0f, 25.0f);
    CHECK_FALSE(input.is_touch_down(7));
    CHECK(input.was_touch_ended(7));

    input.begin_frame();
    // The end edge is gone and the id is no longer tracked.
    CHECK_FALSE(input.was_touch_ended(7));
    CHECK_FALSE(input.is_touch_down(7));
    CHECK(input.touch_position(7) == Vec2{0.0f, 0.0f});
}

TEST_CASE("touch ids are tracked independently") {
    InputState input;
    input.on_touch(TouchPhase::Began, 1, 0.0f, 0.0f);
    input.on_touch(TouchPhase::Began, 2, 100.0f, 100.0f);

    CHECK(input.is_touch_down(1));
    CHECK(input.is_touch_down(2));

    input.on_touch(TouchPhase::Ended, 1, 0.0f, 0.0f);
    CHECK_FALSE(input.is_touch_down(1));
    CHECK(input.was_touch_ended(1));
    // Ending one contact leaves the other untouched.
    CHECK(input.is_touch_down(2));
    CHECK_FALSE(input.was_touch_ended(2));
    CHECK(input.touch_position(2) == Vec2{100.0f, 100.0f});
}

TEST_CASE("begin_frame clears touch edge state") {
    InputState input;
    input.on_touch(TouchPhase::Began, 3, 1.0f, 2.0f);
    CHECK(input.was_touch_began(3));

    input.begin_frame();
    CHECK_FALSE(input.was_touch_began(3));
    CHECK_FALSE(input.was_touch_ended(3));
    CHECK(input.touch_delta(3) == Vec2{0.0f, 0.0f});
}

TEST_CASE("an unknown touch id reports nothing") {
    InputState input;
    // A Moved/Ended for an id that never began is ignored rather than starting
    // a phantom contact.
    input.on_touch(TouchPhase::Moved, 9, 5.0f, 5.0f);
    input.on_touch(TouchPhase::Ended, 9, 5.0f, 5.0f);
    CHECK_FALSE(input.is_touch_down(9));
    CHECK_FALSE(input.was_touch_ended(9));
    CHECK(input.touch_position(9) == Vec2{0.0f, 0.0f});
}
