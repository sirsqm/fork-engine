// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/editor/PropertyEditor.h"

#include "fe/core/Uuid.h"

#include <algorithm>
#include <cstdio>
#include <limits>
#include <string>
#include <variant>

namespace fe::editor {
namespace {

[[nodiscard]] std::string format_double(double value) {
    // "%.17g" is the shortest precision that round-trips a double; trim trailing
    // zeros and a trailing dot so 1.0 prints as "1" and 0.5 as "0.5".
    char buffer[64];
    const int written = std::snprintf(buffer, sizeof(buffer), "%.17g", value);
    if (written <= 0) {
        return {};
    }
    std::string text(buffer, static_cast<std::size_t>(written));

    if (text.find('.') != std::string::npos && text.find('e') == std::string::npos &&
        text.find('E') == std::string::npos) {
        while (!text.empty() && text.back() == '0') {
            text.pop_back();
        }
        if (!text.empty() && text.back() == '.') {
            text.pop_back();
        }
    }
    return text;
}

} // namespace

ResolvedRange resolve_range_hint(const std::optional<reflect::RangeHint>& hint) {
    ResolvedRange range;
    // An absent hint means "unbounded": the panel shows a plain drag.
    if (!hint.has_value()) {
        return range;
    }
    // A degenerate interval (minimum >= maximum) is a mis-registration, not a
    // usable bound, so it degrades to unbounded rather than an inverted slider.
    if (hint->minimum >= hint->maximum) {
        return range;
    }
    range.minimum = hint->minimum;
    range.maximum = hint->maximum;
    range.step = hint->step;
    range.valid = true;
    return range;
}

double clamp_to_range(double value, const ResolvedRange& range) {
    if (!range.valid) {
        return value;
    }
    return std::clamp(value, range.minimum, range.maximum);
}

usize color_channel_count(usize member_count) noexcept {
    // A colour is three or four floats (RGB/RGBA). A two-member group is a Vec2
    // (a position), not a colour, so the hint is ignored for it.
    return member_count == 3 || member_count == 4 ? member_count : 0;
}

std::string format_value(const reflect::Value& value) {
    switch (value.index()) {
    case 0:
        return std::get<bool>(value) ? "true" : "false";
    case 1:
        return std::to_string(std::get<i64>(value));
    case 2: {
        const double number = std::get<f64>(value);
        if (number == std::numeric_limits<double>::infinity()) {
            return "inf";
        }
        if (number == -std::numeric_limits<double>::infinity()) {
            return "-inf";
        }
        if (number != number) {
            return "nan";
        }
        return format_double(number);
    }
    case 3:
        return std::get<std::string>(value);
    default:
        return std::get<fe::Uuid>(value).to_string();
    }
}

std::string unreflected_label(std::string_view type_name) {
    return std::string(type_name) + " (not reflected)";
}

} // namespace fe::editor
