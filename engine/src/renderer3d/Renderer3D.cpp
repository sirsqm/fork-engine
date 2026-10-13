// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/renderer3d/Renderer3D.h"

#include "fe/core/Assert.h"
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

    scene.for_each_node([&](Node& node) {
        const auto* renderer = node.get_component<MeshRenderer3D>();
        if (renderer == nullptr || !renderer->has_mesh()) {
            return;
        }
        const auto* mesh = dynamic_cast<const Mesh*>(renderer->mesh.get());
        if (mesh == nullptr || mesh->index_count() == 0) {
            return;
        }
        instances.push_back(Instance{renderer, mesh, node.world_matrix()});
    });

    if (instances.empty()) {
        return usize{0};
    }

    // --- 2. Frame constants -------------------------------------------------
    const Mat4 view_projection = camera->view_projection(viewport_width, viewport_height);
    const Vec3 camera_position = camera->eye();

    const DirectionalLight3D* light = find_directional_light(scene);
    const Vec3 light_direction = light != nullptr ? light->direction.normalized() : Vec3{0.0f, -1.0f, 0.0f};
    const Vec3 light_color = light != nullptr ? light->color : Vec3{1.0f, 1.0f, 1.0f};
    const f32 light_intensity = light != nullptr ? light->intensity : 1.0f;
    const f32 ambient = light != nullptr ? light->ambient : 0.15f;

    // --- 3. Pass state ------------------------------------------------------
    // The 3D pass is opaque and depth-tested. Blending is disabled so draws do
    // not depend on order.
    device.set_blend_mode(rhi::BlendMode::None);
    device.set_depth_test(true);

    device.bind_shader(*shader_);
    shader_->set_mat4("u_view_projection", view_projection);
    shader_->set_int("u_texture", 0);
    shader_->set_vec3("u_light_color", light_color);
    shader_->set_float("u_light_intensity", light_intensity);
    shader_->set_float("u_ambient", ambient);
    shader_->set_vec3("u_light_direction", light_direction);
    shader_->set_vec3("u_camera_position", camera_position);

    // --- 4. Draw ------------------------------------------------------------
    u32 drawn = 0;
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

    // --- 5. Sweep dead mesh buffers ----------------------------------------
    // A buffer is stale when the only reference to its mesh key is gone. We key
    // by address, so a freed mesh's entry can only be removed by checking
    // liveness against the set of meshes seen this frame. Meshes not present
    // are dropped; this runs rarely because scenes are stable frame to frame.
    std::erase_if(buffers_, [&](const auto& entry) {
        return std::none_of(instances.begin(), instances.end(),
                            [&](const Instance& instance) { return instance.mesh == entry.first; });
    });

    return static_cast<usize>(drawn);
}

} // namespace fe::renderer3d
