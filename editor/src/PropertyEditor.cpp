// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/editor/PropertyEditor.h"

#include "fe/core/Uuid.h"

#include <charconv>
#include <cstdio>
#include <limits>
#include <string>
#include <system_error>
#include <variant>

namespace fe::editor {
namespace {

// Parses one float from `text`, advancing `cursor` past it. Uses from_chars so
// the result is locale-independent: a comma is a separator here, never a
// decimal point, which "0,5" as a range would otherwise be read as.
[[nodiscard]] bool parse_float(std::string_view text, std::size_t& cursor, double& out) {
    while (cursor < text.size() && (text[cursor] == ' ' || text[cursor] == '\t')) {
        ++cursor;
    }
    const char* begin = text.data() + cursor;
    const char* end = text.data() + text.size();
    const auto result = std::from_chars(begin, end, out);
    if (result.ec != std::errc{} || result.ptr == begin) {
        return false;
    }
    cursor = static_cast<std::size_t>(result.ptr - text.data());
    return true;
}

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

RangeHint parse_range_hint(std::string_view text) {
    RangeHint hint;
    std::size_t cursor = 0;

    double minimum = 0.0;
    if (!parse_float(text, cursor, minimum)) {
        return hint;
    }
    while (cursor < text.size() && (text[cursor] == ' ' || text[cursor] == '\t')) {
        ++cursor;
    }
    if (cursor >= text.size() || text[cursor] != ',') {
        return hint;
    }
    ++cursor; // Skip the comma.

    double maximum = 0.0;
    if (!parse_float(text, cursor, maximum)) {
        return hint;
    }

    // Reject trailing content, so "1,2,3" is a malformed hint rather than a
    // silently truncated "1,2".
    while (cursor < text.size() && (text[cursor] == ' ' || text[cursor] == '\t')) {
        ++cursor;
    }
    if (cursor != text.size()) {
        return hint;
    }

    if (minimum >= maximum) {
        return hint;
    }

    hint.minimum = minimum;
    hint.maximum = maximum;
    hint.valid = true;
    return hint;
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
