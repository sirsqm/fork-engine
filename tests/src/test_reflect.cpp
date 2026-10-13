// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include <doctest/doctest.h>

#include "fe/reflect/ComponentTypes.h"
#include "fe/reflect/PropertyBuilders.h"
#include "fe/reflect/Registry.h"
#include "fe/resources/Resource.h"
#include "fe/scene/Camera2D.h"
#include "fe/scene/Component.h"
#include "fe/scene/Node.h"
#include "fe/scene/Scene.h"
#include "fe/scene/Sprite2D.h"

#include <memory>
#include <string>

using namespace fe;

namespace {

const reflect::Property* find_property(const reflect::TypeDesc& type, std::string_view name) {
    for (const auto& property : type.properties) {
        if (property.name == name) {
            return &property;
        }
    }
    return nullptr;
}

const reflect::Property* find_member(const reflect::Property& group, std::string_view name) {
    for (const auto& member : group.members) {
        if (member.name == name) {
            return &member;
        }
    }
    return nullptr;
}

} // namespace

TEST_CASE("registering the built-in types makes them discoverable") {
    reflect::register_builtin_types();

    const reflect::Registry& registry = reflect::Registry::instance();
    CHECK(registry.contains(Sprite2D::kTypeName));
    CHECK(registry.contains(Camera2D::kTypeName));
    CHECK(registry.find("NoSuchComponent") == nullptr);
    CHECK(registry.type_count() >= 2);
}

TEST_CASE("a registered type constructs a default instance through its factory") {
    reflect::register_builtin_types();

    auto component = reflect::Registry::instance().default_construct(Sprite2D::kTypeName);
    REQUIRE(component != nullptr);
    CHECK(std::string(component->type_name()) == Sprite2D::kTypeName);
    CHECK(reflect::Registry::instance().default_construct("NoSuchComponent") == nullptr);
}

TEST_CASE("a scalar property reads and writes its field") {
    Sprite2D sprite;
    sprite.layer = 3;

    const auto property = reflect::make_scalar<Sprite2D, i32>("layer", &Sprite2D::layer, reflect::ValueKind::Int);
    CHECK(property.kind() == reflect::ValueKind::Int);

    CHECK(std::get<i64>(property.getter(&sprite)) == 3);

    REQUIRE(property.setter(&sprite, i64{7}));
    CHECK(sprite.layer == 7);

    // A value of the wrong kind fails cleanly instead of corrupting the field.
    CHECK_FALSE(property.setter(&sprite, std::string("nope")));
    CHECK(sprite.layer == 7);
}

TEST_CASE("a float property accepts an integer value from a document") {
    Camera2D camera;
    const auto property = reflect::make_scalar<Camera2D, f32>("zoom", &Camera2D::zoom, reflect::ValueKind::Float);

    // A hand-edited scene may write "zoom": 2 instead of 2.0.
    REQUIRE(property.setter(&camera, i64{2}));
    CHECK(camera.zoom == doctest::Approx(2.0f));
}

TEST_CASE("a vector group exposes one float member per component") {
    Sprite2D sprite;
    sprite.size = Vec2{16.0f, 32.0f};

    const auto group = reflect::make_vector_group<Sprite2D, Vec2>("size", &Sprite2D::size, "Vec2", {"x", "y"});
    CHECK(group.kind() == reflect::ValueKind::Group);
    CHECK(group.array_length == 2);
    REQUIRE(group.members.size() == 2);

    const reflect::Property* x = find_member(group, "x");
    const reflect::Property* y = find_member(group, "y");
    REQUIRE(x != nullptr);
    REQUIRE(y != nullptr);

    CHECK(std::get<f64>(x->getter(&sprite)) == doctest::Approx(16.0));
    CHECK(std::get<f64>(y->getter(&sprite)) == doctest::Approx(32.0));

    REQUIRE(y->setter(&sprite, f64{64.0}));
    CHECK(sprite.size.y == doctest::Approx(64.0f));
}

TEST_CASE("the built-in Sprite2D registration describes every field") {
    reflect::register_builtin_types();
    const reflect::TypeDesc* type = reflect::Registry::instance().find(Sprite2D::kTypeName);
    REQUIRE(type != nullptr);

    const reflect::Property* size = find_property(*type, "size");
    REQUIRE(size != nullptr);
    CHECK(size->kind() == reflect::ValueKind::Group);

    const reflect::Property* texture = find_property(*type, "texture");
    REQUIRE(texture != nullptr);
    CHECK(texture->kind() == reflect::ValueKind::ResourceRef);
    REQUIRE(texture->resource.has_value());
    CHECK(texture->resource->type_name == "Texture2D");

    const reflect::Property* layer = find_property(*type, "layer");
    REQUIRE(layer != nullptr);
    CHECK(layer->kind() == reflect::ValueKind::Int);
}

TEST_CASE("editor hints are attached to the built-in registrations") {
    reflect::register_builtin_types();
    const reflect::Registry& registry = reflect::Registry::instance();

    // Camera2D::zoom is bounded, so the inspector shows a slider.
    const reflect::TypeDesc* camera = registry.find(Camera2D::kTypeName);
    REQUIRE(camera != nullptr);
    const reflect::Property* zoom = find_property(*camera, "zoom");
    REQUIRE(zoom != nullptr);
    REQUIRE(zoom->range.has_value());
    CHECK(zoom->range->minimum < zoom->range->maximum);

    // Sprite2D::color is a colour group; Sprite2D::uv is a plain vector group,
    // so the hint is what distinguishes them.
    const reflect::TypeDesc* sprite = registry.find(Sprite2D::kTypeName);
    REQUIRE(sprite != nullptr);
    const reflect::Property* color = find_property(*sprite, "color");
    REQUIRE(color != nullptr);
    CHECK(color->color.has_value());
    CHECK_FALSE(color->range.has_value());
    const reflect::Property* uv = find_property(*sprite, "uv");
    REQUIRE(uv != nullptr);
    CHECK_FALSE(uv->color.has_value());
    CHECK_FALSE(uv->range.has_value());
}

TEST_CASE("the built-in Camera2D registration aggregates the clear colour channels") {
    reflect::register_builtin_types();
    const reflect::TypeDesc* type = reflect::Registry::instance().find(Camera2D::kTypeName);
    REQUIRE(type != nullptr);

    const reflect::Property* clear = find_property(*type, "clear_color");
    REQUIRE(clear != nullptr);
    CHECK(clear->kind() == reflect::ValueKind::Group);
    CHECK(clear->array_length == 4);

    Camera2D camera;
    const reflect::Property* green = find_member(*clear, "g");
    REQUIRE(green != nullptr);
    REQUIRE(green->setter(&camera, f64{0.5}));
    CHECK(camera.clear_g == doctest::Approx(0.5f));
    CHECK(camera.clear_r == doctest::Approx(0.10f)); // untouched
}
