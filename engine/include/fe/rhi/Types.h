// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Math.h"
#include "fe/core/Types.h"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

// The Rendering Hardware Interface (RHI).
//
// The RHI is the seam between the engine and a graphics API. Everything above
// it (the 2D renderer, and later the 3D renderer and the editor viewport) talks
// to this interface only; it never calls OpenGL directly. The OpenGL 3.3 core
// backend lives under `fe/rhi/opengl/` and is one implementation among a
// possible set.
//
// Design notes:
//   * The interface is deliberately small and command-oriented. It is the
//     subset the engine actually uses, not an exhaustive wrapper. Growing it is
//     cheap; guessing at unused features is not.
//   * GPU resources are reference-counted for this milestone. A future
//     handle-based resource arena can replace the smart pointers without
//     changing call sites, because callers only ever hold an opaque ref.

namespace fe::rhi {

enum class VertexFormat : u8 {
    Float1,
    Float2,
    Float3,
    Float4,
};

[[nodiscard]] inline u32 component_count(VertexFormat format) noexcept {
    switch (format) {
    case VertexFormat::Float1:
        return 1;
    case VertexFormat::Float2:
        return 2;
    case VertexFormat::Float3:
        return 3;
    case VertexFormat::Float4:
        return 4;
    }
    return 0;
}

// One attribute within a vertex layout.
struct VertexAttribute {
    u32 location = 0;
    VertexFormat format = VertexFormat::Float3;
    u32 offset = 0;
};

// Describes how the bytes of a vertex are interpreted.
struct VertexLayout {
    u32 stride = 0;
    std::vector<VertexAttribute> attributes;

    [[nodiscard]] static VertexLayout position_color_uv() {
        // vec2 position, vec4 colour, vec2 uv - the sprite vertex format.
        VertexLayout layout;
        layout.stride = 2 * 4 + 4 * 4 + 2 * 4;
        layout.attributes = {
            {0, VertexFormat::Float2, 0},
            {1, VertexFormat::Float4, 8},
            {2, VertexFormat::Float2, 24},
        };
        return layout;
    }
};

enum class BlendMode : u8 {
    None,     // Opaque; blending disabled.
    Alpha,    // Straight alpha: src.rgb * src.a + dst.rgb * (1 - src.a)
    Additive, // Additive: src.rgb * src.a + dst.rgb
};

enum class TextureFilter : u8 {
    Nearest,
    Linear,
};

enum class TextureWrap : u8 {
    Repeat,
    ClampToEdge,
};

enum class PixelFormat : u8 {
    Rgba8, // 4 bytes per pixel, 8 bits per channel.
};

struct ShaderDesc {
    std::string name;
    std::string_view vertex_source;
    std::string_view fragment_source;
};

struct TextureDesc {
    std::string name;
    u32 width = 0;
    u32 height = 0;
    PixelFormat format = PixelFormat::Rgba8;
    TextureFilter min_filter = TextureFilter::Linear;
    TextureFilter mag_filter = TextureFilter::Linear;
    TextureWrap wrap_u = TextureWrap::ClampToEdge;
    TextureWrap wrap_v = TextureWrap::ClampToEdge;
    // Tightly packed pixel data, width * height * 4 bytes for Rgba8.
    const u8* pixels = nullptr;
    // Generate a full mip chain after upload. Off by default because most 2D
    // assets in this milestone are drawn at a fixed scale.
    bool generate_mipmaps = false;
};

struct ClearColor {
    f32 r = 0.0f;
    f32 g = 0.0f;
    f32 b = 0.0f;
    f32 a = 1.0f;
};

// Parameters for an off-screen render target.
struct RenderTargetDesc {
    std::string name;
    u32 width = 0;
    u32 height = 0;
    // Allocate a depth buffer. Off for the 2D viewport, on for future 3D
    // passes; it costs memory that a 2D-only target does not need.
    bool depth = false;
};

// Opaque GPU resource handles. Callers never see the backend's own types.
class Shader;
class Buffer;
class Texture;
class RenderTarget;
using ShaderRef = std::shared_ptr<Shader>;
using BufferRef = std::shared_ptr<Buffer>;
using TextureRef = std::shared_ptr<Texture>;
using RenderTargetRef = std::shared_ptr<RenderTarget>;

} // namespace fe::rhi
