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

    // Ambient plus the directional light.
    vec3 l = normalize(-u_light_direction);
    vec3 lighting = vec3(u_ambient) + shade(n, v, l, u_light_color * u_light_intensity, 1.0);

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

// The world-space axis-aligned bounding box of a local AABB under `model`.
//
// Transforming the eight corners and taking the min/max is exact for an AABB
// (the extremes of a linear map over a box occur at vertices) and cheap enough
// for the handful of meshes a frame touches. The tighter alternative - fitting
// an oriented box - buys very little here and complicates the frustum test.
void expand_to_world_aabb(const Vec3& local_min, const Vec3& local_max, const Mat4& model, Vec3& out_min,
                          Vec3& out_max) noexcept {
    const std::array<Vec3, 8> corners = {
        Vec3{local_min.x, local_min.y, local_min.z}, Vec3{local_max.x, local_min.y, local_min.z},
        Vec3{local_min.x, local_max.y, local_min.z}, Vec3{local_max.x, local_max.y, local_min.z},
        Vec3{local_min.x, local_min.y, local_max.z}, Vec3{local_max.x, local_min.y, local_max.z},
        Vec3{local_min.x, local_max.y, local_max.z}, Vec3{local_max.x, local_max.y, local_max.z},
    };
    out_min = transform_point(model, corners[0]);
    out_max = out_min;
    for (usize i = 1; i < corners.size(); ++i) {
        const Vec3 p = transform_point(model, corners[i]);
        out_min.x = std::min(out_min.x, p.x);
        out_min.y = std::min(out_min.y, p.y);
        out_min.z = std::min(out_min.z, p.z);
        out_max.x = std::max(out_max.x, p.x);
        out_max.y = std::max(out_max.y, p.y);
        out_max.z = std::max(out_max.z, p.z);
    }
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

Result<usize> Renderer3D::render(rhi::Device& device, const Scene& scene, u32 viewport_width, u32 viewport_height) {
    FE_CHECK(shader_ != nullptr);
    FE_CHECK(viewport_width > 0 && viewport_height > 0);

    last_draw_calls_ = 0;
    last_culled_ = 0;
    last_point_lights_ = 0;
    last_spot_lights_ = 0;

    const Camera3D* camera = find_active_camera(scene);
    if (camera == nullptr) {
        return usize{0};
    }

    // --- 1. Collect drawable instances -------------------------------------
    struct Instance {
        const MeshRenderer3D* renderer;
        const Mesh* mesh;
        Mat4 model;
    };
    std::vector<Instance> instances;

    // Every mesh referenced this frame, including culled ones. The buffer sweep
    // below keys off this, not off `instances`: a mesh that is merely off screen
    // this frame must keep its GPU buffers, or it would re-upload every time it
    // came back into view.
    std::vector<const Mesh*> meshes_seen;

    scene.for_each_node([&](Node& node) {
        const auto* renderer = node.get_component<MeshRenderer3D>();
        if (renderer == nullptr || !renderer->has_mesh()) {
            return;
        }
        const auto* mesh = dynamic_cast<const Mesh*>(renderer->mesh.get());
        if (mesh == nullptr || mesh->index_count() == 0) {
            return;
        }
        meshes_seen.push_back(mesh);
        instances.push_back(Instance{renderer, mesh, node.world_matrix()});
    });

    if (instances.empty()) {
        return usize{0};
    }

    // --- 2. Frame constants -------------------------------------------------
    const Mat4 view_projection = camera->view_projection(viewport_width, viewport_height);
    const Vec3 camera_position = camera->eye();

    // --- Gather lights ------------------------------------------------------
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

    // --- 3. Frustum culling -------------------------------------------------
    // Build the frustum once and reject instances whose world AABB lies wholly
    // outside it. This is conservative (a straddling box is kept), so culling
    // can only ever save a draw, never hide a visible mesh.
    const Frustum frustum = Frustum::from_view_projection(view_projection);
    std::erase_if(instances, [&](const Instance& instance) {
        Vec3 world_min{};
        Vec3 world_max{};
        expand_to_world_aabb(instance.mesh->bounds_min(), instance.mesh->bounds_max(), instance.model, world_min,
                             world_max);
        if (frustum.intersects_aabb(world_min, world_max)) {
            return false;
        }
        ++last_culled_;
        return true;
    });

    const DirectionalLight3D* light = find_directional_light(scene);
    const Vec3 light_direction = light != nullptr ? light->direction.normalized() : Vec3{0.0f, -1.0f, 0.0f};
    const Vec3 light_color = light != nullptr ? light->color : Vec3{1.0f, 1.0f, 1.0f};
    const f32 light_intensity = light != nullptr ? light->intensity : 1.0f;
    const f32 ambient = light != nullptr ? light->ambient : 0.15f;

    u32 drawn = 0;

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
    for (const Instance& instance : instances) {
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

} // namespace fe::renderer3d
