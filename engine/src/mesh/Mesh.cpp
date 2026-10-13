// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/mesh/Mesh.h"

#include <algorithm>
#include <limits>

namespace fe {

void Mesh::recompute_bounds() {
    if (vertices_.empty()) {
        bounds_min_ = Vec3{};
        bounds_max_ = Vec3{};
        return;
    }

    constexpr f32 kMax = std::numeric_limits<f32>::max();
    bounds_min_ = Vec3{kMax, kMax, kMax};
    bounds_max_ = Vec3{-kMax, -kMax, -kMax};
    for (const Vertex3D& vertex : vertices_) {
        bounds_min_.x = std::min(bounds_min_.x, vertex.position.x);
        bounds_min_.y = std::min(bounds_min_.y, vertex.position.y);
        bounds_min_.z = std::min(bounds_min_.z, vertex.position.z);
        bounds_max_.x = std::max(bounds_max_.x, vertex.position.x);
        bounds_max_.y = std::max(bounds_max_.y, vertex.position.y);
        bounds_max_.z = std::max(bounds_max_.z, vertex.position.z);
    }
}

std::shared_ptr<Mesh> Mesh::make_box(const Vec3& half_extent) {
    const f32 hx = half_extent.x;
    const f32 hy = half_extent.y;
    const f32 hz = half_extent.z;

    // Six faces, four unique vertices each, so each face carries one normal and
    // a full 0..1 UV rectangle. Shared corners would force averaged normals and
    // break flat shading; a box is small enough that the duplicate vertices cost
    // nothing.
    std::vector<Vertex3D> vertices;
    std::vector<u32> indices;
    vertices.reserve(24);
    indices.reserve(36);

    struct Face {
        Vec3 normal;
        Vec3 corners[4]; // Counter-clockwise when viewed from outside.
    };

    const Vec3 n_pos_x{1.0f, 0.0f, 0.0f};
    const Vec3 n_neg_x{-1.0f, 0.0f, 0.0f};
    const Vec3 n_pos_y{0.0f, 1.0f, 0.0f};
    const Vec3 n_neg_y{0.0f, -1.0f, 0.0f};
    const Vec3 n_pos_z{0.0f, 0.0f, 1.0f};
    const Vec3 n_neg_z{0.0f, 0.0f, -1.0f};

    const Face faces[6] = {
        {n_pos_x, {{hx, -hy, hz}, {hx, -hy, -hz}, {hx, hy, -hz}, {hx, hy, hz}}},
        {n_neg_x, {{-hx, -hy, -hz}, {-hx, -hy, hz}, {-hx, hy, hz}, {-hx, hy, -hz}}},
        {n_pos_y, {{-hx, hy, hz}, {hx, hy, hz}, {hx, hy, -hz}, {-hx, hy, -hz}}},
        {n_neg_y, {{-hx, -hy, -hz}, {hx, -hy, -hz}, {hx, -hy, hz}, {-hx, -hy, hz}}},
        {n_pos_z, {{-hx, -hy, hz}, {hx, -hy, hz}, {hx, hy, hz}, {-hx, hy, hz}}},
        {n_neg_z, {{hx, -hy, -hz}, {-hx, -hy, -hz}, {-hx, hy, -hz}, {hx, hy, -hz}}},
    };

    const Vec2 uvs[4] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}};

    for (const Face& face : faces) {
        const u32 base = static_cast<u32>(vertices.size());
        for (int i = 0; i < 4; ++i) {
            vertices.push_back(Vertex3D{face.corners[i], face.normal, uvs[i]});
        }
        indices.push_back(base + 0);
        indices.push_back(base + 1);
        indices.push_back(base + 2);
        indices.push_back(base + 0);
        indices.push_back(base + 2);
        indices.push_back(base + 3);
    }

    return std::make_shared<Mesh>("box", std::move(vertices), std::move(indices));
}

std::shared_ptr<Mesh> Mesh::make_plane(f32 half_width, f32 half_depth) {
    const Vec3 normal{0.0f, 1.0f, 0.0f};

    // Counter-clockwise viewed from +Y, so the normal points up.
    std::vector<Vertex3D> vertices = {
        Vertex3D{{-half_width, 0.0f, half_depth}, normal, {0.0f, 0.0f}},
        Vertex3D{{half_width, 0.0f, half_depth}, normal, {1.0f, 0.0f}},
        Vertex3D{{half_width, 0.0f, -half_depth}, normal, {1.0f, 1.0f}},
        Vertex3D{{-half_width, 0.0f, -half_depth}, normal, {0.0f, 1.0f}},
    };
    std::vector<u32> indices = {0, 1, 2, 0, 2, 3};

    return std::make_shared<Mesh>("plane", std::move(vertices), std::move(indices));
}

} // namespace fe
