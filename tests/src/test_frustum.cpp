// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
//
// Tests for the frustum culling primitive: plane extraction from a
// view-projection matrix and the conservative point/AABB visibility tests.
// Pure maths, no graphics context.

#include <doctest/doctest.h>

#include "fe/core/Frustum.h"
#include "fe/core/Math.h"

#include <cmath>

using namespace fe;

namespace {

// An eye at the origin looking down -Z with a 60-degree vertical FOV, a square
// aspect, and a far plane at 100. A point straight ahead at z = -1 is inside; at
// z = -1 the visible half-extent in x and y is tan(30 degrees) ~= 0.577.
[[nodiscard]] Frustum make_frustum() {
    const Mat4 projection = perspective(radians(60.0f), 1.0f, 0.1f, 100.0f);
    const Mat4 view = look_at(Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.0f, 0.0f, -1.0f}, Vec3{0.0f, 1.0f, 0.0f});
    return Frustum::from_view_projection(view * projection);
}

} // namespace

TEST_CASE("frustum planes extracted from a view-projection matrix are unit length") {
    const Frustum frustum = make_frustum();
    for (const Vec4& plane : frustum.planes) {
        const f32 normal_length = std::sqrt(plane.x * plane.x + plane.y * plane.y + plane.z * plane.z);
        CHECK(normal_length == doctest::Approx(1.0f).epsilon(1e-4));
    }
}

TEST_CASE("a point straight ahead of the camera is inside the frustum") {
    const Frustum frustum = make_frustum();
    CHECK(frustum.contains_point(Vec3{0.0f, 0.0f, -1.0f}));
    CHECK(frustum.contains_point(Vec3{0.0f, 0.0f, -50.0f}));
}

TEST_CASE("points behind, beside, or beyond the camera are outside") {
    const Frustum frustum = make_frustum();
    // Behind the camera (positive Z in front of -Z, i.e. the back side).
    CHECK_FALSE(frustum.contains_point(Vec3{0.0f, 0.0f, 1.0f}));
    // Beyond the far plane.
    CHECK_FALSE(frustum.contains_point(Vec3{0.0f, 0.0f, -200.0f}));
    // Far to the side, well outside the 60-degree cone.
    CHECK_FALSE(frustum.contains_point(Vec3{100.0f, 0.0f, -1.0f}));
    CHECK_FALSE(frustum.contains_point(Vec3{0.0f, 100.0f, -1.0f}));
}

TEST_CASE("the identity matrix yields the clip cube as the frustum") {
    // Not a real camera, but a useful closed case: the identity view-projection
    // defines the box |x|,|y|,|z| <= 1.
    const Frustum frustum = Frustum::from_view_projection(Mat4::identity());
    CHECK(frustum.contains_point(Vec3{0.0f, 0.0f, 0.0f}));
    CHECK(frustum.contains_point(Vec3{0.9f, -0.9f, 0.9f}));
    CHECK_FALSE(frustum.contains_point(Vec3{2.0f, 0.0f, 0.0f}));
    CHECK_FALSE(frustum.contains_point(Vec3{0.0f, 0.0f, -2.0f}));
}

TEST_CASE("an AABB straddling the near plane is kept (conservative test)") {
    const Frustum frustum = make_frustum();
    // Crosses z = 0: part of it is in front of the camera.
    CHECK(frustum.intersects_aabb(Vec3{-0.5f, -0.5f, -0.5f}, Vec3{0.5f, 0.5f, 0.5f}));
}

TEST_CASE("an AABB wholly behind the camera is culled") {
    const Frustum frustum = make_frustum();
    CHECK_FALSE(frustum.intersects_aabb(Vec3{-1.0f, -1.0f, 1.0f}, Vec3{1.0f, 1.0f, 3.0f}));
}

TEST_CASE("an AABB wholly beyond the far plane is culled") {
    const Frustum frustum = make_frustum();
    CHECK_FALSE(frustum.intersects_aabb(Vec3{-1.0f, -1.0f, -200.0f}, Vec3{1.0f, 1.0f, -199.0f}));
}

TEST_CASE("an AABB far to the side is culled") {
    const Frustum frustum = make_frustum();
    CHECK_FALSE(frustum.intersects_aabb(Vec3{999.0f, -1.0f, -2.0f}, Vec3{1000.0f, 1.0f, 0.0f}));
}

TEST_CASE("an AABB enclosing the camera is kept") {
    const Frustum frustum = make_frustum();
    CHECK(frustum.intersects_aabb(Vec3{-10.0f, -10.0f, -10.0f}, Vec3{10.0f, 10.0f, 10.0f}));
}

TEST_CASE("a small box in front of the camera intersects") {
    const Frustum frustum = make_frustum();
    CHECK(frustum.intersects_aabb(Vec3{-0.4f, -0.4f, -1.2f}, Vec3{0.4f, 0.4f, -0.8f}));
}

TEST_CASE("a degenerate zero-size box is tested as a point") {
    const Frustum frustum = make_frustum();
    CHECK(frustum.intersects_aabb(Vec3{0.0f, 0.0f, -1.0f}, Vec3{0.0f, 0.0f, -1.0f}));
    CHECK_FALSE(frustum.intersects_aabb(Vec3{0.0f, 0.0f, 1.0f}, Vec3{0.0f, 0.0f, 1.0f}));
}
