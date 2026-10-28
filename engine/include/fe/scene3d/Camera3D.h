// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Math.h"
#include "fe/core/Types.h"
#include "fe/scene/Component.h"

namespace fe {

// A 3D camera. Attach to a node; the node's world position is the eye, and the
// camera looks along the node's local -Z axis (the same convention as
// look_at()). The node's world rotation therefore aims the camera.
//
// The camera does not own a viewport or a framebuffer. It computes a
// view-projection matrix for a given viewport size, which is what lets the same
// camera drive the game window and an editor viewport of a different size.
class Camera3D final : public Component {
public:
    static constexpr const char* kTypeName = "Camera3D";

    [[nodiscard]] const char* type_name() const noexcept override { return kTypeName; }

    // Vertical field of view, in degrees. Stored in degrees because that is what
    // a designer types into an inspector; converted to radians when the matrix
    // is built.
    f32 fov_degrees = 60.0f;

    f32 near_plane = 0.1f;
    f32 far_plane = 1000.0f;

    // Background colour the renderer clears to when this is the active camera.
    f32 clear_r = 0.10f;
    f32 clear_g = 0.10f;
    f32 clear_b = 0.12f;
    f32 clear_a = 1.0f;

    // When several cameras exist, the renderer uses the first primary one.
    bool primary = true;

    // Environment background: a vertical gradient drawn on a full-screen quad
    // behind the geometry, from `background_bottom` at the horizon to
    // `background_top` at the top of the frame. A vertical screen gradient is
    // the cheapest environment that reads as a sky and needs no cubemap, which
    // is the deliberate first cut of M4's "environment rendering" row. Set
    // `background_enabled = false` for a flat clear instead, which is the
    // behaviour every scene had before this existed.
    bool background_enabled = false;
    Vec3 background_top{0.32f, 0.52f, 0.85f};
    Vec3 background_bottom{0.76f, 0.83f, 0.92f};

    // Builds the view-projection matrix for a viewport of the given pixel size,
    // using the owning node's world transform as the eye and orientation.
    [[nodiscard]] Mat4 view_projection(u32 viewport_width, u32 viewport_height) const noexcept;

    // The eye position and the point the camera is looking at, derived from the
    // owning node's world transform. Exposed so tests can assert orientation
    // without duplicating the maths.
    [[nodiscard]] Vec3 eye() const noexcept;
    [[nodiscard]] Vec3 forward() const noexcept;

    // The camera's world-space up axis (its local +Y rotated by the world
    // matrix), so a listener can take the ear's up as well as its forward. Like
    // `forward()` it is derived from the owning node, so a camera on a banking
    // rig reports a rolled up rather than world up. Falls back to world up when
    // the camera has no owner.
    [[nodiscard]] Vec3 up() const noexcept;
};

} // namespace fe
