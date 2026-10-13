// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include <doctest/doctest.h>

#include "fe/core/Math.h"

using namespace fe;

TEST_CASE("vec2 arithmetic") {
    const Vec2 a{3.0f, 4.0f};
    const Vec2 b{1.0f, 2.0f};

    CHECK(a + b == Vec2{4.0f, 6.0f});
    CHECK(a - b == Vec2{2.0f, 2.0f});
    CHECK(a * 2.0f == Vec2{6.0f, 8.0f});
    CHECK(a.dot(b) == doctest::Approx(11.0f));
    CHECK(a.length() == doctest::Approx(5.0f));
}

TEST_CASE("mat4 identity leaves a point unchanged") {
    const Mat4 identity = Mat4::identity();
    const Vec3 point{1.5f, -2.5f, 0.75f};
    const Vec3 transformed = transform_point(identity, point);

    CHECK(transformed.x == doctest::Approx(point.x));
    CHECK(transformed.y == doctest::Approx(point.y));
    CHECK(transformed.z == doctest::Approx(point.z));
}

TEST_CASE("mat4 translation moves a point") {
    const Mat4 translation = Mat4::translation(Vec3{10.0f, 20.0f, 0.0f});
    const Vec3 transformed = transform_point(translation, Vec3{1.0f, 2.0f, 0.0f});

    CHECK(transformed.x == doctest::Approx(11.0f));
    CHECK(transformed.y == doctest::Approx(22.0f));
}

TEST_CASE("mat4 scale scales a point") {
    const Mat4 scale = Mat4::scale(Vec3{2.0f, 3.0f, 1.0f});
    const Vec3 transformed = transform_point(scale, Vec3{4.0f, 5.0f, 0.0f});

    CHECK(transformed.x == doctest::Approx(8.0f));
    CHECK(transformed.y == doctest::Approx(15.0f));
}

TEST_CASE("mat4 multiplication composes in the expected order") {
    // Applying (translate * scale) to a point must scale first, then translate.
    const Mat4 translate = Mat4::translation(Vec3{100.0f, 0.0f, 0.0f});
    const Mat4 scale = Mat4::scale(Vec3{2.0f, 2.0f, 1.0f});
    const Vec3 transformed = transform_point(translate * scale, Vec3{1.0f, 1.0f, 0.0f});

    CHECK(transformed.x == doctest::Approx(102.0f));
    CHECK(transformed.y == doctest::Approx(2.0f));
}

TEST_CASE("rotation about z maps the x axis to the y axis at 90 degrees") {
    const Mat4 rotation = Mat4::rotation_z(kPi * 0.5f);
    const Vec3 transformed = transform_point(rotation, Vec3{1.0f, 0.0f, 0.0f});

    CHECK(transformed.x == doctest::Approx(0.0f).epsilon(1e-5));
    CHECK(transformed.y == doctest::Approx(1.0f));
}

TEST_CASE("orthographic projection maps the box corners to clip space") {
    const Mat4 projection = Mat4::orthographic(0.0f, 800.0f, 0.0f, 600.0f, -1.0f, 1.0f);

    // Bottom-left maps to (-1, -1).
    const Vec3 bottom_left = transform_point(projection, Vec3{0.0f, 0.0f, 0.0f});
    CHECK(bottom_left.x == doctest::Approx(-1.0f));
    CHECK(bottom_left.y == doctest::Approx(-1.0f));

    // Top-right maps to (+1, +1).
    const Vec3 top_right = transform_point(projection, Vec3{800.0f, 600.0f, 0.0f});
    CHECK(top_right.x == doctest::Approx(1.0f));
    CHECK(top_right.y == doctest::Approx(1.0f));
}

TEST_CASE("vec3 dot and cross products") {
    const Vec3 x{1.0f, 0.0f, 0.0f};
    const Vec3 y{0.0f, 1.0f, 0.0f};

    CHECK(x.dot(y) == doctest::Approx(0.0f));
    const Vec3 z = x.cross(y);
    CHECK(z.x == doctest::Approx(0.0f));
    CHECK(z.y == doctest::Approx(0.0f));
    CHECK(z.z == doctest::Approx(1.0f));
}

TEST_CASE("perspective projection maps the near and far planes to the clip range") {
    const f32 fov = radians(60.0f);
    const f32 aspect = 16.0f / 9.0f;
    const Mat4 p = perspective(fov, aspect, 0.1f, 100.0f);

    CHECK(p.at(0, 0) == doctest::Approx(1.0f / (std::tan(fov * 0.5f) * aspect)));
    CHECK(p.at(1, 1) == doctest::Approx(1.0f / std::tan(fov * 0.5f)));
    CHECK(p.at(3, 2) == doctest::Approx(-1.0f));
    CHECK(p.at(2, 3) == doctest::Approx(2.0f * 100.0f * 0.1f / (0.1f - 100.0f)));

    // A point on the near plane maps to clip z = -1, on the far plane to +1.
    // The clip w is the view-space depth (positive in front), so divide through
    // by it: for view-space z = -d, w = d.
    const f32 near_z = (p.at(2, 2) * -0.1f + p.at(2, 3)) / 0.1f;
    const f32 far_z = (p.at(2, 2) * -100.0f + p.at(2, 3)) / 100.0f;
    CHECK(near_z == doctest::Approx(-1.0f));
    CHECK(far_z == doctest::Approx(1.0f));
}

TEST_CASE("look_at places the target down the camera's negative z axis") {
    const Mat4 view = look_at(Vec3{0.0f, 0.0f, 5.0f}, Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.0f, 1.0f, 0.0f});

    // The origin sits 5 units in front of the camera, i.e. at view-space z = -5.
    const Vec3 origin = transform_point(view, Vec3{0.0f, 0.0f, 0.0f});
    CHECK(origin.x == doctest::Approx(0.0f));
    CHECK(origin.y == doctest::Approx(0.0f));
    CHECK(origin.z == doctest::Approx(-5.0f));

    // A point to the right of the camera stays to the right (+x in view space).
    const Vec3 right_point = transform_point(view, Vec3{2.0f, 0.0f, 0.0f});
    CHECK(right_point.x == doctest::Approx(2.0f));
}

TEST_CASE("inverse undoes a transform") {
    const Mat4 transform =
        Mat4::translation(Vec3{3.0f, -1.0f, 2.0f}) * Mat4::rotation_y(0.7f) * Mat4::scale(Vec3{2.0f, 2.0f, 2.0f});
    const Mat4 inv = inverse(transform);

    // transform * inverse is the identity.
    const Mat4 product = transform * inv;
    for (usize row = 0; row < 4; ++row) {
        for (usize col = 0; col < 4; ++col) {
            CHECK(product.at(row, col) == doctest::Approx(row == col ? 1.0f : 0.0f).epsilon(1e-4));
        }
    }

    // A transformed point round-trips back to where it started.
    const Vec3 point{1.0f, 2.0f, -3.0f};
    const Vec3 back = transform_point(inv, transform_point(transform, point));
    CHECK(back.x == doctest::Approx(point.x).epsilon(1e-4));
    CHECK(back.y == doctest::Approx(point.y).epsilon(1e-4));
    CHECK(back.z == doctest::Approx(point.z).epsilon(1e-4));
}

TEST_CASE("inverting a singular matrix returns identity rather than NaN") {
    const Mat4 zero;
    const Mat4 inv = inverse(zero);
    CHECK(inv.at(0, 0) == doctest::Approx(1.0f));
    CHECK(inv.at(0, 1) == doctest::Approx(0.0f));
}
