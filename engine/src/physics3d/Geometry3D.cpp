// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/physics3d/Geometry3D.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace fe::physics3d {
namespace {

// Below this a direction component is treated as zero, so a ray parallel to a
// slab does not divide by it. The value is well under any meaningful world unit
// and well above denormal noise.
constexpr f32 kDirectionEpsilon = 1e-8f;

// Boxes are axis-aligned, so overlap on each axis is an interval test and the
// separating axis is the one with the smallest overlap. That axis is the contact
// normal, and the smallest overlap is the penetration depth. Ties prefer the
// lowest axis (x before y before z) so the result is deterministic.
Contact3D intersect_boxes(const Aabb3D& a, const Aabb3D& b) noexcept {
    const f32 overlap_x = std::min(a.max_x(), b.max_x()) - std::max(a.min_x(), b.min_x());
    if (overlap_x <= 0.0f) {
        return Contact3D{};
    }
    const f32 overlap_y = std::min(a.max_y(), b.max_y()) - std::max(a.min_y(), b.min_y());
    if (overlap_y <= 0.0f) {
        return Contact3D{};
    }
    const f32 overlap_z = std::min(a.max_z(), b.max_z()) - std::max(a.min_z(), b.min_z());
    if (overlap_z <= 0.0f) {
        return Contact3D{};
    }

    Contact3D contact;
    contact.overlapping = true;
    if (overlap_x <= overlap_y && overlap_x <= overlap_z) {
        contact.penetration = overlap_x;
        // Normal points from A towards B. If B's centre is to the right of A's,
        // that is +x; otherwise -x.
        contact.normal = Vec3{b.center.x >= a.center.x ? 1.0f : -1.0f, 0.0f, 0.0f};
    } else if (overlap_y <= overlap_z) {
        contact.penetration = overlap_y;
        contact.normal = Vec3{0.0f, b.center.y >= a.center.y ? 1.0f : -1.0f, 0.0f};
    } else {
        contact.penetration = overlap_z;
        contact.normal = Vec3{0.0f, 0.0f, b.center.z >= a.center.z ? 1.0f : -1.0f};
    }
    return contact;
}

Contact3D intersect_spheres(const Sphere3D& a, const Sphere3D& b) noexcept {
    const f32 radius_sum = a.radius + b.radius;
    if (radius_sum <= 0.0f) {
        return Contact3D{};
    }
    const Vec3 delta = b.center - a.center;
    const f32 distance_squared = delta.length_squared();
    if (distance_squared >= radius_sum * radius_sum) {
        return Contact3D{};
    }

    const f32 distance = std::sqrt(distance_squared);
    Contact3D contact;
    contact.overlapping = true;
    contact.penetration = radius_sum - distance;
    if (distance > 0.0f) {
        contact.normal = delta / distance;
    } else {
        // Concentric spheres have no defined direction; pick one so the solver
        // still separates them deterministically instead of dividing by zero.
        contact.normal = Vec3{0.0f, 1.0f, 0.0f};
    }
    return contact;
}

Contact3D intersect_sphere_box(const Sphere3D& sphere, const Aabb3D& box) noexcept {
    // Closest point on the box to the sphere centre, in box-local terms. This
    // handles the face, edge, and corner cases uniformly: if the centre is
    // inside the box, the closest point is the centre itself and the depth is
    // measured to the nearest face instead.
    const f32 closest_x = std::clamp(sphere.center.x, box.min_x(), box.max_x());
    const f32 closest_y = std::clamp(sphere.center.y, box.min_y(), box.max_y());
    const f32 closest_z = std::clamp(sphere.center.z, box.min_z(), box.max_z());
    const Vec3 closest{closest_x, closest_y, closest_z};
    const Vec3 delta = closest - sphere.center;
    const f32 distance_squared = delta.length_squared();

    if (distance_squared > sphere.radius * sphere.radius) {
        return Contact3D{};
    }

    Contact3D contact;
    contact.overlapping = true;
    if (distance_squared > 0.0f) {
        const f32 distance = std::sqrt(distance_squared);
        // Normal points from the sphere towards the box.
        contact.normal = delta / distance;
        contact.penetration = sphere.radius - distance;
    } else {
        // The sphere centre is inside the box. Separate along the nearest face.
        const f32 to_min_x = sphere.center.x - box.min_x();
        const f32 to_max_x = box.max_x() - sphere.center.x;
        const f32 to_min_y = sphere.center.y - box.min_y();
        const f32 to_max_y = box.max_y() - sphere.center.y;
        const f32 to_min_z = sphere.center.z - box.min_z();
        const f32 to_max_z = box.max_z() - sphere.center.z;
        const f32 min_x = std::min(to_min_x, to_max_x);
        const f32 min_y = std::min(to_min_y, to_max_y);
        const f32 min_z = std::min(to_min_z, to_max_z);
        if (min_x <= min_y && min_x <= min_z) {
            contact.normal = Vec3{to_max_x < to_min_x ? 1.0f : -1.0f, 0.0f, 0.0f};
            contact.penetration = min_x + sphere.radius;
        } else if (min_y <= min_z) {
            contact.normal = Vec3{0.0f, to_max_y < to_min_y ? 1.0f : -1.0f, 0.0f};
            contact.penetration = min_y + sphere.radius;
        } else {
            contact.normal = Vec3{0.0f, 0.0f, to_max_z < to_min_z ? 1.0f : -1.0f};
            contact.penetration = min_z + sphere.radius;
        }
    }
    return contact;
}

} // namespace

Contact3D intersect(const Aabb3D& a, const Aabb3D& b) noexcept {
    return intersect_boxes(a, b);
}

Contact3D intersect(const Sphere3D& a, const Sphere3D& b) noexcept {
    return intersect_spheres(a, b);
}

Contact3D intersect(const Sphere3D& a, const Aabb3D& b) noexcept {
    return intersect_sphere_box(a, b);
}

bool bounds_overlap(const Aabb3D& a, const Aabb3D& b) noexcept {
    return a.min_x() < b.max_x() && a.max_x() > b.min_x() && a.min_y() < b.max_y() && a.max_y() > b.min_y() &&
           a.min_z() < b.max_z() && a.max_z() > b.min_z();
}

bool bounds_overlap(const Aabb3D& box, const Sphere3D& sphere) noexcept {
    return bounds_overlap(box, bounds_of(sphere));
}

Aabb3D bounds_of(const Sphere3D& sphere) noexcept {
    return Aabb3D{sphere.center, Vec3{sphere.radius, sphere.radius, sphere.radius}};
}

Aabb3D bounds_of(const Aabb3D& box) noexcept {
    return box;
}

Contact3D intersect_shapes(ShapeType3D a_type, const Aabb3D& a_box, const Sphere3D& a_sphere, ShapeType3D b_type,
                           const Aabb3D& b_box, const Sphere3D& b_sphere) noexcept {
    if (a_type == ShapeType3D::Box && b_type == ShapeType3D::Box) {
        return intersect(a_box, b_box);
    }
    if (a_type == ShapeType3D::Sphere && b_type == ShapeType3D::Sphere) {
        return intersect(a_sphere, b_sphere);
    }
    if (a_type == ShapeType3D::Sphere && b_type == ShapeType3D::Box) {
        return intersect(a_sphere, b_box);
    }
    // Box vs sphere: compute sphere-vs-box and flip the normal so it still points
    // from the first shape towards the second.
    Contact3D contact = intersect(b_sphere, a_box);
    contact.normal = Vec3{-contact.normal.x, -contact.normal.y, -contact.normal.z};
    return contact;
}

// --- Ray casts -------------------------------------------------------------

f32 raycast_aabb(const Vec3& origin, const Vec3& direction, const Aabb3D& box) noexcept {
    // Slab test. For each axis the ray is inside the slab for t in [t_near,
    // t_far]; the ray enters the box at the larger of the near values and leaves
    // at the smaller of the far values. An axis-parallel ray (a zero direction
    // component) is inside its slab only if the origin is already between the
    // faces, which is handled without dividing by zero.
    f32 t_near = -std::numeric_limits<f32>::infinity();
    f32 t_far = std::numeric_limits<f32>::infinity();

    const f32 origin_axis[3] = {origin.x, origin.y, origin.z};
    const f32 direction_axis[3] = {direction.x, direction.y, direction.z};
    const f32 min_axis[3] = {box.min_x(), box.min_y(), box.min_z()};
    const f32 max_axis[3] = {box.max_x(), box.max_y(), box.max_z()};

    for (int axis = 0; axis < 3; ++axis) {
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

f32 raycast_sphere(const Vec3& origin, const Vec3& direction, const Sphere3D& sphere) noexcept {
    if (sphere.radius <= 0.0f) {
        return -1.0f;
    }
    // Solve |origin + t*direction - center|^2 = r^2 for the smallest t >= 0.
    const Vec3 to_origin = origin - sphere.center;
    const f32 b = to_origin.dot(direction);
    const f32 c = to_origin.length_squared() - sphere.radius * sphere.radius;
    if (c > 0.0f && b > 0.0f) {
        return -1.0f; // origin outside and pointing away from the sphere
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
    // The origin is inside the sphere; report an immediate hit.
    return 0.0f;
}

f32 raycast_shape(const Vec3& origin, const Vec3& direction, ShapeType3D type, const Aabb3D& box,
                  const Sphere3D& sphere) noexcept {
    return type == ShapeType3D::Sphere ? raycast_sphere(origin, direction, sphere)
                                       : raycast_aabb(origin, direction, box);
}

Vec3 raycast_aabb_normal(const Vec3& origin, const Vec3& direction, const Aabb3D& box, f32 distance) noexcept {
    if (distance <= 0.0f) {
        // The origin is inside (or on) the box; there is no crossed face, so the
        // normal points back along the ray.
        return Vec3{-direction.x, -direction.y, -direction.z};
    }
    const Vec3 point = origin + direction * distance;
    // Which face was crossed: the axis whose hit point sits on a face with the
    // smallest distance to that face. Ties prefer the lowest axis.
    const f32 dx = std::min(std::abs(point.x - box.min_x()), std::abs(point.x - box.max_x()));
    const f32 dy = std::min(std::abs(point.y - box.min_y()), std::abs(point.y - box.max_y()));
    const f32 dz = std::min(std::abs(point.z - box.min_z()), std::abs(point.z - box.max_z()));
    if (dx <= dy && dx <= dz) {
        const f32 sign = (point.x < box.center.x) ? -1.0f : 1.0f;
        return Vec3{sign, 0.0f, 0.0f};
    }
    if (dy <= dz) {
        const f32 sign = (point.y < box.center.y) ? -1.0f : 1.0f;
        return Vec3{0.0f, sign, 0.0f};
    }
    const f32 sign = (point.z < box.center.z) ? -1.0f : 1.0f;
    return Vec3{0.0f, 0.0f, sign};
}

Vec3 raycast_sphere_normal(const Vec3& origin, const Vec3& direction, const Sphere3D& sphere, f32 distance) noexcept {
    const Vec3 point = origin + direction * distance;
    const Vec3 outward = point - sphere.center;
    if (outward.length_squared() <= kDirectionEpsilon * kDirectionEpsilon) {
        return Vec3{-direction.x, -direction.y, -direction.z}; // degenerate: hit at the centre
    }
    return outward.normalized();
}

Vec3 raycast_shape_normal(const Vec3& origin, const Vec3& direction, ShapeType3D type, const Aabb3D& box,
                          const Sphere3D& sphere, f32 distance) noexcept {
    return type == ShapeType3D::Sphere ? raycast_sphere_normal(origin, direction, sphere, distance)
                                       : raycast_aabb_normal(origin, direction, box, distance);
}

bool contains(const Aabb3D& box, const Vec3& point) noexcept {
    return point.x >= box.min_x() && point.x <= box.max_x() && point.y >= box.min_y() && point.y <= box.max_y() &&
           point.z >= box.min_z() && point.z <= box.max_z();
}

bool contains(const Sphere3D& sphere, const Vec3& point) noexcept {
    if (sphere.radius <= 0.0f) {
        return false;
    }
    const Vec3 delta = point - sphere.center;
    return delta.length_squared() <= sphere.radius * sphere.radius;
}

bool contains_shape(ShapeType3D type, const Aabb3D& box, const Sphere3D& sphere, const Vec3& point) noexcept {
    return type == ShapeType3D::Sphere ? contains(sphere, point) : contains(box, point);
}

} // namespace fe::physics3d
