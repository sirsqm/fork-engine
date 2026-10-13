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
