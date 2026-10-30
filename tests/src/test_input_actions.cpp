// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
//
// Tests for the gameplay action layer (docs/SHIPPING_QUEUE.md, `touch-input-actions`).
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

TEST_CASE("safe-area insets keep a region off the OS-reserved bands") {
    // The acceptance test for `mobile-safe-area`: a region declared as the left
    // third of the usable area stays reachable on a phone whose notch and home
    // indicator reserve a band, and a touch in that band fires nothing.
    ActionMap actions;
    const auto move_left = actions.add("MoveLeft");
    actions.bind_touch_region(move_left, 0.0f, 0.0f, 1.0f / 3.0f, 1.0f);

    // A 5% band on every edge, as the web shell reports from env().
    actions.set_safe_area_insets(0.05f, 0.05f, 0.05f, 0.05f);
    CHECK(actions.safe_area_top() == doctest::Approx(0.05f));
    CHECK(actions.safe_area_left() == doctest::Approx(0.05f));

    SUBCASE("a touch in the top reserved band fires nothing") {
        // y = 0.02 is inside the top band, so it is not in the usable area and
        // no region can match it.
        InputState input;
        input.on_touch(TouchPhase::Began, 1, 0.15f * kViewport.x, 0.02f * kViewport.y);
        actions.update(input, kViewport);
        CHECK_FALSE(actions.is_down(move_left));
    }

    SUBCASE("the usable area's own left third still fires") {
        // x = 0.2 of the window is 0.15 into the 0.9-wide usable area, which is
        // inside the left third of it.
        InputState input;
        input.on_touch(TouchPhase::Began, 1, 0.20f * kViewport.x, 0.5f * kViewport.y);
        actions.update(input, kViewport);
        CHECK(actions.is_down(move_left));
    }

    SUBCASE("all-zero insets are the identity mapping") {
        actions.set_safe_area_insets(0.0f, 0.0f, 0.0f, 0.0f);
        // x = 0.2 is inside the left third of the whole viewport, which is where
        // it was before any inset existed.
        InputState input;
        input.on_touch(TouchPhase::Began, 1, 0.2f * kViewport.x, 0.5f * kViewport.y);
        actions.update(input, kViewport);
        CHECK(actions.is_down(move_left));
    }
}

TEST_CASE("safe-area insets are clamped so the usable area is never empty") {
    // A shell that reported insets summing to the whole viewport must not make
    // the usable area zero-sized, which would divide by zero and could report a
    // spurious action.
    ActionMap actions;
    const auto jump = actions.add("Jump");
    actions.bind_touch_region(jump, 0.0f, 0.0f, 1.0f, 1.0f);

    // Nonsense that sums past the viewport on both axes, plus a negative.
    actions.set_safe_area_insets(2.0f, 2.0f, 2.0f, -1.0f);
    CHECK(actions.safe_area_top() >= 0.0f);
    CHECK(actions.safe_area_top() <= 1.0f);
    CHECK(actions.safe_area_left() >= 0.0f);
    CHECK(actions.safe_area_top() + actions.safe_area_bottom() <= 1.0f);
    CHECK(actions.safe_area_left() + actions.safe_area_right() <= 1.0f);

    InputState input;
    input.on_touch(TouchPhase::Began, 1, 0.5f * kViewport.x, 0.5f * kViewport.y);
    actions.update(input, kViewport);
    // The middle of the screen is inside a full-viewport region whatever the
    // clamping did.
    CHECK(actions.is_down(jump));
}

TEST_CASE("safe-area insets offset a declared region rather than only shrinking it") {
    // The acceptance test's second half: a region must be *offset* by the insets,
    // not merely have touches in the band rejected. The same declared region is
    // checked at the same screen point under two different inset reports, and the
    // action must flip: the point that fires with no inset must stop firing once
    // the band is reserved, which is the offset and cannot happen if the insets
    // were ignored or only used to reject touches.
    ActionMap actions;
    const auto move_left = actions.add("MoveLeft");
    // A region covering the left half of the usable area.
    actions.bind_touch_region(move_left, 0.0f, 0.0f, 0.5f, 1.0f);

    // x = 0.2 is inside the left half of the whole viewport with no inset.
    InputState before;
    before.on_touch(TouchPhase::Began, 1, 0.2f * kViewport.x, 0.5f * kViewport.y);
    actions.update(before, kViewport);
    CHECK(actions.is_down(move_left));

    // A 20% left band moves the usable area's left edge to 0.2, so x = 0.2 is now
    // the usable area's own left edge and still inside the left half of it...
    actions.set_safe_area_insets(0.0f, 0.0f, 0.0f, 0.2f);
    InputState at_edge;
    at_edge.on_touch(TouchPhase::Began, 1, 0.2f * kViewport.x, 0.5f * kViewport.y);
    actions.update(at_edge, kViewport);
    CHECK(actions.is_down(move_left));

    // ...while the same point that used to fire for a wider region now does not,
    // because the region's left half is measured from 0.2: a point at 0.2 is the
    // edge and a point at 0.3 is 0.125 into the usable area, still inside the
    // half. The point that proves the offset is one just inside the old region
    // but now past its remapped right edge.
    ActionMap narrow;
    const auto narrow_left = narrow.add("MoveLeft");
    narrow.bind_touch_region(narrow_left, 0.0f, 0.0f, 0.25f, 1.0f);
    // With no inset, x = 0.3 is outside the left quarter.
    InputState outside_before;
    outside_before.on_touch(TouchPhase::Began, 1, 0.3f * kViewport.x, 0.5f * kViewport.y);
    narrow.update(outside_before, kViewport);
    CHECK_FALSE(narrow.is_down(narrow_left));
    // With the 20% band, the quarter is now 0.2..0.4 of the viewport, so x = 0.3
    // is inside it: the region moved right by exactly the inset, which is the
    // offset the item requires.
    narrow.set_safe_area_insets(0.0f, 0.0f, 0.0f, 0.2f);
    InputState inside_after;
    inside_after.on_touch(TouchPhase::Began, 1, 0.3f * kViewport.x, 0.5f * kViewport.y);
    narrow.update(inside_after, kViewport);
    CHECK(narrow.is_down(narrow_left));
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
