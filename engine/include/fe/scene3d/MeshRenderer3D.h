// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Math.h"
#include "fe/core/Types.h"
#include "fe/mesh/Material.h"
#include "fe/mesh/Mesh.h"
#include "fe/resources/Resource.h"
#include "fe/scene/Component.h"

#include <memory>
#include <optional>

namespace fe {

// Draws a Mesh on its node, transformed by the node's world matrix.
//
// The component holds a shared_ptr to the mesh resource, which is what keeps it
// loaded: the ResourceManager caches weakly, so a renderer that owns the last
// reference is also the reason the mesh is not released. This mirrors how
// Sprite2D holds its texture.
//
// Material: the component carries its own `Material` plus the two fields the
// first 3D cut authored directly (`base_color`, `specular`). An imported mesh
// may carry a material of its own (a glTF primitive's, see `mesh::Material`);
// `effective_material()` resolves the two into the one the draw uses. The
// authored fields win, because a designer's edit must not be silently
// overwritten by a re-import, and the imported material is the fallback that
// makes an exported asset look like the tool that made it.
class MeshRenderer3D final : public Component {
public:
    static constexpr const char* kTypeName = "MeshRenderer3D";

    [[nodiscard]] const char* type_name() const noexcept override { return kTypeName; }

    // The mesh to draw. Null is allowed (nothing is drawn) so a scene can be
    // authored before its assets exist.
    std::shared_ptr<Resource> mesh;

    // Albedo, multiplied with the sampled texel (or used alone when untextured).
    // Ignored when `material` is set.
    Vec4 base_color{0.8f, 0.8f, 0.85f, 1.0f};

    // Specular highlight strength, 0 disables the specular term. Ignored when
    // `material` is set.
    f32 specular = 0.3f;

    // Optional albedo texture. Null uses base_color alone (or the material's
    // base-colour texture when one is set).
    std::shared_ptr<Resource> texture;

    // When true, `material` is the material the draw uses and `base_color` /
    // `specular` are ignored. When false, the imported mesh's material is used
    // if it has one, and `base_color` / `specular` otherwise.
    //
    // A flag rather than an `optional<Material>` because that is the shape the
    // reflection system and the serializer already describe (a bool beside a
    // struct), and because the editor wants an explicit "use the imported
    // material" checkbox rather than inferring intent from a value that happens
    // to equal the defaults.
    bool override_material = false;

    // An explicitly authored material, used when `override_material` is set. A
    // plain value, so it is edited and serialized with the component rather than
    // through a resource reference.
    Material material;

    [[nodiscard]] bool has_mesh() const noexcept { return mesh != nullptr; }
    [[nodiscard]] bool has_texture() const noexcept { return texture != nullptr; }

    // The surface a draw uses: the resolved material plus the two Blinn-Phong
    // parameters the shader takes as uniforms. `material` carries colour,
    // textures, and emissive; `specular`/`shininess` are what the shading term
    // needs. Keeping them together is what lets the precedence rule live in one
    // place instead of being re-derived by the renderer.
    struct Surface {
        Material material;
        f32 specular = 0.3f;
        // The Blinn-Phong exponent. The first cut uses one fixed lobe (32), which
        // is what every scene was shaded with before materials existed; a
        // material that carries roughness derives its exponent from it.
        f32 shininess = 32.0f;
    };

    // Resolves the surface the draw uses, in precedence order:
    //   1. this component's `material`, when `override_material` is set;
    //   2. the imported mesh's material, when the mesh carries one;
    //   3. `base_color`/`specular`/`texture`, which is what every scene authored
    //      before materials existed describes, so those scenes render exactly as
    //      they did.
    [[nodiscard]] Surface resolve_surface() const noexcept;

    // The same rule against an imported material the caller supplies, rather
    // than one read off this component's mesh.
    //
    // This exists so the rule has exactly one definition. `resolve_surface()` is
    // this function with the material read off `mesh`; a component that keeps its
    // mesh somewhere else (the skinned renderer, whose mesh is a different type)
    // calls this directly with its own imported material instead of restating the
    // precedence order, which would drift from this one the first time it
    // changed.
    [[nodiscard]] Surface resolve_surface_from(const std::optional<Material>* imported) const noexcept;
};

} // namespace fe
