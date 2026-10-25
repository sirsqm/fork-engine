// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/platform/InputState.h"

#include <string_view>

namespace fe {

// Translates a UI Events keyboard `code` into the engine's Key enum.
//
// `code` is the web's *physical* key naming ("KeyA", "ArrowLeft", "Digit1") as
// opposed to `key`, which carries the produced character and therefore changes
// with the keyboard layout and the modifiers held. Physical naming is what a
// game wants — WASD stays in the same place on an AZERTY keyboard — and it is
// the same choice the desktop backend makes by mapping GLFW's physical key
// codes. Putting the table here, in a pure function, keeps the web backend thin
// and lets the mapping be tested with no browser at all.
//
// The naming is also the obvious vocabulary for a key-rebinding file, so this is
// the engine's stable textual key name rather than a web implementation detail.
//
// Returns Key::Unknown for a code the engine does not model (a media key, an
// international key, an empty string).
[[nodiscard]] Key key_from_code(std::string_view code) noexcept;

// The inverse: the UI Events `code` for a key, or an empty view for
// Key::Unknown/Key::Count. Used to write a binding back out and by tests to
// state an expectation in the same vocabulary a rebinding file would use.
[[nodiscard]] std::string_view code_from_key(Key key) noexcept;

} // namespace fe
