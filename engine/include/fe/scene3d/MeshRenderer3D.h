// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Math.h"
#include "fe/core/Types.h"
#include "fe/mesh/Mesh.h"
#include "fe/resources/Resource.h"
#include "fe/scene/Component.h"

#include <memory>

namespace fe {

// Draws a Mesh on its node, transformed by the node's world matrix.
//
// The component holds a shared_ptr to the mesh resource, which is what keeps it
// loaded: the ResourceManager caches weakly, so a renderer that owns the last
// reference is also the reason the mesh is not released. This mirrors how
// Sprite2D holds its texture.
//
// The material is deliberately minimal for the first 3D cut: a base colour, a
// specular strength, and an optional texture. A full material graph is a later
// step, and modelling it as one flat struct now keeps the shader and the
// inspector honest about what actually exists.
class MeshRenderer3D final : public Component {
public:
    static constexpr const char* kTypeName = "MeshRenderer3D";

    [[nodiscard]] const char* type_name() const noexcept override { return kTypeName; }

    // The mesh to draw. Null is allowed (nothing is drawn) so a scene can be
    // authored before its assets exist.
    std::shared_ptr<Resource> mesh;

    // Albedo, multiplied with the sampled texel (or used alone when untextured).
    Vec4 base_color{0.8f, 0.8f, 0.85f, 1.0f};

    // Specular highlight strength, 0 disables the specular term.
    f32 specular = 0.3f;

    // Optional albedo texture. Null uses base_color alone.
    std::shared_ptr<Resource> texture;

    [[nodiscard]] bool has_mesh() const noexcept { return mesh != nullptr; }
    [[nodiscard]] bool has_texture() const noexcept { return texture != nullptr; }
};

} // namespace fe
