// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Math.h"
#include "fe/core/Types.h"
#include "fe/scene/Component.h"

namespace fe {

// A 2D camera. Attach to a node; the node's world position becomes the camera
// centre, and its Z rotation rotates the view.
//
// The camera does not own a viewport or a framebuffer. It computes a
// view-projection matrix for a given viewport size, which is what lets the same
// camera drive the game window and an editor viewport of a different size.
class Camera2D final : public Component {
public:
    static constexpr const char* kTypeName = "Camera2D";

    [[nodiscard]] const char* type_name() const noexcept override { return kTypeName; }

    // World units per pixel. 1.0 shows one world unit per pixel at the
    // viewport's reference resolution.
    f32 zoom = 1.0f;

    // Background colour the renderer clears to when this is the active camera.
    f32 clear_r = 0.10f;
    f32 clear_g = 0.10f;
    f32 clear_b = 0.12f;
    f32 clear_a = 1.0f;

    // When several cameras exist, the renderer uses the first primary one.
    bool primary = true;

    // Builds the view-projection matrix for a viewport of the given pixel size.
    // The visible world box is centred on the owning node's world position and
    // spans viewport_size / zoom world units.
    [[nodiscard]] Mat4 view_projection(u32 viewport_width, u32 viewport_height) const noexcept;
};

} // namespace fe
