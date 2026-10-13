// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Math.h"
#include "fe/core/Types.h"
#include "fe/core/Uuid.h"
#include "fe/reflect/Property.h"
#include "fe/resources/ResourceManager.h"

#include <memory>
#include <string>
#include <type_traits>
#include <vector>

// Helpers for describing a component's properties.
//
// A registration function reads like the struct it describes:
//
//   desc.properties.push_back(make_vector_group<Sprite2D, Vec2>(
//       "size", &Sprite2D::size, "Vec2", {"x", "y"}));
//
// A group's member accessors take the *owning component* pointer, so one group
// can aggregate fields that are not adjacent in memory (for example a camera's
// four clear-colour channels). The value conversions are deliberately total: a
// float property accepts an integer from a hand-edited document, and a setter
// reports failure (rather than throwing) on a value of the wrong kind, so a bad
// document produces a logged warning and a default instead of a crash.

namespace fe::reflect {

namespace detail {

template <typename>
inline constexpr bool always_false = false;

template <typename T>
[[nodiscard]] Value to_value(const T& value) {
    if constexpr (std::is_same_v<T, bool>) {
        return value;
    } else if constexpr (std::is_integral_v<T>) {
        return static_cast<i64>(value);
    } else if constexpr (std::is_floating_point_v<T>) {
        return static_cast<f64>(value);
    } else if constexpr (std::is_same_v<T, std::string>) {
        return value;
    } else if constexpr (std::is_same_v<T, fe::Uuid>) {
        return value;
    } else {
        static_assert(always_false<T>, "no reflect::Value conversion for this type");
        return {};
    }
}

template <typename T>
[[nodiscard]] bool from_value(const Value& value, T& out) {
    if constexpr (std::is_same_v<T, bool>) {
        if (const auto* p = std::get_if<bool>(&value)) {
            out = *p;
            return true;
        }
        return false;
    } else if constexpr (std::is_integral_v<T>) {
        if (const auto* p = std::get_if<i64>(&value)) {
            out = static_cast<T>(*p);
            return true;
        }
        return false;
    } else if constexpr (std::is_floating_point_v<T>) {
        if (const auto* p = std::get_if<f64>(&value)) {
            out = static_cast<T>(*p);
            return true;
        }
        if (const auto* p = std::get_if<i64>(&value)) {
            out = static_cast<T>(*p);
            return true;
        }
        return false;
    } else if constexpr (std::is_same_v<T, std::string>) {
        if (const auto* p = std::get_if<std::string>(&value)) {
            out = *p;
            return true;
        }
        return false;
    } else if constexpr (std::is_same_v<T, fe::Uuid>) {
        if (const auto* p = std::get_if<fe::Uuid>(&value)) {
            out = *p;
            return true;
        }
        return false;
    } else {
        static_assert(always_false<T>, "no reflect::Value conversion for this type");
        return false;
    }
}

} // namespace detail

// A scalar property over a single member field of `Class`.
template <typename Class, typename FieldT>
[[nodiscard]] Property make_scalar(std::string name, FieldT Class::* field, ValueKind kind,
                                   const void* default_object = nullptr) {
    Property property;
    property.name = std::move(name);
    property.value_kind = kind;
    property.getter = [field](const void* object) -> Value {
        return detail::to_value(static_cast<const Class*>(object)->*field);
    };
    property.setter = [field](void* object, const Value& value) -> bool {
        FieldT out{};
        if (!detail::from_value(value, out)) {
            return false;
        }
        static_cast<Class*>(object)->*field = out;
        return true;
    };
    property.default_value = default_object != nullptr ? property.getter(default_object) : detail::to_value(FieldT{});
    return property;
}

// A group property: a named struct assembled from member properties. The
// members' accessors all take the owning component pointer.
[[nodiscard]] inline Property make_group(std::string name, std::string type_name, u32 array_length,
                                         std::vector<Property> members) {
    Property property;
    property.name = std::move(name);
    property.value_kind = ValueKind::Group;
    property.group_type = std::move(type_name);
    property.array_length = array_length;
    property.members = std::move(members);
    return property;
}

// Attaches a numeric-range hint, so the inspector shows a bounded slider or
// drag. `step` of 0 means "no fixed step" (the inspector chooses). Kept
// separate from make_scalar because only some numeric properties are bounded.
[[nodiscard]] inline Property with_range(Property property, f64 minimum, f64 maximum, f64 step = 0.0) {
    property.range = RangeHint{minimum, maximum, step};
    return property;
}

// Attaches a colour hint, so a float group is shown as a colour picker. The
// group's member count (three or four) is what selects RGB vs RGBA.
[[nodiscard]] inline Property with_color(Property property) {
    property.color = ColorHint{};
    return property;
}

// A group over a fixed-size vector field (Vec2, Vec3, Vec4), with one float
// member per component. The vector must be standard-layout and hold only floats
// so that its storage can be read as a float array; that is asserted, not
// assumed.
template <typename Class, typename VectorT>
[[nodiscard]] Property make_vector_group(std::string name, VectorT Class::* field, std::string type_name,
                                         std::vector<std::string> component_names) {
    static_assert(std::is_standard_layout_v<VectorT>, "reflect vector groups require a standard-layout type");
    static_assert(sizeof(VectorT) == sizeof(f32) * 2 || sizeof(VectorT) == sizeof(f32) * 3 ||
                      sizeof(VectorT) == sizeof(f32) * 4,
                  "reflect vector groups support Vec2, Vec3 and Vec4");

    std::vector<Property> members;
    members.reserve(component_names.size());
    for (u32 i = 0; i < component_names.size(); ++i) {
        Property member;
        member.name = component_names[i];
        member.value_kind = ValueKind::Float;
        member.getter = [field, i](const void* object) -> Value {
            const VectorT& value = static_cast<const Class*>(object)->*field;
            return static_cast<f64>(reinterpret_cast<const f32*>(&value)[i]);
        };
        member.setter = [field, i](void* object, const Value& value) -> bool {
            f32 out = 0.0f;
            if (!detail::from_value(value, out)) {
                return false;
            }
            VectorT& target = static_cast<Class*>(object)->*field;
            reinterpret_cast<f32*>(&target)[i] = out;
            return true;
        };
        members.push_back(std::move(member));
    }
    return make_group(std::move(name), std::move(type_name), static_cast<u32>(component_names.size()),
                      std::move(members));
}

// A group over four separate float fields (an RGBA colour stored across four
// members), with one float member per named channel.
[[nodiscard]] inline Property make_float_group(std::string name, std::string type_name,
                                               std::vector<std::string> channel_names,
                                               std::vector<Property::Getter> getters,
                                               std::vector<Property::Setter> setters) {
    std::vector<Property> members;
    members.reserve(channel_names.size());
    for (u32 i = 0; i < channel_names.size(); ++i) {
        Property member;
        member.name = channel_names[i];
        member.value_kind = ValueKind::Float;
        member.getter = std::move(getters[i]);
        member.setter = std::move(setters[i]);
        members.push_back(std::move(member));
    }
    return make_group(std::move(name), std::move(type_name), static_cast<u32>(channel_names.size()),
                      std::move(members));
}

// A getter/setter pair for a single float member of `Class`.
template <typename Class>
[[nodiscard]] Property::Getter float_getter(f32 Class::* field) {
    return [field](const void* object) -> Value { return static_cast<f64>(static_cast<const Class*>(object)->*field); };
}

template <typename Class>
[[nodiscard]] Property::Setter float_setter(f32 Class::* field) {
    return [field](void* object, const Value& value) -> bool {
        f32 out = 0.0f;
        if (!detail::from_value(value, out)) {
            return false;
        }
        static_cast<Class*>(object)->*field = out;
        return true;
    };
}

// A resource-reference property. `MemberT` is a std::shared_ptr<Resource>
// member; the getter yields the manager key, and the serializer resolves a key
// back into a live resource through resolve_resource.
template <typename Class, typename MemberT>
[[nodiscard]] Property make_resource_ref(std::string name, MemberT Class::* field, std::string resource_type,
                                         std::function<std::string(const MemberT&)> key_of,
                                         std::function<void(MemberT&, std::shared_ptr<Resource>)> assign) {
    Property property;
    property.name = std::move(name);
    property.value_kind = ValueKind::ResourceRef;
    property.resource = ResourceHint{std::move(resource_type)};
    property.resource_ref = [field, key_of](const void* object) -> std::string {
        return key_of(static_cast<const Class*>(object)->*field);
    };
    property.resolve_resource = [field, assign](void* object, std::string_view key,
                                                ResourceManager& manager) -> std::shared_ptr<Resource> {
        // An empty key clears the slot. This is the explicit "remove the
        // resource" edit; a non-empty key that fails to load leaves the slot
        // untouched so a typo never discards a good reference.
        if (key.empty()) {
            assign(static_cast<Class*>(object)->*field, nullptr);
            return nullptr;
        }
        auto loaded = manager.load(key);
        if (!loaded) {
            return nullptr;
        }
        assign(static_cast<Class*>(object)->*field, loaded.value());
        return loaded.value();
    };
    return property;
}

} // namespace fe::reflect
