// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/physics2d/Geometry.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace fe::physics2d {
namespace {

// Below this a direction component is treated as zero, so a ray parallel to a
// slab does not divide by it. The value is well under any meaningful world unit
// and well above denormal noise.
constexpr f32 kDirectionEpsilon = 1e-8f;

// Rotate `v` by `angle` radians about the origin. The 2D rotation used to move
// a circle into a box's local frame and a local normal back into world space.
[[nodiscard]] Vec2 rotate(const Vec2& v, f32 angle) noexcept {
    const f32 c = std::cos(angle);
    const f32 s = std::sin(angle);
    return Vec2{v.x * c - v.y * s, v.x * s + v.y * c};
}

// The half-width of an oriented box's projection onto the unit `axis`: the sum
// of each half-extent times the absolute cosine between that local axis and
// `axis`. This is the `r` term of the separating-axis test, and it is exact
// because the box's axes are orthonormal.
[[nodiscard]] f32 project_radius(const Obb2D& box, const Vec2& axis) noexcept {
    const f32 along_x = std::abs(axis.dot(box.axis_x()));
    const f32 along_y = std::abs(axis.dot(box.axis_y()));
    return box.half_extent.x * along_x + box.half_extent.y * along_y;
}

// The separating-axis test for two oriented boxes. A box is convex with only
// two face normals, so the candidate separating axes are the four face normals
// (two from each box); on each, the boxes are projected to intervals and
// separated if the intervals do not overlap. The axis with the least overlap is
// the contact normal and that overlap is the penetration, matching the
// axis-aligned test this generalises. The result is exact: SAT reports a
// collision if and only if the true shapes overlap, which is what lets a rotated
// box stop being collided as its (larger) bounding box.
Contact2D intersect_obbs(const Obb2D& a, const Obb2D& b) noexcept {
    const Vec2 delta = b.center - a.center;
    const Vec2 axes[4] = {a.axis_x(), a.axis_y(), b.axis_x(), b.axis_y()};

    f32 min_overlap = 0.0f;
    Vec2 min_axis{};
    for (const Vec2& axis : axes) {
        const f32 overlap = project_radius(a, axis) + project_radius(b, axis) - std::abs(delta.dot(axis));
        if (overlap <= 0.0f) {
            return Contact2D{}; // a separating axis: the boxes do not touch
        }
        // Strictly less keeps the first axis on a tie, so the result is
        // deterministic for aligned boxes (a face axis of A wins over B's).
        if (min_overlap == 0.0f || overlap < min_overlap) {
            min_overlap = overlap;
            min_axis = axis;
        }
    }

    Contact2D contact;
    contact.overlapping = true;
    contact.penetration = min_overlap;
    // Normal points from A towards B. A zero projection means the centres are
    // level on this axis; pick + to match the axis-aligned test's tie-break.
    contact.normal = min_axis * (delta.dot(min_axis) < 0.0f ? -1.0f : 1.0f);
    return contact;
}

Contact2D intersect_circles(const Circle2D& a, const Circle2D& b) noexcept {
    const f32 radius_sum = a.radius + b.radius;
    if (radius_sum <= 0.0f) {
        return Contact2D{};
    }
    const Vec2 delta = b.center - a.center;
    const f32 distance_squared = delta.length_squared();
    if (distance_squared >= radius_sum * radius_sum) {
        return Contact2D{};
    }

    const f32 distance = std::sqrt(distance_squared);
    Contact2D contact;
    contact.overlapping = true;
    contact.penetration = radius_sum - distance;
    if (distance > 0.0f) {
        contact.normal = delta / distance;
    } else {
        // Concentric circles have no defined direction; pick one so the solver
        // still separates them deterministically instead of dividing by zero.
        contact.normal = Vec2{0.0f, 1.0f};
    }
    return contact;
}

// Circle against an oriented box, resolved in the box's local frame: rotate the
// circle centre by -rotation so the box is axis-aligned, run the closest-point
// test there, then rotate the normal back. Doing the work in the local frame
// keeps the face, edge, corner, and centre-inside cases identical to the
// axis-aligned version -- the only difference is the frame it is computed in.
Contact2D intersect_circle_obb(const Circle2D& circle, const Obb2D& box) noexcept {
    const Vec2 local = rotate(circle.center - box.center, -box.rotation);
    const f32 closest_x = std::clamp(local.x, -box.half_extent.x, box.half_extent.x);
    const f32 closest_y = std::clamp(local.y, -box.half_extent.y, box.half_extent.y);
    const Vec2 delta = Vec2{closest_x, closest_y} - local;
    const f32 distance_squared = delta.length_squared();

    if (distance_squared > circle.radius * circle.radius) {
        return Contact2D{};
    }

    Contact2D contact;
    contact.overlapping = true;
    Vec2 normal_local{};
    if (distance_squared > 0.0f) {
        const f32 distance = std::sqrt(distance_squared);
        // Normal points from the circle towards the box (in the local frame).
        normal_local = delta / distance;
        contact.penetration = circle.radius - distance;
    } else {
        // The circle centre is inside the box. Separate along the nearest face.
        const f32 to_left = local.x + box.half_extent.x;
        const f32 to_right = box.half_extent.x - local.x;
        const f32 to_bottom = local.y + box.half_extent.y;
        const f32 to_top = box.half_extent.y - local.y;
        const f32 min_x = std::min(to_left, to_right);
        const f32 min_y = std::min(to_bottom, to_top);
        if (min_x < min_y) {
            normal_local = Vec2{to_right < to_left ? 1.0f : -1.0f, 0.0f};
            contact.penetration = min_x + circle.radius;
        } else {
            normal_local = Vec2{0.0f, to_top < to_bottom ? 1.0f : -1.0f};
            contact.penetration = min_y + circle.radius;
        }
    }
    contact.normal = rotate(normal_local, box.rotation);
    return contact;
}

// The zero-rotation `Obb2D` matching an axis-aligned box. The axis-aligned
// entry points are thin wrappers over the oriented ones, so there is one narrow
// phase rather than two that can drift apart.
[[nodiscard]] Obb2D as_obb(const Aabb2D& box) noexcept {
    return Obb2D{box.center, box.half_extent, 0.0f};
}

} // namespace

Contact2D intersect(const Aabb2D& a, const Aabb2D& b) noexcept {
    return intersect_obbs(as_obb(a), as_obb(b));
}

Contact2D intersect(const Circle2D& a, const Circle2D& b) noexcept {
    return intersect_circles(a, b);
}

Contact2D intersect(const Circle2D& a, const Aabb2D& b) noexcept {
    return intersect_circle_obb(a, as_obb(b));
}

Contact2D intersect(const Obb2D& a, const Obb2D& b) noexcept {
    return intersect_obbs(a, b);
}

Contact2D intersect(const Circle2D& a, const Obb2D& b) noexcept {
    return intersect_circle_obb(a, b);
}

// Box vs circle: compute circle-vs-box and flip the normal so it still points
// from the box towards the circle.
Contact2D intersect(const Obb2D& a, const Circle2D& b) noexcept {
    Contact2D contact = intersect_circle_obb(b, a);
    contact.normal = Vec2{-contact.normal.x, -contact.normal.y};
    return contact;
}

bool bounds_overlap(const Aabb2D& a, const Aabb2D& b) noexcept {
    return a.min_x() < b.max_x() && a.max_x() > b.min_x() && a.min_y() < b.max_y() && a.max_y() > b.min_y();
}

bool bounds_overlap(const Aabb2D& box, const Circle2D& circle) noexcept {
    return bounds_overlap(box, bounds_of(circle));
}

Aabb2D bounds_of(const Circle2D& circle) noexcept {
    return Aabb2D{circle.center, Vec2{circle.radius, circle.radius}};
}

Aabb2D bounds_of(const Aabb2D& box) noexcept {
    return box;
}

Aabb2D bounds_of(const Obb2D& box) noexcept {
    // The extent of the oriented box along each world axis is the projection
    // radius, which is exact for a box, so this is the tightest axis-aligned box
    // that contains the rotated shape. With rotation 0 it reduces to the box's
    // own half-extent, matching `bounds_of(const Aabb2D&)`.
    const Vec2 extent{project_radius(box, Vec2{1.0f, 0.0f}), project_radius(box, Vec2{0.0f, 1.0f})};
    return Aabb2D{box.center, extent};
}

f32 support_radius(const Obb2D& box, const Vec2& axis) noexcept {
    return project_radius(box, axis);
}

Contact2D intersect_shapes(ShapeType a_type, const Aabb2D& a_box, const Circle2D& a_circle, ShapeType b_type,
                           const Aabb2D& b_box, const Circle2D& b_circle) noexcept {
    return intersect_shapes(a_type, as_obb(a_box), a_circle, b_type, as_obb(b_box), b_circle);
}

Contact2D intersect_shapes(ShapeType a_type, const Obb2D& a_box, const Circle2D& a_circle, ShapeType b_type,
                           const Obb2D& b_box, const Circle2D& b_circle) noexcept {
    if (a_type == ShapeType::Box && b_type == ShapeType::Box) {
        return intersect(a_box, b_box);
    }
    if (a_type == ShapeType::Circle && b_type == ShapeType::Circle) {
        return intersect(a_circle, b_circle);
    }
    if (a_type == ShapeType::Circle && b_type == ShapeType::Box) {
        return intersect(a_circle, b_box);
    }
    // Box vs circle: compute circle-vs-box and flip the normal so it still
    // points from the first shape towards the second.
    return intersect(a_box, b_circle);
}

// --- Ray casts -------------------------------------------------------------

f32 raycast_aabb(const Vec2& origin, const Vec2& direction, const Aabb2D& box) noexcept {
    // Slab test. For each axis the ray is inside the slab for t in [t_near,
    // t_far]; the ray enters the box at the larger of the near values and leaves
    // at the smaller of the far values. An axis-parallel ray (a zero direction
    // component) is inside its slab only if the origin is already between the
    // faces, which is handled without dividing by zero.
    f32 t_near = -std::numeric_limits<f32>::infinity();
    f32 t_far = std::numeric_limits<f32>::infinity();

    const f32 origin_axis[2] = {origin.x, origin.y};
    const f32 direction_axis[2] = {direction.x, direction.y};
    const f32 min_axis[2] = {box.min_x(), box.min_y()};
    const f32 max_axis[2] = {box.max_x(), box.max_y()};

    for (int axis = 0; axis < 2; ++axis) {
        const f32 o = origin_axis[axis];
        const f32 d = direction_axis[axis];
        if (std::abs(d) < kDirectionEpsilon) {
            // Parallel to this slab: a miss unless the origin is within it.
            if (o < min_axis[axis] || o > max_axis[axis]) {
                return -1.0f;
            }
            continue;
        }
        const f32 inv = 1.0f / d;
        f32 t0 = (min_axis[axis] - o) * inv;
        f32 t1 = (max_axis[axis] - o) * inv;
        if (t0 > t1) {
            std::swap(t0, t1);
        }
        t_near = std::max(t_near, t0);
        t_far = std::min(t_far, t1);
        if (t_near > t_far) {
            return -1.0f;
        }
    }

    // The origin is inside the box when t_near < 0 < t_far; report a hit at 0
    // rather than a negative distance so callers always get a forward hit.
    if (t_far < 0.0f) {
        return -1.0f; // the box is entirely behind the ray
    }
    return t_near > 0.0f ? t_near : 0.0f;
}

f32 raycast_circle(const Vec2& origin, const Vec2& direction, const Circle2D& circle) noexcept {
    if (circle.radius <= 0.0f) {
        return -1.0f;
    }
    // Solve |origin + t*direction - center|^2 = r^2 for the smallest t >= 0.
    const Vec2 to_origin = origin - circle.center;
    const f32 b = to_origin.dot(direction);
    const f32 c = to_origin.length_squared() - circle.radius * circle.radius;
    if (c > 0.0f && b > 0.0f) {
        return -1.0f; // origin outside and pointing away from the circle
    }
    const f32 discriminant = b * b - c;
    if (discriminant < 0.0f) {
        return -1.0f;
    }
    const f32 root = std::sqrt(discriminant);
    const f32 t = -b - root;
    if (t >= 0.0f) {
        return t;
    }
    // The origin is inside the circle; the exit root is behind the far side, so
    // report an immediate hit.
    return 0.0f;
}

f32 raycast_shape(const Vec2& origin, const Vec2& direction, ShapeType type, const Aabb2D& box,
                  const Circle2D& circle) noexcept {
    return type == ShapeType::Circle ? raycast_circle(origin, direction, circle) : raycast_aabb(origin, direction, box);
}

Vec2 raycast_aabb_normal(const Vec2& origin, const Vec2& direction, const Aabb2D& box, f32 distance) noexcept {
    if (distance <= 0.0f) {
        // The origin is inside (or on) the box; there is no crossed face, so the
        // normal points back along the ray.
        return Vec2{-direction.x, -direction.y};
    }
    const Vec2 point = origin + direction * distance;
    // Which face was crossed: the axis whose hit point sits on a face within a
    // small tolerance. Prefer the axis with the smaller distance to that face.
    const f32 dx = std::min(std::abs(point.x - box.min_x()), std::abs(point.x - box.max_x()));
    const f32 dy = std::min(std::abs(point.y - box.min_y()), std::abs(point.y - box.max_y()));
    if (dx <= dy) {
        const f32 sign = (point.x < box.center.x) ? -1.0f : 1.0f;
        return Vec2{sign, 0.0f};
    }
    const f32 sign = (point.y < box.center.y) ? -1.0f : 1.0f;
    return Vec2{0.0f, sign};
}

Vec2 raycast_circle_normal(const Vec2& origin, const Vec2& direction, const Circle2D& circle, f32 distance) noexcept {
    const Vec2 point = origin + direction * distance;
    const Vec2 outward = point - circle.center;
    if (outward.length_squared() <= kDirectionEpsilon * kDirectionEpsilon) {
        return Vec2{-direction.x, -direction.y}; // degenerate: hit at the centre
    }
    return outward.normalized();
}

Vec2 raycast_shape_normal(const Vec2& origin, const Vec2& direction, ShapeType type, const Aabb2D& box,
                          const Circle2D& circle, f32 distance) noexcept {
    return type == ShapeType::Circle ? raycast_circle_normal(origin, direction, circle, distance)
                                     : raycast_aabb_normal(origin, direction, box, distance);
}

bool contains(const Aabb2D& box, const Vec2& point) noexcept {
    return point.x >= box.min_x() && point.x <= box.max_x() && point.y >= box.min_y() && point.y <= box.max_y();
}

bool contains(const Circle2D& circle, const Vec2& point) noexcept {
    if (circle.radius <= 0.0f) {
        return false;
    }
    const Vec2 delta = point - circle.center;
    return delta.length_squared() <= circle.radius * circle.radius;
}

bool contains_shape(ShapeType type, const Aabb2D& box, const Circle2D& circle, const Vec2& point) noexcept {
    return type == ShapeType::Circle ? contains(circle, point) : contains(box, point);
}

} // namespace fe::physics2d
