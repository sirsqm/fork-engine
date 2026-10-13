// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Types.h"
#include "fe/reflect/Property.h"
#include "fe/reflect/Value.h"

#include <optional>
#include <string>
#include <string_view>

namespace fe::editor {

// Pure helpers for the property inspector.
//
// None of these touch ImGui. They are the parts of the inspector that are not
// inherently graphical: turning a property's editor hint into widget bounds,
// clamping an edited number into them, deciding how many colour channels a
// group has, and rendering a `reflect::Value` as human-readable text. Keeping
// them out of `InspectorPanel.cpp` means they are covered by the headless
// editor tests, while the panel itself stays a thin "read the value, draw a
// widget" layer. See docs/adr/0028-editor-hint-metadata.md.

// A numeric range resolved from a property's `RangeHint` into widget bounds.
struct ResolvedRange {
    double minimum = 0.0;
    double maximum = 0.0;
    // The drag increment; 0 means "no fixed step", and the inspector chooses.
    double step = 0.0;
    // False when the hint is absent, or present but not a real interval
    // (minimum >= maximum). The panel then falls back to an unbounded drag, so a
    // typo in a registration degrades to a usable widget rather than a broken
    // one.
    bool valid = false;
};

// Resolves a property's optional range hint into usable bounds.
[[nodiscard]] ResolvedRange resolve_range_hint(const std::optional<reflect::RangeHint>& hint);

// Clamps `value` into `range`. An invalid range leaves the value unchanged, so
// an un-annotated (or malformed) property is never silently bounded.
[[nodiscard]] double clamp_to_range(double value, const ResolvedRange& range);

// The number of colour channels a float group exposes to a colour picker: 3 for
// RGB, 4 for RGBA, and 0 when the group is too small to be a colour (in which
// case the panel does not show a picker even if the hint is present).
[[nodiscard]] usize color_channel_count(usize member_count) noexcept;

// Renders a value for a read-only display. Floats use the shortest textual form
// that round-trips, so 0.5 shows as "0.5" and not "0.500000". Used for values
// the panel does not yet edit (a Uuid) and to seed text fields.
[[nodiscard]] std::string format_value(const reflect::Value& value);

// The label shown for a component whose type is not in the reflection registry.
// The engine may hold such a component (a future type not yet registered), and
// the inspector must say so plainly rather than render nothing or crash.
[[nodiscard]] std::string unreflected_label(std::string_view type_name);

} // namespace fe::editor
