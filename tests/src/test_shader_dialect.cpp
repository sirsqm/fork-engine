// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
//
// Tests for the GLSL dialect translation seam (docs/SHIPPING_QUEUE.md,
// `glsl-dialect-seam`).
//
// The translation is a pure function of (dialect, stage, source), so these are
// pure-logic tests: no device, no context. They prove the desktop path is the
// identity byte for byte, that the ES path rewrites the version directive and
// adds a fragment precision exactly once, and that translation is idempotent.

#include <doctest/doctest.h>

#include "fe/rhi/ShaderDialect.h"

#include <string>

using namespace fe;
using namespace fe::rhi;

namespace {

constexpr std::string_view kDesktopVertex = R"glsl(#version 330 core

layout(location = 0) in vec2 a_corner;

void main() {
    gl_Position = vec4(a_corner, 0.0, 1.0);
}
)glsl";

constexpr std::string_view kDesktopFragment = R"glsl(#version 330 core

out vec4 frag_color;

void main() {
    frag_color = vec4(1.0);
}
)glsl";

[[nodiscard]] std::size_t count_occurrences(std::string_view haystack, std::string_view needle) {
    std::size_t count = 0;
    std::size_t pos = haystack.find(needle);
    while (pos != std::string_view::npos) {
        ++count;
        pos = haystack.find(needle, pos + needle.size());
    }
    return count;
}

} // namespace

TEST_CASE("desktop translation is the identity, byte for byte") {
    CHECK(translate_shader_source(ShaderDialect::DesktopCore, ShaderStage::Vertex, kDesktopVertex) ==
          std::string(kDesktopVertex));
    CHECK(translate_shader_source(ShaderDialect::DesktopCore, ShaderStage::Fragment, kDesktopFragment) ==
          std::string(kDesktopFragment));

    // The identity holds even for a source that is already ES-shaped: desktop
    // never rewrites, so a caller cannot tell the seam is there.
    const std::string es = translate_shader_source(ShaderDialect::Es, ShaderStage::Fragment, kDesktopFragment);
    CHECK(translate_shader_source(ShaderDialect::DesktopCore, ShaderStage::Fragment, es) == es);
}

TEST_CASE("ES vertex source rewrites the version and adds no precision") {
    const std::string out = translate_shader_source(ShaderDialect::Es, ShaderStage::Vertex, kDesktopVertex);
    CHECK(out.find("#version 300 es") != std::string::npos);
    CHECK(out.find("#version 330 core") == std::string::npos);
    CHECK(count_occurrences(out, "#version") == 1);
    // A vertex shader has a default float precision in ES, so none is injected.
    CHECK(out.find("precision") == std::string::npos);
}

TEST_CASE("ES fragment source rewrites the version and adds one precision") {
    const std::string out = translate_shader_source(ShaderDialect::Es, ShaderStage::Fragment, kDesktopFragment);
    CHECK(out.find("#version 300 es") != std::string::npos);
    CHECK(out.find("#version 330 core") == std::string::npos);
    CHECK(count_occurrences(out, "#version") == 1);
    CHECK(out.find("precision highp float;") != std::string::npos);
    CHECK(out.find("precision highp int;") != std::string::npos);
    CHECK(count_occurrences(out, "precision highp float;") == 1);
    // The directive must precede the injected precision, which in turn precedes
    // the body: an ES compiler rejects a precision before the version.
    CHECK(out.find("#version 300 es") < out.find("precision highp float;"));
    CHECK(out.find("precision highp float;") < out.find("out vec4 frag_color;"));
}

TEST_CASE("an ES fragment source with a precision is not given a second one") {
    constexpr std::string_view source = R"glsl(#version 330 core

precision mediump float;

out vec4 frag_color;

void main() {
    frag_color = vec4(1.0);
}
)glsl";
    const std::string out = translate_shader_source(ShaderDialect::Es, ShaderStage::Fragment, source);
    CHECK(out.find("#version 300 es") != std::string::npos);
    // The author's own declaration is kept, and nothing is injected beside it.
    CHECK(out.find("precision mediump float;") != std::string::npos);
    CHECK(count_occurrences(out, "precision") == 1);
}

TEST_CASE("translating an already-translated ES source adds nothing") {
    const std::string once = translate_shader_source(ShaderDialect::Es, ShaderStage::Fragment, kDesktopFragment);
    const std::string twice = translate_shader_source(ShaderDialect::Es, ShaderStage::Fragment, once);
    CHECK(twice == once);
    CHECK(count_occurrences(twice, "#version") == 1);
    CHECK(count_occurrences(twice, "precision highp float;") == 1);

    const std::string vertex_once = translate_shader_source(ShaderDialect::Es, ShaderStage::Vertex, kDesktopVertex);
    CHECK(translate_shader_source(ShaderDialect::Es, ShaderStage::Vertex, vertex_once) == vertex_once);
}

TEST_CASE("an ES source with no version directive still gets one") {
    constexpr std::string_view source = "out vec4 frag_color;\nvoid main() { frag_color = vec4(1.0); }\n";
    const std::string out = translate_shader_source(ShaderDialect::Es, ShaderStage::Fragment, source);
    CHECK(out.starts_with("#version 300 es\n"));
    CHECK(count_occurrences(out, "#version") == 1);
    CHECK(count_occurrences(out, "precision highp float;") == 1);
}
