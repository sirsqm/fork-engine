// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/physics2d/PhysicsTypes.h"

#include "fe/physics2d/Collider2D.h"
#include "fe/physics2d/DistanceJoint2D.h"
#include "fe/physics2d/RevoluteJoint2D.h"
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
    desc.properties.push_back(make_scalar<physics2d::RigidBody2D, f32>(
        "angular_velocity", &physics2d::RigidBody2D::angular_velocity, ValueKind::Float, &defaults));
    desc.properties.push_back(make_scalar<physics2d::RigidBody2D, f32>("torque", &physics2d::RigidBody2D::torque,
                                                                       ValueKind::Float, &defaults));
    desc.properties.push_back(
        make_scalar<physics2d::RigidBody2D, f32>("mass", &physics2d::RigidBody2D::mass, ValueKind::Float, &defaults));
    desc.properties.push_back(make_scalar<physics2d::RigidBody2D, f32>("inertia", &physics2d::RigidBody2D::inertia,
                                                                       ValueKind::Float, &defaults));
    desc.properties.push_back(make_scalar<physics2d::RigidBody2D, f32>(
        "linear_damping", &physics2d::RigidBody2D::linear_damping, ValueKind::Float, &defaults));
    desc.properties.push_back(make_scalar<physics2d::RigidBody2D, f32>(
        "angular_damping", &physics2d::RigidBody2D::angular_damping, ValueKind::Float, &defaults));
    desc.properties.push_back(make_scalar<physics2d::RigidBody2D, bool>("kinematic", &physics2d::RigidBody2D::kinematic,
                                                                        ValueKind::Bool, &defaults));
    desc.properties.push_back(make_scalar<physics2d::RigidBody2D, bool>(
        "affected_by_gravity", &physics2d::RigidBody2D::affected_by_gravity, ValueKind::Bool, &defaults));
    desc.properties.push_back(make_scalar<physics2d::RigidBody2D, bool>(
        "fixed_rotation", &physics2d::RigidBody2D::fixed_rotation, ValueKind::Bool, &defaults));

    // Sleeping. The authored knobs (whether it may sleep, how long it must be
    // still, and the thresholds) serialize; the live state (is_sleeping and the
    // rest timer) is runtime and deliberately does not, so a saved scene does not
    // reload with bodies frozen mid-settle.
    desc.properties.push_back(make_scalar<physics2d::RigidBody2D, bool>("can_sleep", &physics2d::RigidBody2D::can_sleep,
                                                                        ValueKind::Bool, &defaults));
    desc.properties.push_back(make_scalar<physics2d::RigidBody2D, f32>(
        "sleep_time", &physics2d::RigidBody2D::sleep_time, ValueKind::Float, &defaults));
    desc.properties.push_back(make_scalar<physics2d::RigidBody2D, f32>(
        "sleep_linear_threshold", &physics2d::RigidBody2D::sleep_linear_threshold, ValueKind::Float, &defaults));
    desc.properties.push_back(make_scalar<physics2d::RigidBody2D, f32>(
        "sleep_angular_threshold", &physics2d::RigidBody2D::sleep_angular_threshold, ValueKind::Float, &defaults));

    registry.register_type(std::move(desc));
}

// A body reference: a node id, stored as a Uuid. The default is a nil id, which
// the solver treats as "unset" and skips, so a freshly created joint is inert
// until it is pointed at two bodies.
Property make_body_ref_property(std::string name, Uuid physics2d::DistanceJoint2D::* field) {
    Property property;
    property.name = std::move(name);
    property.value_kind = ValueKind::Uuid;
    property.getter = [field](const void* object) -> Value {
        return static_cast<const physics2d::DistanceJoint2D*>(object)->*field;
    };
    property.setter = [field](void* object, const Value& value) -> bool {
        const auto* as_id = std::get_if<Uuid>(&value);
        if (as_id == nullptr) {
            return false;
        }
        static_cast<physics2d::DistanceJoint2D*>(object)->*field = *as_id;
        return true;
    };
    property.default_value = Uuid{};
    return property;
}

Property make_body_ref_property(std::string name, Uuid physics2d::RevoluteJoint2D::* field) {
    Property property;
    property.name = std::move(name);
    property.value_kind = ValueKind::Uuid;
    property.getter = [field](const void* object) -> Value {
        return static_cast<const physics2d::RevoluteJoint2D*>(object)->*field;
    };
    property.setter = [field](void* object, const Value& value) -> bool {
        const auto* as_id = std::get_if<Uuid>(&value);
        if (as_id == nullptr) {
            return false;
        }
        static_cast<physics2d::RevoluteJoint2D*>(object)->*field = *as_id;
        return true;
    };
    property.default_value = Uuid{};
    return property;
}

void register_distance_joint2d(Registry& registry) {
    physics2d::DistanceJoint2D defaults;

    TypeDesc desc;
    desc.name = physics2d::DistanceJoint2D::kTypeName;
    desc.factory = [] { return std::unique_ptr<Component>(std::make_unique<physics2d::DistanceJoint2D>()); };

    desc.properties.push_back(make_body_ref_property("body_a", &physics2d::DistanceJoint2D::body_a));
    desc.properties.push_back(make_body_ref_property("body_b", &physics2d::DistanceJoint2D::body_b));
    desc.properties.push_back(make_scalar<physics2d::DistanceJoint2D, f32>(
        "rest_length", &physics2d::DistanceJoint2D::rest_length, ValueKind::Float, &defaults));
    desc.properties.push_back(make_scalar<physics2d::DistanceJoint2D, f32>(
        "stiffness", &physics2d::DistanceJoint2D::stiffness, ValueKind::Float, &defaults));
    desc.properties.push_back(make_scalar<physics2d::DistanceJoint2D, f32>(
        "damping", &physics2d::DistanceJoint2D::damping, ValueKind::Float, &defaults));

    registry.register_type(std::move(desc));
}

void register_revolute_joint2d(Registry& registry) {
    physics2d::RevoluteJoint2D defaults;

    TypeDesc desc;
    desc.name = physics2d::RevoluteJoint2D::kTypeName;
    desc.factory = [] { return std::unique_ptr<Component>(std::make_unique<physics2d::RevoluteJoint2D>()); };

    desc.properties.push_back(make_body_ref_property("body_a", &physics2d::RevoluteJoint2D::body_a));
    desc.properties.push_back(make_body_ref_property("body_b", &physics2d::RevoluteJoint2D::body_b));
    desc.properties.push_back(make_vector_group<physics2d::RevoluteJoint2D, Vec2>(
        "anchor_a", &physics2d::RevoluteJoint2D::anchor_a, "Vec2", {"x", "y"}));
    desc.properties.push_back(make_vector_group<physics2d::RevoluteJoint2D, Vec2>(
        "anchor_b", &physics2d::RevoluteJoint2D::anchor_b, "Vec2", {"x", "y"}));

    registry.register_type(std::move(desc));
}

} // namespace

void register_physics_types() {
    Registry& registry = Registry::instance();
    register_collider2d(registry);
    register_rigid_body2d(registry);
    register_distance_joint2d(registry);
    register_revolute_joint2d(registry);
}

} // namespace fe::reflect
