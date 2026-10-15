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
    [[nodiscard]] virtual Result<RenderTargetRef> create_render_target(const RenderTargetDesc& desc) = 0;

    // --- Frame lifecycle ---------------------------------------------------
    // Clears the default framebuffer and establishes per-frame state. The
    // caller has already made the context current and sized the viewport.
    virtual void begin_frame(u32 framebuffer_width, u32 framebuffer_height, const ClearColor& clear_color) = 0;
    virtual void end_frame() = 0;

    // --- Render targets ----------------------------------------------------
    // Directs subsequent drawing into `target`. Binding nullptr restores the
    // window's default framebuffer. Binding a target also sets the viewport to
    // its size, so a panel can render a scene at the panel's dimensions
    // independent of the window size.
    virtual void bind_render_target(RenderTarget* target) = 0;

    // The framebuffer currently bound for drawing, or nullptr for the window.
    [[nodiscard]] virtual RenderTarget* bound_render_target() const noexcept = 0;

    // --- State -------------------------------------------------------------
    virtual void set_viewport(u32 x, u32 y, u32 width, u32 height) = 0;
    virtual void set_blend_mode(BlendMode mode) = 0;
    virtual void set_depth_test(bool enabled) = 0;

    // How the next indexed draw assembles its indices. Defaults to Triangles, so
    // a pass that never calls this is unaffected. A debug-line pass binds Lines;
    // it is the caller's job to set it back.
    virtual void set_primitive_topology(PrimitiveTopology topology) = 0;

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

// Creates a null (headless) device: inert resources, no-op commands, no
// graphics context.
//
// This is the backend for a run with no display that still wants the render path
// -- extraction, batching, and draw submission -- to execute rather than be
// skipped. It never fails and rasterises nothing, so it proves *submission*, not
// pixels. The runtime selects it for a contextless window when headless
// rendering is requested (see Engine::Config::headless_render); a test may also
// create one directly to drive a renderer with no GL context.
[[nodiscard]] Result<std::unique_ptr<Device>> create_null_device();

} // namespace rhi
} // namespace fe
