// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/anim/SkinnedMesh.h"
#include "fe/core/Math.h"
#include "fe/core/Types.h"
#include "fe/mesh/Material.h"
#include "fe/resources/Resource.h"
#include "fe/scene/Component.h"
#include "fe/scene3d/MeshRenderer3D.h"

#include <memory>
#include <optional>

namespace fe::anim {

// Draws a `SkinnedMesh` on its node, deformed by the `Animator` beside it.
//
// Why a separate component rather than a mode on `MeshRenderer3D`. A skinned draw
// has a different vertex stream (joint indices and weights), a different shader,
// and a per-instance palette instead of only a transform, so it shares almost
// nothing with the static draw but the material resolution. Folding it into
// `MeshRenderer3D` would mean every static instance carried skinning fields it
// never sets and the renderer's batch key had to branch on a mode that is fixed
// per component. Two components keep the static path — which is what every
// existing scene and every test uses — byte-identical.
//
// The material handling is deliberately the *same* as `MeshRenderer3D`, down to
// the precedence rule, because a skinned character and a static prop are lit by
// one shader with one material model. Rather than restate the rule, this holds a
// `MeshRenderer3D` and delegates `resolve_surface()` to it, so there is one
// definition of "which material wins" and the skinned shader cannot drift from
// the static one.
//
// A skinned mesh with no animator (or an animator with no palette yet) draws in
// its bind pose, which is exactly its static shape. That is the honest behaviour
// for a character whose clip has not been assigned: it appears, standing, rather
// than vanishing.
class SkinnedMeshRenderer3D final : public Component {
public:
    static constexpr const char* kTypeName = "SkinnedMeshRenderer3D";

    [[nodiscard]] const char* type_name() const noexcept override { return kTypeName; }

    // The mesh to draw. Null is allowed (nothing is drawn), matching
    // `MeshRenderer3D::mesh`.
    std::shared_ptr<Resource> mesh;

    // The authored surface. Held in a `MeshRenderer3D` so the precedence rule
    // (component material when overridden, else the imported mesh's, else the
    // authored fields) and the Blinn-Phong parameters live in one place and are
    // reflected by the same builders the static component uses.
    MeshRenderer3D surface;

    [[nodiscard]] bool has_mesh() const noexcept { return mesh != nullptr; }

    // The skinned mesh, or null when `mesh` is unset or is not one.
    [[nodiscard]] const SkinnedMesh* skinned_mesh() const noexcept;

    // The surface the draw uses. Delegates to the held `MeshRenderer3D`, with the
    // mesh's imported material resolved from the skinned mesh rather than a
    // static one, so a glTF character's own material is used when the component
    // does not override it.
    [[nodiscard]] MeshRenderer3D::Surface resolve_surface() const noexcept;
};

} // namespace fe::anim
