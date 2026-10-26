// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/physics3d/Physics3DTypes.h"

#include "fe/physics3d/Collider3D.h"
#include "fe/physics3d/DistanceJoint3D.h"
#include "fe/physics3d/RigidBody3D.h"
#include "fe/reflect/PropertyBuilders.h"
#include "fe/reflect/Registry.h"

#include <memory>

// The physics3d component registrations, in the same style as PhysicsTypes.cpp:
// each block describes a component's properties once, and the serializer and the
// inspector read those descriptions rather than naming the types. This is what
// makes a collider and a rigid body serialize and appear in the inspector with no
// per-type code in either.

namespace fe::reflect {
namespace {

// A property over the Collider3D shape enum. ValueKind has no enum kind, so the
// shape is exposed as an int (0 = box, 1 = sphere). The setter clamps to the valid
// range rather than trusting a hand-edited document, matching the total
// conversions the other builders use.
Property make_shape_property() {
    Property property;
    property.name = "shape";
    property.value_kind = ValueKind::Int;
    property.getter = [](const void* object) -> Value {
        return static_cast<i64>(static_cast<const physics3d::Collider3D*>(object)->shape);
    };
    property.setter = [](void* object, const Value& value) -> bool {
        const auto* as_int = std::get_if<i64>(&value);
        if (as_int == nullptr) {
            return false;
        }
        auto* collider = static_cast<physics3d::Collider3D*>(object);
        collider->shape = *as_int == 1 ? physics3d::ShapeType3D::Sphere : physics3d::ShapeType3D::Box;
        return true;
    };
    property.default_value = static_cast<i64>(physics3d::ShapeType3D::Box);
    return property;
}

void register_collider3d(Registry& registry) {
    physics3d::Collider3D defaults;

    TypeDesc desc;
    desc.name = physics3d::Collider3D::kTypeName;
    desc.factory = [] { return std::unique_ptr<Component>(std::make_unique<physics3d::Collider3D>()); };

    desc.properties.push_back(make_shape_property());
    desc.properties.push_back(make_vector_group<physics3d::Collider3D, Vec3>(
        "half_extent", &physics3d::Collider3D::half_extent, "Vec3", {"x", "y", "z"}));
    desc.properties.push_back(
        make_scalar<physics3d::Collider3D, f32>("radius", &physics3d::Collider3D::radius, ValueKind::Float, &defaults));
    desc.properties.push_back(make_scalar<physics3d::Collider3D, f32>(
        "restitution", &physics3d::Collider3D::restitution, ValueKind::Float, &defaults));
    desc.properties.push_back(make_scalar<physics3d::Collider3D, f32>("friction", &physics3d::Collider3D::friction,
                                                                      ValueKind::Float, &defaults));

    // Collision filtering. `layer` and `mask` are bit sets, exposed as integers
    // (the inspector shows them as an int field); `is_trigger` is a bool. All
    // three serialize, so a saved scene keeps its filtering.
    desc.properties.push_back(
        make_scalar<physics3d::Collider3D, u32>("layer", &physics3d::Collider3D::layer, ValueKind::Int, &defaults));
    desc.properties.push_back(
        make_scalar<physics3d::Collider3D, u32>("mask", &physics3d::Collider3D::mask, ValueKind::Int, &defaults));
    desc.properties.push_back(make_scalar<physics3d::Collider3D, bool>("is_trigger", &physics3d::Collider3D::is_trigger,
                                                                       ValueKind::Bool, &defaults));

    registry.register_type(std::move(desc));
}

void register_rigid_body3d(Registry& registry) {
    physics3d::RigidBody3D defaults;

    TypeDesc desc;
    desc.name = physics3d::RigidBody3D::kTypeName;
    desc.factory = [] { return std::unique_ptr<Component>(std::make_unique<physics3d::RigidBody3D>()); };

    desc.properties.push_back(make_vector_group<physics3d::RigidBody3D, Vec3>(
        "velocity", &physics3d::RigidBody3D::velocity, "Vec3", {"x", "y", "z"}));
    desc.properties.push_back(make_vector_group<physics3d::RigidBody3D, Vec3>(
        "angular_velocity", &physics3d::RigidBody3D::angular_velocity, "Vec3", {"x", "y", "z"}));
    desc.properties.push_back(make_vector_group<physics3d::RigidBody3D, Vec3>("torque", &physics3d::RigidBody3D::torque,
                                                                              "Vec3", {"x", "y", "z"}));
    desc.properties.push_back(
        make_scalar<physics3d::RigidBody3D, f32>("mass", &physics3d::RigidBody3D::mass, ValueKind::Float, &defaults));
    desc.properties.push_back(make_scalar<physics3d::RigidBody3D, f32>("inertia", &physics3d::RigidBody3D::inertia,
                                                                       ValueKind::Float, &defaults));
    desc.properties.push_back(make_scalar<physics3d::RigidBody3D, f32>(
        "linear_damping", &physics3d::RigidBody3D::linear_damping, ValueKind::Float, &defaults));
    desc.properties.push_back(make_scalar<physics3d::RigidBody3D, f32>(
        "angular_damping", &physics3d::RigidBody3D::angular_damping, ValueKind::Float, &defaults));
    desc.properties.push_back(make_scalar<physics3d::RigidBody3D, bool>("kinematic", &physics3d::RigidBody3D::kinematic,
                                                                        ValueKind::Bool, &defaults));
    desc.properties.push_back(make_scalar<physics3d::RigidBody3D, bool>(
        "affected_by_gravity", &physics3d::RigidBody3D::affected_by_gravity, ValueKind::Bool, &defaults));
    desc.properties.push_back(make_scalar<physics3d::RigidBody3D, bool>(
        "fixed_rotation", &physics3d::RigidBody3D::fixed_rotation, ValueKind::Bool, &defaults));

    registry.register_type(std::move(desc));
}

// A body reference: a node id, stored as a Uuid. The default is a nil id, which
// the solver treats as "unset" and skips, so a freshly created joint is inert
// until it is pointed at two bodies.
Property make_body_ref_property(std::string name, Uuid physics3d::DistanceJoint3D::* field) {
    Property property;
    property.name = std::move(name);
    property.value_kind = ValueKind::Uuid;
    property.getter = [field](const void* object) -> Value {
        return static_cast<const physics3d::DistanceJoint3D*>(object)->*field;
    };
    property.setter = [field](void* object, const Value& value) -> bool {
        const auto* as_id = std::get_if<Uuid>(&value);
        if (as_id == nullptr) {
            return false;
        }
        static_cast<physics3d::DistanceJoint3D*>(object)->*field = *as_id;
        return true;
    };
    property.default_value = Uuid{};
    return property;
}

void register_distance_joint3d(Registry& registry) {
    physics3d::DistanceJoint3D defaults;

    TypeDesc desc;
    desc.name = physics3d::DistanceJoint3D::kTypeName;
    desc.factory = [] { return std::unique_ptr<Component>(std::make_unique<physics3d::DistanceJoint3D>()); };

    desc.properties.push_back(make_body_ref_property("body_a", &physics3d::DistanceJoint3D::body_a));
    desc.properties.push_back(make_body_ref_property("body_b", &physics3d::DistanceJoint3D::body_b));
    desc.properties.push_back(make_scalar<physics3d::DistanceJoint3D, f32>(
        "rest_length", &physics3d::DistanceJoint3D::rest_length, ValueKind::Float, &defaults));
    desc.properties.push_back(make_scalar<physics3d::DistanceJoint3D, f32>(
        "stiffness", &physics3d::DistanceJoint3D::stiffness, ValueKind::Float, &defaults));
    desc.properties.push_back(make_scalar<physics3d::DistanceJoint3D, f32>(
        "damping", &physics3d::DistanceJoint3D::damping, ValueKind::Float, &defaults));

    registry.register_type(std::move(desc));
}

} // namespace

void register_physics3d_types() {
    Registry& registry = Registry::instance();
    register_collider3d(registry);
    register_rigid_body3d(registry);
    register_distance_joint3d(registry);
}

} // namespace fe::reflect
