// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Math.h"
#include "fe/core/Types.h"

#include <span>

namespace fe {

class Camera2D;
class Node;
class Scene;

namespace editor {

// The 2D viewport's camera and hit-testing, with no toolkit and no GPU.
//
// The viewport panel is a thin projection of this type: the panel forwards the
// mouse's pan/zoom/click into it and draws the result, while the transform maths
// and the picking live here so they are testable without a window. This is the
// same split the editor uses elsewhere (see `HierarchyViewModel`): the decision
// logic is headless, the drawing is a shell over it.
//
// The view is a `Camera2D` component attached to a node in the scene, so what
// the viewport shows is the scene's real camera and the renderer stays the only
// thing that decides how a camera maps to the screen.
class Viewport {
public:
    Viewport() = default;

    // --- Size --------------------------------------------------------------
    // The pixel size of the panel's render target. Set every frame from the
    // panel's content region; a zero size means there is nothing to draw or pick
    // and every query below returns "miss".
    void set_size(u32 width, u32 height) noexcept;
    [[nodiscard]] u32 width() const noexcept { return width_; }
    [[nodiscard]] u32 height() const noexcept { return height_; }

    // --- Camera control ----------------------------------------------------
    // Pans the camera by a mouse drag in screen pixels. `mouse_delta` is the
    // movement since the previous frame (Y down, as the input layer reports).
    // The world moves with the cursor, so a drag right reveals content to the
    // left, exactly as a hand-tool would.
    void pan_by_pixels(Camera2D& camera, const Vec2& mouse_delta) noexcept;

    // Zooms about the cursor. `scroll` is the wheel delta (positive scrolls up,
    // i.e. zooms in). `cursor` is the mouse position in the content region. The
    // world point under the cursor stays under the cursor, so zooming feels
    // anchored rather than jumping to the centre.
    void zoom_at_cursor(Camera2D& camera, const Vec2& cursor, f32 scroll) noexcept;

    // --- Picking -----------------------------------------------------------
    // The world-space point under a screen position in the content region.
    [[nodiscard]] Vec2 screen_to_world(const Camera2D& camera, const Vec2& screen_point) const noexcept;

    // The node under `screen_point`, or nullptr. The first node in scene order
    // whose sprite quad contains the point wins; ties break toward the smaller
    // (more specific) sprite so a small icon on a large panel is selectable.
    [[nodiscard]] Node* pick(const Scene& scene, const Camera2D& camera, const Vec2& screen_point) const noexcept;

    // True when the world point is inside the node's sprite quad. Exposed so the
    // hit-test itself is testable, independent of picking.
    [[nodiscard]] static bool node_contains_world_point(const Node& node, const Vec2& world_point) noexcept;

    // The node's sprite quad in world space, as (min, max) axis-aligned bounds.
    // Returns false when the node has no sprite (nothing to pick or outline).
    [[nodiscard]] static bool node_world_bounds(const Node& node, Vec2& out_min, Vec2& out_max) noexcept;

    // The smallest sprite quad under the cursor, for drawing a selection
    // outline. Same rule as pick(), but it returns the bounds so the caller
    // draws something even before picking is wired to selection.
    [[nodiscard]] bool pick_bounds(const Scene& scene, const Camera2D& camera, const Vec2& screen_point, Vec2& out_min,
                                   Vec2& out_max) const noexcept;

    // Converts world bounds to the four content-region corners of a screen-space
    // quad, in the order min-x/min-y, max-x/min-y, max-x/max-y, min-x/max-y. Empty
    // when the output span is shorter than four points, which keeps the caller
    // from over-reading.
    void world_bounds_to_screen(const Camera2D& camera, const Vec2& min, const Vec2& max,
                                std::span<Vec2> out_quad) const noexcept;

private:
    // The world half-extents and the camera centre of the current view, so the
    // transform is defined in exactly one place.
    void world_view(const Camera2D& camera, Vec2& out_centre, Vec2& out_half_extent) const noexcept;

    u32 width_ = 0;
    u32 height_ = 0;
};

} // namespace editor
} // namespace fe
