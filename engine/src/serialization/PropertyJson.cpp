// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/serialization/PropertyJson.h"

#include "fe/core/Log.h"
#include "fe/resources/AssetId.h"
#include "fe/resources/ResourceManager.h"
#include "fe/serialization/Json.h"

#include <string>

namespace fe::serialization {
namespace {

using json = nlohmann::json;

} // namespace

json property_to_json(const reflect::Property& property, const void* object, const ResourceManager* resources) {
    using reflect::ValueKind;
    switch (property.kind()) {
    case ValueKind::Bool:
        return std::get<bool>(property.getter(object));
    case ValueKind::Int:
        return std::get<i64>(property.getter(object));
    case ValueKind::Float:
        return std::get<f64>(property.getter(object));
    case ValueKind::String:
        return std::get<std::string>(property.getter(object));
    case ValueKind::Uuid:
        return std::get<Uuid>(property.getter(object));
    case ValueKind::ResourceRef: {
        const std::string key = property.resource_ref ? property.resource_ref(object) : std::string{};
        // The property yields a loader reference key ("tex:assets/hero.png").
        // When an asset database knows that path, write its stable GUID instead
        // so the reference survives a file move. Without a database the path key
        // is written unchanged, which is the version-1 shape and still loads.
        if (!key.empty() && resources != nullptr) {
            return resources->resolve_reference(key);
        }
        return key;
    }
    case ValueKind::Group: {
        if (property.array_length > 0) {
            json array = json::array();
            for (const auto& member : property.members) {
                array.push_back(property_to_json(member, object, resources));
            }
            return array;
        }
        json struct_json = json::object();
        for (const auto& member : property.members) {
            struct_json[member.name] = property_to_json(member, object, resources);
        }
        return struct_json;
    }
    }
    return json();
}

Error apply_property(const reflect::Property& property, void* object, const json& source, ResourceManager* resources,
                     i32 source_version) {
    using reflect::ValueKind;

    switch (property.kind()) {
    case ValueKind::Bool:
        if (source.is_boolean() && property.setter(object, source.get<bool>())) {
            return Error::none();
        }
        break;
    case ValueKind::Int:
        if (source.is_number_integer() && property.setter(object, source.get<i64>())) {
            return Error::none();
        }
        break;
    case ValueKind::Float:
        if (source.is_number() && property.setter(object, source.get<f64>())) {
            return Error::none();
        }
        break;
    case ValueKind::String:
        if (source.is_string() && property.setter(object, source.get<std::string>())) {
            return Error::none();
        }
        break;
    case ValueKind::Uuid:
        if (source.is_string()) {
            Uuid id;
            if (Uuid::try_parse(source.get<std::string>(), id) && property.setter(object, id)) {
                return Error::none();
            }
        }
        break;
    case ValueKind::ResourceRef: {
        if (!source.is_string()) {
            break;
        }
        std::string key = source.get<std::string>();
        if (key.empty()) {
            return Error::none();
        }
        if (resources == nullptr) {
            // A reference with no manager to resolve it against. Nothing to load;
            // the object keeps a null reference. This is the same outcome as a
            // missing asset and is not fatal.
            return Error::none();
        }
        // Migrate a version-1 path key to the asset's stable GUID. A path with no
        // asset entry cannot be migrated: report it rather than loading a
        // silently broken reference.
        if (source_version < 2 && key.rfind(std::string(kGuidScheme) + ":", 0) != 0) {
            const std::string resolved = resources->resolve_reference(key);
            if (resolved == key) {
                return Error("serialization: resource reference '" + key + "' for property '" + property.name +
                             "' is not in the asset database and cannot be migrated");
            }
            FE_LOG_INFO("serialization: migrated resource reference '%s' to '%s'", key.c_str(), resolved.c_str());
            key = resolved;
        }
        if (!property.resolve_resource) {
            return Error::none();
        }
        if (property.resolve_resource(object, key, *resources) == nullptr) {
            // A missing texture is not fatal: the object still loads and simply
            // draws nothing. Report it so the user can fix the reference.
            FE_LOG_WARN("serialization: could not load resource '%s' for property '%s'", key.c_str(),
                        property.name.c_str());
        }
        return Error::none();
    }
    case ValueKind::Group: {
        if (property.array_length > 0) {
            if (!source.is_array()) {
                break;
            }
            for (u32 i = 0; i < property.members.size() && i < source.size(); ++i) {
                if (Error error = apply_property(property.members[i], object, source[i], resources, source_version);
                    error) {
                    return error;
                }
            }
            return Error::none();
        }
        if (!source.is_object()) {
            break;
        }
        for (const auto& member : property.members) {
            if (source.contains(member.name)) {
                if (Error error = apply_property(member, object, source.at(member.name), resources, source_version);
                    error) {
                    return error;
                }
            }
        }
        return Error::none();
    }
    }
    FE_LOG_WARN("serialization: property '%s' has an unexpected value; keeping default", property.name.c_str());
    return Error::none();
}

json properties_to_json(const std::vector<reflect::Property>& properties, const void* object,
                        const ResourceManager* resources) {
    json fields = json::object();
    for (const auto& property : properties) {
        fields[property.name] = property_to_json(property, object, resources);
    }
    return fields;
}

Error apply_properties(const std::vector<reflect::Property>& properties, void* object, const json& source,
                       ResourceManager* resources, i32 source_version) {
    for (const auto& property : properties) {
        if (source.contains(property.name)) {
            if (Error error = apply_property(property, object, source.at(property.name), resources, source_version);
                error) {
                return error;
            }
        }
    }
    return Error::none();
}

} // namespace fe::serialization
