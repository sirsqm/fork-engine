// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/physics2d/PhysicsTypes.h"

#include "fe/physics2d/Collider2D.h"
#include "fe/physics2d/RigidBody2D.h"
#include "fe/reflect/PropertyBuilders.h"
#include "fe/reflect/Registry.h"

#include <memory>

// The physics2d component registrations, in the same style as
// ComponentTypes.cpp: each block describes a component's properties once, and
// the serializer and the inspector read those descriptions rather than naming
// the types. This is what makes a collider and a rigid body serialize and appear
// in the inspector with no per-type code in either.

namespace fe::reflect {
namespace {

// A property over the Collider2D shape enum. ValueKind has no enum kind, so the
// shape is exposed as an int (0 = box, 1 = circle). The setter clamps to the
// valid range rather than trusting a hand-edited document, matching the total
// conversions the other builders use.
Property make_shape_property() {
    Property property;
    property.name = "shape";
    property.value_kind = ValueKind::Int;
    property.getter = [](const void* object) -> Value {
        return static_cast<i64>(static_cast<const physics2d::Collider2D*>(object)->shape);
    };
    property.setter = [](void* object, const Value& value) -> bool {
        const auto* as_int = std::get_if<i64>(&value);
        if (as_int == nullptr) {
            return false;
        }
        auto* collider = static_cast<physics2d::Collider2D*>(object);
        collider->shape = *as_int == 1 ? physics2d::ShapeType::Circle : physics2d::ShapeType::Box;
        return true;
    };
    property.default_value = static_cast<i64>(physics2d::ShapeType::Box);
    return property;
}

void register_collider2d(Registry& registry) {
    physics2d::Collider2D defaults;

    TypeDesc desc;
    desc.name = physics2d::Collider2D::kTypeName;
    desc.factory = [] { return std::unique_ptr<Component>(std::make_unique<physics2d::Collider2D>()); };

    desc.properties.push_back(make_shape_property());
    desc.properties.push_back(make_vector_group<physics2d::Collider2D, Vec2>(
        "half_extent", &physics2d::Collider2D::half_extent, "Vec2", {"x", "y"}));
    desc.properties.push_back(
        make_scalar<physics2d::Collider2D, f32>("radius", &physics2d::Collider2D::radius, ValueKind::Float, &defaults));
    desc.properties.push_back(make_scalar<physics2d::Collider2D, f32>(
        "restitution", &physics2d::Collider2D::restitution, ValueKind::Float, &defaults));
    desc.properties.push_back(make_scalar<physics2d::Collider2D, f32>("friction", &physics2d::Collider2D::friction,
                                                                      ValueKind::Float, &defaults));

    registry.register_type(std::move(desc));
}

void register_rigid_body2d(Registry& registry) {
    physics2d::RigidBody2D defaults;

    TypeDesc desc;
    desc.name = physics2d::RigidBody2D::kTypeName;
    desc.factory = [] { return std::unique_ptr<Component>(std::make_unique<physics2d::RigidBody2D>()); };

    desc.properties.push_back(make_vector_group<physics2d::RigidBody2D, Vec2>(
        "velocity", &physics2d::RigidBody2D::velocity, "Vec2", {"x", "y"}));
    desc.properties.push_back(
        make_scalar<physics2d::RigidBody2D, f32>("mass", &physics2d::RigidBody2D::mass, ValueKind::Float, &defaults));
    desc.properties.push_back(make_scalar<physics2d::RigidBody2D, f32>(
        "linear_damping", &physics2d::RigidBody2D::linear_damping, ValueKind::Float, &defaults));
    desc.properties.push_back(make_scalar<physics2d::RigidBody2D, bool>("kinematic", &physics2d::RigidBody2D::kinematic,
                                                                        ValueKind::Bool, &defaults));
    desc.properties.push_back(make_scalar<physics2d::RigidBody2D, bool>(
        "affected_by_gravity", &physics2d::RigidBody2D::affected_by_gravity, ValueKind::Bool, &defaults));

    registry.register_type(std::move(desc));
}

} // namespace

void register_physics_types() {
    Registry& registry = Registry::instance();
    register_collider2d(registry);
    register_rigid_body2d(registry);
}

} // namespace fe::reflect
