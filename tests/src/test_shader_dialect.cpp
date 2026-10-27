// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
//
// Tests for the GLSL dialect translation seam (docs/PLAN.md,
// `glsl-dialect-seam`).
//
// The translation is a pure function of (dialect, stage, source), so these are
// pure-logic tests: no device, no context. They prove the desktop path is the
// identity byte for byte, that the ES path rewrites the version directive and
// adds a fragment precision exactly once, and that translation is idempotent.

#include <doctest/doctest.h>

#include "fe/rhi/Device.h"
#include "fe/rhi/ShaderDialect.h"

#include <memory>
#include <span>
#include <string>
#include <string_view>

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

TEST_CASE("a leading blank line does not push the version directive off line one") {
    // The engine's own shaders are raw string literals that begin with a newline,
    // and GLSL ES rejects a version directive that is not on the first line. This
    // is the shape that reaches the WebGL2 compiler in a shipped web build, so it
    // is checked directly rather than only through the compiler.
    constexpr std::string_view source = "\n\n#version 330 core\n\nvoid main() {}\n";

    const std::string vertex = translate_shader_source(ShaderDialect::Es, ShaderStage::Vertex, source);
    CHECK(vertex.starts_with("#version 300 es\n"));
    CHECK(count_occurrences(vertex, "#version") == 1);

    const std::string fragment = translate_shader_source(ShaderDialect::Es, ShaderStage::Fragment, source);
    CHECK(fragment.starts_with("#version 300 es\n"));
    CHECK(fragment.find("precision highp float;") == std::string("#version 300 es\n").size());
    CHECK(count_occurrences(fragment, "#version") == 1);
    CHECK(count_occurrences(fragment, "precision highp float;") == 1);
}

TEST_CASE("a source with no version directive and leading whitespace still starts with one") {
    constexpr std::string_view source = "\n   \nvoid main() {}\n";
    const std::string out = translate_shader_source(ShaderDialect::Es, ShaderStage::Fragment, source);
    CHECK(out.starts_with("#version 300 es\n"));
    CHECK(count_occurrences(out, "#version") == 1);
    CHECK(count_occurrences(out, "precision highp float;") == 1);
}

// ---------------------------------------------------------------------------
// Wiring the seam into the device (docs/PLAN.md,
// `shader-dialect-wiring`).
//
// The dialect is a property of the device, chosen once where the device is
// created, not a parameter of create_shader. These tests prove that: the same
// descriptor, unchanged, compiles differently through two devices that differ
// only in the dialect they report.
// ---------------------------------------------------------------------------

namespace {

// A device double that overrides only the dialect. It calls the base class's
// compiled_shader_source(), which is the code the real backends share, so this
// exercises production code rather than a reimplementation.
class DialectDevice final : public rhi::Device {
public:
    explicit DialectDevice(rhi::ShaderDialect dialect) : dialect_(dialect) {}

    [[nodiscard]] const char* backend_name() const noexcept override { return "dialect-test"; }
    [[nodiscard]] std::string info() const override { return "dialect test device"; }
    [[nodiscard]] rhi::ShaderDialect shader_dialect() const noexcept override { return dialect_; }

    Result<rhi::ShaderRef> create_shader(const rhi::ShaderDesc& desc) override {
        vertex_seen = compiled_shader_source(rhi::ShaderStage::Vertex, desc.vertex_source);
        fragment_seen = compiled_shader_source(rhi::ShaderStage::Fragment, desc.fragment_source);
        return std::static_pointer_cast<rhi::Shader>(std::make_shared<InertShader>(desc.name));
    }

    Result<rhi::BufferRef> create_vertex_buffer(const rhi::VertexLayout&, u32) override { return Error("unused"); }
    Result<rhi::BufferRef> create_index_buffer(u32) override { return Error("unused"); }
    Result<rhi::TextureRef> create_texture(const rhi::TextureDesc&) override { return Error("unused"); }
    Result<rhi::RenderTargetRef> create_render_target(const rhi::RenderTargetDesc&) override { return Error("unused"); }

    void begin_frame(u32, u32, const rhi::ClearColor&) override {}
    void end_frame() override {}
    void bind_render_target(rhi::RenderTarget*) override {}
    [[nodiscard]] rhi::RenderTarget* bound_render_target() const noexcept override { return nullptr; }
    void set_viewport(u32, u32, u32, u32) override {}
    void set_blend_mode(rhi::BlendMode) override {}
    void set_depth_test(bool) override {}
    void set_primitive_topology(rhi::PrimitiveTopology) override {}
    void bind_shader(rhi::Shader&) override {}
    void bind_vertex_buffer(rhi::Buffer&) override {}
    void bind_index_buffer(rhi::Buffer&) override {}
    void bind_instance_buffer(rhi::Buffer&, u32 = 0) override {}
    void bind_texture(u32, rhi::Texture&) override {}
    void draw_indexed(u32) override {}
    void draw_indexed_instanced(u32, u32) override {}
    void read_pixels(u32, u32, u32, u32, std::span<u8>) override {}

    std::string vertex_seen;
    std::string fragment_seen;

private:
    class InertShader final : public rhi::Shader {
    public:
        explicit InertShader(std::string name) : name_(std::move(name)) {}
        [[nodiscard]] const std::string& name() const noexcept override { return name_; }
        void set_mat4(std::string_view, const Mat4&) override {}
        void set_mat3(std::string_view, const f32*) override {}
        void set_vec4(std::string_view, const Vec4&) override {}
        void set_vec3(std::string_view, const Vec3&) override {}
        void set_int(std::string_view, i32) override {}
        void set_float(std::string_view, f32) override {}
        void set_vec4_array(std::string_view, const Vec4*, u32) override {}

    private:
        std::string name_;
    };

    rhi::ShaderDialect dialect_;
};

} // namespace

TEST_CASE("the device applies its dialect at creation, not the caller") {
    rhi::ShaderDesc desc;
    desc.name = "wired";
    desc.vertex_source = kDesktopVertex;
    desc.fragment_source = kDesktopFragment;

    DialectDevice desktop(rhi::ShaderDialect::DesktopCore);
    DialectDevice es(rhi::ShaderDialect::Es);

    // The same descriptor, unchanged, through two devices.
    REQUIRE(desktop.create_shader(desc).ok());
    REQUIRE(es.create_shader(desc).ok());

    CHECK(desktop.shader_dialect() == rhi::ShaderDialect::DesktopCore);
    CHECK(es.shader_dialect() == rhi::ShaderDialect::Es);

    // Desktop: byte for byte the authored source, so nothing regressed.
    CHECK(desktop.vertex_seen == std::string(kDesktopVertex));
    CHECK(desktop.fragment_seen == std::string(kDesktopFragment));

    // ES: translated by the device, with no change at the call site.
    CHECK(es.vertex_seen.find("#version 300 es") != std::string::npos);
    CHECK(es.vertex_seen.find("#version 330 core") == std::string::npos);
    CHECK(es.fragment_seen.find("#version 300 es") != std::string::npos);
    CHECK(count_occurrences(es.fragment_seen, "precision highp float;") == 1);
}

TEST_CASE("compiled_shader_source is exactly translate_shader_source for the device dialect") {
    DialectDevice es(rhi::ShaderDialect::Es);
    CHECK(es.compiled_shader_source(rhi::ShaderStage::Vertex, kDesktopVertex) ==
          translate_shader_source(rhi::ShaderDialect::Es, rhi::ShaderStage::Vertex, kDesktopVertex));
    CHECK(es.compiled_shader_source(rhi::ShaderStage::Fragment, kDesktopFragment) ==
          translate_shader_source(rhi::ShaderDialect::Es, rhi::ShaderStage::Fragment, kDesktopFragment));
}
