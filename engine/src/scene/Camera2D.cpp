// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/scene/Camera2D.h"

#include "fe/core/Assert.h"
#include "fe/scene/Node.h"

namespace fe {

Mat4 Camera2D::view_projection(u32 viewport_width, u32 viewport_height) const noexcept {
    FE_CHECK(viewport_width > 0 && viewport_height > 0);
    FE_CHECK(zoom > 0.0f);

    // Centre the visible box on the owning node's world position. The camera
    // node's own scale does not change the visible extent; only `zoom` does,
    // which keeps camera behaviour predictable when a parent scales.
    const Vec3 centre = node() != nullptr ? transform_point(node()->world_matrix(), Vec3{}) : Vec3{};

    const f32 half_width = static_cast<f32>(viewport_width) * 0.5f / zoom;
    const f32 half_height = static_cast<f32>(viewport_height) * 0.5f / zoom;

    const Mat4 view = Mat4::translation(Vec3{-centre.x, -centre.y, 0.0f});
    const Mat4 projection = Mat4::orthographic(-half_width, half_width, -half_height, half_height, -1.0f, 1.0f);
    return projection * view;
}

} // namespace fe
