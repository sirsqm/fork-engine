// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Math.h"
#include "fe/core/Types.h"
#include "fe/scene/Component.h"

namespace fe {

// A directional light: an infinitely distant source with a direction and a
// colour. This is the first and only light type in the initial 3D cut.
//
// A directional light has no position, so it is attached to a node only for
// organisation; the direction is authored directly rather than derived from the
// node's transform. That keeps the first cut simple and unambiguous. Point and
// spot lights, which do need a position, are a later step and will read their
// node's world position instead.
class DirectionalLight3D final : public Component {
public:
    static constexpr const char* kTypeName = "DirectionalLight3D";

    [[nodiscard]] const char* type_name() const noexcept override { return kTypeName; }

    // The direction the light travels, in world space. The shader normalises it.
    // Defaults to pointing down and towards the viewer, so an unedited light
    // still reveals form.
    Vec3 direction{-0.4f, -1.0f, -0.3f};

    Vec3 color{1.0f, 1.0f, 1.0f};

    // Scales the light's contribution.
    f32 intensity = 1.0f;

    // A low ambient term so surfaces facing away from the light are not pure
    // black. Kept on the light rather than as a separate ambient component,
    // because with one light there is nothing to blend.
    f32 ambient = 0.15f;
};

} // namespace fe
