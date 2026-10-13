// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/reflect/Value.h"

#include <string>
#include <string_view>

namespace fe::editor {

// Pure helpers for the property inspector.
//
// None of these touch ImGui. They are the parts of the inspector that are not
// inherently graphical: turning a property's hint text into a numeric range and
// rendering a `reflect::Value` as human-readable text. Keeping them out of
// `InspectorPanel.cpp` means they are covered by the headless editor tests,
// while the panel itself stays a thin "read the value, draw a widget" layer.

// The numeric bounds decoded from a `Hint::Range` property's `hint_text`.
struct RangeHint {
    double minimum = 0.0;
    double maximum = 0.0;
    // False when `hint_text` is empty, malformed, or not "min,max" with
    // min < max. The panel falls back to an unbounded widget in that case, so a
    // typo in a registration degrades to a usable widget rather than a broken
    // one.
    bool valid = false;
};

// Parses a `Hint::Range` hint. The accepted form is two floats separated by a
// comma, with optional surrounding whitespace, for example "0,1" or "0.0, 1.0".
[[nodiscard]] RangeHint parse_range_hint(std::string_view text);

// Renders a value for a read-only display. Floats use the shortest textual form
// that round-trips, so 0.5 shows as "0.5" and not "0.500000". Used for values
// the panel does not yet edit (a Uuid) and to seed text fields.
[[nodiscard]] std::string format_value(const reflect::Value& value);

// The label shown for a component whose type is not in the reflection registry.
// The engine may hold such a component (a future type not yet registered), and
// the inspector must say so plainly rather than render nothing or crash.
[[nodiscard]] std::string unreflected_label(std::string_view type_name);

} // namespace fe::editor
