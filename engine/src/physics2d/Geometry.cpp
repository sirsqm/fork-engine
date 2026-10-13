// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/physics2d/Geometry.h"

#include <algorithm>
#include <cmath>

namespace fe::physics2d {
namespace {

// Boxes are axis-aligned, so overlap on each axis is an interval test and the
// separating axis is the one with the smallest overlap. That axis is the
// contact normal, and the smallest overlap is the penetration depth.
Contact2D intersect_boxes(const Aabb2D& a, const Aabb2D& b) noexcept {
    const f32 overlap_x = std::min(a.max_x(), b.max_x()) - std::max(a.min_x(), b.min_x());
    if (overlap_x <= 0.0f) {
        return Contact2D{};
    }
    const f32 overlap_y = std::min(a.max_y(), b.max_y()) - std::max(a.min_y(), b.min_y());
    if (overlap_y <= 0.0f) {
        return Contact2D{};
    }

    Contact2D contact;
    contact.overlapping = true;
    if (overlap_x < overlap_y) {
        contact.penetration = overlap_x;
        // Normal points from A towards B. If B's centre is to the right of A's,
        // that is +x; otherwise -x.
        contact.normal = Vec2{b.center.x >= a.center.x ? 1.0f : -1.0f, 0.0f};
    } else {
        contact.penetration = overlap_y;
        contact.normal = Vec2{0.0f, b.center.y >= a.center.y ? 1.0f : -1.0f};
    }
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

Contact2D intersect_circle_box(const Circle2D& circle, const Aabb2D& box) noexcept {
    // Closest point on the box to the circle centre, in box-local terms. This
    // handles the face, edge, and corner cases uniformly: if the centre is
    // inside the box, the closest point is the centre itself and the depth is
    // measured to the nearest face instead.
    const f32 closest_x = std::clamp(circle.center.x, box.min_x(), box.max_x());
    const f32 closest_y = std::clamp(circle.center.y, box.min_y(), box.max_y());
    const Vec2 closest{closest_x, closest_y};
    const Vec2 delta = closest - circle.center;
    const f32 distance_squared = delta.length_squared();

    if (distance_squared > circle.radius * circle.radius) {
        return Contact2D{};
    }

    Contact2D contact;
    contact.overlapping = true;
    if (distance_squared > 0.0f) {
        const f32 distance = std::sqrt(distance_squared);
        // Normal points from the circle towards the box.
        contact.normal = delta / distance;
        contact.penetration = circle.radius - distance;
    } else {
        // The circle centre is inside the box. Separate along the nearest face.
        const f32 to_left = circle.center.x - box.min_x();
        const f32 to_right = box.max_x() - circle.center.x;
        const f32 to_bottom = circle.center.y - box.min_y();
        const f32 to_top = box.max_y() - circle.center.y;
        const f32 min_x = std::min(to_left, to_right);
        const f32 min_y = std::min(to_bottom, to_top);
        if (min_x < min_y) {
            contact.normal = Vec2{to_right < to_left ? 1.0f : -1.0f, 0.0f};
            contact.penetration = min_x + circle.radius;
        } else {
            contact.normal = Vec2{0.0f, to_top < to_bottom ? 1.0f : -1.0f};
            contact.penetration = min_y + circle.radius;
        }
    }
    return contact;
}

} // namespace

Contact2D intersect(const Aabb2D& a, const Aabb2D& b) noexcept {
    return intersect_boxes(a, b);
}

Contact2D intersect(const Circle2D& a, const Circle2D& b) noexcept {
    return intersect_circles(a, b);
}

Contact2D intersect(const Circle2D& a, const Aabb2D& b) noexcept {
    return intersect_circle_box(a, b);
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

Contact2D intersect_shapes(ShapeType a_type, const Aabb2D& a_box, const Circle2D& a_circle, ShapeType b_type,
                           const Aabb2D& b_box, const Circle2D& b_circle) noexcept {
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
    Contact2D contact = intersect(b_circle, a_box);
    contact.normal = Vec2{-contact.normal.x, -contact.normal.y};
    return contact;
}

} // namespace fe::physics2d
