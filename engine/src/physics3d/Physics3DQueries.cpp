// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/physics3d/Physics3DQueries.h"

#include "fe/physics3d/Collider3D.h"
#include "fe/physics3d/Geometry3D.h"
#include "fe/scene/Node.h"
#include "fe/scene/Scene.h"

namespace fe::physics3d {
namespace {

// A collider's shape placed at its node's world position, so the pure geometry
// helpers can be reused without the query layer duplicating any shape maths.
struct PlacedShape {
    ShapeType3D type = ShapeType3D::Box;
    Aabb3D box{};
    Sphere3D sphere{};
};

[[nodiscard]] PlacedShape place(const Collider3D& collider, const Node& node) noexcept {
    const Vec3& position = node.transform().position;
    PlacedShape shape;
    shape.type = collider.shape;
    shape.box = collider.box(position);
    shape.sphere = collider.sphere(position);
    return shape;
}

} // namespace

RaycastHit raycast(const Scene& scene, const Vec3& origin, const Vec3& direction, f32 max_distance) noexcept {
    RaycastHit best;
    const f32 length = direction.length();
    if (length <= 0.0f) {
        return best; // a zero-length direction cannot hit anything
    }
    const Vec3 unit = direction / length;

    scene.for_each_node([&](Node& node) {
        const auto* collider = node.get_component<Collider3D>();
        if (collider == nullptr) {
            return;
        }
        const PlacedShape shape = place(*collider, node);
        const f32 distance = raycast_shape(origin, unit, shape.type, shape.box, shape.sphere);
        if (distance < 0.0f) {
            return;
        }
        if (max_distance > 0.0f && distance > max_distance) {
            return;
        }
        if (best.hit && distance >= best.distance) {
            return;
        }
        best.hit = true;
        best.node = &node;
        best.distance = distance;
        best.point = origin + unit * distance;
        best.normal = raycast_shape_normal(origin, unit, shape.type, shape.box, shape.sphere, distance);
    });
    return best;
}

usize overlap_sphere(const Scene& scene, const Vec3& center, f32 radius, std::vector<OverlapHit>& out) {
    const Sphere3D query{center, radius};
    usize count = 0;
    scene.for_each_node([&](Node& node) {
        const auto* collider = node.get_component<Collider3D>();
        if (collider == nullptr) {
            return;
        }
        const PlacedShape shape = place(*collider, node);
        // Query sphere first, so the returned normal points from the query
        // towards the collider, matching the documented convention.
        const Contact3D contact =
            intersect_shapes(ShapeType3D::Sphere, Aabb3D{}, query, shape.type, shape.box, shape.sphere);
        if (!contact.overlapping) {
            return;
        }
        out.push_back(OverlapHit{&node, contact.normal, contact.penetration});
        ++count;
    });
    return count;
}

usize overlap_box(const Scene& scene, const Vec3& center, const Vec3& half_extent, std::vector<OverlapHit>& out) {
    const Aabb3D query{center, half_extent};
    usize count = 0;
    scene.for_each_node([&](Node& node) {
        const auto* collider = node.get_component<Collider3D>();
        if (collider == nullptr) {
            return;
        }
        const PlacedShape shape = place(*collider, node);
        const Contact3D contact =
            intersect_shapes(ShapeType3D::Box, query, Sphere3D{}, shape.type, shape.box, shape.sphere);
        if (!contact.overlapping) {
            return;
        }
        out.push_back(OverlapHit{&node, contact.normal, contact.penetration});
        ++count;
    });
    return count;
}

bool overlap_point(const Scene& scene, const Vec3& point, OverlapHit* out) noexcept {
    bool found = false;
    scene.for_each_node([&](Node& node) {
        if (found) {
            return;
        }
        const auto* collider = node.get_component<Collider3D>();
        if (collider == nullptr) {
            return;
        }
        const PlacedShape shape = place(*collider, node);
        if (!contains_shape(shape.type, shape.box, shape.sphere, point)) {
            return;
        }
        found = true;
        if (out != nullptr) {
            out->node = &node;
            out->normal = Vec3{};
            out->penetration = 0.0f;
        }
    });
    return found;
}

} // namespace fe::physics3d
