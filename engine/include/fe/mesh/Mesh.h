// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Math.h"
#include "fe/core/Types.h"
#include "fe/mesh/Material.h"
#include "fe/mesh/Vertex3D.h"
#include "fe/resources/Resource.h"

#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace fe {

// A triangle mesh in main memory: interleaved vertices and 32-bit indices.
//
// The mesh holds CPU data only; it does not know about the GPU. The 3D renderer
// owns a buffer cache keyed by the mesh and uploads on first use, which is the
// same split the 2D renderer uses for its own buffers. Keeping the mesh free of
// the RHI means it can be generated, loaded, and unit-tested with no graphics
// context.
//
// A mesh with no vertices is valid and draws nothing.
class Mesh final : public Resource {
public:
    static constexpr const char* kTypeName = "Mesh";

    Mesh(std::string path, std::vector<Vertex3D> vertices, std::vector<u32> indices)
        : Resource(std::move(path)), vertices_(std::move(vertices)), indices_(std::move(indices)) {
        recompute_bounds();
    }

    [[nodiscard]] const char* type_name() const noexcept override { return kTypeName; }

    [[nodiscard]] const std::vector<Vertex3D>& vertices() const noexcept { return vertices_; }
    [[nodiscard]] const std::vector<u32>& indices() const noexcept { return indices_; }

    [[nodiscard]] usize vertex_count() const noexcept { return vertices_.size(); }
    [[nodiscard]] usize index_count() const noexcept { return indices_.size(); }
    [[nodiscard]] usize triangle_count() const noexcept { return indices_.size() / 3; }

    // The local-space axis-aligned bounds, for culling and for framing the
    // camera. Empty meshes report a zero-size box at the origin.
    [[nodiscard]] const Vec3& bounds_min() const noexcept { return bounds_min_; }
    [[nodiscard]] const Vec3& bounds_max() const noexcept { return bounds_max_; }

    // The material the source file declared for this primitive, when it declared
    // one. Absent for a mesh built in code (a primitive generator) and for a
    // format with no material concept, which is why this is an `optional` rather
    // than a default-constructed `Material`: "the file said nothing" and "the
    // file said default" are different states, and a component must be able to
    // tell them apart before it overwrites authored values.
    //
    // glTF puts the material on the primitive, so a mesh is the right home for
    // it. The renderer prefers a component's explicit material when the
    // component has one, and falls back to this otherwise.
    [[nodiscard]] const std::optional<Material>& material() const noexcept { return material_; }
    void set_material(std::optional<Material> material) noexcept { material_ = std::move(material); }
    [[nodiscard]] bool has_material() const noexcept { return material_.has_value(); }

    [[nodiscard]] usize memory_usage() const noexcept override {
        return vertices_.size() * sizeof(Vertex3D) + indices_.size() * sizeof(u32);
    }

    // --- Primitive generators ---------------------------------------------
    // Small built-in meshes so a scene can be authored and rendered before any
    // asset pipeline exists, and so tests do not depend on a file on disk.

    // An axis-aligned box centred on the origin, spanning +/- half_extent on
    // each axis, with flat per-face normals and UVs.
    [[nodiscard]] static std::shared_ptr<Mesh> make_box(const Vec3& half_extent);

    // A unit cube: make_box({0.5, 0.5, 0.5}).
    [[nodiscard]] static std::shared_ptr<Mesh> make_cube() { return make_box(Vec3{0.5f, 0.5f, 0.5f}); }

    // A flat quad in the XZ plane centred on the origin, facing +Y.
    [[nodiscard]] static std::shared_ptr<Mesh> make_plane(f32 half_width, f32 half_depth);

private:
    void recompute_bounds();

    std::vector<Vertex3D> vertices_;
    std::vector<u32> indices_;
    Vec3 bounds_min_{};
    Vec3 bounds_max_{};
    std::optional<Material> material_;
};

} // namespace fe
