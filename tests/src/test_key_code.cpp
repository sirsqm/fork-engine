// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
//
// Tests for the DOM physical-key translation (docs/SHIPPING_QUEUE.md,
// `touch-input-actions`).
//
// The mapping is a pure function of a `code` string, so these need no browser:
// they pin the spellings the web backend depends on and, more importantly, prove
// the two directions cannot drift.

#include <doctest/doctest.h>

#include "fe/platform/InputState.h"
#include "fe/platform/KeyCode.h"

#include <string_view>

using namespace fe;

TEST_CASE("UI Events codes map to the engine's keys") {
    CHECK(key_from_code("KeyA") == Key::A);
    CHECK(key_from_code("KeyW") == Key::W);
    CHECK(key_from_code("Digit0") == Key::Num0);
    CHECK(key_from_code("Digit9") == Key::Num9);
    CHECK(key_from_code("ArrowLeft") == Key::Left);
    CHECK(key_from_code("ArrowRight") == Key::Right);
    CHECK(key_from_code("ArrowUp") == Key::Up);
    CHECK(key_from_code("ArrowDown") == Key::Down);
    CHECK(key_from_code("Space") == Key::Space);
    CHECK(key_from_code("Enter") == Key::Enter);
    CHECK(key_from_code("Escape") == Key::Escape);
    CHECK(key_from_code("F12") == Key::F12);
    CHECK(key_from_code("ShiftLeft") == Key::LeftShift);
    CHECK(key_from_code("ControlRight") == Key::RightControl);
}

TEST_CASE("the awkwardly named keys are spelled the UI Events way") {
    // These are the spellings that differ from the obvious guess, so they are
    // asserted explicitly: a binding file or a test that guessed "LeftBracket"
    // or "Quote" would silently bind nothing.
    CHECK(key_from_code("BracketLeft") == Key::LeftBracket);
    CHECK(key_from_code("BracketRight") == Key::RightBracket);
    CHECK(key_from_code("Quote") == Key::Apostrophe);
    CHECK(key_from_code("Backquote") == Key::Grave);
    CHECK(key_from_code("Backslash") == Key::Backslash);
    CHECK(key_from_code("Semicolon") == Key::Semicolon);
}

TEST_CASE("an unmapped or empty code is Unknown rather than a wrong key") {
    CHECK(key_from_code("") == Key::Unknown);
    CHECK(key_from_code("MediaPlayPause") == Key::Unknown);
    CHECK(key_from_code("IntlBackslash") == Key::Unknown);
    // The layout-dependent `key` value is not a `code`: "a" is not "KeyA", and
    // treating it as one would be exactly the layout bug the physical table
    // exists to avoid.
    CHECK(key_from_code("a") == Key::Unknown);
}

TEST_CASE("code_from_key inverts key_from_code for every key") {
    // Round-tripping every key is what keeps the table from drifting: a row with
    // a typo fails here, and a key added to the enum with no row fails to
    // compile.
    for (u16 value = 1; value < static_cast<u16>(Key::Count); ++value) {
        const auto key = static_cast<Key>(value);
        const std::string_view code = code_from_key(key);
        REQUIRE_FALSE(code.empty());
        CHECK(key_from_code(code) == key);
    }
}

TEST_CASE("code_from_key rejects the sentinel keys") {
    CHECK(code_from_key(Key::Unknown).empty());
    CHECK(code_from_key(Key::Count).empty());
}
