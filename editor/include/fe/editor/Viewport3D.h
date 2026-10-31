// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Math.h"
#include "fe/core/Types.h"

#include <span>

namespace fe {

class Camera3D;
class Node;
class Scene;

namespace editor {

// The 3D viewport's camera rig, hit-testing, and framing maths, with no toolkit
// and no GPU.
//
// This is the 3D counterpart of `Viewport`, and it exists for the same reason:
// the parts of a 3D view that can be wrong in ways a screenshot would not make
// obvious - which node a click lands on, where the orbit camera points, how a
// "focus the selection" press frames a box - are pure functions of a camera and
// a scene, so they belong in a headless type a test can drive without a window.
// The panel is the thin toolkit projection of this type.
//
// The orbit camera is the authoring convention every 3D editor uses: a pivot
// the view orbits around, a yaw and a pitch, and a distance. It is stored as
// those four values rather than as a free-floating transform because that is
// what makes orbit, pan, and dolly independent and reversible - a free camera
// that integrates an arbitrary delta drifts into an orientation no gesture can
// undo.
//
// The rig drives a real `Camera3D` component on a node in the scene, exactly as
// the 2D `Viewport` drives a `Camera2D`, so the renderer stays the only thing
// that decides how a camera maps to the screen. `apply_to` writes the rig's
// state into the camera node's transform and the camera's field of view.
class Viewport3D {
public:
    // The state of an orbit camera. `pivot` is the world point the view looks
    // at, `yaw`/`pitch` are radians (yaw about world +Y, pitch about the
    // camera's right), and `distance` is how far the eye sits from the pivot.
    struct OrbitState {
        Vec3 pivot{};
        f32 yaw = 0.0f;
        f32 pitch = 0.0f;
        f32 distance = 10.0f;
    };

    Viewport3D() = default;

    // --- Size --------------------------------------------------------------
    // The pixel size of the panel's render target, set every frame from the
    // panel's content region. A zero size means there is nothing to pick and
    // every query returns "miss", mirroring `Viewport`.
    void set_size(u32 width, u32 height) noexcept;
    [[nodiscard]] u32 width() const noexcept { return width_; }
    [[nodiscard]] u32 height() const noexcept { return height_; }

    // --- Rig state ---------------------------------------------------------
    [[nodiscard]] const OrbitState& orbit() const noexcept { return orbit_; }
    void set_orbit(const OrbitState& orbit) noexcept { orbit_ = orbit; }

    // Reads the rig back out of a camera node's transform and field of view, so
    // a camera authored in a scene file is the starting view. The pivot is
    // placed `distance` in front of the eye along the camera's forward, which is
    // the point the orbit will then rotate about.
    void sync_from_camera(const Camera3D& camera) noexcept;

    // Whether the rig has been seeded from a camera. The panel seeds once, the
    // first time it shows a 3D view, and re-seeds when the view is entered
    // again, so an authored camera is the starting view rather than being
    // overwritten by the default rig - but the user's orbit is not undone every
    // frame.
    [[nodiscard]] bool is_seeded() const noexcept { return seeded_; }
    void mark_seeded() noexcept { seeded_ = true; }
    void clear_seeded() noexcept { seeded_ = false; }

    // Writes the rig into `camera` (its field of view) and its owning node's
    // transform (the eye position and orientation). A camera with no owning node
    // is a no-op: there is nothing to move.
    void apply_to(Camera3D& camera) const noexcept;

    // The eye position the current rig places the camera at.
    [[nodiscard]] Vec3 eye() const noexcept;

    // --- Gestures ----------------------------------------------------------
    // Orbits the view by a mouse drag in screen pixels. `mouse_delta` is the
    // movement since the previous frame (Y down, as the input layer reports).
    // Yaw follows the horizontal drag and pitch the vertical, and pitch is
    // clamped short of the poles so the up vector never degenerates.
    void orbit_by_pixels(const Vec3& mouse_delta) noexcept;

    // Pans the pivot (and so the whole view) in the plane facing the camera, by
    // a mouse drag in screen pixels. The world tracks the cursor, as the 2D
    // viewport's pan does. `camera` supplies the frame the pan happens in.
    void pan_by_pixels(const Camera3D& camera, const Vec2& mouse_delta) noexcept;

    // Dollies the eye toward or away from the pivot by a wheel delta (positive
    // scrolls up, i.e. moves closer). The distance is multiplicative so a fast
    // scroll changes it proportionally, and clamped so the eye cannot reach the
    // pivot or run off to infinity.
    void dolly_by_scroll(f32 scroll) noexcept;

    // --- Framing -----------------------------------------------------------
    // Sets the rig so a world-space AABB fills the view: the pivot moves to the
    // box centre and the distance is chosen so the box's radius fits the
    // vertical field of view with a small margin. This is "focus the selection",
    // and it is why a selected node can always be found again. A degenerate box
    // (radius ~ 0) falls back to a minimum distance rather than a zero one.
    void frame_bounds(const Vec3& box_min, const Vec3& box_max, f32 fov_degrees) noexcept;

    // The world-space AABB of a node's drawable content (its mesh), so
    // `frame_bounds` has something to frame. Returns false when the node has no
    // drawable content - a bare transform, a camera, a light - because such a
    // node has no extent to frame or to hit, and giving it a placeholder box
    // would make the active camera selectable through the view it renders. The
    // box is in world space, so a child node's bounds include its parent
    // transform.
    [[nodiscard]] static bool node_world_bounds(const Node& node, Vec3& out_min, Vec3& out_max) noexcept;

    // The union of the world-space AABBs of every node that has bounds, so
    // "frame all" has a box. Returns false for an empty scene.
    [[nodiscard]] static bool scene_world_bounds(const Scene& scene, Vec3& out_min, Vec3& out_max) noexcept;

    // --- Picking -----------------------------------------------------------
    // The world-space ray through a screen position in the content region, as
    // (origin, direction) with a unit direction. `screen_point` is measured from
    // the top-left of the content region.
    [[nodiscard]] bool screen_ray(const Camera3D& camera, const Vec2& screen_point, Vec3& out_origin,
                                  Vec3& out_direction) const noexcept;

    // The node under `screen_point`, or nullptr. Each candidate's world AABB is
    // intersected with the ray and the nearest hit wins, so a small object in
    // front of a large one is selected. The candidate set is the nodes that have
    // bounds, walked in scene order; ties break toward the nearer hit.
    [[nodiscard]] Node* pick(const Scene& scene, const Camera3D& camera, const Vec2& screen_point) const noexcept;

    // Slab-method ray/AABB intersection. Returns true and the entry distance
    // `t` (>= 0) when the ray meets the box. A ray that starts inside the box
    // reports t = 0. Exposed so the hit-test is testable independent of picking.
    [[nodiscard]] static bool ray_intersects_aabb(const Vec3& origin, const Vec3& direction, const Vec3& box_min,
                                                  const Vec3& box_max, f32& out_t) noexcept;

    // Projects a world point to content-region screen coordinates (top-left
    // origin). Returns false when the point is behind the camera, in which case
    // the output is left untouched - so a caller drawing a gizmo can skip the
    // point rather than draw it at a mirrored position.
    [[nodiscard]] bool world_to_screen(const Camera3D& camera, const Vec3& world, Vec2& out_screen) const noexcept;

    // The world point where the ray through `screen_point` meets the plane
    // through `plane_point` with normal `plane_normal`. Returns false when the
    // ray is parallel to the plane or the plane is behind the camera. This is
    // how a drag converts a cursor position into a world point: the drag plane
    // is the camera-facing plane through the grabbed point, so the object tracks
    // the cursor without the depth ambiguity of a single ray.
    [[nodiscard]] bool screen_to_plane(const Camera3D& camera, const Vec2& screen_point, const Vec3& plane_point,
                                       const Vec3& plane_normal, Vec3& out_world) const noexcept;

    // The eight corners of a world-space AABB, written in the order
    // (min,min,min), (max,min,min), (max,max,min), (min,max,min), then the same
    // four with max.z. Provided so a caller drawing a selection box does not
    // duplicate the corner order. Writes nothing when `out` is shorter than
    // eight entries.
    static void aabb_corners(const Vec3& box_min, const Vec3& box_max, std::span<Vec3> out) noexcept;

private:
    // The camera basis (forward, right, up) for the current rig, so the ray and
    // projection maths are defined in exactly one place.
    void basis(Vec3& out_forward, Vec3& out_right, Vec3& out_up) const noexcept;

    u32 width_ = 0;
    u32 height_ = 0;
    OrbitState orbit_{};
    bool seeded_ = false;
};

} // namespace editor
} // namespace fe
