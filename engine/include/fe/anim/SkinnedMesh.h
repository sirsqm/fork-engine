// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Math.h"
#include "fe/core/Types.h"
#include "fe/mesh/Material.h"
#include "fe/mesh/SkinnedVertex3D.h"
#include "fe/resources/Resource.h"

#include <memory>
#include <optional>
#include <vector>

namespace fe::anim {

class Skeleton;

// A skinned triangle mesh: interleaved vertices that carry joint indices and
// weights, plus the skeleton they are bound to.
//
// Why a separate resource type rather than a flag on `Mesh`. A skinned mesh has a
// different vertex stream (two extra attributes), a different shader, and a
// required companion resource (the skeleton), and it is only meaningful with
// that companion present. Putting a "skinned" flag on `Mesh` would mean every
// consumer of `Mesh` — the OBJ loader, the primitive generators, the static
// renderer path, the mesh tests — has to ask which kind it is holding, and the
// static path would carry a branch it never takes. Two types keep the static path
// exactly as it was.
//
// The skeleton is held by `shared_ptr` rather than by path, for the same reason
// `MeshRenderer3D` holds its mesh that way: the resource manager caches weakly,
// so the mesh owning the last reference is also what keeps the rig loaded. A
// skinned mesh with a null skeleton is legal and draws nothing skinned — it is
// the state an asset is in between "mesh decoded" and "rig decoded".
//
// The mesh keeps its own bind-pose copy of the vertex data. Skinning to a new
// pose never mutates the source, so the CPU skinner writes into a separate
// output buffer and a re-bind is always possible. That is the same "resources are
// immutable" rule the resource manager relies on.
class SkinnedMesh final : public Resource {
public:
    static constexpr const char* kTypeName = "SkinnedMesh";

    SkinnedMesh(std::string path, std::vector<SkinnedVertex3D> vertices, std::vector<u32> indices,
                std::shared_ptr<Skeleton> skeleton, std::optional<Material> material = std::nullopt);

    [[nodiscard]] const char* type_name() const noexcept override { return kTypeName; }

    [[nodiscard]] const std::vector<SkinnedVertex3D>& vertices() const noexcept { return vertices_; }
    [[nodiscard]] const std::vector<u32>& indices() const noexcept { return indices_; }
    [[nodiscard]] usize vertex_count() const noexcept { return vertices_.size(); }
    [[nodiscard]] usize index_count() const noexcept { return indices_.size(); }
    [[nodiscard]] usize triangle_count() const noexcept { return indices_.size() / 3; }

    [[nodiscard]] const std::shared_ptr<Skeleton>& skeleton() const noexcept { return skeleton_; }
    void set_skeleton(std::shared_ptr<Skeleton> skeleton) { skeleton_ = std::move(skeleton); }
    [[nodiscard]] bool has_skeleton() const noexcept { return skeleton_ != nullptr; }

    // The material the source file declared for this primitive, when it declared
    // one. Mirrors `Mesh::material()` exactly, including why it is an `optional`
    // rather than a default-constructed value: "the file said nothing" and "the
    // file said default" are different states, and the renderer's precedence rule
    // has to tell them apart before it overwrites an authored value.
    [[nodiscard]] const std::optional<Material>& material() const noexcept { return material_; }
    void set_material(std::optional<Material> material) noexcept { material_ = std::move(material); }
    [[nodiscard]] bool has_material() const noexcept { return material_.has_value(); }

    // The local-space bounds of the bind pose, for culling and for framing the
    // camera. A skinned mesh's *posed* extent can differ (a raised arm reaches
    // further than the bind pose), so a caller that needs the animated bounds
    // expands these by the palette; the renderer's extract step does exactly
    // that.
    [[nodiscard]] const Vec3& bounds_min() const noexcept { return bounds_min_; }
    [[nodiscard]] const Vec3& bounds_max() const noexcept { return bounds_max_; }

    [[nodiscard]] usize memory_usage() const noexcept override;

private:
    void recompute_bounds();

    std::vector<SkinnedVertex3D> vertices_;
    std::vector<u32> indices_;
    std::shared_ptr<Skeleton> skeleton_;
    std::optional<Material> material_;
    Vec3 bounds_min_{};
    Vec3 bounds_max_{};
};

// Transforms `vertices` by `palette` into `out`, using linear blend skinning.
//
// This is the CPU form of exactly what the skinned vertex shader does, and it
// exists for two reasons that both matter:
//
//   * It makes skinning testable with no GPU. A pose, a palette, and a vertex
//     buffer are values, so a test asserts that a joint rotation moves a vertex
//     the way the rig says it should — an assertion no read-back of a rendered
//     frame can make precisely.
//   * It is the fallback for a caller with no shader (a tool that bakes a pose,
//     a physics proxy built from a posed mesh, an exporter writing a static
//     frame).
//
// The normal is transformed by the palette's upper-left 3x3, which is correct
// when the pose has no non-uniform scale; a general pose would need the
// inverse-transpose. Skinning a mesh whose palette carries a non-uniform scale
// is not something a rig does, and the cheaper form is what the shader uses too,
// so the two agree.
//
// A vertex whose weights sum to zero is written as the identity-transformed
// position (its bind-pose position), not a zero vector, so an unweighted vertex
// stays put rather than collapsing to the origin. The weights are normalised by
// their sum when it is non-zero, so an asset whose weights sum to 0.5 still
// renders at full size.
//
// A joint index past the end of `palette` is skipped, and the remaining weights
// are used; a vertex all of whose joints are out of range falls back to the
// bind-pose position. That is deliberate: a mesh bound to a shorter rig than it
// was authored for renders in its rest shape instead of reading past the end of
// the palette.
void skin_vertices(const std::vector<SkinnedVertex3D>& vertices, const std::vector<Mat4>& palette,
                   std::vector<SkinnedVertex3D>& out);

// The per-vertex form, for a caller that already has one vertex in hand.
[[nodiscard]] SkinnedVertex3D skin_vertex(const SkinnedVertex3D& vertex, const std::vector<Mat4>& palette) noexcept;

// The world-space AABB of `vertices` skinned by `palette`.
//
// A posed mesh can reach outside its bind-pose bounds, so a cull that used the
// bind-pose box would drop a character whose arm is raised. This expands the box
// over the skinned positions, which is what the renderer's extraction uses.
// Returns false (leaving `out_min`/`out_max` untouched) when there are no
// vertices, so a caller can tell "empty" from "degenerate at the origin".
[[nodiscard]] bool skinned_bounds(const std::vector<SkinnedVertex3D>& vertices, const std::vector<Mat4>& palette,
                                  Vec3& out_min, Vec3& out_max);

} // namespace fe::anim
