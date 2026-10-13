// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Error.h"
#include "fe/core/Math.h"
#include "fe/core/Types.h"
#include "fe/mesh/Mesh.h"
#include "fe/rhi/Device.h"

#include <array>
#include <unordered_map>
#include <vector>

namespace fe {

class Scene;
class Node;
class Camera3D;
class MeshRenderer3D;
class DirectionalLight3D;
class PointLight3D;
class SpotLight3D;

namespace renderer3d {

// Draws a Scene's 3D content through the RHI.
//
// Responsibilities:
//   * Walk the scene, collect mesh renderers, and pair each with its world
//     transform.
//   * Reject instances whose world-space bounds lie outside the camera frustum,
//     so they never reach the GPU.
//   * Cache GPU vertex/index buffers per mesh, uploading on first use and
//     evicting buffers for meshes that are no longer alive.
//   * Bind the lit shader once per frame and issue one indexed draw per visible
//     mesh instance.
//
// Non-responsibilities: it does not own the window or the frame, does not
// resolve the depth policy beyond enabling depth testing for its pass, and does
// not know how a mesh was authored. It is a pure consumer of scene data and RHI
// resources, exactly like Renderer2D.
//
// Batching model: for the first 3D cut every visible instance is its own draw
// call. The mesh buffer cache removes the per-frame upload cost, and frustum
// culling removes off-screen instances, which are the two real wins. Instance
// batching and material sorting are later revisions that do not change this
// interface.
//
// Culling model: culling tests each mesh's world-space AABB against the six
// frustum planes and is conservative - a box that straddles a plane is kept - so
// it can only save a draw, never hide a visible mesh. It is a flat pass, not a
// spatial hierarchy: the renderer still visits every node. A tree over static
// geometry is a later revision, and this AABB test is the leaf operation it
// would call.
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

    // The number of instances skipped by frustum culling in the last frame.
    // Draw calls + culled instances is the number of drawable instances seen;
    // both are reported so a test can assert culling happened without guessing.
    [[nodiscard]] u32 last_culled() const noexcept { return last_culled_; }

    // The number of point/spot lights forwarded to the shader in the last
    // frame, after the per-frame cap. Bounded by kMaxPointLights/kMaxSpotLights.
    [[nodiscard]] u32 last_point_lights() const noexcept { return last_point_lights_; }
    [[nodiscard]] u32 last_spot_lights() const noexcept { return last_spot_lights_; }

    // Selects the camera that should drive rendering for `scene`: the first
    // camera with `primary` set, or the first camera found if none is primary,
    // or nullptr if the scene has no 3D camera. Mirrors
    // Renderer2D::find_active_camera so the runtime and the renderer agree on
    // the clear colour.
    [[nodiscard]] static const Camera3D* find_active_camera(const Scene& scene);

    // The first directional light, or nullptr. A scene may have more, but the
    // first is used; a directional light is global, so one is the common case.
    [[nodiscard]] static const DirectionalLight3D* find_directional_light(const Scene& scene);

    // The per-frame light budget. Point and spot lights are forwarded to the
    // shader in fixed-size arrays, so a scene with more lights than this renders
    // with the `kMaxPointLights`/`kMaxSpotLights` brightest (by intensity scaled
    // by range). The cap exists so the shader's loop is bounded and the uniform
    // block has a fixed size; a clustered or tiled light path would lift it
    // without changing the component API.
    static constexpr u32 kMaxPointLights = 8;
    static constexpr u32 kMaxSpotLights = 8;

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
    u32 last_culled_ = 0;
    u32 last_point_lights_ = 0;
    u32 last_spot_lights_ = 0;

    // Reused across frames to avoid per-frame allocation. Each holds one vec4
    // per light in a fixed-size array, packed as the shader expects:
    //   point_light_data_  [i] = position.xyz, range
    //   point_color_data_  [i] = color.rgb,    intensity
    //   spot_light_data_   [i] = position.xyz, range       (i in [0, 8))
    //   spot_light_data_   [i] = direction.xyz, unused     (i in [8, 16))
    //   spot_color_data_   [i] = color.rgb,    intensity
    //   spot_cone_data_    [i] = cos(inner),   cos(outer), 0, 0
    std::array<Vec4, kMaxPointLights> point_light_data_{};
    std::array<Vec4, kMaxPointLights> point_color_data_{};
    std::array<Vec4, kMaxSpotLights * 2> spot_light_data_{};
    std::array<Vec4, kMaxSpotLights> spot_color_data_{};
    std::array<Vec4, kMaxSpotLights> spot_cone_data_{};
};

} // namespace renderer3d
} // namespace fe
