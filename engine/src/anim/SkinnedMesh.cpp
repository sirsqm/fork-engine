// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/anim/SkinnedMesh.h"

#include "fe/anim/Skeleton.h"

#include <algorithm>
#include <limits>

namespace fe::anim {
namespace {

// Transforms a position by a matrix's upper-left 3x3 plus translation. Written
// out rather than reusing transform_point so the skinner has no hidden dependency
// on how the engine lays a Mat4 out beyond the accessor it already uses.
[[nodiscard]] Vec3 skin_position(const Mat4& matrix, const Vec3& p) noexcept {
    return Vec3{matrix.at(0, 0) * p.x + matrix.at(0, 1) * p.y + matrix.at(0, 2) * p.z + matrix.at(0, 3),
                matrix.at(1, 0) * p.x + matrix.at(1, 1) * p.y + matrix.at(1, 2) * p.z + matrix.at(1, 3),
                matrix.at(2, 0) * p.x + matrix.at(2, 1) * p.y + matrix.at(2, 2) * p.z + matrix.at(2, 3)};
}

// Transforms a direction by a matrix's upper-left 3x3, ignoring translation.
[[nodiscard]] Vec3 skin_direction(const Mat4& matrix, const Vec3& v) noexcept {
    return Vec3{matrix.at(0, 0) * v.x + matrix.at(0, 1) * v.y + matrix.at(0, 2) * v.z,
                matrix.at(1, 0) * v.x + matrix.at(1, 1) * v.y + matrix.at(1, 2) * v.z,
                matrix.at(2, 0) * v.x + matrix.at(2, 1) * v.y + matrix.at(2, 2) * v.z};
}

} // namespace

SkinnedMesh::SkinnedMesh(std::string path, std::vector<SkinnedVertex3D> vertices, std::vector<u32> indices,
                         std::shared_ptr<Skeleton> skeleton, std::optional<Material> material)
    : Resource(std::move(path)), vertices_(std::move(vertices)), indices_(std::move(indices)),
      skeleton_(std::move(skeleton)), material_(std::move(material)) {
    recompute_bounds();
}

void SkinnedMesh::recompute_bounds() {
    if (vertices_.empty()) {
        bounds_min_ = Vec3{};
        bounds_max_ = Vec3{};
        return;
    }
    Vec3 minimum = vertices_.front().position;
    Vec3 maximum = vertices_.front().position;
    for (const SkinnedVertex3D& vertex : vertices_) {
        minimum.x = std::min(minimum.x, vertex.position.x);
        minimum.y = std::min(minimum.y, vertex.position.y);
        minimum.z = std::min(minimum.z, vertex.position.z);
        maximum.x = std::max(maximum.x, vertex.position.x);
        maximum.y = std::max(maximum.y, vertex.position.y);
        maximum.z = std::max(maximum.z, vertex.position.z);
    }
    bounds_min_ = minimum;
    bounds_max_ = maximum;
}

usize SkinnedMesh::memory_usage() const noexcept {
    return vertices_.size() * sizeof(SkinnedVertex3D) + indices_.size() * sizeof(u32);
}

SkinnedVertex3D skin_vertex(const SkinnedVertex3D& vertex, const std::vector<Mat4>& palette) noexcept {
    // Sum only the weights whose joint the palette actually has. A mesh bound to
    // a shorter rig than it was authored for keeps the influences it can resolve.
    f32 total = 0.0f;
    for (usize influence = 0; influence < 4; ++influence) {
        const u16 joint = vertex.joints[influence];
        if (static_cast<usize>(joint) < palette.size()) {
            total += vertex.weights[influence];
        }
    }

    SkinnedVertex3D out = vertex;
    if (total <= 0.0f) {
        // No usable influence: leave the vertex where the bind pose put it rather
        // than collapsing it to the origin. This is what makes an unweighted
        // vertex (a stray, or a mesh from a rig the clip does not know) render
        // sensibly.
        return out;
    }
    const f32 inverse_total = 1.0f / total;

    Vec3 position{};
    Vec3 normal{};
    for (usize influence = 0; influence < 4; ++influence) {
        const u16 joint = vertex.joints[influence];
        if (static_cast<usize>(joint) >= palette.size()) {
            continue;
        }
        const f32 weight = vertex.weights[influence] * inverse_total;
        if (weight == 0.0f) {
            continue;
        }
        const Mat4& matrix = palette[joint];
        position += skin_position(matrix, vertex.position) * weight;
        normal += skin_direction(matrix, vertex.normal) * weight;
    }
    out.position = position;
    // Renormalise: a weighted sum of rotated normals is not unit length, and the
    // shader's lighting assumes it is.
    out.normal = normal.normalized();
    // The skinning data rides along unchanged, so a skinned buffer can be
    // re-skinned (and so a test can compare against the source).
    return out;
}

void skin_vertices(const std::vector<SkinnedVertex3D>& vertices, const std::vector<Mat4>& palette,
                   std::vector<SkinnedVertex3D>& out) {
    out.resize(vertices.size());
    for (usize i = 0; i < vertices.size(); ++i) {
        out[i] = skin_vertex(vertices[i], palette);
    }
}

bool skinned_bounds(const std::vector<SkinnedVertex3D>& vertices, const std::vector<Mat4>& palette, Vec3& out_min,
                    Vec3& out_max) {
    if (vertices.empty()) {
        return false;
    }
    const f32 lowest = std::numeric_limits<f32>::lowest();
    const f32 highest = std::numeric_limits<f32>::max();
    Vec3 minimum{highest, highest, highest};
    Vec3 maximum{lowest, lowest, lowest};
    for (const SkinnedVertex3D& vertex : vertices) {
        const Vec3 posed = skin_vertex(vertex, palette).position;
        minimum.x = std::min(minimum.x, posed.x);
        minimum.y = std::min(minimum.y, posed.y);
        minimum.z = std::min(minimum.z, posed.z);
        maximum.x = std::max(maximum.x, posed.x);
        maximum.y = std::max(maximum.y, posed.y);
        maximum.z = std::max(maximum.z, posed.z);
    }
    out_min = minimum;
    out_max = maximum;
    return true;
}

} // namespace fe::anim
