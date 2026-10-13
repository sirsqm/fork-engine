// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Types.h"
#include "fe/reflect/Value.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace fe {

class Resource;
class ResourceManager;

namespace reflect {

// Editor hints attached to a property.
//
// The hints are what let a single description drive more than serialization:
// the inspector reads them to decide which widget to show ("a float in
// [0, 1]" is a slider, a resource ref is an asset drop target). They are plain
// metadata and have no effect on the serialized value.
enum class Hint : u8 {
    None,
    Range,     // hint_text is "min,max" (floats).
    Multiline, // a string shown in a multi-line box.
    FilePath,  // a string that names a file.
    Resource,  // a resource reference; the type is in Property::resource_type.
    Color,     // a group of floats shown as a colour swatch.
};

// One property of a registered type.
//
// A Property is either a scalar leaf (Bool/Int/Float/String/Uuid/ResourceRef)
// or a Group. A Group is a struct-valued field (a Vec2, a Transform, a colour);
// its members are nested Property entries whose accessors take the *owning
// component* pointer, so a group can aggregate fields that are not contiguous.
//
// Accessors are std::function rather than raw function pointers so a property
// can be built from a member pointer with no per-call boilerplate. Registration
// happens once at startup and reads happen at editor/load rates, so the
// indirection is not on any measured hot path.
struct Property {
    using Getter = std::function<Value(const void*)>;
    using Setter = std::function<bool(void*, const Value&)>;

    std::string name;

    // The property's kind, set explicitly by the builder helpers.
    ValueKind value_kind = ValueKind::Float;

    // Leaf scalar access. Empty for a group.
    Getter getter;
    Setter setter;
    // The value a freshly constructed instance reports, captured once at
    // registration. Used to write a complete document even when the owning
    // object is unavailable.
    Value default_value{};

    // For a group: the registered struct type name ("Vec2", "Transform").
    std::string group_type;
    // For a group laid over a fixed-size array (Vec2 = 2): the element count.
    u32 array_length = 0;

    // For a resource reference: reads the referenced resource's manager key (for
    // example "tex:assets/hero.png"), or an empty string when unset. Writing
    // raw through this would discard the resolved resource, so a ResourceRef
    // property carries a resolver the serializer uses instead.
    std::function<std::string(const void*)> resource_ref;
    std::function<std::shared_ptr<Resource>(void*, std::string_view, ResourceManager&)> resolve_resource;

    // Editor hints.
    Hint hint = Hint::None;
    std::string hint_text;
    // For ResourceRef: the expected resource type name ("Texture2D").
    std::string resource_type;

    // For a group: the member properties.
    std::vector<Property> members;

    [[nodiscard]] ValueKind kind() const noexcept { return value_kind; }

    [[nodiscard]] bool is_group() const noexcept { return value_kind == ValueKind::Group; }
};

} // namespace reflect
} // namespace fe
