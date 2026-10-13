// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Types.h"
#include "fe/rhi/Types.h"

#include <span>
#include <string>
#include <string_view>

namespace fe::rhi {

// A compiled shader program.
class Shader {
public:
    virtual ~Shader() = default;

    [[nodiscard]] virtual const std::string& name() const noexcept = 0;

    // Sets a 4x4 matrix uniform by name. Silently ignored if the uniform does
    // not exist, which keeps optional uniforms cheap; a missing *required*
    // uniform is a shader-authoring bug caught by the renderer's own tests.
    virtual void set_mat4(std::string_view name, const Mat4& value) = 0;
    // Sets a 3x3 matrix uniform from 9 column-major floats. Used for the normal
    // matrix, whose inverse-transpose form needs a non-4x4 type.
    virtual void set_mat3(std::string_view name, const f32* value) = 0;
    virtual void set_vec4(std::string_view name, const Vec4& value) = 0;
    virtual void set_vec3(std::string_view name, const Vec3& value) = 0;
    virtual void set_int(std::string_view name, i32 value) = 0;
    virtual void set_float(std::string_view name, f32 value) = 0;
};

// A GPU vertex or index buffer.
class Buffer {
public:
    enum class Kind : u8 {
        Vertex,
        Index,
    };

    virtual ~Buffer() = default;

    [[nodiscard]] virtual Kind kind() const noexcept = 0;
    [[nodiscard]] virtual u32 element_count() const noexcept = 0;

    // Replaces the buffer contents. `data` must contain element_count() *
    // (stride or sizeof(u32)) bytes.
    virtual void update(std::span<const u8> data) = 0;
};

// A GPU texture.
class Texture {
public:
    virtual ~Texture() = default;

    [[nodiscard]] virtual const std::string& name() const noexcept = 0;
    [[nodiscard]] virtual u32 width() const noexcept = 0;
    [[nodiscard]] virtual u32 height() const noexcept = 0;

    // The backend's own identifier for this texture, or 0 if it has none.
    //
    // This is an interop escape hatch, not general API: it exists for exactly
    // one consumer today, the editor viewport, which hands the identifier to
    // Dear ImGui's OpenGL backend so a render-target colour attachment can be
    // shown in a panel. It is intentionally *not* a GL type in this header; the
    // value is opaque and only meaningful to a backend that shares the same
    // graphics API. A non-OpenGL backend (or a future handle-based arena) would
    // return its own value, and code that needs more than an integer handoff
    // should grow a proper interop type rather than widen this.
    [[nodiscard]] virtual u64 native_handle() const noexcept = 0;
};

// An off-screen target that can be rendered into and then sampled like a
// texture.
//
// This is what lets the editor render a scene into a panel instead of the
// window: bind the target, draw, unbind, and blit color_texture() into the
// panel. It is also the seam a future post-processing chain and a 3D shadow
// pass will use, which is why it lives in the RHI rather than in the 2D
// renderer.
//
// Lifecycle: a target and its textures are owned by whoever holds the
// RenderTargetRef. Like every GPU resource they must be released before the
// device.
class RenderTarget {
public:
    virtual ~RenderTarget() = default;

    [[nodiscard]] virtual const std::string& name() const noexcept = 0;
    [[nodiscard]] virtual u32 width() const noexcept = 0;
    [[nodiscard]] virtual u32 height() const noexcept = 0;

    // The color attachment as a shared handle, so a caller (an editor panel)
    // can keep it alive and sample it with bind_texture() like any Texture.
    [[nodiscard]] virtual TextureRef color_texture() const noexcept = 0;

    // Binds the target for drawing: subsequent draws go to its color
    // attachment instead of the window. The viewport is set to the target's
    // full size. Bind the null target (bind_render_target(*device, nullptr)) or
    // call bind_default_target() to go back to the window. Binding a target
    // does not clear it; use clear().
    virtual void bind() = 0;

    // Unbinds, returning drawing to the window's default framebuffer.
    virtual void unbind() = 0;

    // Clears the bound target's color (and depth, when present) attachment.
    virtual void clear(const ClearColor& color) = 0;
};

} // namespace fe::rhi
