// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Math.h"
#include "fe/core/Types.h"

#include <array>

namespace fe {

// The six planes bounding a view frustum, for visibility tests.
//
// This is the first culling primitive. It exists so the 3D renderer can skip
// meshes that cannot appear on screen without touching the GPU, which is where
// the win is: a rejected instance costs a handful of dot products instead of a
// draw call. It deliberately does not model a spatial hierarchy yet - the
// renderer still walks every node, it just does not draw the invisible ones.
//
// Planes are stored in world space as `(normal.xyz, d)` with the plane's normal
// pointing *inward* (towards the visible region), so a point is inside the
// frustum when `dot(normal, point) + d >= 0` for all six planes. That sign
// convention is what makes the AABB test below a simple max/min over corners.
//
// The frustum's shape is captured once from a view-projection matrix; it is a
// plain value type with no lifetime coupling to the matrix.
struct Frustum {
    // Inward-facing planes in the order left, right, bottom, top, near, far.
    std::array<Vec4, 6> planes{};

    enum PlaneIndex : usize { Left = 0, Right = 1, Bottom = 2, Top = 3, Near = 4, Far = 5 };

    // Extracts the six planes from a clip-space matrix (typically
    // view * projection), using the standard row-sum method for the OpenGL
    // clip convention (x/y/z in [-1, 1], w > 0 in front of the camera).
    //
    // The matrix is column-major: row `r` of the logical matrix is
    // `{at(r,0), at(r,1), at(r,2), at(r,3)}`, and each clip coordinate is that
    // row dotted with the world-space point. Each plane is a sum or difference
    // of a clip row and the w row.
    [[nodiscard]] static Frustum from_view_projection(const Mat4& view_projection) noexcept {
        const Vec4 row0{view_projection.at(0, 0), view_projection.at(0, 1), view_projection.at(0, 2),
                        view_projection.at(0, 3)};
        const Vec4 row1{view_projection.at(1, 0), view_projection.at(1, 1), view_projection.at(1, 2),
                        view_projection.at(1, 3)};
        const Vec4 row2{view_projection.at(2, 0), view_projection.at(2, 1), view_projection.at(2, 2),
                        view_projection.at(2, 3)};
        const Vec4 row3{view_projection.at(3, 0), view_projection.at(3, 1), view_projection.at(3, 2),
                        view_projection.at(3, 3)};

        Frustum frustum;
        frustum.planes[Left] = normalize_plane(row3 + row0);
        frustum.planes[Right] = normalize_plane(row3 - row0);
        frustum.planes[Bottom] = normalize_plane(row3 + row1);
        frustum.planes[Top] = normalize_plane(row3 - row1);
        frustum.planes[Near] = normalize_plane(row3 + row2);
        frustum.planes[Far] = normalize_plane(row3 - row2);
        return frustum;
    }

    // True if `point` is on the inward side of every plane.
    [[nodiscard]] bool contains_point(const Vec3& point) const noexcept {
        for (const Vec4& plane : planes) {
            if (plane.x * point.x + plane.y * point.y + plane.z * point.z + plane.w < 0.0f) {
                return false;
            }
        }
        return true;
    }

    // Conservative axis-aligned-box test: false only when the box is *definitely*
    // outside, i.e. entirely on the outer side of at least one plane. A box that
    // straddles a plane is reported as intersecting, so the worst case is an
    // extra draw, never a wrongly hidden mesh.
    //
    // For each plane it picks the box corner furthest along the plane normal
    // (the "positive vertex"); if even that corner is outside, every corner is.
    [[nodiscard]] bool intersects_aabb(const Vec3& box_min, const Vec3& box_max) const noexcept {
        for (const Vec4& plane : planes) {
            const Vec3 positive{plane.x >= 0.0f ? box_max.x : box_min.x, plane.y >= 0.0f ? box_max.y : box_min.y,
                                plane.z >= 0.0f ? box_max.z : box_min.z};
            if (plane.x * positive.x + plane.y * positive.y + plane.z * positive.z + plane.w < 0.0f) {
                return false;
            }
        }
        return true;
    }

private:
    // Scales `(a, b, c, d)` so `(a, b, c)` is unit length. The sign of d is
    // preserved, so the "inside is >= 0" convention survives. A degenerate
    // plane (zero normal) is returned unchanged rather than divided by zero;
    // a zero-normal plane contributes nothing to either test.
    [[nodiscard]] static Vec4 normalize_plane(const Vec4& plane) noexcept {
        const f32 length = std::sqrt(plane.x * plane.x + plane.y * plane.y + plane.z * plane.z);
        if (length <= 0.0f) {
            return plane;
        }
        const f32 inv = 1.0f / length;
        return Vec4{plane.x * inv, plane.y * inv, plane.z * inv, plane.w * inv};
    }
};

} // namespace fe
