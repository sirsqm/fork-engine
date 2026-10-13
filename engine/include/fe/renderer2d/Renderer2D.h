// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Error.h"
#include "fe/core/Math.h"
#include "fe/core/Types.h"
#include "fe/rhi/Device.h"

#include <memory>
#include <vector>

namespace fe {

class Scene;
class Node;
class Camera2D;
class Sprite2D;
class ResourceManager;

namespace renderer2d {

// Draws a Scene's 2D content through the RHI.
//
// Responsibilities:
//   * Walk the scene, collect sprites, and sort them into a stable draw order.
//   * Build a vertex/index stream in CPU memory and upload it in batches.
//   * Issue one indexed draw per texture batch.
//
// Non-responsibilities: it does not own the window, does not decide the
// clear colour policy beyond asking the camera, and does not know how a texture
// was decoded. It is a pure consumer of scene data and RHI resources.
//
// Batching model: sprites that share a texture are merged into one draw call.
// The sort key is (texture pointer, layer), so switching textures or crossing a
// layer boundary starts a new batch. This is the simplest correct scheme; a
// future revision can sort by (layer, texture) and use a texture atlas to cut
// draw calls further, without changing this interface.
//
// The renderer owns its GPU resources (one shader, one dynamic vertex buffer,
// one dynamic index buffer) and grows the buffers on demand.
class Renderer2D {
public:
    // The sprite vertex format: position (vec2), colour (vec4), uv (vec2).
    struct Vertex {
        Vec2 position;
        Vec4 color;
        Vec2 uv;
    };

    Renderer2D();
    ~Renderer2D();

    Renderer2D(const Renderer2D&) = delete;
    Renderer2D& operator=(const Renderer2D&) = delete;

    // Creates the renderer's GPU resources. Returns an Error if shader
    // compilation fails.
    [[nodiscard]] Error initialize(rhi::Device& device);

    // Renders `scene` into the current frame. The caller must have called
    // Device::begin_frame() first. `viewport_width`/`viewport_height` are in
    // pixels and are used to build the camera projection.
    //
    // Returns the number of sprites drawn, which the example uses to prove the
    // pipeline actually produced geometry. A return value of 0 is valid (an
    // empty scene).
    [[nodiscard]] Result<usize> render(rhi::Device& device, const Scene& scene, u32 viewport_width,
                                       u32 viewport_height);

    // Number of draw calls issued by the most recent render(). Exposed for the
    // example's on-screen statistics and for tests that assert batching.
    [[nodiscard]] u32 last_draw_calls() const noexcept { return last_draw_calls_; }

    [[nodiscard]] u32 vertex_capacity() const noexcept { return vertex_capacity_; }
    [[nodiscard]] u32 index_capacity() const noexcept { return index_capacity_; }

    // Selects the camera that should drive rendering for `scene`: the first
    // camera with `primary` set, or the first camera found if none is primary,
    // or nullptr if the scene has no camera. Public because the runtime needs
    // the same answer to choose the clear colour, and two independent
    // implementations of "which camera is active" is exactly the kind of
    // divergence that produces subtle bugs.
    [[nodiscard]] static const Camera2D* find_active_camera(const Scene& scene);

    // The same selection for a mutable scene, so a caller that drives the
    // camera (the editor viewport's pan/zoom) edits exactly the camera the
    // renderer will use. One implementation, so the two can never diverge.
    [[nodiscard]] static Camera2D* find_active_camera(Scene& scene);

private:
    // One contiguous run of quads that share a texture.
    struct Batch {
        rhi::Texture* texture = nullptr;
        u32 index_offset = 0; // In indices.
        u32 index_count = 0;
    };

    void ensure_capacity(rhi::Device& device, u32 vertex_count, u32 index_count);
    void append_quad(const Sprite2D& sprite, const Mat4& world, const Vec2& size, const Vec2& pivot_offset);

    rhi::ShaderRef shader_;
    rhi::BufferRef vertex_buffer_;
    rhi::BufferRef index_buffer_;
    u32 vertex_capacity_ = 0;
    u32 index_capacity_ = 0;

    std::vector<Vertex> vertices_;
    std::vector<u32> indices_;
    std::vector<Batch> batches_;
    u32 last_draw_calls_ = 0;

    // A 1x1 white texture used when a sprite has no texture assigned, so an
    // untextured sprite still renders as a solid coloured quad rather than
    // disappearing. Owned here; created lazily on first use.
    rhi::TextureRef white_texture_;
};

} // namespace renderer2d
} // namespace fe
