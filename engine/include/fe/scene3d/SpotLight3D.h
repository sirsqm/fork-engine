// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Math.h"
#include "fe/core/Types.h"
#include "fe/scene/Component.h"

namespace fe {

// A spot light: a point light whose emission is confined to a cone about the
// owning node's forward axis.
//
// The cone is defined by two angles. `inner_cone_degrees` is the half-angle of
// full intensity; `outer_cone_degrees` is the half-angle at which intensity
// reaches zero. Between the two the light fades smoothly, which is what stops a
// spot light from having a hard, aliased edge. The renderer keeps the outer
// angle strictly greater than the inner one so the fade has a non-zero width.
//
// The light points along the node's local -Z, the same convention Camera3D
// uses, so a node's rotation aims a spot light the way it aims a camera.
class SpotLight3D final : public Component {
public:
    static constexpr const char* kTypeName = "SpotLight3D";

    [[nodiscard]] const char* type_name() const noexcept override { return kTypeName; }

    Vec3 color{1.0f, 1.0f, 1.0f};

    f32 intensity = 1.0f;

    // Distance at which the light's contribution reaches zero.
    f32 range = 15.0f;

    // Half-angles of the cone, in degrees.
    f32 inner_cone_degrees = 20.0f;
    f32 outer_cone_degrees = 30.0f;
};

} // namespace fe
