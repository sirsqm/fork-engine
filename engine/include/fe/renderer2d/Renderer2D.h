// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Error.h"
#include "fe/core/Math.h"
#include "fe/core/Types.h"
#include "fe/render/RenderExtract.h"
#include "fe/render/RenderGraph.h"
#include "fe/rhi/Device.h"

#include <memory>
#include <span>
#include <vector>

namespace fe {

class FrameArena;
class JobSystem;
class Scene;
class Node;
class Camera2D;
class Sprite2D;
class ResourceManager;

namespace renderer2d {

// Draws a Scene's 2D content through the RHI.
//
// Responsibilities, in the order render() performs them:
//   * Extract: walk the scene and emit a flat array of `render::SpriteInstance`.
//   * Cull: drop sprites whose world AABB lies outside the view frustum.
//   * Draw: sort the survivors into batches, build a vertex/index stream in CPU
//     memory, upload it, and issue one indexed draw per texture batch.
//
// The first two steps are the extraction seam (ADR-0017, ADR-0052) and are
// public, so a caller can extract and cull without drawing and a test can assert
// on the array with no GPU.
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
// Instancing model: every sprite is one *instance* of a single shared quad, so a
// batch uploads one per-instance record per sprite and issues one
// `draw_indexed_instanced` call. The per-sprite CPU cost is the record (the
// world transform's columns, the tint, and the uv rectangle) rather than four
// transformed vertices and six indices, and the vertex and index buffers stop
// growing with the sprite count entirely.
//
// The renderer owns its GPU resources (one shader, one static quad vertex
// buffer, one dynamic instance buffer, one index buffer) and grows the instance
// buffer on demand.
class Renderer2D {
public:
    // One corner of the shared unit quad. The quad is in [-0.5, 0.5] so the
    // instance's world columns carry the sprite's size.
    struct Vertex {
        Vec2 position;
    };

    // One sprite, as the GPU reads it. The world columns are stored with the
    // sprite's size already folded in, so the shader needs no separate size.
    struct Instance {
        Vec4 world_x; // World matrix column 0 * size.x.
        Vec4 world_y; // World matrix column 1 * size.y.
        Vec4 world_z; // World matrix column 3 (translation).
        Vec4 color;
        Vec4 uv; // (u0, v0, u1, v1).
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
    // `jobs` is optional. When a real `JobSystem` is supplied and the scene is
    // large enough to pay for it (see `render::kParallelExtractThreshold`), the
    // extract and cull steps run in parallel; the result is identical to the
    // serial path, so the image does not change. A null pointer (the default) is
    // the serial path, which is what every existing caller gets.
    //
    // Returns the number of sprites drawn, which the example uses to prove the
    // pipeline actually produced geometry. A return value of 0 is valid (an
    // empty scene).
    [[nodiscard]] Result<usize> render(rhi::Device& device, const Scene& scene, u32 viewport_width, u32 viewport_height,
                                       JobSystem* jobs = nullptr);

    // The arena-backed render. Identical output to the overload above, but the
    // extraction output and the cull scratch come from `arena` instead of the
    // heap, so a frame driven by the runtime's per-frame arena performs no heap
    // allocation in the 2D pass (ADR-0054 follow-on).
    //
    // The arena does not grow, so this sizes its destination from
    // `render::sprite_instance_bound(scene)` and treats a short arena as an
    // expected condition: if the arena cannot satisfy the bound, it falls back to
    // the heap path above, so the image is never wrong -- only the allocation
    // profile differs. That keeps the optimisation invisible to correctness.
    [[nodiscard]] Result<usize> render(rhi::Device& device, const Scene& scene, u32 viewport_width, u32 viewport_height,
                                       FrameArena& arena, JobSystem* jobs = nullptr);

    // --- The frame as a graph (ADR-0100) -----------------------------------
    // Runs the 2D frame's passes in a declared order rather than as a fixed
    // sequence of calls: the world pass, then the UI pass over it. The two passes
    // are declared as a `render::RenderGraph` (`ui` after `world`) and run in the
    // order the graph returns, so the frame's ordering is data a caller can
    // extend or reorder instead of the order of two statements in this class.
    //
    // Output is identical to calling `render()` then `render_ui()`.
    // `world_drawn` and `ui_quads_drawn` receive each pass's count, so the two
    // figures stay separate even though `last_draw_calls()` is overwritten by
    // whichever pass ran last.
    //
    // Returns the first error either pass reports.
    [[nodiscard]] Error render_frame(rhi::Device& device, const Scene& scene, u32 viewport_width, u32 viewport_height,
                                     usize& world_drawn, usize& ui_quads_drawn, FrameArena* arena = nullptr,
                                     JobSystem* jobs = nullptr);

    // Number of draw calls issued by the most recent pass -- render() or
    // render_ui(), whichever ran last. Exposed for the example's on-screen
    // statistics and for tests that assert batching. The UI pass overwrites it, so
    // a caller that wants both numbers reads this between the two calls.
    [[nodiscard]] u32 last_draw_calls() const noexcept { return last_draw_calls_; }

    // The number of sprites dropped by frustum culling in the most recent
    // render(). Reported so a test can prove culling happened, and so
    // `drawn + last_culled()` is the number of extracted sprites.
    [[nodiscard]] u32 last_culled() const noexcept { return last_culled_; }

    // --- Extraction seam ---------------------------------------------------
    // render() is these three steps in sequence. They are public so a caller
    // (and a test) can run a step in isolation, and so the culling step can be
    // handed a frustum of its own choosing.

    // Step 1: collect the scene's drawable sprites into `out` (cleared first).
    // No GPU work; the renderer's upload buffers are untouched. `jobs` is passed
    // through to `render::extract_sprites`; null means serial.
    void extract(const Scene& scene, std::vector<render::SpriteInstance>& out, JobSystem* jobs = nullptr) const;

    // Step 2: drop sprites whose quad-sized world AABB lies wholly outside
    // `frustum`; a null frustum keeps everything. Returns the number dropped.
    // A pure function of `out`, so it is testable with no scene. `jobs` is passed
    // through to `render::cull_sprites`; null means serial.
    [[nodiscard]] static usize cull(std::vector<render::SpriteInstance>& out, const Frustum* frustum,
                                    JobSystem* jobs = nullptr);

    // Step 3: build geometry from `instances`, upload, and issue draws. Returns
    // the number of sprites drawn. Does no culling; the caller has done it.
    //
    // Takes a span rather than a vector so the caller can hand it either a
    // `std::vector` (which converts implicitly) or a slice of the frame arena.
    [[nodiscard]] Result<usize> draw(rhi::Device& device, const Scene& scene, u32 viewport_width, u32 viewport_height,
                                     std::span<render::SpriteInstance> instances);

    // --- UI pass (ADR-0084) ------------------------------------------------
    // Draws the scene's screen-space UI over whatever the world pass already drew,
    // with a pixel-space projection of its own. This is the second of the two
    // passes the 2D renderer runs, and it is a separate call rather than part of
    // render() because the two use different matrices: a HUD must not move when the
    // camera pans, so it cannot share the world pass's view-projection.
    //
    // The caller must have called render() first in the same frame: this draws on
    // top of the current frame's contents and does not clear. It extracts through
    // `render::extract_ui`, which lays the UI out with `ui::layout` -- the same
    // call the input system makes -- so the pixels and the hit test cannot diverge.
    //
    // Returns the number of quads drawn (fills, UI textures, and glyphs). Zero is
    // valid: a scene with no canvas draws nothing and costs nothing.
    [[nodiscard]] Result<usize> render_ui(rhi::Device& device, const Scene& scene, u32 viewport_width,
                                          u32 viewport_height, JobSystem* jobs = nullptr);

    // The arena-backed UI pass, identical output to the overload above with the
    // extraction destination sized from the frame arena.
    [[nodiscard]] Result<usize> render_ui(rhi::Device& device, const Scene& scene, u32 viewport_width,
                                          u32 viewport_height, FrameArena& arena, JobSystem* jobs = nullptr);

    [[nodiscard]] u32 vertex_capacity() const noexcept { return vertex_capacity_; }
    [[nodiscard]] u32 index_capacity() const noexcept { return index_capacity_; }

    // Capacity of the per-instance buffer, in instances. The buffer that grows
    // with the sprite count is this one now, not the vertex buffer: the quad is
    // static, so `vertex_capacity()` is a fixed four and this is what a large
    // scene grows. Exposed for tests and the editor's statistics.
    [[nodiscard]] u32 instance_capacity() const noexcept { return instance_capacity_; }

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
    // One contiguous run of instances that share a texture.
    struct Batch {
        rhi::Texture* texture = nullptr;
        u32 instance_offset = 0; // First instance of the run, in the instance buffer.
        u32 instance_count = 0;
    };

    void ensure_capacity(rhi::Device& device, u32 instance_count);
    void append_instance(const render::SpriteInstance& instance);

    // Builds the instance stream from `instances`, uploads it, and issues one
    // indexed draw per texture batch with `view_projection`. Shared by the world
    // pass and the UI pass, which differ only in the matrix and the ordering
    // (`sort_by_texture`). Sets `last_draw_calls_`.
    [[nodiscard]] Result<usize> submit(rhi::Device& device, std::span<render::SpriteInstance> instances,
                                       const Mat4& view_projection, bool sort_by_texture);

    rhi::ShaderRef shader_;
    // The shared quad: four vertices and six indices, built once in initialize()
    // and never changed. Instancing is what makes a fixed quad correct: every
    // sprite is the same geometry, so the vertex and index buffers stop growing
    // with the scene.
    rhi::BufferRef vertex_buffer_;
    rhi::BufferRef index_buffer_;
    // The per-instance buffer. This is the dynamic one; it grows with the sprite
    // count.
    rhi::BufferRef instance_buffer_;
    u32 vertex_capacity_ = 0;
    u32 index_capacity_ = 0;
    u32 instance_capacity_ = 0;

    std::vector<Instance> instances_;
    std::vector<Batch> batches_;
    u32 last_draw_calls_ = 0;
    u32 last_culled_ = 0;

    // A 1x1 white texture used when a sprite has no texture assigned, so an
    // untextured sprite still renders as a solid coloured quad rather than
    // disappearing. Owned here; created lazily on first use.
    rhi::TextureRef white_texture_;
};

} // namespace renderer2d
} // namespace fe
