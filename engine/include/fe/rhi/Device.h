// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Error.h"
#include "fe/core/Types.h"
#include "fe/rhi/Resources.h"
#include "fe/rhi/Types.h"

#include <memory>
#include <span>
#include <string>

namespace fe {

class Window;

namespace rhi {

// The graphics device: a command-oriented abstraction over a graphics API.
//
// The engine talks to a Device and to the resource interfaces above it. The
// concrete backend (currently OpenGL 3.3 core) is selected once, at startup,
// through create_device(). Nothing above this line includes a backend header.
//
// Threading: a Device and the resources it creates are owned by the render
// thread. This milestone runs a single-threaded frame loop, so the constraint
// is satisfied by construction; it is stated here so it is not violated later.
class Device {
public:
    virtual ~Device() = default;

    [[nodiscard]] virtual const char* backend_name() const noexcept = 0;

    // A short human-readable description: API version and renderer string.
    [[nodiscard]] virtual std::string info() const = 0;

    // --- Resource creation -------------------------------------------------
    [[nodiscard]] virtual Result<ShaderRef> create_shader(const ShaderDesc& desc) = 0;
    [[nodiscard]] virtual Result<BufferRef> create_vertex_buffer(const VertexLayout& layout, u32 capacity_bytes) = 0;
    [[nodiscard]] virtual Result<BufferRef> create_index_buffer(u32 capacity_indices) = 0;
    [[nodiscard]] virtual Result<TextureRef> create_texture(const TextureDesc& desc) = 0;

    // --- Frame lifecycle ---------------------------------------------------
    // Clears the default framebuffer and establishes per-frame state. The
    // caller has already made the context current and sized the viewport.
    virtual void begin_frame(u32 framebuffer_width, u32 framebuffer_height, const ClearColor& clear_color) = 0;
    virtual void end_frame() = 0;

    // --- State -------------------------------------------------------------
    virtual void set_viewport(u32 x, u32 y, u32 width, u32 height) = 0;
    virtual void set_blend_mode(BlendMode mode) = 0;
    virtual void set_depth_test(bool enabled) = 0;

    // --- Binding and drawing ----------------------------------------------
    virtual void bind_shader(Shader& shader) = 0;
    virtual void bind_vertex_buffer(Buffer& buffer) = 0;
    virtual void bind_index_buffer(Buffer& buffer) = 0;
    virtual void bind_texture(u32 slot, Texture& texture) = 0;
    virtual void draw_indexed(u32 index_count) = 0;

    // --- Readback ----------------------------------------------------------
    // Copies a region of the current framebuffer into `out` as tightly packed
    // RGBA8 (4 bytes per pixel), with row 0 at the bottom (OpenGL convention).
    // `out` must hold width * height * 4 bytes.
    //
    // This exists for two real consumers: the automated graphics tests, which
    // assert that something was actually drawn, and the future editor, which
    // needs screenshots and thumbnail capture. It is not a general image API.
    virtual void read_pixels(u32 x, u32 y, u32 width, u32 height, std::span<u8> out) = 0;
};

// Creates the default graphics device for `window`.
//
// The window must already own a current graphics context. Returns an Error if
// the backend's entry points cannot be resolved (for example, a driver that
// does not provide an OpenGL 3.3 core context).
[[nodiscard]] Result<std::unique_ptr<Device>> create_device(Window& window);

} // namespace rhi
} // namespace fe
