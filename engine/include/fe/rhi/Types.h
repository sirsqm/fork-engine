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

    // Advance this attribute once per instance rather than once per vertex. The
    // backend sets the divisor from this, so an instanced pass binds one buffer
    // of per-instance data and the shader reads it per instance. Zero is the
    // ordinary per-vertex case and the default, so a layout that does not think
    // about instancing is unaffected.
    u32 divisor = 0;
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

    // The instanced sprite path uses two buffers and two layouts.
    //
    // `sprite_quad()` is the shared per-vertex stream: four unit-square corners in
    // [-0.5, 0.5], drawn once and instanced. It replaces the old four
    // world-space vertices and six indices per sprite with four vertices and six
    // indices total, regardless of how many sprites are drawn.
    [[nodiscard]] static VertexLayout sprite_quad() {
        VertexLayout layout;
        layout.stride = 2 * 4;
        layout.attributes = {
            {0, VertexFormat::Float2, 0, 0},
        };
        return layout;
    }

    // `sprite_instance()` is the per-instance stream, one record per sprite. The
    // attributes are, in order: the world matrix's columns 0, 1, and 3, the tint,
    // and the uv rectangle. Columns 0 and 1 arrive already scaled by the sprite's
    // size, so the shader needs no separate size. Column 3 is the translation.
    // Column 2 is omitted because a 2D transform never writes a world z, which is
    // the same reason `transform_point` reads only rows 0-2.
    [[nodiscard]] static VertexLayout sprite_instance() {
        VertexLayout layout;
        layout.stride = 5 * 16;
        layout.attributes = {
            {1, VertexFormat::Float4, 0, 1},  // world column 0 * size.x
            {2, VertexFormat::Float4, 16, 1}, // world column 1 * size.y
            {3, VertexFormat::Float4, 32, 1}, // world column 3 (translation)
            {4, VertexFormat::Float4, 48, 1}, // colour
            {5, VertexFormat::Float4, 64, 1}, // uv rectangle (u0, v0, u1, v1)
        };
        return layout;
    }

    [[nodiscard]] static VertexLayout position_normal_uv() {
        // vec3 position, vec3 normal, vec2 uv - the 3D mesh vertex format,
        // matching fe::Vertex3D.
        VertexLayout layout;
        layout.stride = 3 * 4 + 3 * 4 + 2 * 4;
        layout.attributes = {
            {0, VertexFormat::Float3, 0},
            {1, VertexFormat::Float3, 12},
            {2, VertexFormat::Float2, 24},
        };
        return layout;
    }

    // The instanced mesh path pairs `position_normal_uv()` (locations 0-2) with
    // this per-instance stream (locations 3-9). One record is the instance's model
    // matrix followed by its normal matrix, each as four-column vectors in the
    // same column-major order the RHI uploads a `Mat4`, so the vertex shader
    // assembles both with no repacking.
    //
    // The normal matrix travels per instance rather than being derived in the
    // shader: `transpose(inverse(model))` per vertex is real vertex work, and the
    // CPU already computes it once per instance. The fourth column of each normal
    // column is padding the shader ignores; it keeps every attribute a vec4 and
    // the stride 16-byte aligned, which is the shape every backend accepts.
    //
    // Unlike the sprite path, no column is dropped: a sprite transform never
    // writes a world z, but a mesh's model matrix scales, rotates, and translates
    // on all three axes, so the shader needs the full matrix to place a vertex.
    [[nodiscard]] static VertexLayout mesh_instance() {
        VertexLayout layout;
        layout.stride = 7 * 16;
        layout.attributes = {
            {3, VertexFormat::Float4, 0, 1},  // model column 0
            {4, VertexFormat::Float4, 16, 1}, // model column 1
            {5, VertexFormat::Float4, 32, 1}, // model column 2
            {6, VertexFormat::Float4, 48, 1}, // model column 3
            {7, VertexFormat::Float4, 64, 1}, // normal matrix column 0
            {8, VertexFormat::Float4, 80, 1}, // normal matrix column 1
            {9, VertexFormat::Float4, 96, 1}, // normal matrix column 2
        };
        return layout;
    }

    [[nodiscard]] static VertexLayout position_uv() {
        // vec2 clip-space position, vec2 uv - the full-screen quad format used
        // by a background or post-processing pass. The position is already in
        // clip space, so the pass needs no camera matrix.
        VertexLayout layout;
        layout.stride = 2 * 4 + 2 * 4;
        layout.attributes = {
            {0, VertexFormat::Float2, 0},
            {1, VertexFormat::Float2, 8},
        };
        return layout;
    }

    [[nodiscard]] static VertexLayout position_color() {
        // vec3 position, vec4 colour - the debug-line vertex format. A line list
        // carries its own colour per endpoint so a wireframe can be tinted
        // without a uniform per draw.
        VertexLayout layout;
        layout.stride = 3 * 4 + 4 * 4;
        layout.attributes = {
            {0, VertexFormat::Float3, 0},
            {1, VertexFormat::Float4, 12},
        };
        return layout;
    }
};

// How the bound index buffer's indices are assembled into primitives.
//
// Triangles is the default and what every existing pass uses; a debug line pass
// binds Lines. Kept as device state rather than a draw parameter so the existing
// `draw_indexed(index_count)` signature is unchanged.
enum class PrimitiveTopology : u8 {
    Triangles,
    Lines,
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
    // Allocate a colour attachment. On for everything that draws colour -- the
    // window, the editor viewport, the post-process target. A depth-only pass
    // (the shadow map) turns it off: the pass writes no colour, and a WebGL2
    // context rejects a draw whose program has no fragment output when the
    // framebuffer it is bound to has a colour attachment enabled. A target with
    // `color = false` must have `depth = true` and `sample_depth = true`, since a
    // target with no attachment at all would be incomplete.
    bool color = true;
    // Allocate a depth buffer. Off for the 2D viewport, on for future 3D
    // passes; it costs memory that a 2D-only target does not need.
    bool depth = false;
    // Allocate the depth buffer as a sampleable texture instead of an
    // unattachable renderbuffer, exposed through RenderTarget::depth_texture().
    // Only meaningful when `depth` is set. A shadow pass needs this: it renders
    // depth from the light and then reads that depth back in the main pass,
    // which a renderbuffer cannot do. A target that only depth-tests (the
    // editor viewport) leaves this off and keeps the cheaper renderbuffer.
    bool sample_depth = false;
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
