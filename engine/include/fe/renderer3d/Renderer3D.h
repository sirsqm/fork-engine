// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Error.h"
#include "fe/core/Math.h"
#include "fe/core/SpatialBvh.h"
#include "fe/core/Types.h"
#include "fe/mesh/Mesh.h"
#include "fe/render/RenderExtract.h"
#include "fe/renderer3d/PostProcess.h"
#include "fe/rhi/Device.h"

#include <array>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace fe {

class JobSystem;
class ResourceManager;
class Scene;
class Node;
class Texture2D;
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
// Batching model: instances that share a mesh, both material textures, and the
// per-batch shading state (base colour, specular, shininess, emissive) are
// drawn in one instanced call. The instance's model and normal matrices travel
// in a per-instance vertex stream, so the transform is no longer a uniform and
// one call can cover many instances. The mesh buffer cache removes the per-frame
// upload cost and frustum culling removes off-screen instances; instancing then
// removes the per-instance draw call, which is what lets a scene of repeated
// meshes (a floor of tiles, a forest of one tree) hold a frame budget. A scene
// whose instances differ in material still batches per run, so the draw-call
// count is bounded by the number of distinct (mesh, material) pairs, not the
// instance count.
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

    // Supplies the resource manager material texture references resolve through.
    // Optional: a renderer with none simply cannot resolve a material's texture
    // maps and falls back to the component's own texture or the default texel,
    // which is what a standalone test wants. The Engine sets this on the
    // renderer it owns.
    void set_resource_manager(ResourceManager* resources) noexcept { resources_ = resources; }

    // Renders `scene` into the current frame. The caller must have called
    // Device::begin_frame() first. `viewport_width`/`viewport_height` are in
    // pixels and are used to build the camera projection.
    //
    // Returns the number of mesh instances drawn. A return value of 0 is valid
    // (a scene with no 3D content, or no camera).
    //
    // `settings` opts into the post-processing chain. With `settings.enabled`
    // false this is the pre-chain behaviour exactly.
    //
    // `jobs` is optional. When supplied and the scene is large enough to pay for
    // it, extraction runs in parallel (see `render::kParallelExtractThreshold`);
    // the result is identical to the serial path. The cull keeps its BVH, which
    // is already sub-linear, so a pool does not change it here. A null pointer
    // (the default) is the serial path every existing caller gets.
    [[nodiscard]] Result<usize> render(rhi::Device& device, const Scene& scene, u32 viewport_width, u32 viewport_height,
                                       const PostProcessSettings& settings = {}, JobSystem* jobs = nullptr);

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
    // here) for the caller's buffer-liveness sweep. No GPU work. `jobs` is passed
    // through to `render::extract_meshes`; null means serial.
    void extract(const Scene& scene, std::vector<render::MeshInstance>& out, std::vector<const Mesh*>* meshes_seen,
                 JobSystem* jobs = nullptr) const;

    // Step 2: drop instances whose world AABB lies wholly outside `frustum`; a
    // null frustum keeps everything. Returns the number dropped. A pure function
    // of `out`, so it is testable with no scene. Uses the cached BVH, rebuilding
    // it only when the instances' world bounds changed since the last frame.
    [[nodiscard]] usize cull(std::vector<render::MeshInstance>& out, const Frustum* frustum);

    // The flat, hierarchy-free cull: tests every instance's world AABB directly.
    // Kept as the reference the BVH path must match, and as the fallback for a
    // caller that does not want the renderer to hold tree state (a static
    // helper, like the sprite cull). A null `frustum` keeps everything. `jobs`
    // parallelises the test above `render::kParallelCullThreshold`; null means
    // serial. It is a static helper, so unlike the BVH cull it holds no state and
    // is safe to run from a caller that has no renderer instance.
    [[nodiscard]] static usize cull_flat(std::vector<render::MeshInstance>& out, const Frustum* frustum,
                                         JobSystem* jobs = nullptr);

    // Step 3: acquire mesh buffers and issue the shadow and main passes over
    // `instances`. `view_projection` and `camera_position` come from the active
    // camera, which the caller has already resolved. Returns the number drawn.
    [[nodiscard]] Result<usize> draw(rhi::Device& device, const Scene& scene, const Mat4& view_projection,
                                     const Vec3& camera_position, std::vector<render::MeshInstance>& instances,
                                     const std::vector<const Mesh*>& meshes_seen);

    // The post-processing chain this renderer drives. Exposed so a caller can
    // inspect `pass_count()` and so tests can assert the chain ran.
    [[nodiscard]] PostProcess& post_process() noexcept { return post_process_; }

    // The number of frames in which the post-processing chain drew. A subset of
    // the frames `render` ran, since the chain is opt-in.
    [[nodiscard]] u32 post_process_frames() const noexcept { return post_process_frames_; }

    // The number of background gradient passes drawn over the lifetime of this
    // renderer. Reported so a test can prove the environment pass ran without
    // reading back pixels (the 3D pass resets `last_draw_calls_`, so the
    // background cannot be counted there).
    [[nodiscard]] u32 background_draws() const noexcept { return background_draws_; }

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
    // The renderer picks it up on its per-node light gather rather than walking
    // the scene a second time, so there is no standalone lookup to call: the
    // first directional light in the gather is the one that lights the frame.

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

    // The per-batch shader state: every field a draw sets once per batch rather
    // than once per instance. Two instances may share a draw call only when all
    // of these are equal and they use the same mesh and textures, which is what
    // makes the key a batch is grouped by.
    struct BatchState {
        Vec4 base_color{};
        f32 specular = 0.0f;
        f32 shininess = 0.0f;
        Vec3 emissive{};

        [[nodiscard]] constexpr bool operator==(const BatchState&) const noexcept = default;
    };

    // One instance's resolved draw data, before grouping: the instance itself,
    // the textures its material resolves to, and its batch state.
    struct DrawItem {
        const render::MeshInstance* instance = nullptr;
        const Texture2D* base_texture = nullptr;
        const Texture2D* emissive_texture = nullptr;
        BatchState state;
    };

    // A run of consecutive draw items sharing a mesh, both textures, and the
    // batch state, so the whole run is one instanced draw call.
    // `instance_offset`/`instance_count` index the frame's instance stream.
    struct Batch {
        const Mesh* mesh = nullptr;
        const Texture2D* base_texture = nullptr;
        const Texture2D* emissive_texture = nullptr;
        BatchState state;
        u32 instance_offset = 0;
        u32 instance_count = 0;
    };

    // The per-instance record uploaded for an instanced mesh draw. Its layout is
    // VertexLayout::mesh_instance(): the model matrix's four columns then the
    // normal matrix's three, each padded to a vec4. The padding components are
    // written but never read by the shader.
    struct Instance {
        Vec4 model[4];
        Vec4 normal[3];
    };

    // Builds this frame's instance stream and batches from `instances`: resolves
    // each instance's surface and textures, groups instances that share every
    // per-batch field, and fills `frame_instance_data_` and `frame_batches_`.
    // Reuses both buffers across frames, so a steady-state frame does not grow.
    void build_batches(const std::vector<render::MeshInstance>& instances);

    // Ensures the shared instance buffer can hold `count` records, growing it on
    // demand. A no-op when the current capacity already covers `count`.
    void ensure_instance_capacity(rhi::Device& device, u32 count);

    // A point or spot light gathered from the scene for the shader, before the
    // per-frame cap. Named here rather than locally in draw() so the gathered
    // lists can be reused buffers (see frame_point_lights_ below).
    struct PointLightEntry {
        Vec3 position;
        Vec3 color;
        f32 intensity;
        f32 range;
        f32 brightness;
    };
    struct SpotLightEntry {
        Vec3 position;
        Vec3 direction; // Direction the light travels.
        Vec3 color;
        f32 intensity;
        f32 range;
        f32 cos_inner;
        f32 cos_outer;
        f32 brightness;
    };

    // Returns the GPU buffers for `mesh`, uploading on first use.
    [[nodiscard]] Result<const GpuMesh*> acquire_mesh(rhi::Device& device, const Mesh& mesh);

    // Resolves a material texture reference ("tex:assets/hero.png") to a loaded
    // Texture2D, or nullptr when it cannot be loaded. The result is memoised per
    // frame so a material shared by many instances resolves once; a reference
    // that fails to load is remembered as a miss so a typo does not retry every
    // instance.
    [[nodiscard]] const Texture2D* load_texture(const std::string& reference);

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

    // Runs the 3D pass, optionally wrapping it in the post-processing chain. When
    // `settings.enabled` is false this is `draw()`; when it is true the pass
    // renders into an off-screen colour target and the chain grades that image
    // into the caller's target. `width`/`height` are the viewport size.
    //
    // The camera's gradient background (when enabled) is drawn here, after the
    // destination target is bound, so it lands in whichever image the pass draws
    // into and is graded along with the geometry.
    [[nodiscard]] Result<usize> draw_chain(rhi::Device& device, const Scene& scene, const Mat4& view_projection,
                                           const Vec3& camera_position, const Camera3D& camera,
                                           std::vector<render::MeshInstance>& instances,
                                           const std::vector<const Mesh*>& meshes_seen,
                                           const PostProcessSettings& settings, u32 width, u32 height);

    // Ensures the off-screen colour target matches `width`x`height`, creating it
    // on first use and recreating it on a size change. Returns an Error if the
    // device rejects it.
    [[nodiscard]] Error ensure_scene_target(rhi::Device& device, u32 width, u32 height);

    // Draws the camera's gradient background on a full-screen quad, before the
    // geometry, using the now-current viewport size for the gradient axis.
    void draw_background(rhi::Device& device, const Camera3D& camera);

    // The background pass' shader and quad, created lazily the first time a camera
    // enables `background_enabled`, so a scene that never asks for a background
    // never pays for them.
    [[nodiscard]] Error ensure_background(rhi::Device& device);

    rhi::ShaderRef background_shader_;
    rhi::BufferRef background_quad_vertex_buffer_;
    rhi::BufferRef background_quad_index_buffer_;
    bool background_attempted_ = false;

    // The post-processing chain, and the off-screen target the 3D pass renders
    // into when the chain is enabled.
    PostProcess post_process_;
    rhi::RenderTargetRef scene_target_;
    u32 scene_target_width_ = 0;
    u32 scene_target_height_ = 0;
    u32 post_process_frames_ = 0;
    u32 background_draws_ = 0;

    // The resource manager material textures resolve through, borrowed from the
    // Engine that created this renderer. Null when the renderer was constructed
    // with no manager (a test that builds one directly), in which case a material
    // texture cannot be resolved and the draw falls back to the component's own
    // texture or the default white texel.
    ResourceManager* resources_ = nullptr;

    // The per-frame texture-resolution memo: reference -> loaded texture (null
    // when the reference failed to load). Cleared at the top of each draw, so a
    // hot-reloaded asset is picked up on the next frame rather than pinned.
    std::unordered_map<std::string, const Texture2D*> texture_cache_;

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

    // --- Per-frame scratch, reused across frames ---------------------------
    // render() runs every frame and its working set -- the extracted instance
    // array, the mesh-liveness list, and the gathered light lists -- has the same
    // shape from one frame to the next. Holding them as members means a
    // steady-state frame reuses capacity instead of allocating, which is what
    // keeps a 3D game's frame time free of allocator noise. They are cleared, not
    // freed, at the top of each use; the extracted instances in particular are
    // cleared by extract(), which is the documented contract of that step.
    //
    // A caller that wants to run render()'s steps itself still passes its own
    // vectors to extract()/cull()/draw(); these buffers only serve the
    // single-call render() path.
    std::vector<render::MeshInstance> frame_instances_;
    std::vector<const Mesh*> frame_meshes_seen_;
    std::vector<PointLightEntry> frame_point_lights_;
    std::vector<SpotLightEntry> frame_spot_lights_;

    // The instanced draw path's per-frame working set, reused across frames for
    // the same reason as the buffers above. `frame_instance_data_` is the record
    // stream uploaded to the shared instance buffer; `frame_batches_` groups it
    // into one draw call per (mesh, textures, batch state) run.
    std::vector<Instance> frame_instance_data_;
    std::vector<Batch> frame_batches_;

    // The shared per-instance buffer the mesh draws read from, and its capacity
    // in records. Created lazily on the first frame that has a drawable instance,
    // so a scene with no 3D content never allocates it.
    rhi::BufferRef instance_buffer_;
    u32 instance_capacity_ = 0;

    // The live mesh set for one frame's buffer sweep, reused across frames so the
    // sweep does not allocate. Rebuilt from frame_meshes_seen_ each frame; see
    // the sweep at the end of draw().
    std::unordered_set<const Mesh*> live_meshes_;
};

} // namespace renderer3d
} // namespace fe
