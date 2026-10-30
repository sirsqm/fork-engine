// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/anim/SkinnedMeshRenderer3D.h"

#include "fe/mesh/Mesh.h"

#include <optional>

namespace fe::anim {

const SkinnedMesh* SkinnedMeshRenderer3D::skinned_mesh() const noexcept {
    return dynamic_cast<const SkinnedMesh*>(mesh.get());
}

MeshRenderer3D::Surface SkinnedMeshRenderer3D::resolve_surface() const noexcept {
    // The precedence rule lives in `MeshRenderer3D` and is reached through
    // `resolve_surface_from`, which takes the imported material as an argument
    // rather than reading it off the holder's mesh. That is what lets a skinned
    // character use the identical rule: this component's mesh is a `SkinnedMesh`,
    // which the static component could not read, so the material is resolved here
    // and the rule itself is not restated.
    const SkinnedMesh* typed = skinned_mesh();
    const std::optional<Material>* imported =
        (typed != nullptr && typed->has_material()) ? &typed->material() : nullptr;
    return surface.resolve_surface_from(imported);
}

} // namespace fe::anim
