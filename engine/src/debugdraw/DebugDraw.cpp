// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/debugdraw/DebugDraw.h"

#include "fe/core/Log.h"
#include "fe/physics2d/Collider2D.h"
#include "fe/physics2d/RigidBody2D.h"
#include "fe/physics3d/Collider3D.h"
#include "fe/physics3d/RigidBody3D.h"
#include "fe/rhi/Device.h"
#include "fe/scene/Node.h"
#include "fe/scene/Scene.h"

#include <array>
#include <cmath>
#include <span>

namespace fe::debugdraw {
namespace {

// The two unit vectors spanning the plane whose normal is `axis` (0 = x, 1 = y,
// 2 = z). A circle on that plane is drawn in the other two axes.
[[nodiscard]] std::array<Vec3, 2> plane_basis(u32 axis) noexcept {
    switch (axis) {
    case 0:
        return {Vec3{0.0f, 1.0f, 0.0f}, Vec3{0.0f, 0.0f, 1.0f}};
    case 1:
        return {Vec3{1.0f, 0.0f, 0.0f}, Vec3{0.0f, 0.0f, 1.0f}};
    default:
        return {Vec3{1.0f, 0.0f, 0.0f}, Vec3{0.0f, 1.0f, 0.0f}};
    }
}

// The world-space centre of a node: the translation column of its world matrix.
// The physics systems place a collider at the node's world position, so this is
// the same point the wireframe must be centred on.
[[nodiscard]] Vec3 world_center_of(const Node& node) noexcept {
    const Mat4 world = node.world_matrix();
    return Vec3{world.at(0, 3), world.at(1, 3), world.at(2, 3)};
}

constexpr const char* kLineVertexShaderSource = R"glsl(
#version 330 core

layout(location = 0) in vec3 a_position;
layout(location = 1) in vec4 a_color;

uniform mat4 u_view_projection;

out vec4 v_color;

void main() {
    v_color = a_color;
    gl_Position = u_view_projection * vec4(a_position, 1.0);
}
)glsl";

constexpr const char* kLineFragmentShaderSource = R"glsl(
#version 330 core

in vec4 v_color;

out vec4 frag_color;

void main() {
    frag_color = v_color;
}
)glsl";

} // namespace

void DebugDrawList::add_box(const Vec3& center, const Vec3& half_extent, const Vec4& color) {
    // The eight corners, indexed by (x, y, z) sign bit: bit 0 = +x, bit 1 = +y,
    // bit 2 = +z.
    std::array<Vec3, 8> corners{};
    for (u32 i = 0; i < 8; ++i) {
        const f32 sx = (i & 1U) != 0U ? 1.0f : -1.0f;
        const f32 sy = (i & 2U) != 0U ? 1.0f : -1.0f;
        const f32 sz = (i & 4U) != 0U ? 1.0f : -1.0f;
        corners[i] = Vec3{center.x + sx * half_extent.x, center.y + sy * half_extent.y, center.z + sz * half_extent.z};
    }

    // The 12 edges: pairs of corners that differ in exactly one sign bit.
    static constexpr std::array<std::array<u32, 2>, 12> kEdges = {{
        {0, 1},
        {2, 3},
        {4, 5},
        {6, 7}, // along x
        {0, 2},
        {1, 3},
        {4, 6},
        {5, 7}, // along y
        {0, 4},
        {1, 5},
        {2, 6},
        {3, 7}, // along z
    }};
    for (const auto& edge : kEdges) {
        add_line(corners[edge[0]], corners[edge[1]], color);
    }
}

void DebugDrawList::add_circle(const Vec3& center, f32 radius, u32 axis, u32 segments, const Vec4& color) {
    if (segments < 3) {
        segments = 3;
    }
    const auto basis = plane_basis(axis);
    const Vec3& u = basis[0];
    const Vec3& v = basis[1];

    const f32 step = 6.28318530717958647692f / static_cast<f32>(segments);
    Vec3 previous{};
    for (u32 i = 0; i <= segments; ++i) {
        const f32 angle = step * static_cast<f32>(i);
        const f32 c = std::cos(angle) * radius;
        const f32 s = std::sin(angle) * radius;
        const Vec3 point{center.x + u.x * c + v.x * s, center.y + u.y * c + v.y * s, center.z + u.z * c + v.z * s};
        if (i > 0) {
            add_line(previous, point, color);
        }
        previous = point;
    }
}

void DebugDrawList::add_sphere(const Vec3& center, f32 radius, u32 segments, const Vec4& color) {
    add_circle(center, radius, 0, segments, color);
    add_circle(center, radius, 1, segments, color);
    add_circle(center, radius, 2, segments, color);
}

void DebugDrawList::add_cross(const Vec3& center, f32 size, const Vec4& color) {
    add_line(Vec3{center.x - size, center.y, center.z}, Vec3{center.x + size, center.y, center.z}, color);
    add_line(Vec3{center.x, center.y - size, center.z}, Vec3{center.x, center.y + size, center.z}, color);
    add_line(Vec3{center.x, center.y, center.z - size}, Vec3{center.x, center.y, center.z + size}, color);
}

Error DebugRenderer::initialize(rhi::Device& device, usize initial_capacity_lines) {
    if (shader_ == nullptr) {
        rhi::ShaderDesc desc;
        desc.name = "debugdraw_lines";
        desc.vertex_source = kLineVertexShaderSource;
        desc.fragment_source = kLineFragmentShaderSource;
        auto shader = device.create_shader(desc);
        if (!shader) {
            return shader.error();
        }
        shader_ = shader.value();
    }

    if (initial_capacity_lines > capacity_lines_) {
        // Two vertices per line, each (vec3 position + vec4 colour).
        const u32 bytes = static_cast<u32>(initial_capacity_lines * 2U * (3U + 4U) * sizeof(f32));
        auto buffer = device.create_vertex_buffer(rhi::VertexLayout::position_color(), bytes);
        if (!buffer) {
            return buffer.error();
        }
        vertex_buffer_ = buffer.value();
        capacity_lines_ = initial_capacity_lines;
    }

    return Error::none();
}

void DebugRenderer::render(rhi::Device& device, const DebugDrawList& list, const Mat4& view_projection) {
    last_line_count_ = 0;
    if (list.empty() || shader_ == nullptr || vertex_buffer_ == nullptr) {
        return;
    }

    // Grow the buffer if the caller outgrew the last capacity. The scratch buffer
    // is reused so a steady-state frame allocates nothing.
    if (list.size() > capacity_lines_) {
        if (Error error = initialize(device, list.size()); error) {
            FE_LOG_ERROR("debugdraw: buffer growth failed: %s", error.message().c_str());
            return;
        }
    }

    scratch_.clear();
    scratch_.reserve(list.size() * 14U);
    for (const Line& line : list.lines()) {
        scratch_.insert(scratch_.end(),
                        {line.a.x, line.a.y, line.a.z, line.color.x, line.color.y, line.color.z, line.color.w});
        scratch_.insert(scratch_.end(),
                        {line.b.x, line.b.y, line.b.z, line.color.x, line.color.y, line.color.z, line.color.w});
    }

    vertex_buffer_->update(
        std::span<const u8>(reinterpret_cast<const u8*>(scratch_.data()), scratch_.size() * sizeof(f32)));

    device.set_depth_test(true);
    device.set_blend_mode(rhi::BlendMode::Alpha);
    device.set_primitive_topology(rhi::PrimitiveTopology::Lines);
    device.bind_shader(*shader_);
    shader_->set_mat4("u_view_projection", view_projection);
    device.bind_vertex_buffer(*vertex_buffer_);

    // A line list has no index buffer; the vertices are consumed in order. The
    // RHI's indexed draw is the only draw, so a trivial 0..n-1 index buffer is
    // what lets a line list ride on it without a second draw call type. It is
    // built once here and kept with the renderer.
    if (index_buffer_ == nullptr || index_buffer_capacity_ < list.size() * 2U) {
        const u32 count = static_cast<u32>(list.size() * 2U);
        auto buffer = device.create_index_buffer(count);
        if (!buffer) {
            FE_LOG_ERROR("debugdraw: index buffer unavailable: %s", buffer.error().message().c_str());
            device.set_primitive_topology(rhi::PrimitiveTopology::Triangles);
            return;
        }
        index_buffer_ = buffer.value();
        index_buffer_capacity_ = count;
        scratch_indices_.resize(count);
        for (u32 i = 0; i < count; ++i) {
            scratch_indices_[i] = i;
        }
        index_buffer_->update(std::span<const u8>(reinterpret_cast<const u8*>(scratch_indices_.data()),
                                                  scratch_indices_.size() * sizeof(u32)));
    }

    device.bind_index_buffer(*index_buffer_);
    device.draw_indexed(static_cast<u32>(list.size() * 2U));

    // Restore triangle assembly so a later pass that forgets to set it is not
    // silently drawing triangles as lines.
    device.set_primitive_topology(rhi::PrimitiveTopology::Triangles);

    last_line_count_ = static_cast<u32>(list.size());
    ++draw_calls_;
}

usize append_collider_wireframes(const Scene& scene, DebugDrawList& out) {
    usize drawn = 0;
    scene.for_each_node([&](const Node& node) {
        const auto* collider = node.get_component<physics3d::Collider3D>();
        if (collider == nullptr) {
            return;
        }
        const auto* body = node.get_component<physics3d::RigidBody3D>();
        const bool dynamic = body != nullptr && !body->kinematic && body->mass > 0.0f;
        const Vec4 color = dynamic ? dynamic_collider_color() : static_collider_color();
        const Vec3 center = world_center_of(node);

        if (collider->shape == physics3d::ShapeType3D::Sphere) {
            out.add_sphere(center, collider->radius, 16, color);
        } else {
            out.add_box(center, collider->half_extent, color);
        }
        ++drawn;
    });
    return drawn;
}

usize append_collider_wireframes_2d(const Scene& scene, DebugDrawList& out) {
    usize drawn = 0;
    scene.for_each_node([&](const Node& node) {
        const auto* collider = node.get_component<physics2d::Collider2D>();
        if (collider == nullptr) {
            return;
        }
        const auto* body = node.get_component<physics2d::RigidBody2D>();
        const bool dynamic = body != nullptr && !body->kinematic && body->mass > 0.0f;
        const Vec4 color = dynamic ? dynamic_collider_color() : static_collider_color();
        const Vec3 center = world_center_of(node);

        if (collider->shape == physics2d::ShapeType::Circle) {
            // A circle on the xy plane, drawn at z = 0.
            out.add_circle(center, collider->radius, 2, 24, color);
        } else {
            out.add_box(center, Vec3{collider->half_extent.x, collider->half_extent.y, 0.0f}, color);
        }
        ++drawn;
    });
    return drawn;
}

} // namespace fe::debugdraw
