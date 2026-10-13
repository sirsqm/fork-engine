// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Math.h"
#include "fe/core/Types.h"
#include "fe/scene/Component.h"

namespace fe {

// A point light: a source at a position that radiates in all directions and
// falls off to nothing at `range`.
//
// Unlike DirectionalLight3D, a point light needs a position, so it reads its
// owning node's world position rather than storing one. That is the same rule
// the camera follows, and it means a light can be parented and animated like
// any other node.
//
// The renderer draws a bounded number of point lights per frame (see
// Renderer3D::kMaxPointLights); the brightest are kept when a scene has more.
class PointLight3D final : public Component {
public:
    static constexpr const char* kTypeName = "PointLight3D";

    [[nodiscard]] const char* type_name() const noexcept override { return kTypeName; }

    Vec3 color{1.0f, 1.0f, 1.0f};

    // Scales the light's contribution.
    f32 intensity = 1.0f;

    // Distance at which the light's contribution reaches zero. A finite range
    // keeps the falloff cheap (no division by distance squared that never
    // actually reaches zero) and bounds a light's influence, which matters once
    // culling by light is added. Must be greater than zero to be visible.
    f32 range = 10.0f;
};

} // namespace fe
