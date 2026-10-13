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

#include <algorithm>
#include <array>
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
uniform vec3 u_light_direction;  // Direction the light travels.
uniform vec3 u_light_color;
uniform float u_light_intensity;
uniform float u_ambient;

out vec4 frag_color;

void main() {
    vec4 albedo = texture(u_texture, v_uv) * u_base_color;

    vec3 n = normalize(v_normal);
    vec3 l = normalize(-u_light_direction);                       // Towards the light.
    vec3 v = normalize(u_camera_position - v_world_position);     // Towards the viewer.

    float diffuse = max(dot(n, l), 0.0);
    vec3 h = normalize(l + v);
    float spec = pow(max(dot(n, h), 0.0), 32.0) * u_specular;

    vec3 lighting = u_light_color * u_light_intensity * (diffuse + spec) + vec3(u_ambient);
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
