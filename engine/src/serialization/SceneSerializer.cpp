// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/serialization/SceneSerializer.h"

#include "fe/core/FileSystem.h"
#include "fe/core/Log.h"
#include "fe/reflect/ComponentTypes.h"
#include "fe/reflect/Registry.h"
#include "fe/resources/ResourceManager.h"
#include "fe/scene/Node.h"
#include "fe/scene/Scene.h"
#include "fe/serialization/Json.h"

#include <nlohmann/json.hpp>

#include <string>
#include <string_view>
#include <utility>

namespace fe::serialization::scene {
namespace {

using json = nlohmann::json;

// ---------------------------------------------------------------------------
// Property values <-> JSON
// ---------------------------------------------------------------------------
// The document shape is a stable part of the file format, so the mapping lives
// here rather than in the reflection layer:
//
//   Bool            -> boolean
//   Int, Float      -> number
//   String          -> string
//   Uuid            -> string
//   ResourceRef     -> string ("<scheme>:<path>", e.g. "tex:assets/hero.png")
//   Group (array)   -> array of the members, e.g. [x, y]
//   Group (struct)  -> object keyed by member name, e.g. {"x":..,"y":..}
//
// The serializer no longer names any component type. It asks the reflection
// registry for a type by name, constructs it through the factory, and applies
// the properties the descriptor lists. Adding a property is one edit in
// ComponentTypes.cpp and it both persists and (next milestone) appears in the
// inspector.

[[nodiscard]] json value_to_json(const reflect::Property& property, const void* object) {
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
    case ValueKind::ResourceRef:
        return property.resource_ref ? property.resource_ref(object) : std::string{};
    case ValueKind::Group: {
        if (property.array_length > 0) {
            json array = json::array();
            for (const auto& member : property.members) {
                array.push_back(value_to_json(member, object));
            }
            return array;
        }
        json struct_json = json::object();
        for (const auto& member : property.members) {
            struct_json[member.name] = value_to_json(member, object);
        }
        return struct_json;
    }
    }
    return json();
}

// Applies one JSON value to a property. Malformed input is logged and skipped
// rather than fatal: a hand-edited scene should still load, and the property
// simply keeps its default. This matches the reader's policy for unknown
// component types.
void apply_value(const reflect::Property& property, void* object, const json& source, ResourceManager* resources) {
    using reflect::ValueKind;

    switch (property.kind()) {
    case ValueKind::Bool:
        if (source.is_boolean() && property.setter(object, source.get<bool>())) {
            return;
        }
        break;
    case ValueKind::Int:
        if (source.is_number_integer() && property.setter(object, source.get<i64>())) {
            return;
        }
        break;
    case ValueKind::Float:
        if (source.is_number() && property.setter(object, source.get<f64>())) {
            return;
        }
        break;
    case ValueKind::String:
        if (source.is_string() && property.setter(object, source.get<std::string>())) {
            return;
        }
        break;
    case ValueKind::Uuid:
        if (source.is_string()) {
            Uuid id;
            if (Uuid::try_parse(source.get<std::string>(), id) && property.setter(object, id)) {
                return;
            }
        }
        break;
    case ValueKind::ResourceRef: {
        if (!source.is_string()) {
            break;
        }
        const std::string key = source.get<std::string>();
        if (key.empty() || resources == nullptr || !property.resolve_resource) {
            return;
        }
        if (property.resolve_resource(object, key, *resources) == nullptr) {
            // A missing texture is not fatal: the object still loads and simply
            // draws nothing. Report it so the user can fix the reference.
            FE_LOG_WARN("scene: could not load resource '%s' for property '%s'", key.c_str(), property.name.c_str());
        }
        return;
    }
    case ValueKind::Group: {
        if (property.array_length > 0) {
            if (!source.is_array()) {
                break;
            }
            for (u32 i = 0; i < property.members.size() && i < source.size(); ++i) {
                apply_value(property.members[i], object, source[i], resources);
            }
            return;
        }
        if (!source.is_object()) {
            break;
        }
        for (const auto& member : property.members) {
            if (source.contains(member.name)) {
                apply_value(member, object, source.at(member.name), resources);
            }
        }
        return;
    }
    }
    FE_LOG_WARN("scene: property '%s' has an unexpected value; keeping default", property.name.c_str());
}

[[nodiscard]] json component_to_json(const Component& component) {
    json fields = json::object();
    fields["type"] = component.type_name();

    if (const reflect::TypeDesc* type = reflect::Registry::instance().find(component.type_name())) {
        for (const auto& property : type->properties) {
            fields[property.name] = value_to_json(property, &component);
        }
    }
    return fields;
}

void component_from_json(const json& source, Node& node, ResourceManager* resources) {
    const std::string type_name = source.value("type", std::string{});
    const reflect::TypeDesc* type = reflect::Registry::instance().find(type_name);
    if (type == nullptr) {
        // An unknown component type is recorded by name in the document; the
        // reader warns instead of silently dropping it, so a scene authored for
        // a build with more component types is not quietly corrupted.
        FE_LOG_WARN("scene: ignoring unknown component type '%s'", type_name.c_str());
        return;
    }

    Component* component = node.add_component(type->factory());
    for (const auto& property : type->properties) {
        if (source.contains(property.name)) {
            apply_value(property, component, source.at(property.name), resources);
        }
    }
}

// ---------------------------------------------------------------------------
// Nodes
// ---------------------------------------------------------------------------

[[nodiscard]] json node_to_json(const Node& node) {
    json j;
    j["id"] = node.id();
    j["name"] = node.name();
    j["type"] = node.type_name();
    j["transform"] = node.transform();

    json components = json::array();
    for (const auto& component : node.components()) {
        components.push_back(component_to_json(*component));
    }
    j["components"] = std::move(components);

    json children = json::array();
    for (const auto& child : node.children()) {
        children.push_back(node_to_json(*child));
    }
    j["children"] = std::move(children);
    return j;
}

[[nodiscard]] Error node_from_json(const json& j, Node& node, ResourceManager* resources) {
    // The node's identity and name are restored in place; the caller created
    // the node so the type is already correct.
    const std::string name = j.value("name", std::string("Node"));
    node.set_name(name);

    // Restore identity so that references to this node survive a save/load
    // cycle. Absent ids (hand-written or older documents) keep the id assigned
    // when the node was created.
    if (j.contains("id")) {
        node.set_id(j.at("id").get<Uuid>());
    }

    if (j.contains("transform")) {
        node.transform() = j.at("transform").get<Transform>();
    }

    if (j.contains("components")) {
        for (const auto& component_json : j.at("components")) {
            component_from_json(component_json, node, resources);
        }
    }

    if (j.contains("children")) {
        for (const auto& child_json : j.at("children")) {
            // Plain nodes are the only node type this milestone persists. A
            // future node subtype would be dispatched on child_json["type"].
            Node* child = node.create_child<Node>();
            if (Error error = node_from_json(child_json, *child, resources); error) {
                return error;
            }
        }
    }
    return Error::none();
}

} // namespace

Result<std::string> to_json(const Scene& scene) {
    json document;
    document["format"] = "fork_scene";
    document["format_version"] = kFormatVersion;
    document["name"] = scene.name();

    json roots = json::array();
    for (const auto& root : scene.roots()) {
        roots.push_back(node_to_json(*root));
    }
    document["roots"] = std::move(roots);

    return document.dump(2);
}

Error from_json(std::string_view text, Scene& out_scene, ResourceManager* resources) {
    // A loader may be called before any Engine exists (a test, a tool), so the
    // built-in component types are ensured here. Registration is idempotent.
    reflect::register_builtin_types();

    json document;
    try {
        document = json::parse(text);
    } catch (const json::parse_error& e) {
        return Error(std::string("scene parse error: ") + e.what());
    }

    const std::string format = document.value("format", std::string{});
    if (format != "fork_scene") {
        return Error("not a fork scene document (format='" + format + "')");
    }

    const i32 version = document.value("format_version", 0);
    if (version > kFormatVersion) {
        return Error("scene format version " + std::to_string(version) + " is newer than supported version " +
                     std::to_string(kFormatVersion));
    }

    out_scene.clear();
    out_scene.set_name(document.value("name", std::string("Scene")));

    if (!document.contains("roots")) {
        return Error::none();
    }

    for (const auto& root_json : document.at("roots")) {
        Node* root = out_scene.create_node();
        if (Error error = node_from_json(root_json, *root, resources); error) {
            return error;
        }
    }
    return Error::none();
}

Error save(const Scene& scene, std::string_view path) {
    auto text = to_json(scene);
    if (!text) {
        return text.error();
    }
    return fs::write_text_file(path, text.value());
}

Error load(std::string_view path, Scene& out_scene, ResourceManager* resources) {
    auto text = fs::read_text_file(path);
    if (!text) {
        return text.error();
    }
    return from_json(text.value(), out_scene, resources);
}

} // namespace fe::serialization::scene
