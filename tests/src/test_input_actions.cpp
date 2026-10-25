// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
//
// Tests for the gameplay action layer (docs/PLAN.md, `touch-input-actions`).
//
// The acceptance test for the queue item is here: a touch sequence drives the
// same action a keyboard press produces. Everything is pure logic over an
// InputState, so no window and no browser is involved — the same reason
// InputState's own tests can call its on_*() methods directly.

#include <doctest/doctest.h>

#include "fe/platform/InputActions.h"
#include "fe/platform/InputState.h"

// doctest stringifies a std::string_view through std::ostream, and MSVC's
// <string_view> does not pull <ostream> in. Without this the CHECK on
// action_name() below fails to compile on Windows only (the trap in AGENTS.md).
#include <ostream>

using namespace fe;

namespace {

// A viewport big enough that a normalised region is easy to reason about.
constexpr Vec2 kViewport{1280.0f, 720.0f};

} // namespace

TEST_CASE("a key binding drives its action") {
    ActionMap actions;
    const auto move_left = actions.add("MoveLeft");
    actions.bind_key(move_left, Key::A);

    InputState input;
    input.on_key(Key::A, true);
    actions.update(input, kViewport);

    CHECK(actions.is_down(move_left));
    CHECK(actions.was_pressed(move_left));

    input.begin_frame();
    actions.update(input, kViewport);
    CHECK(actions.is_down(move_left));
    CHECK_FALSE(actions.was_pressed(move_left));

    input.on_key(Key::A, false);
    actions.update(input, kViewport);
    CHECK_FALSE(actions.is_down(move_left));
    CHECK(actions.was_released(move_left));
}

TEST_CASE("several keys can drive one action") {
    ActionMap actions;
    const auto move_left = actions.add("MoveLeft");
    actions.bind_key(move_left, Key::Left);
    actions.bind_key(move_left, Key::A);

    // Either key alone is enough; that is the point of binding two.
    InputState input;
    input.on_key(Key::Left, true);
    actions.update(input, kViewport);
    CHECK(actions.is_down(move_left));

    // Releasing one while the other is still held keeps the action down, which
    // is what stops a player who holds both from stuttering.
    input.begin_frame();
    input.on_key(Key::Left, false);
    input.on_key(Key::A, true);
    actions.update(input, kViewport);
    CHECK(actions.is_down(move_left));
}

TEST_CASE("a touch region drives the same action a key does") {
    // The acceptance test for `touch-input-actions`: the gameplay code asks the
    // same question and gets the same answer whether a key or a finger produced
    // it.
    ActionMap actions;
    const auto move_left = actions.add("MoveLeft");
    actions.bind_key(move_left, Key::Left);
    // The left third of the screen, full height.
    actions.bind_touch_region(move_left, 0.0f, 0.0f, 1.0f / 3.0f, 1.0f);

    SUBCASE("a key press produces the action") {
        InputState input;
        input.on_key(Key::Left, true);
        actions.update(input, kViewport);
        CHECK(actions.is_down(move_left));
    }

    SUBCASE("a touch inside the region produces the same action") {
        InputState input;
        input.on_touch(TouchPhase::Began, 1, 100.0f, 400.0f);
        actions.update(input, kViewport);
        CHECK(actions.is_down(move_left));
        CHECK(actions.was_pressed(move_left));

        // Holding the finger keeps it down across frames, exactly as holding the
        // key does.
        input.begin_frame();
        actions.update(input, kViewport);
        CHECK(actions.is_down(move_left));
        CHECK_FALSE(actions.was_pressed(move_left));

        input.on_touch(TouchPhase::Ended, 1, 100.0f, 400.0f);
        actions.update(input, kViewport);
        CHECK_FALSE(actions.is_down(move_left));
        CHECK(actions.was_released(move_left));
    }

    SUBCASE("a touch outside the region produces nothing") {
        InputState input;
        input.on_touch(TouchPhase::Began, 1, 1100.0f, 400.0f);
        actions.update(input, kViewport);
        CHECK_FALSE(actions.is_down(move_left));
    }
}

TEST_CASE("touch positions are normalised against the viewport") {
    ActionMap actions;
    const auto jump = actions.add("Jump");
    // The bottom-right quadrant.
    actions.bind_touch_region(jump, 0.5f, 0.5f, 1.0f, 1.0f);

    // A contact at the same *fraction* of two different viewports must trigger
    // the action on both: that is what makes a binding survive a resize and a
    // change of phone orientation.
    SUBCASE("on a landscape viewport") {
        InputState input;
        input.on_touch(TouchPhase::Began, 1, 1000.0f, 600.0f);
        actions.update(input, Vec2{1280.0f, 720.0f});
        CHECK(actions.is_down(jump));
    }

    SUBCASE("on a portrait viewport") {
        InputState input;
        input.on_touch(TouchPhase::Began, 1, 400.0f, 1000.0f);
        actions.update(input, Vec2{720.0f, 1280.0f});
        CHECK(actions.is_down(jump));
    }

    SUBCASE("outside on the same viewport") {
        InputState input;
        input.on_touch(TouchPhase::Began, 1, 200.0f, 200.0f);
        actions.update(input, Vec2{1280.0f, 720.0f});
        CHECK_FALSE(actions.is_down(jump));
    }
}

TEST_CASE("two fingers hold two actions at once") {
    // The case a keyboard cannot express and a phone must: steering and acting
    // simultaneously.
    ActionMap actions;
    const auto move_left = actions.add("MoveLeft");
    const auto jump = actions.add("Jump");
    actions.bind_touch_region(move_left, 0.0f, 0.0f, 0.25f, 1.0f);
    actions.bind_touch_region(jump, 0.75f, 0.0f, 1.0f, 1.0f);

    InputState input;
    input.on_touch(TouchPhase::Began, 1, 100.0f, 400.0f);
    input.on_touch(TouchPhase::Began, 2, 1200.0f, 400.0f);
    actions.update(input, kViewport);

    CHECK(actions.is_down(move_left));
    CHECK(actions.is_down(jump));

    // Lifting one finger leaves the other action down.
    input.begin_frame();
    input.on_touch(TouchPhase::Ended, 2, 1200.0f, 400.0f);
    actions.update(input, kViewport);
    CHECK(actions.is_down(move_left));
    CHECK_FALSE(actions.is_down(jump));
}

TEST_CASE("a degenerate viewport does not trigger touch regions") {
    // A canvas that has not been sized yet must not divide by zero or report a
    // spurious action.
    ActionMap actions;
    const auto jump = actions.add("Jump");
    actions.bind_touch_region(jump, 0.0f, 0.0f, 1.0f, 1.0f);

    InputState input;
    input.on_touch(TouchPhase::Began, 1, 0.0f, 0.0f);
    actions.update(input, Vec2{0.0f, 0.0f});
    CHECK_FALSE(actions.is_down(jump));
}

TEST_CASE("an unknown action handle fails safe") {
    ActionMap actions;
    const auto known = actions.add("Known");
    actions.bind_key(known, Key::Space);

    // kNone and an out-of-range handle answer false rather than aliasing the
    // first action, so a default-constructed handle cannot silently mean "Jump".
    CHECK_FALSE(actions.is_down(ActionMap::kNone));
    CHECK_FALSE(actions.was_pressed(ActionMap::kNone));
    CHECK_FALSE(actions.was_released(ActionMap::kNone));
    CHECK_FALSE(actions.is_down(999));
    CHECK(actions.action_name(999).empty());

    InputState input;
    input.on_key(Key::Space, true);
    actions.update(input, kViewport);
    CHECK(actions.is_down(known));
}

TEST_CASE("actions can be found by name") {
    ActionMap actions;
    const auto jump = actions.add("Jump");
    actions.add("Fire");

    CHECK(actions.find("Jump") == jump);
    CHECK(actions.find("Fire") != jump);
    CHECK(actions.find("Missing") == ActionMap::kNone);
    CHECK(actions.action_name(jump) == "Jump");
    CHECK(actions.action_count() == 2);
}

TEST_CASE("update is idempotent within a frame") {
    // The map recomputes from InputState rather than accumulating, so calling
    // update() twice in one frame must not invent an edge.
    ActionMap actions;
    const auto move = actions.add("Move");
    actions.bind_key(move, Key::Right);

    InputState input;
    input.on_key(Key::Right, true);
    actions.update(input, kViewport);
    CHECK(actions.was_pressed(move));

    actions.update(input, kViewport);
    CHECK(actions.is_down(move));
    CHECK_FALSE(actions.was_pressed(move));
}

TEST_CASE("a mouse button can drive an action") {
    ActionMap actions;
    const auto fire = actions.add("Fire");
    actions.bind_mouse_button(fire, MouseButton::Left);

    InputState input;
    input.on_mouse_button(MouseButton::Left, true);
    actions.update(input, kViewport);
    CHECK(actions.is_down(fire));
}
