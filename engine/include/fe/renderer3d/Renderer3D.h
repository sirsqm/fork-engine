// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Error.h"
#include "fe/core/Math.h"
#include "fe/core/Types.h"
#include "fe/mesh/Mesh.h"
#include "fe/rhi/Device.h"

#include <unordered_map>
#include <vector>

namespace fe {

class Scene;
class Node;
class Camera3D;
class MeshRenderer3D;
class DirectionalLight3D;

namespace renderer3d {

// Draws a Scene's 3D content through the RHI.
//
// Responsibilities:
//   * Walk the scene, collect mesh renderers, and pair each with its world
//     transform.
//   * Cache GPU vertex/index buffers per mesh, uploading on first use and
//     evicting buffers for meshes that are no longer alive.
//   * Bind the lit shader once per frame and issue one indexed draw per mesh
//     instance.
//
// Non-responsibilities: it does not own the window or the frame, does not
// resolve the depth policy beyond enabling depth testing for its pass, and does
// not know how a mesh was authored. It is a pure consumer of scene data and RHI
// resources, exactly like Renderer2D.
//
// Batching model: for the first 3D cut every instance is its own draw call. The
// mesh buffer cache removes the per-frame upload cost, which is the real win;
// instance batching, material sorting, and a spatial partition are later
// revisions that do not change this interface.
//
// Depth: the 3D pass enables depth testing and writes depth. It expects the
// caller's frame to have a depth buffer available. The window's default
// framebuffer has one; a render target must be created with depth = true.
class Renderer3D {
public:
    Renderer3D();
    ~Renderer3D();

    Renderer3D(const Renderer3D&) = delete;
    Renderer3D& operator=(const Renderer3D&) = delete;

    // Creates the renderer's GPU resources. Returns an Error if shader
    // compilation fails.
    [[nodiscard]] Error initialize(rhi::Device& device);

    // Renders `scene` into the current frame. The caller must have called
    // Device::begin_frame() first. `viewport_width`/`viewport_height` are in
    // pixels and are used to build the camera projection.
    //
    // Returns the number of mesh instances drawn. A return value of 0 is valid
    // (a scene with no 3D content, or no camera).
    [[nodiscard]] Result<usize> render(rhi::Device& device, const Scene& scene, u32 viewport_width,
                                       u32 viewport_height);

    [[nodiscard]] u32 last_draw_calls() const noexcept { return last_draw_calls_; }
    [[nodiscard]] usize cached_mesh_count() const noexcept { return buffers_.size(); }

    // Selects the camera that should drive rendering for `scene`: the first
    // camera with `primary` set, or the first camera found if none is primary,
    // or nullptr if the scene has no 3D camera. Mirrors
    // Renderer2D::find_active_camera so the runtime and the renderer agree on
    // the clear colour.
    [[nodiscard]] static const Camera3D* find_active_camera(const Scene& scene);

    // The first directional light, or nullptr. With one light type there is
    // nothing to choose between; this is the single lookup the renderer needs.
    [[nodiscard]] static const DirectionalLight3D* find_directional_light(const Scene& scene);

private:
    struct GpuMesh {
        rhi::BufferRef vertex_buffer;
        rhi::BufferRef index_buffer;
        u32 index_count = 0;
    };

    // Returns the GPU buffers for `mesh`, uploading on first use.
    [[nodiscard]] Result<const GpuMesh*> acquire_mesh(rhi::Device& device, const Mesh& mesh);

    rhi::ShaderRef shader_;

    // Bound for untextured meshes so the sampler always reads a valid object.
    // A white texel makes the shader's albedo equal to the base colour, which
    // is the expected look for an untextured surface.
    rhi::TextureRef default_texture_;

    // Keyed by the mesh's raw pointer. A mesh is a Resource owned by a
    // shared_ptr; its address is stable while alive. Dead entries are swept at
    // the end of each render(), so a mesh that was unloaded does not leak its
    // buffers.
    std::unordered_map<const Mesh*, GpuMesh> buffers_;

    u32 last_draw_calls_ = 0;
};

} // namespace renderer3d
} // namespace fe
