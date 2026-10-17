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
#include "fe/serialization/PropertyJson.h"

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace fe::serialization::scene {
namespace {

using json = nlohmann::json;

// ---------------------------------------------------------------------------
// Components
// ---------------------------------------------------------------------------
// The serializer names no component type. It asks the reflection registry for a
// type by name, constructs it through the factory, and applies the properties
// the descriptor lists through the shared property<->JSON mapping
// (PropertyJson.h), which the script bindings use too. Adding a property is one
// edit in ComponentTypes.cpp and it both persists and appears in the inspector.

[[nodiscard]] json component_to_json(const Component& component, const ResourceManager* resources) {
    json fields = json::object();
    fields["type"] = component.type_name();
    // The component's stable id, written so that an edit that names this exact
    // component (and the operation log of collaborative editing) survives a
    // save/load cycle. A component is written by id, not by index, for the same
    // reason a node is: index is not identity, and a node may carry more than
    // one component of the same type.
    fields["id"] = component.id();

    if (const reflect::TypeDesc* type = reflect::Registry::instance().find(component.type_name())) {
        json reflected = properties_to_json(type->properties, &component, resources);
        for (auto& [key, value] : reflected.items()) {
            fields[key] = std::move(value);
        }
    }
    return fields;
}

[[nodiscard]] Error component_from_json(const json& source, Node& node, ResourceManager* resources,
                                        i32 source_version) {
    const std::string type_name = source.value("type", std::string{});
    const reflect::TypeDesc* type = reflect::Registry::instance().find(type_name);
    if (type == nullptr) {
        // An unknown component type is recorded by name in the document; the
        // reader warns instead of silently dropping it, so a scene authored for
        // a build with more component types is not quietly corrupted.
        FE_LOG_WARN("scene: ignoring unknown component type '%s'", type_name.c_str());
        return Error::none();
    }

    Component* component = node.add_component(type->factory());

    // Restore identity so references to this component survive a save/load
    // cycle. A document without ids (hand-written, or version 2 and older)
    // keeps the id the component was assigned when it was created.
    if (source.contains("id")) {
        component->set_id(source.at("id").get<Uuid>());
    }

    return apply_properties(type->properties, component, source, resources, source_version);
}

// ---------------------------------------------------------------------------
// Nodes
// ---------------------------------------------------------------------------

[[nodiscard]] json node_to_json(const Node& node, const ResourceManager* resources, const ScriptBinding* scripts) {
    json j;
    j["id"] = node.id();
    j["name"] = node.name();
    j["type"] = node.type_name();
    j["transform"] = node.transform();

    json components = json::array();
    for (const auto& component : node.components()) {
        components.push_back(component_to_json(*component, resources));
    }
    j["components"] = std::move(components);

    // The scripts bound to this node, when a binding is present. Written after
    // the components so a reader sees a node's data before its behaviour.
    if (scripts != nullptr) {
        scripts->write(node, j);
    }

    json children = json::array();
    for (const auto& child : node.children()) {
        children.push_back(node_to_json(*child, resources, scripts));
    }
    j["children"] = std::move(children);
    return j;
}

[[nodiscard]] Error node_from_json(const json& j, Node& node, ResourceManager* resources, i32 source_version,
                                   ScriptBinding* scripts) {
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
            if (Error error = component_from_json(component_json, node, resources, source_version); error) {
                return error;
            }
        }
    }

    // Record the node's scripts now that the node exists. They are not attached
    // here: a binding may name a node that has not been read yet, so attaching
    // waits until the whole scene is in place (see ScriptBinding).
    if (scripts != nullptr) {
        scripts->read(node, j);
    }

    if (j.contains("children")) {
        for (const auto& child_json : j.at("children")) {
            // Plain nodes are the only node type this milestone persists. A
            // future node subtype would be dispatched on child_json["type"].
            Node* child = node.create_child<Node>();
            if (Error error = node_from_json(child_json, *child, resources, source_version, scripts); error) {
                return error;
            }
        }
    }
    return Error::none();
}

} // namespace

Result<std::string> to_json(const Scene& scene, const ResourceManager* resources, const ScriptBinding* scripts) {
    json document;
    document["format"] = "fork_scene";
    document["format_version"] = kFormatVersion;
    document["name"] = scene.name();

    json roots = json::array();
    for (const auto& root : scene.roots()) {
        roots.push_back(node_to_json(*root, resources, scripts));
    }
    document["roots"] = std::move(roots);

    return document.dump(2);
}

Error from_json(std::string_view text, Scene& out_scene, ResourceManager* resources, ScriptBinding* scripts) {
    // A loader may be called before any Engine exists (a test, a tool), so the
    // built-in component types are ensured here. Registration is idempotent.
    reflect::register_builtin_types();

    if (scripts != nullptr) {
        scripts->reset();
    }

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
        if (Error error = node_from_json(root_json, *root, resources, version, scripts); error) {
            return error;
        }
    }

    // Deserialization assigned the saved ids after each node was attached, so
    // the scene's id index still holds the placeholder ids from creation.
    // Rebuild it now that every id is final, so `find_node` resolves saved ids
    // and any component holding a node reference (a joint's body_a/body_b)
    // points at the right node.
    out_scene.rebuild_index();

    // Every node exists and the index is final, so a recorded script can now be
    // constructed and attached.
    if (scripts != nullptr) {
        return scripts->finish(out_scene);
    }
    return Error::none();
}

Error save(const Scene& scene, std::string_view path, const ResourceManager* resources, const ScriptBinding* scripts) {
    auto text = to_json(scene, resources, scripts);
    if (!text) {
        return text.error();
    }
    return fs::write_text_file(path, text.value());
}

Error load(std::string_view path, Scene& out_scene, ResourceManager* resources, ScriptBinding* scripts) {
    // Read through the manager's source when there is one, so a mounted pack
    // serves the scene as transparently as the filesystem does. Without a
    // manager (a tool, a test) the file is read directly.
    Result<std::string> text = Error{};
    if (resources != nullptr) {
        text = resources->read_text(path);
    } else {
        text = fs::read_text_file(path);
    }
    if (!text) {
        return text.error();
    }
    return from_json(text.value(), out_scene, resources, scripts);
}

} // namespace fe::serialization::scene
