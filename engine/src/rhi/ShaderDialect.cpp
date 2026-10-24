// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/rhi/ShaderDialect.h"

#include <array>
#include <cstddef>

namespace fe::rhi {
namespace {

constexpr std::string_view kDesktopVersion = "#version 330 core";
constexpr std::string_view kEsVersion = "#version 300 es";

// A version directive is only recognised on its own line, ignoring leading
// whitespace. The engine's own shaders all start with one at the top of the
// string, but a source may carry a comment or a blank line above it, so the
// search walks lines rather than assuming the first bytes.
[[nodiscard]] bool is_version_directive(std::string_view line) noexcept {
    const std::size_t first = line.find_first_not_of(" \t\r");
    if (first == std::string_view::npos) {
        return false;
    }
    line.remove_prefix(first);
    return line.starts_with("#version");
}

// True when the fragment source already sets the default float precision.
// GLSL ES accepts `precision highp float;`, `precision mediump float;`, and
// `precision lowp float;`; any of them satisfies the requirement, so the check
// looks for the declaration form rather than one specific qualifier.
[[nodiscard]] bool declares_precision(std::string_view source) noexcept {
    return source.find("precision") != std::string_view::npos;
}

// Appends `source` with every line that is a version directive rewritten to
// `#version 300 es`; a source with no directive is copied through and the
// directive is prepended instead. Returns true when a directive was found.
[[nodiscard]] bool rewrite_version(std::string& out, std::string_view source) {
    bool found = false;
    std::size_t line_start = 0;
    while (line_start <= source.size()) {
        std::size_t line_end = source.find('\n', line_start);
        const bool last = line_end == std::string_view::npos;
        if (last) {
            line_end = source.size();
        }
        const std::string_view line = source.substr(line_start, line_end - line_start);
        if (!found && is_version_directive(line)) {
            found = true;
            out.append(kEsVersion);
        } else {
            out.append(line);
        }
        if (!last) {
            out.push_back('\n');
            line_start = line_end + 1;
        } else {
            break;
        }
    }
    return found;
}

[[nodiscard]] std::string translate_to_es(ShaderStage stage, std::string_view source) {
    std::string out;
    out.reserve(source.size() + 64);

    const bool had_version = rewrite_version(out, source);
    if (!had_version) {
        // No directive to replace: emit one so the result is a valid ES source,
        // then keep the body as it was.
        out = std::string(kEsVersion);
        out.push_back('\n');
        out.append(source);
    }

    if (stage == ShaderStage::Fragment && !declares_precision(source)) {
        // The default precision must follow the version directive and precede
        // any other declaration. Insert it right after the first newline; a
        // single-line source (no newline) gets one appended first so the
        // directives stay on their own lines.
        std::size_t newline = out.find('\n');
        if (newline == std::string::npos) {
            out.push_back('\n');
            newline = out.size() - 1;
        }
        out.insert(newline + 1, "precision highp float;\nprecision highp int;\n");
    }

    return out;
}

} // namespace

std::string translate_shader_source(ShaderDialect dialect, ShaderStage stage, std::string_view source) {
    switch (dialect) {
    case ShaderDialect::DesktopCore:
        // The identity, deliberately: the desktop path must hand the driver the
        // exact bytes it was given, so nothing above the seam can observe it.
        return std::string(source);
    case ShaderDialect::Es:
        return translate_to_es(stage, source);
    }
    return std::string(source);
}

} // namespace fe::rhi
