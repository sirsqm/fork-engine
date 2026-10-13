// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
//
// CPU-side tests for the 3D scene components: the camera's derived view
// transform and the reflection registration of the 3D types. The lit draw path
// is covered by the graphics tests, which need a context.

#include <doctest/doctest.h>

#include "fe/reflect/Registry.h"
#include "fe/scene/Component.h"
#include "fe/scene/Node.h"
#include "fe/scene/Scene.h"
#include "fe/scene3d/Camera3D.h"
#include "fe/scene3d/DirectionalLight3D.h"
#include "fe/scene3d/MeshRenderer3D.h"
#include "fe/scene3d/PointLight3D.h"
#include "fe/scene3d/Scene3DTypes.h"
#include "fe/scene3d/SpotLight3D.h"

#include <cmath>
#include <memory>
#include <string>

using namespace fe;

TEST_CASE("a camera with no node reports the origin and looks down -Z") {
    Camera3D camera;
    const Vec3 eye = camera.eye();
    CHECK(eye.x == doctest::Approx(0.0f));
    CHECK(eye.y == doctest::Approx(0.0f));
    CHECK(eye.z == doctest::Approx(0.0f));

    const Vec3 forward = camera.forward();
    CHECK(forward.z == doctest::Approx(-1.0f));
}

TEST_CASE("camera eye and forward follow the owning node's transform") {
    Scene scene;
    Node* node = scene.create_node("Camera");
    node->transform().position = Vec3{0.0f, 0.0f, 5.0f};
    auto* camera = node->add_component<Camera3D>();
    REQUIRE(camera != nullptr);

    const Vec3 eye = camera->eye();
    CHECK(eye.z == doctest::Approx(5.0f));

    // Default orientation: still looking down -Z, towards the origin.
    const Vec3 forward = camera->forward();
    CHECK(forward.x == doctest::Approx(0.0f));
    CHECK(forward.y == doctest::Approx(0.0f));
    CHECK(forward.z == doctest::Approx(-1.0f));

    // The origin projects to the centre of the view, in front of the camera.
    const Mat4 view_projection = camera->view_projection(800, 600);
    const Vec3 origin_clip = transform_point(view_projection, Vec3{});
    CHECK(origin_clip.x == doctest::Approx(0.0f).epsilon(1e-4));
    CHECK(origin_clip.y == doctest::Approx(0.0f).epsilon(1e-4));
}

TEST_CASE("a camera rotated 90 degrees about Y looks along -X") {
    Scene scene;
    Node* node = scene.create_node("Camera");
    node->transform().rotation = Vec3{0.0f, radians(90.0f), 0.0f};
    auto* camera = node->add_component<Camera3D>();
    REQUIRE(camera != nullptr);

    // Rotating -Z by +90 degrees about Y points it along -X (right-handed).
    const Vec3 forward = camera->forward();
    CHECK(forward.x == doctest::Approx(-1.0f).epsilon(1e-4));
    CHECK(forward.z == doctest::Approx(0.0f).epsilon(1e-4));
}

TEST_CASE("a degenerate near/far pair still yields a usable projection") {
    Scene scene;
    Node* node = scene.create_node("Camera");
    auto* camera = node->add_component<Camera3D>();
    REQUIRE(camera != nullptr);
    camera->near_plane = 0.0f;
    camera->far_plane = 0.0f;

    const Mat4 view_projection = camera->view_projection(64, 64);
    // No NaNs or infinities leak into the matrix.
    for (f32 value : view_projection.m) {
        CHECK(std::isfinite(value));
    }
}

TEST_CASE("register_scene3d_types makes the 3D components visible to reflection") {
    reflect::register_scene3d_types();
    reflect::Registry& registry = reflect::Registry::instance();

    CHECK(registry.find("Camera3D") != nullptr);
    CHECK(registry.find("MeshRenderer3D") != nullptr);
    CHECK(registry.find("DirectionalLight3D") != nullptr);
    CHECK(registry.find("PointLight3D") != nullptr);
    CHECK(registry.find("SpotLight3D") != nullptr);
}

TEST_CASE("a point light exposes its editable properties to reflection") {
    reflect::register_scene3d_types();
    reflect::Registry& registry = reflect::Registry::instance();

    const reflect::TypeDesc* type = registry.find("PointLight3D");
    REQUIRE(type != nullptr);
    REQUIRE(type->factory != nullptr);

    std::unique_ptr<Component> instance = type->factory();
    REQUIRE(instance != nullptr);
    CHECK(std::string(instance->type_name()) == "PointLight3D");

    // The properties a designer needs must be present, by name.
    const auto has = [&](std::string_view name) {
        for (const reflect::Property& property : type->properties) {
            if (property.name == name) {
                return true;
            }
        }
        return false;
    };
    CHECK(has("color"));
    CHECK(has("intensity"));
    CHECK(has("range"));
}

TEST_CASE("a spot light exposes its cone properties to reflection") {
    reflect::register_scene3d_types();
    reflect::Registry& registry = reflect::Registry::instance();

    const reflect::TypeDesc* type = registry.find("SpotLight3D");
    REQUIRE(type != nullptr);
    REQUIRE(type->factory != nullptr);

    std::unique_ptr<Component> instance = type->factory();
    REQUIRE(instance != nullptr);
    CHECK(std::string(instance->type_name()) == "SpotLight3D");

    const auto has = [&](std::string_view name) {
        for (const reflect::Property& property : type->properties) {
            if (property.name == name) {
                return true;
            }
        }
        return false;
    };
    CHECK(has("color"));
    CHECK(has("intensity"));
    CHECK(has("range"));
    CHECK(has("inner_cone_degrees"));
    CHECK(has("outer_cone_degrees"));
}

TEST_CASE("the 3D types can be constructed from their factories") {
    reflect::register_scene3d_types();
    reflect::Registry& registry = reflect::Registry::instance();

    const reflect::TypeDesc* camera = registry.find("Camera3D");
    REQUIRE(camera != nullptr);
    REQUIRE(camera->factory != nullptr);
    const std::unique_ptr<Component> instance = camera->factory();
    CHECK(instance != nullptr);
    CHECK(std::string(instance->type_name()) == "Camera3D");
}
