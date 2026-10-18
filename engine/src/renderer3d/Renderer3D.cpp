// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/renderer3d/Renderer3D.h"

#include "fe/core/Assert.h"
#include "fe/core/Frustum.h"
#include "fe/core/Log.h"
#include "fe/resources/Texture2D.h"
#include "fe/scene/Node.h"
#include "fe/scene/Scene.h"
#include "fe/scene3d/Camera3D.h"
#include "fe/scene3d/DirectionalLight3D.h"
#include "fe/scene3d/MeshRenderer3D.h"
#include "fe/scene3d/PointLight3D.h"
#include "fe/scene3d/SpotLight3D.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <span>

namespace fe::renderer3d {
namespace {

constexpr const char* kVertexShaderSource = R"glsl(
#version 330 core

layout(location = 0) in vec3 a_position;
layout(location = 1) in vec3 a_normal;
layout(location = 2) in vec2 a_uv;

uniform mat4 u_view_projection;
uniform mat4 u_model;
uniform mat3 u_normal_matrix;

out vec3 v_world_position;
out vec3 v_normal;
out vec2 v_uv;

void main() {
    vec4 world = u_model * vec4(a_position, 1.0);
    v_world_position = world.xyz;
    v_normal = u_normal_matrix * a_normal;
    v_uv = a_uv;
    gl_Position = u_view_projection * world;
}
)glsl";

constexpr const char* kFragmentShaderSource = R"glsl(
#version 330 core

in vec3 v_world_position;
in vec3 v_normal;
in vec2 v_uv;

uniform sampler2D u_texture;
uniform vec4 u_base_color;
uniform float u_specular;

uniform vec3 u_camera_position;

// Directional light: global, no position.
uniform vec3 u_light_direction;  // Direction the light travels.
uniform vec3 u_light_color;
uniform float u_light_intensity;
uniform float u_ambient;

// Point lights. Each element packs position.xyz and range in .w.
uniform int u_point_light_count;
uniform vec4 u_point_light_position_range[8];
uniform vec4 u_point_light_color_intensity[8];  // rgb + intensity.

// Spot lights. Each light uses one vec4 in each of three arrays.
//   u_spot_light_data[i]        = position.xyz, range in .w
//   u_spot_light_data[i + 8]    = direction.xyz (the direction the light travels)
//   u_spot_light_color_intensity[i] = color.rgb, intensity in .w
//   u_spot_light_cone[i]        = cos(inner half-angle), cos(outer half-angle), 0, 0
uniform int u_spot_light_count;
uniform vec4 u_spot_light_data[16];
uniform vec4 u_spot_light_color_intensity[8];
uniform vec4 u_spot_light_cone[8];

out vec4 frag_color;

// Shadow map: depth rendered from the directional light's point of view. The
// comparison is done by hand (compare mode NONE) so the shader stays
// backend-agnostic; `u_light_space_matrix` maps world space into the map's
// clip space, and its .xyz/.w gives the depth the light saw at this fragment.
uniform sampler2D u_shadow_map;
uniform mat4 u_light_space_matrix;
uniform float u_shadow_bias;

// 1.0 where the fragment is lit, 0.0 where it is occluded. Samples a 3x3
// neighbourhood and averages the pass/fail, which softens the hard texel edge
// enough to read as a shadow rather than a staircase.
float shadow_factor(vec3 world_position, vec3 to_light) {
    vec4 light_space = u_light_space_matrix * vec4(world_position, 1.0);
    vec3 projected = light_space.xyz / light_space.w;
    projected = projected * 0.5 + 0.5; // Clip [-1, 1] to texture [0, 1].

    // Outside the map there is no occlusion information, so treat it as lit.
    if (projected.x < 0.0 || projected.x > 1.0 || projected.y < 0.0 || projected.y > 1.0 || projected.z > 1.0) {
        return 1.0;
    }

    // Slope-scaled bias: a surface nearly edge-on to the light needs a larger
    // offset to avoid self-shadowing acne.
    float slope = max(1.0 - dot(to_light, vec3(0.0, 0.0, 1.0)), 0.5);
    float bias = u_shadow_bias * slope;

    float occluded = 0.0;
    vec2 texel = 1.0 / vec2(textureSize(u_shadow_map, 0));
    for (int x = -1; x <= 1; ++x) {
        for (int y = -1; y <= 1; ++y) {
            float closest = texture(u_shadow_map, projected.xy + vec2(float(x), float(y)) * texel).r;
            occluded += projected.z - bias > closest ? 1.0 : 0.0;
        }
    }
    return 1.0 - occluded / 9.0;
}

// Blinn-Phong diffuse + specular for a light arriving along `to_light`
// (unit vector towards the light) with the given colour and distance falloff.
vec3 shade(vec3 n, vec3 v, vec3 to_light, vec3 light_color, float attenuation) {
    float diffuse = max(dot(n, to_light), 0.0);
    vec3 h = normalize(to_light + v);
    float spec = pow(max(dot(n, h), 0.0), 32.0) * u_specular;
    return light_color * (diffuse + spec) * attenuation;
}

void main() {
    vec4 albedo = texture(u_texture, v_uv) * u_base_color;

    vec3 n = normalize(v_normal);
    vec3 v = normalize(u_camera_position - v_world_position);

    // Ambient plus the directional light. Only the directional light casts a
    // shadow; ambient stays unshadowed so occluded areas keep some fill.
    vec3 l = normalize(-u_light_direction);
    float shadow = shadow_factor(v_world_position, l);
    vec3 lighting = vec3(u_ambient) + shadow * shade(n, v, l, u_light_color * u_light_intensity, 1.0);

    // Point lights, with an inverse-square falloff windowed to reach zero at
    // the light's range so a light has a bounded influence.
    for (int i = 0; i < u_point_light_count && i < 8; ++i) {
        vec3 to_light = u_point_light_position_range[i].xyz - v_world_position;
        float distance = length(to_light);
        float range = u_point_light_position_range[i].w;
        if (range <= 0.0) {
            continue;
        }
        float window = max(1.0 - distance / range, 0.0);
        float attenuation = window * window;
        lighting += shade(n, v, to_light / max(distance, 1e-4), u_point_light_color_intensity[i].rgb *
                          u_point_light_color_intensity[i].a, attenuation);
    }

    // Spot lights: a point light gated by a smooth cone.
    for (int i = 0; i < u_spot_light_count && i < 8; ++i) {
        vec3 to_light = u_spot_light_data[i].xyz - v_world_position;
        float distance = length(to_light);
        float range = u_spot_light_data[i].w;
        if (range <= 0.0) {
            continue;
        }
        vec3 light_dir = u_spot_light_data[i + 8].xyz;   // Direction the light travels.
        float cos_inner = u_spot_light_cone[i].x;
        float cos_outer = u_spot_light_cone[i].y;

        float cos_angle = dot(normalize(-to_light), light_dir);
        // Smoothstep between the outer and inner cosine gives a soft edge. The
        // division is guarded: the renderer keeps outer < inner, but a hand-set
        // equal pair must not divide by zero.
        float denom = max(cos_inner - cos_outer, 1e-4);
        float cone = clamp((cos_angle - cos_outer) / denom, 0.0, 1.0);

        float window = max(1.0 - distance / range, 0.0);
        float attenuation = window * window * cone;
        lighting += shade(n, v, to_light / max(distance, 1e-4), u_spot_light_color_intensity[i].rgb *
                          u_spot_light_color_intensity[i].a, attenuation);
    }

    frag_color = vec4(albedo.rgb * lighting, albedo.a);
}
)glsl";

// Depth-only pass. Renders the scene from the light's point of view; the depth
// buffer is the only output the shadow map needs. There is no colour work and
// no fragment output, so the driver can discard the fragment stage entirely.
constexpr const char* kShadowVertexShaderSource = R"glsl(
#version 330 core

layout(location = 0) in vec3 a_position;

uniform mat4 u_light_view_projection;
uniform mat4 u_model;

void main() {
    gl_Position = u_light_view_projection * u_model * vec4(a_position, 1.0);
}
)glsl";

constexpr const char* kShadowFragmentShaderSource = R"glsl(
#version 330 core

void main() {
}
)glsl";

// The environment background: a full-screen quad whose fragment colour is a
// vertical gradient from `u_bottom_color` at the bottom of the frame to
// `u_top_color` at the top. The quad's position is already in clip space, so no
// camera matrix is needed and the gradient follows the screen rather than the
// camera's pitch, which is the cheap, always-correct behaviour for a first cut.
constexpr const char* kBackgroundVertexShaderSource = R"glsl(
#version 330 core

layout(location = 0) in vec2 a_position;
layout(location = 1) in vec2 a_uv;

out vec2 v_uv;

void main() {
    v_uv = a_uv;
    gl_Position = vec4(a_position, 0.0, 1.0);
}
)glsl";

constexpr const char* kBackgroundFragmentShaderSource = R"glsl(
#version 330 core

in vec2 v_uv;

uniform vec3 u_top_color;
uniform vec3 u_bottom_color;

out vec4 frag_color;

void main() {
    frag_color = vec4(mix(u_bottom_color, u_top_color, v_uv.y), 1.0);
}
)glsl";

// Two triangles covering clip space, UV (0,0) at the bottom-left so the gradient
// runs bottom to top.
constexpr std::array<f32, 16> kBackgroundQuadVertices = {
    // position.xy   uv.xy
    -1.0f, -1.0f, 0.0f, 0.0f, 1.0f, -1.0f, 1.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, -1.0f, 1.0f, 0.0f, 1.0f,
};

constexpr std::array<u32, 6> kBackgroundQuadIndices = {0, 1, 2, 0, 2, 3};

// The normal matrix is transpose(inverse(upper-left 3x3 of model)), so a
// non-uniform scale does not skew normals. Returned in column-major order,
// which is what the RHI uploads.
[[nodiscard]] std::array<f32, 9> normal_matrix(const Mat4& model) noexcept {
    const Mat4 inv = inverse(model);
    std::array<f32, 9> out{};
    for (usize col = 0; col < 3; ++col) {
        for (usize row = 0; row < 3; ++row) {
            // transpose(inv).at(row, col) == inv.at(col, row)
            out[col * 3 + row] = inv.at(col, row);
        }
    }
    return out;
}

// The world-space AABB of a local AABB under `model` now lives in the shared
// extraction seam (`fe/render/RenderExtract.h`), because extraction bakes it
// into each `render::MeshInstance` and the culling pass is a pure function of
// that array. See ADR-0052.

// Builds a world-to-light-clip matrix for a directional shadow map that covers
// the given world AABB.
//
// A directional light has no position, so the light "eye" is placed back along
// the light direction far enough that the whole scene sits in front of it. The
// orthographic box is square and centred on the scene, sized from the scene's
// bounding sphere: the sphere radius is view-independent, so the coverage does
// not shrink when the scene is viewed edge-on, which an AABB fit would.
//
// One cascade over the whole scene is the deliberate first cut: it is correct
// for the common "sun over a scene" case and needs no frustum fitting. The cost
// is resolution - a large scene shares one map - which cascaded shadow maps
// would fix later without changing this call site.
[[nodiscard]] Mat4 directional_light_view_projection(const Vec3& scene_min, const Vec3& scene_max,
                                                     const Vec3& light_direction) noexcept {
    const Vec3 center = (scene_min + scene_max) * 0.5f;
    const Vec3 extent = (scene_max - scene_min) * 0.5f;
    const f32 radius = std::max(extent.length(), 1e-3f);

    // `light_direction` is the direction the light travels, so the eye sits
    // opposite it, one full diameter back plus a margin for geometry that sits
    // above the scene box.
    const Vec3 to_light = light_direction.normalized() * -1.0f;
    const Vec3 eye = center + to_light * (radius * 2.0f + 1.0f);

    // Pick an up vector that is not parallel to the light, or look_at degenerates.
    const Vec3 up = std::abs(to_light.y) > 0.99f ? Vec3{0.0f, 0.0f, 1.0f} : Vec3{0.0f, 1.0f, 0.0f};

    const Mat4 view = look_at(eye, center, up);
    const Mat4 projection = Mat4::orthographic(-radius, radius, -radius, radius, 0.01f, radius * 4.0f + 2.0f);
    return projection * view;
}

} // namespace

Renderer3D::Renderer3D() = default;
Renderer3D::~Renderer3D() = default;

Error Renderer3D::initialize(rhi::Device& device) {
    rhi::ShaderDesc desc;
    desc.name = "renderer3d_lit";
    desc.vertex_source = kVertexShaderSource;
    desc.fragment_source = kFragmentShaderSource;

    auto shader = device.create_shader(desc);
    if (!shader) {
        return shader.error();
    }
    shader_ = shader.value();

    rhi::ShaderDesc shadow_desc;
    shadow_desc.name = "renderer3d_shadow";
    shadow_desc.vertex_source = kShadowVertexShaderSource;
    shadow_desc.fragment_source = kShadowFragmentShaderSource;
    auto shadow_shader = device.create_shader(shadow_desc);
    if (!shadow_shader) {
        return shadow_shader.error();
    }
    shadow_shader_ = shadow_shader.value();

    const u8 white[4] = {255, 255, 255, 255};
    rhi::TextureDesc white_desc;
    white_desc.name = "renderer3d_white";
    white_desc.width = 1;
    white_desc.height = 1;
    white_desc.pixels = white;
    auto white_texture = device.create_texture(white_desc);
    if (!white_texture) {
        return white_texture.error();
    }
    default_texture_ = white_texture.value();

    return Error::none();
}

const Camera3D* Renderer3D::find_active_camera(const Scene& scene) {
    const Camera3D* fallback = nullptr;
    const Camera3D* found = nullptr;
    scene.for_each_node([&](Node& node) {
        if (const auto* camera = node.get_component<Camera3D>(); camera != nullptr) {
            if (fallback == nullptr) {
                fallback = camera;
            }
            if (camera->primary && found == nullptr) {
                found = camera;
            }
        }
    });
    return found != nullptr ? found : fallback;
}

const DirectionalLight3D* Renderer3D::find_directional_light(const Scene& scene) {
    const DirectionalLight3D* found = nullptr;
    scene.for_each_node([&](Node& node) {
        if (found == nullptr) {
            found = node.get_component<DirectionalLight3D>();
        }
    });
    return found;
}

Result<const Renderer3D::GpuMesh*> Renderer3D::acquire_mesh(rhi::Device& device, const Mesh& mesh) {
    const auto it = buffers_.find(&mesh);
    if (it != buffers_.end()) {
        return &it->second;
    }

    if (mesh.vertex_count() == 0 || mesh.index_count() == 0) {
        return Error("renderer3d: mesh has no geometry");
    }

    auto vertex_buffer = device.create_vertex_buffer(rhi::VertexLayout::position_normal_uv(),
                                                     static_cast<u32>(mesh.vertex_count() * sizeof(Vertex3D)));
    if (!vertex_buffer) {
        return vertex_buffer.error();
    }
    auto index_buffer = device.create_index_buffer(static_cast<u32>(mesh.index_count()));
    if (!index_buffer) {
        return index_buffer.error();
    }

    GpuMesh gpu{};
    gpu.vertex_buffer = vertex_buffer.value();
    gpu.index_buffer = index_buffer.value();
    gpu.index_count = static_cast<u32>(mesh.index_count());

    gpu.vertex_buffer->update(std::span<const u8>(reinterpret_cast<const u8*>(mesh.vertices().data()),
                                                  mesh.vertices().size() * sizeof(Vertex3D)));
    gpu.index_buffer->update(
        std::span<const u8>(reinterpret_cast<const u8*>(mesh.indices().data()), mesh.indices().size() * sizeof(u32)));

    const auto [inserted, _] = buffers_.emplace(&mesh, std::move(gpu));
    return &inserted->second;
}

Error Renderer3D::ensure_shadow_target(rhi::Device& device) {
    if (shadow_target_ != nullptr || shadow_target_attempted_) {
        return Error::none();
    }
    shadow_target_attempted_ = true;

    rhi::RenderTargetDesc desc;
    desc.name = "renderer3d_shadow_map";
    desc.width = kShadowMapSize;
    desc.height = kShadowMapSize;
    desc.depth = true;
    desc.sample_depth = true; // The main pass samples this depth; a renderbuffer would not do.
    auto target = device.create_render_target(desc);
    if (!target) {
        return target.error();
    }
    shadow_target_ = target.value();
    return Error::none();
}

void Renderer3D::extract(const Scene& scene, std::vector<render::MeshInstance>& out,
                         std::vector<const Mesh*>* meshes_seen, JobSystem* jobs) const {
    render::extract_meshes(scene, out, meshes_seen, jobs);
}

usize Renderer3D::cull(std::vector<render::MeshInstance>& out, const Frustum* frustum) {
    if (frustum == nullptr) {
        return 0;
    }
    if (out.empty()) {
        cull_bvh_valid_ = false;
        return 0;
    }

    refresh_cull_bvh(out);

    // Query the tree, then drop everything it did not return. The BVH's frustum
    // query is the same conservative `Frustum::intersects_aabb` test the flat
    // pass used, so the surviving set is identical; the tree only skips the
    // boxes a rejected subtree would have tested one by one. The output is
    // sorted by primitive index, which is the extraction order, so the draw
    // order - and therefore the rendered image - is unchanged.
    cull_bvh_.query(*frustum, cull_hits_);
    cull_visible_.assign(out.size(), false);
    for (u32 index : cull_hits_) {
        cull_visible_[index] = true;
    }

    const usize before = out.size();
    usize write = 0;
    for (usize read = 0; read < before; ++read) {
        if (cull_visible_[read]) {
            out[write++] = out[read];
        }
    }
    out.resize(write);
    return before - write;
}

usize Renderer3D::cull_flat(std::vector<render::MeshInstance>& out, const Frustum* frustum, JobSystem* jobs) {
    return render::cull_meshes(out, frustum, jobs);
}

bool Renderer3D::refresh_cull_bvh(const std::vector<render::MeshInstance>& instances) {
    const usize count = instances.size();
    if (cull_bvh_valid_ && cull_keys_.size() == count) {
        bool same = true;
        for (usize i = 0; i < count; ++i) {
            if (!(cull_keys_[i] == render::instance_key(instances[i]))) {
                same = false;
                break;
            }
        }
        if (same) {
            return false;
        }
    }

    cull_bounds_.resize(count);
    cull_keys_.resize(count);
    for (usize i = 0; i < count; ++i) {
        const render::MeshInstance& instance = instances[i];
        cull_bounds_[i] = Aabb{instance.world_min, instance.world_max};
        cull_keys_[i] = render::instance_key(instance);
    }
    cull_bvh_.build(cull_bounds_);
    cull_bvh_valid_ = true;
    ++bvh_rebuilds_;
    return true;
}

Result<usize> Renderer3D::render(rhi::Device& device, const Scene& scene, u32 viewport_width, u32 viewport_height,
                                 const PostProcessSettings& settings, JobSystem* jobs) {
    FE_CHECK(shader_ != nullptr);
    FE_CHECK(viewport_width > 0 && viewport_height > 0);

    last_draw_calls_ = 0;
    last_culled_ = 0;
    last_point_lights_ = 0;
    last_spot_lights_ = 0;
    last_shadow_casters_ = 0;

    const Camera3D* camera = find_active_camera(scene);
    if (camera == nullptr) {
        return usize{0};
    }

    // --- 1. Extract --------------------------------------------------------
    std::vector<render::MeshInstance> instances;
    std::vector<const Mesh*> meshes_seen;
    extract(scene, instances, &meshes_seen, jobs);

    // --- 2. Cull -----------------------------------------------------------
    // Reject instances whose world AABB lies wholly outside the view. The test is
    // conservative, so this can only save a draw. The frustum is built from the
    // same matrix the draw uses, so the two can never disagree.
    const Mat4 view_projection = camera->view_projection(viewport_width, viewport_height);
    const Frustum frustum = Frustum::from_view_projection(view_projection);
    last_culled_ = static_cast<u32>(cull(instances, &frustum));

    // --- 3. Draw -----------------------------------------------------------
    // The camera's background gradient and the post-processing chain are handled
    // inside `draw_chain`, because both need the destination target bound.
    return draw_chain(device, scene, view_projection, camera->eye(), *camera, instances, meshes_seen, settings,
                      viewport_width, viewport_height);
}

Result<usize> Renderer3D::draw(rhi::Device& device, const Scene& scene, const Mat4& view_projection,
                               const Vec3& camera_position, std::vector<render::MeshInstance>& instances,
                               const std::vector<const Mesh*>& meshes_seen) {
    FE_CHECK(shader_ != nullptr);

    last_draw_calls_ = 0;
    last_point_lights_ = 0;
    last_spot_lights_ = 0;
    last_shadow_casters_ = 0;

    if (instances.empty()) {
        return usize{0};
    }

    // --- Frame constants ----------------------------------------------------
    // Point and spot lights need their node's world position (and, for spots,
    // the node's forward direction), so they are collected here where the node
    // is still in hand. Brightness is intensity scaled by range, which is a
    // cheap proxy for influence used only to pick which lights survive the cap.
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
    std::vector<PointLightEntry> point_lights;
    std::vector<SpotLightEntry> spot_lights;

    scene.for_each_node([&](Node& node) {
        const Mat4 world = node.world_matrix();
        if (const auto* light = node.get_component<PointLight3D>(); light != nullptr) {
            point_lights.push_back(PointLightEntry{transform_point(world, Vec3{}), light->color, light->intensity,
                                                   light->range, light->intensity * light->range});
        }
        if (const auto* light = node.get_component<SpotLight3D>(); light != nullptr) {
            // The node's local -Z is the cone axis, matching Camera3D. A zero
            // scale would collapse it, so normalise defensively.
            const Vec3 forward = transform_direction(world, Vec3{0.0f, 0.0f, -1.0f}).normalized();
            const f32 inner = radians(light->inner_cone_degrees);
            const f32 outer = radians(light->outer_cone_degrees);
            spot_lights.push_back(SpotLightEntry{transform_point(world, Vec3{}), forward, light->color,
                                                 light->intensity, light->range, std::cos(inner), std::cos(outer),
                                                 light->intensity * light->range});
        }
    });

    const auto by_brightness = [](const auto& a, const auto& b) { return a.brightness > b.brightness; };
    if (point_lights.size() > kMaxPointLights) {
        std::nth_element(point_lights.begin(), point_lights.begin() + kMaxPointLights, point_lights.end(),
                         by_brightness);
        point_lights.resize(kMaxPointLights);
    }
    if (spot_lights.size() > kMaxSpotLights) {
        std::nth_element(spot_lights.begin(), spot_lights.begin() + kMaxSpotLights, spot_lights.end(), by_brightness);
        spot_lights.resize(kMaxSpotLights);
    }

    last_point_lights_ = static_cast<u32>(point_lights.size());
    last_spot_lights_ = static_cast<u32>(spot_lights.size());

    const DirectionalLight3D* light = find_directional_light(scene);
    const Vec3 light_direction = light != nullptr ? light->direction.normalized() : Vec3{0.0f, -1.0f, 0.0f};
    const Vec3 light_color = light != nullptr ? light->color : Vec3{1.0f, 1.0f, 1.0f};
    const f32 light_intensity = light != nullptr ? light->intensity : 1.0f;
    const f32 ambient = light != nullptr ? light->ambient : 0.15f;

    u32 drawn = 0;

    // --- 3b. Shadow pass ----------------------------------------------------
    // Render the visible geometry's depth from the light's point of view into
    // the shadow map, then restore the main pass' target. The map's coverage is
    // fitted to the world bounds of the instances that survived culling: those
    // are the only casters whose shadows can land on something visible, and it
    // keeps the map from being spread thin by distant geometry.
    bool shadows_enabled = false;
    Mat4 light_view_projection = Mat4::identity();
    if (light != nullptr && !instances.empty()) {
        Vec3 scene_min{};
        Vec3 scene_max{};
        bool bounds_valid = false;
        for (const render::MeshInstance& instance : instances) {
            // The world AABB was computed once at extraction; the shadow fit
            // reads it rather than re-expanding the mesh's local bounds.
            const Vec3& world_min = instance.world_min;
            const Vec3& world_max = instance.world_max;
            if (!bounds_valid) {
                scene_min = world_min;
                scene_max = world_max;
                bounds_valid = true;
            } else {
                scene_min.x = std::min(scene_min.x, world_min.x);
                scene_min.y = std::min(scene_min.y, world_min.y);
                scene_min.z = std::min(scene_min.z, world_min.z);
                scene_max.x = std::max(scene_max.x, world_max.x);
                scene_max.y = std::max(scene_max.y, world_max.y);
                scene_max.z = std::max(scene_max.z, world_max.z);
            }
        }

        if (Error error = ensure_shadow_target(device); error) {
            FE_LOG_ERROR("renderer3d: shadow map unavailable: %s", error.message().c_str());
        } else {
            light_view_projection = directional_light_view_projection(scene_min, scene_max, light_direction);

            rhi::RenderTarget* previous_target = device.bound_render_target();
            device.bind_render_target(shadow_target_.get());
            shadow_target_->clear(rhi::ClearColor{1.0f, 1.0f, 1.0f, 1.0f}); // Far depth.
            device.set_depth_test(true);
            device.bind_shader(*shadow_shader_);
            shadow_shader_->set_mat4("u_light_view_projection", light_view_projection);

            for (const render::MeshInstance& instance : instances) {
                auto gpu = acquire_mesh(device, *instance.mesh);
                if (!gpu) {
                    continue;
                }
                shadow_shader_->set_mat4("u_model", instance.model);
                device.bind_vertex_buffer(*gpu.value()->vertex_buffer);
                device.bind_index_buffer(*gpu.value()->index_buffer);
                device.draw_indexed(gpu.value()->index_count);
                ++last_shadow_casters_;
            }

            // Restore whatever the caller had bound (the window, or an editor
            // panel's target) so the main pass draws where it was meant to.
            device.bind_render_target(previous_target);
            shadows_enabled = true;
        }
    }

    // --- 4. Pass state ------------------------------------------------------
    // The 3D pass is opaque and depth-tested. Blending is disabled so draws do
    // not depend on order.
    device.set_blend_mode(rhi::BlendMode::None);
    device.set_depth_test(true);

    if (!instances.empty()) {
        device.bind_shader(*shader_);
        shader_->set_mat4("u_view_projection", view_projection);
        shader_->set_int("u_texture", 0);
        shader_->set_vec3("u_light_color", light_color);
        shader_->set_float("u_light_intensity", light_intensity);
        shader_->set_float("u_ambient", ambient);
        shader_->set_vec3("u_light_direction", light_direction);
        shader_->set_vec3("u_camera_position", camera_position);

        // Shadow uniforms. When no shadow map exists (no directional light, or
        // creation failed) the light-space matrix stays identity and the map is
        // never sampled usefully, but the sampler still needs a bound texture to
        // be a complete program, so bind the white texel.
        shader_->set_int("u_shadow_map", 1);
        shader_->set_mat4("u_light_space_matrix", light_view_projection);
        shader_->set_float("u_shadow_bias", kShadowBias);
        if (shadows_enabled && shadow_target_->depth_texture() != nullptr) {
            device.bind_texture(1, *shadow_target_->depth_texture());
        } else {
            device.bind_texture(1, *default_texture_);
        }

        // Pack and upload the bounded light arrays. Only the first
        // point_count / spot_count elements are read by the shader, whose loop
        // is bounded by the count uniforms.
        const u32 point_count = static_cast<u32>(point_lights.size());
        for (u32 i = 0; i < point_count; ++i) {
            const PointLightEntry& entry = point_lights[i];
            point_light_data_[i] = Vec4{entry.position, entry.range};
            point_color_data_[i] = Vec4{entry.color, entry.intensity};
        }
        shader_->set_int("u_point_light_count", static_cast<i32>(point_count));
        shader_->set_vec4_array("u_point_light_position_range", point_light_data_.data(), point_count);
        shader_->set_vec4_array("u_point_light_color_intensity", point_color_data_.data(), point_count);

        const u32 spot_count = static_cast<u32>(spot_lights.size());
        for (u32 i = 0; i < spot_count; ++i) {
            const SpotLightEntry& entry = spot_lights[i];
            // The position/direction arrays are uploaded as one 16-element
            // block: [0, 8) positions + ranges, [8, 16) directions.
            spot_light_data_[i] = Vec4{entry.position, entry.range};
            spot_light_data_[i + kMaxSpotLights] = Vec4{entry.direction, 0.0f};
            spot_color_data_[i] = Vec4{entry.color, entry.intensity};
            spot_cone_data_[i] = Vec4{entry.cos_inner, entry.cos_outer, 0.0f, 0.0f};
        }
        shader_->set_int("u_spot_light_count", static_cast<i32>(spot_count));
        // The direction half of the block must be uploaded even when only the
        // first few entries are used, so always send the full 2*max block; the
        // shader reads it only for i < spot_count.
        shader_->set_vec4_array("u_spot_light_data", spot_light_data_.data(), spot_count == 0 ? 0 : kMaxSpotLights * 2);
        shader_->set_vec4_array("u_spot_light_color_intensity", spot_color_data_.data(), spot_count);
        shader_->set_vec4_array("u_spot_light_cone", spot_cone_data_.data(), spot_count);
    }

    // --- 5. Draw ------------------------------------------------------------
    for (const render::MeshInstance& instance : instances) {
        auto gpu = acquire_mesh(device, *instance.mesh);
        if (!gpu) {
            FE_LOG_ERROR("renderer3d: %s", gpu.error().message().c_str());
            continue;
        }

        shader_->set_mat4("u_model", instance.model);
        const std::array<f32, 9> normal_mat = normal_matrix(instance.model);
        shader_->set_mat3("u_normal_matrix", normal_mat.data());
        shader_->set_vec4("u_base_color", instance.renderer->base_color);
        shader_->set_float("u_specular", instance.renderer->specular);

        if (instance.renderer->has_texture()) {
            if (const auto* texture = dynamic_cast<const Texture2D*>(instance.renderer->texture.get());
                texture != nullptr) {
                device.bind_texture(0, texture->texture());
            } else {
                device.bind_texture(0, *default_texture_);
            }
        } else {
            device.bind_texture(0, *default_texture_);
        }

        device.bind_vertex_buffer(*gpu.value()->vertex_buffer);
        device.bind_index_buffer(*gpu.value()->index_buffer);
        device.draw_indexed(gpu.value()->index_count);
        ++last_draw_calls_;
        ++drawn;
    }

    // --- 6. Sweep dead mesh buffers ----------------------------------------
    // A buffer is stale when its mesh is no longer referenced by any renderer in
    // the scene. Liveness is checked against every mesh seen this frame, culled
    // or not, so a mesh that is simply off screen keeps its buffers.
    std::erase_if(buffers_, [&](const auto& entry) {
        return std::none_of(meshes_seen.begin(), meshes_seen.end(),
                            [&](const Mesh* mesh) { return mesh == entry.first; });
    });

    return static_cast<usize>(drawn);
}

Result<usize> Renderer3D::draw_chain(rhi::Device& device, const Scene& scene, const Mat4& view_projection,
                                     const Vec3& camera_position, const Camera3D& camera,
                                     std::vector<render::MeshInstance>& instances,
                                     const std::vector<const Mesh*>& meshes_seen, const PostProcessSettings& settings,
                                     u32 width, u32 height) {
    // Resolve the target the 3D pass should draw into and bind it *before* the
    // background, so the gradient lands in the same image as the geometry: the
    // caller's target when the chain is off, the off-screen target when it is on.
    rhi::RenderTarget* caller_target = device.bound_render_target();
    rhi::RenderTarget* pass_target = caller_target;
    bool chained = false;
    if (settings.enabled) {
        if (Error target_error = ensure_scene_target(device, width, height); target_error) {
            FE_LOG_ERROR("renderer3d: post-process target unavailable: %s", target_error.message().c_str());
        } else if (Error shader_error = post_process_.initialize(device); shader_error) {
            FE_LOG_ERROR("renderer3d: post-process shader unavailable: %s", shader_error.message().c_str());
        } else {
            pass_target = scene_target_.get();
            chained = true;
            device.bind_render_target(pass_target);
            scene_target_->clear(rhi::ClearColor{0.0f, 0.0f, 0.0f, 1.0f});
        }
    }

    // The background gradient is geometry-independent, so it draws even when the
    // cull left nothing: an empty scene under a sky is a valid frame.
    if (camera.background_enabled) {
        if (Error error = ensure_background(device); error) {
            FE_LOG_ERROR("renderer3d: background unavailable: %s", error.message().c_str());
        } else {
            draw_background(device, camera);
        }
    }

    const Result<usize> drawn = draw(device, scene, view_projection, camera_position, instances, meshes_seen);

    if (chained) {
        // Restore the caller's target and grade the scene image into it. The
        // source texture belongs to the off-screen target, so it is distinct from
        // the destination's attachment and safe to sample.
        device.bind_render_target(caller_target);
        if (Error error = post_process_.apply(device, *scene_target_->color_texture(), settings); error) {
            FE_LOG_ERROR("renderer3d: post-process pass failed: %s", error.message().c_str());
        } else {
            ++post_process_frames_;
        }
    }

    return drawn;
}

Error Renderer3D::ensure_scene_target(rhi::Device& device, u32 width, u32 height) {
    if (scene_target_ != nullptr && scene_target_width_ == width && scene_target_height_ == height) {
        return Error::none();
    }

    rhi::RenderTargetDesc desc;
    desc.name = "renderer3d_scene";
    desc.width = width;
    desc.height = height;
    desc.depth = true; // The 3D pass depth-tests, so the off-screen image needs a depth buffer.
    desc.sample_depth = false;
    auto target = device.create_render_target(desc);
    if (!target) {
        return target.error();
    }
    scene_target_ = target.value();
    scene_target_width_ = width;
    scene_target_height_ = height;
    return Error::none();
}

Error Renderer3D::ensure_background(rhi::Device& device) {
    if (background_shader_ != nullptr) {
        return Error::none();
    }
    if (background_attempted_) {
        // A previous attempt failed; do not retry every frame.
        return Error("background pass unavailable");
    }
    background_attempted_ = true;

    rhi::ShaderDesc desc;
    desc.name = "renderer3d_background";
    desc.vertex_source = kBackgroundVertexShaderSource;
    desc.fragment_source = kBackgroundFragmentShaderSource;
    auto shader = device.create_shader(desc);
    if (!shader) {
        return shader.error();
    }
    background_shader_ = shader.value();

    auto vertex_buffer = device.create_vertex_buffer(rhi::VertexLayout::position_uv(),
                                                     static_cast<u32>(kBackgroundQuadVertices.size() * sizeof(f32)));
    if (!vertex_buffer) {
        return vertex_buffer.error();
    }
    background_quad_vertex_buffer_ = vertex_buffer.value();
    background_quad_vertex_buffer_->update(std::span<const u8>(
        reinterpret_cast<const u8*>(kBackgroundQuadVertices.data()), kBackgroundQuadVertices.size() * sizeof(f32)));

    auto index_buffer = device.create_index_buffer(static_cast<u32>(kBackgroundQuadIndices.size()));
    if (!index_buffer) {
        return index_buffer.error();
    }
    background_quad_index_buffer_ = index_buffer.value();
    background_quad_index_buffer_->update(std::span<const u8>(
        reinterpret_cast<const u8*>(kBackgroundQuadIndices.data()), kBackgroundQuadIndices.size() * sizeof(u32)));

    return Error::none();
}

void Renderer3D::draw_background(rhi::Device& device, const Camera3D& camera) {
    FE_CHECK(background_shader_ != nullptr);

    // A background fills the frame and must not occlude the geometry that follows,
    // so it draws with no depth write and no depth test, and it does not blend.
    device.set_depth_test(false);
    device.set_blend_mode(rhi::BlendMode::None);
    device.bind_shader(*background_shader_);
    background_shader_->set_vec3("u_top_color", camera.background_top);
    background_shader_->set_vec3("u_bottom_color", camera.background_bottom);
    device.bind_vertex_buffer(*background_quad_vertex_buffer_);
    device.bind_index_buffer(*background_quad_index_buffer_);
    device.draw_indexed(static_cast<u32>(kBackgroundQuadIndices.size()));
    ++background_draws_;
}

} // namespace fe::renderer3d
