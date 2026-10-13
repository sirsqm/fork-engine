// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/editor/PropertyEditor.h"

#include "fe/core/Uuid.h"
#include "fe/reflect/ComponentTypes.h"
#include "fe/reflect/Property.h"
#include "fe/reflect/Registry.h"
#include "fe/reflect/TypeDesc.h"
#include "fe/reflect/Value.h"
#include "fe/scene/Component.h"
#include "fe/scene/Sprite2D.h"

#include <doctest/doctest.h>

#include <memory>
#include <string>
#include <variant>

using fe::editor::format_value;
using fe::editor::parse_range_hint;
using fe::editor::unreflected_label;
using fe::reflect::Value;

TEST_CASE("parse_range_hint accepts a well-formed min,max") {
    const auto hint = parse_range_hint("0,1");
    CHECK(hint.valid);
    CHECK(hint.minimum == doctest::Approx(0.0));
    CHECK(hint.maximum == doctest::Approx(1.0));
}

TEST_CASE("parse_range_hint tolerates whitespace and signs") {
    const auto hint = parse_range_hint(" -2.5 , 10.0 ");
    CHECK(hint.valid);
    CHECK(hint.minimum == doctest::Approx(-2.5));
    CHECK(hint.maximum == doctest::Approx(10.0));
}

TEST_CASE("parse_range_hint rejects malformed hints") {
    CHECK_FALSE(parse_range_hint("").valid);
    CHECK_FALSE(parse_range_hint("1").valid);
    CHECK_FALSE(parse_range_hint("1;2").valid);
    CHECK_FALSE(parse_range_hint("a,b").valid);
    // Trailing content is rejected rather than silently truncated.
    CHECK_FALSE(parse_range_hint("1,2,3").valid);
    CHECK_FALSE(parse_range_hint("1,2x").valid);
}

TEST_CASE("parse_range_hint reads '0,5' as the interval zero to five") {
    // The comma is the separator, so this is a range and not the decimal 0.5.
    const auto hint = parse_range_hint("0,5");
    CHECK(hint.valid);
    CHECK(hint.minimum == doctest::Approx(0.0));
    CHECK(hint.maximum == doctest::Approx(5.0));
}

TEST_CASE("parse_range_hint rejects an empty or inverted interval") {
    CHECK_FALSE(parse_range_hint("1,1").valid);
    CHECK_FALSE(parse_range_hint("2,1").valid);
}

TEST_CASE("format_value renders each value kind") {
    CHECK(format_value(Value{true}) == "true");
    CHECK(format_value(Value{false}) == "false");
    CHECK(format_value(Value{static_cast<fe::i64>(42)}) == "42");
    CHECK(format_value(Value{0.5}) == "0.5");
    CHECK(format_value(Value{1.0}) == "1");
    CHECK(format_value(Value{std::string{"hello"}}) == "hello");
}

TEST_CASE("unreflected_label names the type and marks it") {
    CHECK(unreflected_label("Mystery") == "Mystery (not reflected)");
}

// ---------------------------------------------------------------------------
// The registry contract the inspector depends on. These assert the shapes the
// inspector reads (getter/setter round-trips and group membership), not UI, so
// they stay headless. They also stand in for the panel's data path: an inspector
// edit is a `property.setter(component, value)` call, so if the setter is broken
// the panel would be broken too.
// ---------------------------------------------------------------------------
TEST_CASE("the built-in component types are registered") {
    fe::reflect::register_builtin_types();
    const auto& registry = fe::reflect::Registry::instance();

    CHECK(registry.find("Sprite2D") != nullptr);
    CHECK(registry.find("Camera2D") != nullptr);
    CHECK(registry.find("NoSuchType") == nullptr);
}

TEST_CASE("Sprite2D's reflected scalar properties round-trip") {
    fe::reflect::register_builtin_types();
    const fe::reflect::TypeDesc* type = fe::reflect::Registry::instance().find("Sprite2D");
    REQUIRE(type != nullptr);

    const fe::reflect::Property* layer = nullptr;
    for (const auto& property : type->properties) {
        if (property.name == "layer") {
            layer = &property;
        }
    }
    REQUIRE(layer != nullptr);
    CHECK(layer->value_kind == fe::reflect::ValueKind::Int);

    fe::Sprite2D sprite;
    REQUIRE(layer->setter(&sprite, fe::reflect::Value{static_cast<fe::i64>(7)}));
    CHECK(sprite.layer == 7);
    CHECK(std::get<fe::i64>(layer->getter(&sprite)) == 7);
}

TEST_CASE("Sprite2D's size group is a Vec2 over the owning component") {
    fe::reflect::register_builtin_types();
    const fe::reflect::TypeDesc* type = fe::reflect::Registry::instance().find("Sprite2D");
    REQUIRE(type != nullptr);

    const fe::reflect::Property* size = nullptr;
    for (const auto& property : type->properties) {
        if (property.name == "size") {
            size = &property;
        }
    }
    REQUIRE(size != nullptr);
    CHECK(size->is_group());
    CHECK(size->array_length == 2);
    REQUIRE(size->members.size() == 2);

    fe::Sprite2D sprite;
    // Members take the owning component, not the group.
    REQUIRE(size->members[0].setter(&sprite, fe::reflect::Value{32.0}));
    REQUIRE(size->members[1].setter(&sprite, fe::reflect::Value{16.0}));
    CHECK(sprite.size.x == doctest::Approx(32.0f));
    CHECK(sprite.size.y == doctest::Approx(16.0f));
}

TEST_CASE("Camera2D's clear_color is a four-member group") {
    fe::reflect::register_builtin_types();
    const fe::reflect::TypeDesc* type = fe::reflect::Registry::instance().find("Camera2D");
    REQUIRE(type != nullptr);

    const fe::reflect::Property* clear = nullptr;
    for (const auto& property : type->properties) {
        if (property.name == "clear_color") {
            clear = &property;
        }
    }
    REQUIRE(clear != nullptr);
    CHECK(clear->is_group());
    CHECK(clear->array_length == 4);
    CHECK(clear->members.size() == 4);
}
