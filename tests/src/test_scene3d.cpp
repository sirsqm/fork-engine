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

TEST_CASE("a mesh renderer exposes its material to reflection") {
    reflect::register_scene3d_types();
    reflect::Registry& registry = reflect::Registry::instance();

    const reflect::TypeDesc* type = registry.find("MeshRenderer3D");
    REQUIRE(type != nullptr);

    const auto find = [&](std::string_view name) -> const reflect::Property* {
        for (const reflect::Property& property : type->properties) {
            if (property.name == name) {
                return &property;
            }
        }
        return nullptr;
    };

    const reflect::Property* override_flag = find("override_material");
    REQUIRE(override_flag != nullptr);
    CHECK(override_flag->kind() == reflect::ValueKind::Bool);

    const reflect::Property* material = find("material");
    REQUIRE(material != nullptr);
    REQUIRE(material->is_group());

    // The material's editable fields, by name, and each with a working accessor
    // so the inspector and the serializer can read and write them.
    const auto member = [&](std::string_view name) -> const reflect::Property* {
        for (const reflect::Property& property : material->members) {
            if (property.name == name) {
                return &property;
            }
        }
        return nullptr;
    };
    for (const std::string_view name : {"base_color", "metallic", "roughness", "emissive", "double_sided"}) {
        CHECK(member(name) != nullptr);
    }
    CHECK(member("base_color")->color.has_value());
    CHECK(member("emissive")->color.has_value());

    // A value written through a nested member lands on the component's material.
    MeshRenderer3D renderer;
    const reflect::Property* metallic = member("metallic");
    REQUIRE(metallic != nullptr);
    REQUIRE(metallic->setter(&renderer, reflect::Value{f64{0.75}}));
    CHECK(renderer.material.metallic == doctest::Approx(0.75f));

    const reflect::Property* base_color = member("base_color");
    REQUIRE(base_color != nullptr);
    REQUIRE(base_color->members.size() == 4);
    REQUIRE(base_color->members[1].setter(&renderer, reflect::Value{f64{0.25}}));
    CHECK(renderer.material.base_color.y == doctest::Approx(0.25f));
}

TEST_CASE("resolve_surface prefers an authored material over the imported one") {
    MeshRenderer3D renderer;

    // No mesh, no override: the component's own fields are the surface.
    renderer.base_color = Vec4{0.1f, 0.2f, 0.3f, 1.0f};
    renderer.specular = 0.5f;
    MeshRenderer3D::Surface plain = renderer.resolve_surface();
    CHECK(plain.material.base_color.x == doctest::Approx(0.1f));
    CHECK(plain.material.base_color.z == doctest::Approx(0.3f));
    CHECK(plain.specular == doctest::Approx(0.5f));
    CHECK(plain.shininess == doctest::Approx(32.0f));

    // An authored material replaces the base colour and derives its own
    // specular/shininess from metallic and roughness.
    renderer.override_material = true;
    renderer.material.base_color = Vec4{0.9f, 0.8f, 0.7f, 1.0f};
    renderer.material.metallic = 1.0f;
    renderer.material.roughness = 0.25f;
    MeshRenderer3D::Surface authored = renderer.resolve_surface();
    CHECK(authored.material.base_color.x == doctest::Approx(0.9f));
    CHECK(authored.specular == doctest::Approx(1.0f * (1.0f - 0.25f)));
    CHECK(authored.shininess > 32.0f); // A smooth material gets a tighter lobe.
}

TEST_CASE("a material's specular and shininess follow from metallic and roughness") {
    Material rough;
    rough.metallic = 0.0f;
    rough.roughness = 1.0f;
    CHECK(rough.specular_strength() == doctest::Approx(0.0f));

    Material shiny;
    shiny.metallic = 1.0f;
    shiny.roughness = 0.0f;
    // Fully metallic, fully smooth: near-maximal specular and a clamped exponent.
    CHECK(shiny.specular_strength() == doctest::Approx(1.0f));
    CHECK(shiny.shininess() == doctest::Approx(512.0f));

    Material dielectric;
    dielectric.metallic = 0.0f;
    dielectric.roughness = 0.5f;
    CHECK(dielectric.specular_strength() == doctest::Approx(0.04f * 0.5f));
}
