// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/reflect/Property.h"

#include <string>

namespace fe::reflect {

const Property* resolve_property(const std::vector<Property>& properties, const std::vector<std::string>& path) {
    const Property* current = nullptr;
    const std::vector<Property>* level = &properties;
    for (const std::string& name : path) {
        current = nullptr;
        for (const Property& candidate : *level) {
            if (candidate.name == name) {
                current = &candidate;
                break;
            }
        }
        if (current == nullptr) {
            return nullptr;
        }
        level = &current->members;
    }
    return current;
}

std::optional<Value> read_property(const Property& property, const void* object) {
    if (!property.getter) {
        return std::nullopt;
    }
    return property.getter(object);
}

bool write_property(const Property& property, void* object, const Value& value) {
    if (!property.setter) {
        return false;
    }
    return property.setter(object, value);
}

} // namespace fe::reflect
