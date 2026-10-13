// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Types.h"
#include "fe/core/Uuid.h"

#include <string>
#include <variant>

namespace fe::reflect {

// The set of property kinds the reflection system understands.
//
// This is deliberately a closed, small set rather than "any type". The two
// consumers (the scene serializer and, next milestone, the property inspector)
// only need to read and write leaf values; keeping the set closed makes both of
// them a single obvious switch and makes an unrecognised document fail loudly
// instead of silently.
//
// A struct-valued property (a Vec2, a Transform) is not a Value kind. It is
// modelled as a Group of scalar properties, which is what lets the inspector
// expand it and what lets the on-disk representation nest naturally.
enum class ValueKind : u8 {
    Bool,
    Int,
    Float,
    String,
    Uuid,
    // A resource reference, stored as "<scheme>:<path>" (for example
    // "tex:assets/hero.png"). The expected resource type is carried in
    // Property::resource (ResourceHint::type_name).
    ResourceRef,
    // A struct: "value" identifies the registered struct type ("Vec2", "Vec4",
    // "Transform"), and the members are listed in TypeDesc::groups.
    Group,
};

// A leaf or reference value. Mirrors ValueKind exactly.
using Value = std::variant<bool, i64, f64, std::string, fe::Uuid>;

[[nodiscard]] inline ValueKind kind_of(const Value& value) noexcept {
    switch (value.index()) {
    case 0:
        return ValueKind::Bool;
    case 1:
        return ValueKind::Int;
    case 2:
        return ValueKind::Float;
    case 3:
        return ValueKind::String;
    default:
        return ValueKind::Uuid;
    }
}

[[nodiscard]] inline const char* to_string(ValueKind kind) noexcept {
    switch (kind) {
    case ValueKind::Bool:
        return "bool";
    case ValueKind::Int:
        return "int";
    case ValueKind::Float:
        return "float";
    case ValueKind::String:
        return "string";
    case ValueKind::Uuid:
        return "uuid";
    case ValueKind::ResourceRef:
        return "resource_ref";
    case ValueKind::Group:
        return "group";
    }
    return "bool";
}

} // namespace fe::reflect
