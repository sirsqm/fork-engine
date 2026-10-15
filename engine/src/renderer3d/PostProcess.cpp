// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/renderer3d/PostProcess.h"

#include "fe/core/Assert.h"
#include "fe/core/Log.h"

#include <array>
#include <cstddef>
#include <span>

namespace fe::renderer3d {
namespace {

// A full-screen triangle-pair in clip space. The vertex carries the position and
// the UV, so no uniform matrix is needed: the quad *is* the viewport.
constexpr const char* kQuadVertexShaderSource = R"glsl(
#version 330 core

layout(location = 0) in vec2 a_position;
layout(location = 1) in vec2 a_uv;

out vec2 v_uv;

void main() {
    v_uv = a_uv;
    gl_Position = vec4(a_position, 0.0, 1.0);
}
)glsl";

constexpr const char* kPostFragmentShaderSource = R"glsl(
#version 330 core

in vec2 v_uv;

uniform sampler2D u_source;
uniform float u_exposure;
uniform float u_vignette;
uniform int u_tonemap;

out vec4 frag_color;

void main() {
    vec3 color = texture(u_source, v_uv).rgb;
    color *= u_exposure;

    if (u_tonemap != 0) {
        color = color / (color + vec3(1.0));
    }

    if (u_vignette > 0.0) {
        // Distance from the centre of the frame, remapped so the middle is
        // untouched and the corners darken towards (1 - u_vignette).
        vec2 offset = v_uv - vec2(0.5);
        float radius = length(offset) * 1.41421356; // 1.0 at a corner.
        float shade = 1.0 - u_vignette * clamp(radius * radius, 0.0, 1.0);
        color *= shade;
    }

    frag_color = vec4(color, 1.0);
}
)glsl";

// Two triangles covering clip space, with UV (0,0) at the bottom-left so the
// sampled texture is not flipped.
constexpr std::array<f32, 16> kQuadVertices = {
    // position.xy   uv.xy
    -1.0f, -1.0f, 0.0f, 0.0f, 1.0f, -1.0f, 1.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, -1.0f, 1.0f, 0.0f, 1.0f,
};

constexpr std::array<u32, 6> kQuadIndices = {0, 1, 2, 0, 2, 3};

} // namespace

Error PostProcess::initialize(rhi::Device& device) {
    if (shader_ != nullptr) {
        // Already built; the renderer calls this every frame the chain runs.
        return Error::none();
    }

    rhi::ShaderDesc desc;
    desc.name = "post_process";
    desc.vertex_source = kQuadVertexShaderSource;
    desc.fragment_source = kPostFragmentShaderSource;

    auto shader = device.create_shader(desc);
    if (!shader) {
        return shader.error();
    }
    shader_ = shader.value();

    auto vertex_buffer = device.create_vertex_buffer(rhi::VertexLayout::position_uv(),
                                                     static_cast<u32>(kQuadVertices.size() * sizeof(f32)));
    if (!vertex_buffer) {
        return vertex_buffer.error();
    }
    quad_vertex_buffer_ = vertex_buffer.value();
    quad_vertex_buffer_->update(
        std::span<const u8>(reinterpret_cast<const u8*>(kQuadVertices.data()), kQuadVertices.size() * sizeof(f32)));

    auto index_buffer = device.create_index_buffer(static_cast<u32>(kQuadIndices.size()));
    if (!index_buffer) {
        return index_buffer.error();
    }
    quad_index_buffer_ = index_buffer.value();
    quad_index_buffer_->update(
        std::span<const u8>(reinterpret_cast<const u8*>(kQuadIndices.data()), kQuadIndices.size() * sizeof(u32)));

    return Error::none();
}

Error PostProcess::apply(rhi::Device& device, rhi::Texture& source, const PostProcessSettings& settings) {
    FE_CHECK(shader_ != nullptr);

    // The pass draws a flat quad: no depth test, no blending, so the graded image
    // replaces the destination rather than layering over it.
    device.set_depth_test(false);
    device.set_blend_mode(rhi::BlendMode::None);

    device.bind_shader(*shader_);
    device.bind_texture(0, source);
    shader_->set_int("u_source", 0);
    shader_->set_float("u_exposure", settings.exposure);
    shader_->set_float("u_vignette", settings.vignette);
    shader_->set_int("u_tonemap", settings.tonemap ? 1 : 0);

    device.bind_vertex_buffer(*quad_vertex_buffer_);
    device.bind_index_buffer(*quad_index_buffer_);
    device.draw_indexed(static_cast<u32>(kQuadIndices.size()));

    ++pass_count_;
    return Error::none();
}

} // namespace fe::renderer3d
