// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Error.h"
#include "fe/core/Math.h"
#include "fe/core/SpatialBvh.h"
#include "fe/core/Types.h"
#include "fe/mesh/Mesh.h"
#include "fe/render/RenderExtract.h"
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
// Responsibilities, in the order render() performs them:
//   * Extract: walk the scene and emit a flat array of `render::MeshInstance`,
//     each carrying its mesh's world-space AABB.
//   * Cull: reject instances whose world AABB lies outside the camera frustum.
//   * Draw: cache GPU vertex/index buffers per mesh, bind the lit shader, and
//     issue one indexed draw per visible instance.
//
// The first two steps are the extraction seam (ADR-0017, ADR-0052) and are
// public. Extraction also records every mesh seen (culled or not), which the
// buffer sweep uses.
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
// Culling model: culling tests each instance's world-space AABB against the six
// frustum planes and is conservative - a box that straddles a plane is kept - so
// it can only save a draw, never hide a visible mesh. It is backed by a spatial
// hierarchy over the static instances (`SpatialBvh`, ADR-0053): the tree is built
// once and rebuilt only when an instance's world AABB changes, so a moving camera
// over static geometry descends the tree instead of testing every box. The tree
// calls the same `Frustum::intersects_aabb` leaf test the flat pass used, so the
// visible set is identical to the pre-BVH behaviour - only the work to find it
// changes. `cull_flat` is kept as that reference and is what a test compares
// against.
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

    // The number of times the cull BVH was rebuilt from scratch over the
    // lifetime of this renderer. A rebuild is triggered by a change in the
    // instances' world bounds, so a static scene under a moving camera reports a
    // small, constant count - that is the whole point of caching the tree, and
    // it is what a test asserts on rather than a timing.
    [[nodiscard]] u32 bvh_rebuilds() const noexcept { return bvh_rebuilds_; }

    // --- Extraction seam ---------------------------------------------------
    // render() is these three steps in sequence. They are public so a caller
    // (and a test) can run a step in isolation, and so the culling step can be
    // handed a frustum of its own choosing.

    // Step 1: collect the scene's drawable mesh instances into `out` (cleared
    // first), and append every referenced mesh to `meshes_seen` (never cleared
    // here) for the caller's buffer-liveness sweep. No GPU work.
    void extract(const Scene& scene, std::vector<render::MeshInstance>& out,
                 std::vector<const Mesh*>* meshes_seen) const;

    // Step 2: drop instances whose world AABB lies wholly outside `frustum`; a
    // null frustum keeps everything. Returns the number dropped. A pure function
    // of `out`, so it is testable with no scene. Uses the cached BVH, rebuilding
    // it only when the instances' world bounds changed since the last frame.
    [[nodiscard]] usize cull(std::vector<render::MeshInstance>& out, const Frustum* frustum);

    // The flat, hierarchy-free cull: tests every instance's world AABB directly.
    // Kept as the reference the BVH path must match, and as the fallback for a
    // caller that does not want the renderer to hold tree state (a static
    // helper, like the sprite cull). A null `frustum` keeps everything.
    [[nodiscard]] static usize cull_flat(std::vector<render::MeshInstance>& out, const Frustum* frustum);

    // Step 3: acquire mesh buffers and issue the shadow and main passes over
    // `instances`. `view_projection` and `camera_position` come from the active
    // camera, which the caller has already resolved. Returns the number drawn.
    [[nodiscard]] Result<usize> draw(rhi::Device& device, const Scene& scene, const Mat4& view_projection,
                                     const Vec3& camera_position, std::vector<render::MeshInstance>& instances,
                                     const std::vector<const Mesh*>& meshes_seen);

    // The number of point/spot lights forwarded to the shader in the last
    // frame, after the per-frame cap. Bounded by kMaxPointLights/kMaxSpotLights.
    [[nodiscard]] u32 last_point_lights() const noexcept { return last_point_lights_; }
    [[nodiscard]] u32 last_spot_lights() const noexcept { return last_spot_lights_; }

    // The number of mesh instances rendered into the shadow map in the last
    // frame (0 when the scene has no directional light). Reported so a test can
    // prove the depth pass ran without inspecting GPU state.
    [[nodiscard]] u32 last_shadow_casters() const noexcept { return last_shadow_casters_; }

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

    // The directional shadow map is square. 1024 is a deliberate middle ground:
    // enough texel density for a scene of a few units to show a clean edge,
    // small enough to keep the depth pass cheap. A fixed size keeps the first
    // cut free of a quality setting; a resolution knob is a later addition.
    static constexpr u32 kShadowMapSize = 1024;

    // Depth offset used when comparing a fragment against the shadow map, in
    // the map's normalised depth range. Tuned to remove acne on lit surfaces
    // without detaching a shadow from its caster.
    static constexpr f32 kShadowBias = 0.0015f;

private:
    struct GpuMesh {
        rhi::BufferRef vertex_buffer;
        rhi::BufferRef index_buffer;
        u32 index_count = 0;
    };

    // Returns the GPU buffers for `mesh`, uploading on first use.
    [[nodiscard]] Result<const GpuMesh*> acquire_mesh(rhi::Device& device, const Mesh& mesh);

    // Creates the shadow map render target on first use. Returns an Error if the
    // device rejects it; the caller reports it and skips shadows for the frame.
    [[nodiscard]] Error ensure_shadow_target(rhi::Device& device);

    rhi::ShaderRef shader_;

    // Depth-only shader and the render target whose depth texture is the shadow
    // map. The target is created lazily on the first frame with a directional
    // light, so a 2D-only or unlit scene never pays for a 1024x1024 allocation.
    rhi::ShaderRef shadow_shader_;
    rhi::RenderTargetRef shadow_target_;
    bool shadow_target_attempted_ = false;

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
    u32 last_shadow_casters_ = 0;

    // --- Static-geometry cull cache ----------------------------------------
    // The BVH over the instances' world AABBs, and the key list it was built
    // from. `cull` reuses the tree while the key list is unchanged and rebuilds
    // otherwise, so a static scene under a moving camera descends the tree
    // instead of rebuilding it every frame. Both are reused buffers: a rebuild
    // refills them in place rather than reallocating, so a steady-state frame
    // does not grow.
    SpatialBvh cull_bvh_;
    std::vector<render::MeshInstanceKey> cull_keys_;
    std::vector<Aabb> cull_bounds_;
    std::vector<u32> cull_hits_;
    std::vector<bool> cull_visible_;
    bool cull_bvh_valid_ = false;
    u32 bvh_rebuilds_ = 0;

    // Rebuilds `cull_bvh_` from `instances` when their keys differ from the
    // cached ones. Returns true when a rebuild happened.
    bool refresh_cull_bvh(const std::vector<render::MeshInstance>& instances);

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
