// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/platform/KeyCode.h"

#include <array>

namespace fe {
namespace {

// The physical-key table. One row per Key, so the two directions cannot drift:
// code_from_key() reads the same row key_from_code() matches on, and the array
// is indexed by the enum value with a static_assert pinning its size. Adding a
// key to the enum without a row here fails to compile rather than silently
// dropping the key at run time.
struct Entry {
    Key key;
    std::string_view code;
};

// Ordered by the Key enum so a reader can check a key's spelling at a glance.
// The letters and digits are spelled out rather than generated from a formula:
// the UI Events spec defines these names, and an explicit table is what a
// rebinding file and a reviewer both read.
constexpr std::array<Entry, static_cast<usize>(Key::Count) - 1> kTable{{
    {Key::A, "KeyA"},
    {Key::B, "KeyB"},
    {Key::C, "KeyC"},
    {Key::D, "KeyD"},
    {Key::E, "KeyE"},
    {Key::F, "KeyF"},
    {Key::G, "KeyG"},
    {Key::H, "KeyH"},
    {Key::I, "KeyI"},
    {Key::J, "KeyJ"},
    {Key::K, "KeyK"},
    {Key::L, "KeyL"},
    {Key::M, "KeyM"},
    {Key::N, "KeyN"},
    {Key::O, "KeyO"},
    {Key::P, "KeyP"},
    {Key::Q, "KeyQ"},
    {Key::R, "KeyR"},
    {Key::S, "KeyS"},
    {Key::T, "KeyT"},
    {Key::U, "KeyU"},
    {Key::V, "KeyV"},
    {Key::W, "KeyW"},
    {Key::X, "KeyX"},
    {Key::Y, "KeyY"},
    {Key::Z, "KeyZ"},
    {Key::Num0, "Digit0"},
    {Key::Num1, "Digit1"},
    {Key::Num2, "Digit2"},
    {Key::Num3, "Digit3"},
    {Key::Num4, "Digit4"},
    {Key::Num5, "Digit5"},
    {Key::Num6, "Digit6"},
    {Key::Num7, "Digit7"},
    {Key::Num8, "Digit8"},
    {Key::Num9, "Digit9"},
    {Key::Escape, "Escape"},
    {Key::Enter, "Enter"},
    {Key::Tab, "Tab"},
    {Key::Backspace, "Backspace"},
    {Key::Insert, "Insert"},
    {Key::Delete, "Delete"},
    {Key::Right, "ArrowRight"},
    {Key::Left, "ArrowLeft"},
    {Key::Down, "ArrowDown"},
    {Key::Up, "ArrowUp"},
    {Key::PageUp, "PageUp"},
    {Key::PageDown, "PageDown"},
    {Key::Home, "Home"},
    {Key::End, "End"},
    {Key::Space, "Space"},
    {Key::Minus, "Minus"},
    {Key::Equal, "Equal"},
    {Key::LeftBracket, "BracketLeft"},
    {Key::RightBracket, "BracketRight"},
    {Key::Backslash, "Backslash"},
    {Key::Semicolon, "Semicolon"},
    {Key::Apostrophe, "Quote"},
    {Key::Comma, "Comma"},
    {Key::Period, "Period"},
    {Key::Slash, "Slash"},
    {Key::Grave, "Backquote"},
    {Key::F1, "F1"},
    {Key::F2, "F2"},
    {Key::F3, "F3"},
    {Key::F4, "F4"},
    {Key::F5, "F5"},
    {Key::F6, "F6"},
    {Key::F7, "F7"},
    {Key::F8, "F8"},
    {Key::F9, "F9"},
    {Key::F10, "F10"},
    {Key::F11, "F11"},
    {Key::F12, "F12"},
    {Key::LeftShift, "ShiftLeft"},
    {Key::RightShift, "ShiftRight"},
    {Key::LeftControl, "ControlLeft"},
    {Key::RightControl, "ControlRight"},
    {Key::LeftAlt, "AltLeft"},
    {Key::RightAlt, "AltRight"},
}};

// A linear scan is deliberate: the table is under ninety short strings, it is
// consulted once per key event (tens per second at most, never per frame), and
// it keeps the mapping readable and impossible to get out of sync. A hash map
// here would buy nothing measurable and cost a static initialiser.

} // namespace

Key key_from_code(std::string_view code) noexcept {
    if (code.empty()) {
        return Key::Unknown;
    }
    for (const Entry& entry : kTable) {
        if (entry.code == code) {
            return entry.key;
        }
    }
    return Key::Unknown;
}

std::string_view code_from_key(Key key) noexcept {
    if (key == Key::Unknown || key == Key::Count) {
        return {};
    }
    // The table is indexed by enum value, so this is a direct lookup rather than
    // a scan; the static_assert above keeps that index valid.
    return kTable[static_cast<usize>(key) - 1].code;
}

} // namespace fe
