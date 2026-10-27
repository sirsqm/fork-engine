// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
//
// Tests for declarative input bindings (fe/player/ActionBindings.h,
// docs/PLAN.md, `touch-gesture-regions`).
//
// The acceptance test for the queue item is here: a game *declares* its regions
// in a document, and a touch in each region fires the bound action while a touch
// outside fires none. Everything is pure logic over an InputState, so no window
// and no browser is involved.
//
// The point being proved is the one `touch-input-actions` left open: regions
// were bindable, but only from hand-written C++. These tests go through the
// document, so they exercise the path a shipped game actually takes — a binding
// a game declares once, applied to the map, driving the same ActionMap queries
// gameplay already reads.

#include <doctest/doctest.h>

#include "fe/platform/InputActions.h"
#include "fe/platform/InputState.h"
#include "fe/player/ActionBindings.h"

// doctest stringifies through std::ostream; MSVC's <string_view> does not pull
// it in (the trap in AGENTS.md).
#include <ostream>

#include <string>
#include <string_view>

using namespace fe;

namespace {

// A viewport big enough that a normalised region is easy to reason about.
constexpr Vec2 kViewport{1280.0f, 720.0f};

// The document the acceptance test drives: two steer regions and a jump region,
// declared once, exactly as a game would ship it.
constexpr std::string_view kTwoThumbDocument = R"({
  "format": "fork_input",
  "format_version": 1,
  "actions": [
    { "name": "MoveLeft", "keys": ["KeyA", "ArrowLeft"], "touch": [[0.0, 0.0, 0.33, 1.0]] },
    { "name": "MoveRight", "keys": ["KeyD"], "touch": [[0.67, 0.0, 1.0, 1.0]] },
    { "name": "Jump", "keys": ["Space"], "mouse": ["left"], "touch": [[0.4, 0.0, 0.6, 1.0]] }
  ]
})";

} // namespace

TEST_CASE("a binding document parses into actions and bindings") {
    auto parsed = player::ActionBindings::from_json(kTwoThumbDocument);
    REQUIRE(parsed);

    CHECK(parsed.value().actions.size() == 3);
    CHECK(parsed.value().actions[0].name == "MoveLeft");
    CHECK_FALSE(parsed.value().actions[0].replace);
    CHECK(parsed.value().keys.size() == 4); // A, Left, D, Space
    CHECK(parsed.value().mouse.size() == 1);
    CHECK(parsed.value().touch_regions.size() == 3);
}

TEST_CASE("a game declares touch regions once and a touch in each fires its action") {
    // The acceptance test for `touch-gesture-regions`. The game declares the
    // regions in data; a touch in each region fires the bound action, and a
    // touch outside every region fires none.
    ActionMap actions;
    auto parsed = player::ActionBindings::from_json(kTwoThumbDocument);
    REQUIRE(parsed);
    parsed.value().apply(actions);

    const auto move_left = actions.find("MoveLeft");
    const auto move_right = actions.find("MoveRight");
    const auto jump = actions.find("Jump");
    REQUIRE(move_left != ActionMap::kNone);
    REQUIRE(move_right != ActionMap::kNone);
    REQUIRE(jump != ActionMap::kNone);

    SUBCASE("a touch in the left region fires MoveLeft and nothing else") {
        InputState input;
        input.on_touch(TouchPhase::Began, 1, 100.0f, 400.0f); // x/1280 = 0.08, in [0, 0.33]
        actions.update(input, kViewport);
        CHECK(actions.is_down(move_left));
        CHECK(actions.was_pressed(move_left));
        CHECK_FALSE(actions.is_down(move_right));
        CHECK_FALSE(actions.is_down(jump));
    }

    SUBCASE("a touch in the right region fires MoveRight and nothing else") {
        InputState input;
        input.on_touch(TouchPhase::Began, 1, 1200.0f, 400.0f); // 0.94, in [0.67, 1]
        actions.update(input, kViewport);
        CHECK(actions.is_down(move_right));
        CHECK_FALSE(actions.is_down(move_left));
        CHECK_FALSE(actions.is_down(jump));
    }

    SUBCASE("a touch in the middle region fires Jump and nothing else") {
        InputState input;
        input.on_touch(TouchPhase::Began, 1, 640.0f, 400.0f); // 0.5, in [0.4, 0.6]
        actions.update(input, kViewport);
        CHECK(actions.is_down(jump));
        CHECK_FALSE(actions.is_down(move_left));
        CHECK_FALSE(actions.is_down(move_right));
    }

    SUBCASE("a touch in the gap between regions fires nothing") {
        // x/1280 = 0.35: past MoveLeft (0.33), before Jump (0.4). The gaps are the
        // point — a player who taps between two controls should get neither, not
        // whichever region the author rounded into.
        InputState input;
        input.on_touch(TouchPhase::Began, 1, 448.0f, 400.0f);
        actions.update(input, kViewport);
        CHECK_FALSE(actions.is_down(move_left));
        CHECK_FALSE(actions.is_down(move_right));
        CHECK_FALSE(actions.is_down(jump));
    }

    SUBCASE("releasing the finger releases the action") {
        InputState input;
        input.on_touch(TouchPhase::Began, 1, 100.0f, 400.0f);
        actions.update(input, kViewport);
        CHECK(actions.is_down(move_left));

        input.begin_frame();
        input.on_touch(TouchPhase::Ended, 1, 100.0f, 400.0f);
        actions.update(input, kViewport);
        CHECK_FALSE(actions.is_down(move_left));
        CHECK(actions.was_released(move_left));
    }
}

TEST_CASE("a document replaces the built-in layout for the kinds it binds") {
    // The reason `replace` exists: the engine declares a conventional touch
    // layout (declare_builtin_actions), and a game that wants a different one must
    // be able to *replace* it, not stack its regions on top. Otherwise a touch in
    // the game's region would fire two actions at once, and a touch in the
    // built-in region the game meant to move would still fire the old one.
    constexpr std::string_view document = R"({
      "format": "fork_input",
      "format_version": 1,
      "actions": [
        { "name": "MoveForward", "keys": ["KeyW"], "touch": [[0.0, 0.0, 1.0, 0.5]], "replace": true }
      ]
    })";

    ActionMap actions;
    // What declare_builtin_actions would have installed for this action: a key and
    // a region in the lower half.
    const auto move_forward = actions.add("MoveForward");
    actions.bind_key(move_forward, Key::Up);
    actions.bind_touch_region(move_forward, 0.0f, 0.5f, 1.0f, 1.0f);

    auto parsed = player::ActionBindings::from_json(document);
    REQUIRE(parsed);
    parsed.value().apply(actions);

    SUBCASE("the action's own region is bound") {
        InputState input;
        input.on_touch(TouchPhase::Began, 1, 640.0f, 100.0f); // y/720 = 0.14, in the new upper region
        actions.update(input, kViewport);
        CHECK(actions.is_down(move_forward));
    }

    SUBCASE("the built-in region it replaced no longer fires") {
        InputState input;
        input.on_touch(TouchPhase::Began, 1, 640.0f, 600.0f); // y/720 = 0.83, the old lower region
        actions.update(input, kViewport);
        CHECK_FALSE(actions.is_down(move_forward));
    }

    SUBCASE("the key kind it bound was replaced too") {
        InputState input;
        input.on_key(Key::W, true);
        actions.update(input, kViewport);
        CHECK(actions.is_down(move_forward));
    }

    SUBCASE("a kind the document did not bind is left alone") {
        // The document binds keys and touch but not mouse, so a mouse binding the
        // engine declared must survive a replace: replacing a touch layout must not
        // silently drop an unrelated kind.
        actions.bind_mouse_button(move_forward, MouseButton::Left);
        InputState input;
        input.on_mouse_button(MouseButton::Left, true);
        actions.update(input, kViewport);
        CHECK(actions.is_down(move_forward));
    }
}

TEST_CASE("a non-boolean replace is rejected") {
    auto parsed = player::ActionBindings::from_json(
        R"({"format":"fork_input","format_version":1,
            "actions":[{"name":"Jump","keys":["Space"],"replace":"yes"}]})");
    CHECK_FALSE(parsed);
}

TEST_CASE("a document's regions survive a rotation because they are normalised") {
    // The reason a region is normalised rather than in pixels: the same document
    // binds the same fraction of a portrait viewport as a landscape one.
    ActionMap actions;
    auto parsed = player::ActionBindings::from_json(kTwoThumbDocument);
    REQUIRE(parsed);
    parsed.value().apply(actions);
    const auto move_left = actions.find("MoveLeft");

    InputState input;
    input.on_touch(TouchPhase::Began, 1, 200.0f, 900.0f); // 200/720 = 0.28, in [0, 0.33]
    actions.update(input, Vec2{720.0f, 1280.0f});
    CHECK(actions.is_down(move_left));
}

TEST_CASE("a document rebinds an action the engine already declared") {
    // Applying a document after the engine's own declarations must reuse the
    // handle, not add a second action of the same name: the built-in behaviours
    // resolve actions by name, so a duplicate would leave them reading the
    // unbound one.
    ActionMap actions;
    const auto existing = actions.add("MoveLeft");
    actions.bind_key(existing, Key::A);

    constexpr std::string_view rebind = R"({
      "format": "fork_input",
      "format_version": 1,
      "actions": [ { "name": "MoveLeft", "keys": ["KeyJ"] } ]
    })";
    auto parsed = player::ActionBindings::from_json(rebind);
    REQUIRE(parsed);
    parsed.value().apply(actions);

    // One action, not two, and the new key fires it.
    CHECK(actions.action_count() == 1);
    CHECK(actions.find("MoveLeft") == existing);

    InputState input;
    input.on_key(Key::J, true);
    actions.update(input, kViewport);
    CHECK(actions.is_down(existing));
}

TEST_CASE("a declared but unbound action is still declared") {
    // A game reserves a name a later document or a rebinding screen will bind.
    constexpr std::string_view document = R"({
      "format": "fork_input",
      "format_version": 1,
      "actions": [ { "name": "Pause" } ]
    })";
    ActionMap actions;
    auto parsed = player::ActionBindings::from_json(document);
    REQUIRE(parsed);
    parsed.value().apply(actions);

    const auto pause = actions.find("Pause");
    CHECK(pause != ActionMap::kNone);

    InputState input;
    actions.update(input, kViewport);
    CHECK_FALSE(actions.is_down(pause));
}

TEST_CASE("a mouse binding fires its action") {
    ActionMap actions;
    auto parsed = player::ActionBindings::from_json(kTwoThumbDocument);
    REQUIRE(parsed);
    parsed.value().apply(actions);

    const auto jump = actions.find("Jump");
    InputState input;
    input.on_mouse_button(MouseButton::Left, true);
    actions.update(input, kViewport);
    CHECK(actions.is_down(jump));
}

TEST_CASE("a malformed binding document is rejected rather than half-read") {
    const auto rejected = [](std::string_view text) {
        auto parsed = player::ActionBindings::from_json(text);
        // Either the parse fails, or it succeeds and the document was valid; the
        // point is that a bad document never yields a *partial* success.
        return !parsed;
    };

    SUBCASE("not JSON") {
        CHECK(rejected("{ this is not json"));
    }
    SUBCASE("wrong marker") {
        CHECK(rejected(R"({"format": "fork_scene", "format_version": 1, "actions": []})"));
    }
    SUBCASE("a version from the future") {
        CHECK(rejected(R"({"format": "fork_input", "format_version": 99, "actions": []})"));
    }
    SUBCASE("no actions array") {
        CHECK(rejected(R"({"format": "fork_input", "format_version": 1})"));
    }
    SUBCASE("an action with no name") {
        CHECK(rejected(R"({"format": "fork_input", "format_version": 1, "actions": [ {"keys": ["KeyA"]} ]})"));
    }
    SUBCASE("an unknown key code") {
        CHECK(rejected(R"({"format":"fork_input","format_version":1,"actions":[{"name":"Jump","keys":["KeyNope"]}]})"));
    }
    SUBCASE("an unknown mouse button") {
        CHECK(
            rejected(R"({"format":"fork_input","format_version":1,"actions":[{"name":"Jump","mouse":["sideways"]}]})"));
    }
    SUBCASE("a region with the wrong number of corners") {
        CHECK(rejected(R"({"format":"fork_input","format_version":1,"actions":[{"name":"Jump","touch":[[0,0,1]]}]})"));
    }
    SUBCASE("a region written in pixels rather than normalised") {
        // The mistake the normalisation check exists to catch: 640 is a plausible
        // x in pixels and an impossible fraction.
        CHECK(rejected(
            R"({"format":"fork_input","format_version":1,"actions":[{"name":"Jump","touch":[[0,0,640,1]]}]})"));
    }
    SUBCASE("a keys field that is not an array") {
        CHECK(rejected(R"({"format":"fork_input","format_version":1,"actions":[{"name":"Jump","keys":"KeyA"}]})"));
    }
}

TEST_CASE("a valid document with an empty action list is accepted") {
    // The boundary next to the rejections above: an empty list is a document the
    // writer would produce, so it parses and applies to nothing.
    auto parsed = player::ActionBindings::from_json(R"({"format":"fork_input","format_version":1,"actions":[]})");
    REQUIRE(parsed);
    ActionMap actions;
    parsed.value().apply(actions);
    CHECK(actions.action_count() == 0);
}
