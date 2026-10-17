// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/serialization/Operation.h"

#include "fe/reflect/Registry.h"
#include "fe/reflect/TypeDesc.h"
#include "fe/resources/ResourceManager.h"
#include "fe/scene/Component.h"
#include "fe/scene/Node.h"
#include "fe/scene/Scene.h"
#include "fe/serialization/ScriptTarget.h"

#include <nlohmann/json.hpp>

#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fe::serialization {
namespace {

using json = nlohmann::json;

// --- small helpers shared with the command implementations ----------------

[[nodiscard]] usize sibling_index(const Scene& scene, const Node& node) {
    if (const Node* parent = node.parent(); parent != nullptr) {
        const auto& children = parent->children();
        for (usize i = 0; i < children.size(); ++i) {
            if (children[i].get() == &node) {
                return i;
            }
        }
        return 0;
    }
    const auto& roots = scene.roots();
    for (usize i = 0; i < roots.size(); ++i) {
        if (roots[i].get() == &node) {
            return i;
        }
    }
    return 0;
}

[[nodiscard]] std::unique_ptr<Node> detach_node(Scene& scene, Node* node) {
    if (node == nullptr) {
        return nullptr;
    }
    if (Node* parent = node->parent(); parent != nullptr) {
        return parent->remove_child(node);
    }
    return scene.remove_root(node);
}

void attach_node(Scene& scene, std::unique_ptr<Node> node, const Uuid& parent_id, usize index) {
    if (node == nullptr) {
        return;
    }
    Node* parent = parent_id.is_nil() ? nullptr : scene.find_node(parent_id);
    if (parent != nullptr) {
        parent->add_child(std::move(node), index);
    } else {
        scene.add_root(std::move(node), index);
    }
}

// The component with `id` on `node`, or nullptr.
[[nodiscard]] Component* component_by_id(Node& node, const Uuid& id) noexcept {
    for (const auto& component : node.components()) {
        if (component->id() == id) {
            return component.get();
        }
    }
    return nullptr;
}

// The index of the component with `id` on `node`, or `npos`.
[[nodiscard]] usize component_index_by_id(const Node& node, const Uuid& id) noexcept {
    const auto& components = node.components();
    for (usize i = 0; i < components.size(); ++i) {
        if (components[i]->id() == id) {
            return i;
        }
    }
    return static_cast<usize>(-1);
}

// The property named by `path` within a component type. Delegates to the shared
// resolver so a component path and a script path are walked by the same code.
[[nodiscard]] const reflect::Property* resolve_component_property(const reflect::TypeDesc& type,
                                                                  const std::vector<std::string>& path) {
    return reflect::resolve_property(type.properties, path);
}

// --- value <-> JSON --------------------------------------------------------
//
// reflect::Value is a variant, and JSON alone cannot tell an int from a float
// from a string, so a value is tagged with its kind. This mirrors the reflection
// ValueKind set rather than the serializer's untagged document form, because an
// operation must round-trip exactly.

[[nodiscard]] json value_to_json(const reflect::Value& value) {
    switch (value.index()) {
    case 0:
        return json{{"kind", "bool"}, {"value", std::get<bool>(value)}};
    case 1:
        return json{{"kind", "int"}, {"value", std::get<i64>(value)}};
    case 2:
        return json{{"kind", "float"}, {"value", std::get<f64>(value)}};
    case 3:
        return json{{"kind", "string"}, {"value", std::get<std::string>(value)}};
    default:
        return json{{"kind", "uuid"}, {"value", std::get<Uuid>(value).to_string()}};
    }
}

[[nodiscard]] Error value_from_json(const json& source, reflect::Value& out) {
    if (!source.is_object() || !source.contains("kind") || !source.contains("value")) {
        return Error("operation: a property value must be a {kind, value} object");
    }
    const std::string kind = source.at("kind").get<std::string>();
    const json& value = source.at("value");
    if (kind == "bool") {
        out = value.get<bool>();
    } else if (kind == "int") {
        out = value.get<i64>();
    } else if (kind == "float") {
        out = value.get<f64>();
    } else if (kind == "string") {
        out = value.get<std::string>();
    } else if (kind == "uuid") {
        Uuid id;
        if (!Uuid::try_parse(value.get<std::string>(), id)) {
            return Error("operation: malformed uuid value");
        }
        out = id;
    } else {
        return Error("operation: unknown value kind '" + kind + "'");
    }
    return Error::none();
}

[[nodiscard]] Error parse_uuid(const json& source, const char* field, Uuid& out) {
    if (!source.contains(field) || !source.at(field).is_string()) {
        return Error(std::string("operation: missing or non-string '") + field + "'");
    }
    if (!Uuid::try_parse(source.at(field).get<std::string>(), out)) {
        return Error(std::string("operation: malformed '") + field + "'");
    }
    return Error::none();
}

} // namespace

const char* to_string(OperationKind kind) noexcept {
    switch (kind) {
    case OperationKind::CreateNode:
        return "create_node";
    case OperationKind::DeleteNode:
        return "delete_node";
    case OperationKind::ReparentNode:
        return "reparent_node";
    case OperationKind::RenameNode:
        return "rename_node";
    case OperationKind::AddComponent:
        return "add_component";
    case OperationKind::RemoveComponent:
        return "remove_component";
    case OperationKind::SetProperty:
        return "set_property";
    case OperationKind::SetResource:
        return "set_resource";
    case OperationKind::SetScriptProperty:
        return "set_script_property";
    }
    return "rename_node";
}

bool operation_kind_from_string(std::string_view text, OperationKind& out) noexcept {
    const OperationKind kinds[] = {
        OperationKind::CreateNode,  OperationKind::DeleteNode,   OperationKind::ReparentNode,
        OperationKind::RenameNode,  OperationKind::AddComponent, OperationKind::RemoveComponent,
        OperationKind::SetProperty, OperationKind::SetResource,  OperationKind::SetScriptProperty,
    };
    for (const OperationKind kind : kinds) {
        if (text == to_string(kind)) {
            out = kind;
            return true;
        }
    }
    return false;
}

std::string to_json(const Operation& op) {
    json j;
    j["kind"] = to_string(op.kind);
    switch (op.kind) {
    case OperationKind::CreateNode:
        j["node_id"] = op.node_id.to_string();
        j["parent_id"] = op.parent_id.to_string();
        j["index"] = op.index;
        j["name"] = op.name;
        break;
    case OperationKind::DeleteNode:
        j["node_id"] = op.node_id.to_string();
        break;
    case OperationKind::ReparentNode:
        j["node_id"] = op.node_id.to_string();
        j["parent_id"] = op.parent_id.to_string();
        j["index"] = op.index;
        break;
    case OperationKind::RenameNode:
        j["node_id"] = op.node_id.to_string();
        j["name"] = op.name;
        break;
    case OperationKind::AddComponent:
        j["node_id"] = op.node_id.to_string();
        j["component_id"] = op.component_id.to_string();
        j["type_name"] = op.name;
        j["index"] = op.index;
        break;
    case OperationKind::RemoveComponent:
        j["node_id"] = op.node_id.to_string();
        j["component_id"] = op.component_id.to_string();
        break;
    case OperationKind::SetProperty:
        j["node_id"] = op.node_id.to_string();
        j["component_id"] = op.component_id.to_string();
        j["path"] = op.path;
        j["value"] = value_to_json(op.value);
        break;
    case OperationKind::SetResource:
        j["node_id"] = op.node_id.to_string();
        j["component_id"] = op.component_id.to_string();
        j["property_name"] = op.property_name;
        j["reference"] = op.reference;
        break;
    case OperationKind::SetScriptProperty:
        j["node_id"] = op.node_id.to_string();
        j["behaviour"] = op.name;
        j["path"] = op.path;
        j["value"] = value_to_json(op.value);
        break;
    }
    return j.dump();
}

Error from_json(std::string_view text, Operation& out) {
    json j = json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object()) {
        return Error("operation: not a JSON object");
    }
    if (!j.contains("kind") || !j.at("kind").is_string()) {
        return Error("operation: missing 'kind'");
    }
    OperationKind kind{};
    if (!operation_kind_from_string(j.at("kind").get<std::string>(), kind)) {
        return Error("operation: unknown kind '" + j.at("kind").get<std::string>() + "'");
    }

    out = Operation{};
    out.kind = kind;
    switch (kind) {
    case OperationKind::CreateNode:
        if (Error error = parse_uuid(j, "node_id", out.node_id); error) {
            return error;
        }
        if (Error error = parse_uuid(j, "parent_id", out.parent_id); error) {
            return error;
        }
        out.index = j.value("index", static_cast<usize>(0));
        out.name = j.value("name", std::string{});
        break;
    case OperationKind::DeleteNode:
        if (Error error = parse_uuid(j, "node_id", out.node_id); error) {
            return error;
        }
        break;
    case OperationKind::ReparentNode:
        if (Error error = parse_uuid(j, "node_id", out.node_id); error) {
            return error;
        }
        if (Error error = parse_uuid(j, "parent_id", out.parent_id); error) {
            return error;
        }
        out.index = j.value("index", static_cast<usize>(0));
        break;
    case OperationKind::RenameNode:
        if (Error error = parse_uuid(j, "node_id", out.node_id); error) {
            return error;
        }
        if (!j.contains("name") || !j.at("name").is_string()) {
            return Error("operation: rename_node requires a string 'name'");
        }
        out.name = j.at("name").get<std::string>();
        break;
    case OperationKind::AddComponent:
        if (Error error = parse_uuid(j, "node_id", out.node_id); error) {
            return error;
        }
        if (Error error = parse_uuid(j, "component_id", out.component_id); error) {
            return error;
        }
        if (!j.contains("type_name") || !j.at("type_name").is_string()) {
            return Error("operation: add_component requires a string 'type_name'");
        }
        out.name = j.at("type_name").get<std::string>();
        out.index = j.value("index", static_cast<usize>(0));
        break;
    case OperationKind::RemoveComponent:
        if (Error error = parse_uuid(j, "node_id", out.node_id); error) {
            return error;
        }
        if (Error error = parse_uuid(j, "component_id", out.component_id); error) {
            return error;
        }
        break;
    case OperationKind::SetProperty:
        if (Error error = parse_uuid(j, "node_id", out.node_id); error) {
            return error;
        }
        if (Error error = parse_uuid(j, "component_id", out.component_id); error) {
            return error;
        }
        if (!j.contains("path") || !j.at("path").is_array()) {
            return Error("operation: set_property requires a 'path' array");
        }
        out.path = j.at("path").get<std::vector<std::string>>();
        if (!j.contains("value")) {
            return Error("operation: set_property requires a 'value'");
        }
        if (Error error = value_from_json(j.at("value"), out.value); error) {
            return error;
        }
        break;
    case OperationKind::SetResource:
        if (Error error = parse_uuid(j, "node_id", out.node_id); error) {
            return error;
        }
        if (Error error = parse_uuid(j, "component_id", out.component_id); error) {
            return error;
        }
        if (!j.contains("property_name") || !j.at("property_name").is_string()) {
            return Error("operation: set_resource requires a string 'property_name'");
        }
        out.property_name = j.at("property_name").get<std::string>();
        out.reference = j.value("reference", std::string{});
        break;
    case OperationKind::SetScriptProperty:
        if (Error error = parse_uuid(j, "node_id", out.node_id); error) {
            return error;
        }
        if (!j.contains("behaviour") || !j.at("behaviour").is_string()) {
            return Error("operation: set_script_property requires a string 'behaviour'");
        }
        out.name = j.at("behaviour").get<std::string>();
        if (!j.contains("path") || !j.at("path").is_array()) {
            return Error("operation: set_script_property requires a 'path' array");
        }
        out.path = j.at("path").get<std::vector<std::string>>();
        if (!j.contains("value")) {
            return Error("operation: set_script_property requires a 'value'");
        }
        if (Error error = value_from_json(j.at("value"), out.value); error) {
            return error;
        }
        break;
    }
    return Error::none();
}

AppliedObjects::~AppliedObjects() = default;
AppliedObjects::AppliedObjects() = default;
AppliedObjects::AppliedObjects(AppliedObjects&&) noexcept = default;
AppliedObjects& AppliedObjects::operator=(AppliedObjects&&) noexcept = default;

Error apply(const Operation& op, Scene& scene, ResourceManager* resources, AppliedObjects* captured,
            ScriptTarget* scripts) {
    reflect::Registry& registry = reflect::Registry::instance();

    switch (op.kind) {
    case OperationKind::CreateNode: {
        if (scene.find_node(op.node_id) != nullptr) {
            // Already present: the operation is idempotent (a redo after an undo
            // that restored the same node).
            break;
        }
        Node* created = scene.create_node(op.name);
        created->set_id(op.node_id);
        attach_node(scene, scene.remove_root(created), op.parent_id, op.index);
        break;
    }
    case OperationKind::DeleteNode: {
        Node* node = scene.find_node(op.node_id);
        if (node == nullptr) {
            break;
        }
        const Uuid parent_id = node->parent() != nullptr ? node->parent()->id() : Uuid{};
        const usize index = sibling_index(scene, *node);
        std::unique_ptr<Node> owned = detach_node(scene, node);
        if (captured != nullptr) {
            captured->parent_id = parent_id;
            captured->index = index;
            captured->node = std::move(owned);
        }
        break;
    }
    case OperationKind::ReparentNode: {
        Node* node = scene.find_node(op.node_id);
        if (node == nullptr) {
            break;
        }
        attach_node(scene, detach_node(scene, node), op.parent_id, op.index);
        break;
    }
    case OperationKind::RenameNode: {
        if (Node* node = scene.find_node(op.node_id); node != nullptr) {
            node->set_name(op.name);
        }
        break;
    }
    case OperationKind::AddComponent: {
        Node* node = scene.find_node(op.node_id);
        if (node == nullptr || component_by_id(*node, op.component_id) != nullptr) {
            break;
        }
        std::unique_ptr<Component> component = registry.default_construct(op.name);
        if (component == nullptr) {
            break;
        }
        component->set_id(op.component_id);
        node->insert_component(op.index, std::move(component));
        break;
    }
    case OperationKind::RemoveComponent: {
        Node* node = scene.find_node(op.node_id);
        if (node == nullptr) {
            break;
        }
        const usize index = component_index_by_id(*node, op.component_id);
        if (index == static_cast<usize>(-1)) {
            break;
        }
        std::unique_ptr<Component> owned = node->detach_component(index);
        if (captured != nullptr) {
            captured->owner_id = op.node_id;
            captured->index = index;
            captured->component = std::move(owned);
        }
        break;
    }
    case OperationKind::SetProperty: {
        Node* node = scene.find_node(op.node_id);
        Component* component = node != nullptr ? component_by_id(*node, op.component_id) : nullptr;
        if (component == nullptr) {
            break;
        }
        const reflect::TypeDesc* type = registry.find(component->type_name());
        if (type == nullptr) {
            break;
        }
        const reflect::Property* property = resolve_component_property(*type, op.path);
        if (property == nullptr || !property->setter) {
            break;
        }
        (void)property->setter(component, op.value);
        break;
    }
    case OperationKind::SetResource: {
        if (resources == nullptr) {
            break;
        }
        Node* node = scene.find_node(op.node_id);
        Component* component = node != nullptr ? component_by_id(*node, op.component_id) : nullptr;
        if (component == nullptr) {
            break;
        }
        const reflect::TypeDesc* type = registry.find(component->type_name());
        if (type == nullptr) {
            break;
        }
        for (const reflect::Property& property : type->properties) {
            if (property.name == op.property_name && property.value_kind == reflect::ValueKind::ResourceRef) {
                if (property.resolve_resource) {
                    (void)property.resolve_resource(component, op.reference, *resources);
                }
                break;
            }
        }
        break;
    }
    case OperationKind::SetScriptProperty: {
        // Scripts are not components: they live in a host reached through the
        // ScriptTarget seam. A missing target, a node that is gone, an unscripted
        // node, or a node whose behaviour changed since the edit was recorded all
        // make this a no-op, matching the component policy.
        if (scripts == nullptr || scene.find_node(op.node_id) == nullptr) {
            break;
        }
        if (scripts->behaviour_of(op.node_id) != op.name) {
            break;
        }
        (void)scripts->write(op.node_id, op.path, op.value);
        break;
    }
    }
    return Error::none();
}

Result<Operation> inverse(const Operation& op, const Scene& scene, ScriptTarget* scripts) {
    reflect::Registry& registry = reflect::Registry::instance();

    switch (op.kind) {
    case OperationKind::CreateNode: {
        Operation inv;
        inv.kind = OperationKind::DeleteNode;
        inv.node_id = op.node_id;
        return inv;
    }
    case OperationKind::DeleteNode:
        return Error("operation: a deleted node's inverse is the captured object, not an operation");
    case OperationKind::ReparentNode: {
        const Node* node = scene.find_node(op.node_id);
        if (node == nullptr) {
            return Error("operation: cannot invert a reparent of a missing node");
        }
        Operation inv;
        inv.kind = OperationKind::ReparentNode;
        inv.node_id = op.node_id;
        inv.parent_id = node->parent() != nullptr ? node->parent()->id() : Uuid{};
        inv.index = sibling_index(scene, *node);
        return inv;
    }
    case OperationKind::RenameNode: {
        const Node* node = scene.find_node(op.node_id);
        if (node == nullptr) {
            return Error("operation: cannot invert a rename of a missing node");
        }
        Operation inv;
        inv.kind = OperationKind::RenameNode;
        inv.node_id = op.node_id;
        inv.name = node->name();
        return inv;
    }
    case OperationKind::AddComponent: {
        Operation inv;
        inv.kind = OperationKind::RemoveComponent;
        inv.node_id = op.node_id;
        inv.component_id = op.component_id;
        return inv;
    }
    case OperationKind::RemoveComponent:
        return Error("operation: a removed component's inverse is the captured object, not an operation");
    case OperationKind::SetProperty: {
        const Node* node = scene.find_node(op.node_id);
        Component* component = nullptr;
        if (node != nullptr) {
            for (const auto& owned : node->components()) {
                if (owned->id() == op.component_id) {
                    component = owned.get();
                    break;
                }
            }
        }
        if (component == nullptr) {
            return Error("operation: cannot invert a property write on a missing component");
        }
        const reflect::TypeDesc* type = registry.find(component->type_name());
        if (type == nullptr) {
            return Error("operation: cannot invert a property write of an unregistered type");
        }
        const reflect::Property* property = resolve_component_property(*type, op.path);
        if (property == nullptr || !property->getter) {
            return Error("operation: cannot invert a property write with no getter");
        }
        Operation inv;
        inv.kind = OperationKind::SetProperty;
        inv.node_id = op.node_id;
        inv.component_id = op.component_id;
        inv.path = op.path;
        inv.value = property->getter(component);
        return inv;
    }
    case OperationKind::SetResource: {
        const Node* node = scene.find_node(op.node_id);
        Component* component = nullptr;
        if (node != nullptr) {
            for (const auto& owned : node->components()) {
                if (owned->id() == op.component_id) {
                    component = owned.get();
                    break;
                }
            }
        }
        if (component == nullptr) {
            return Error("operation: cannot invert a resource write on a missing component");
        }
        const reflect::TypeDesc* type = registry.find(component->type_name());
        if (type == nullptr) {
            return Error("operation: cannot invert a resource write of an unregistered type");
        }
        for (const reflect::Property& property : type->properties) {
            if (property.name == op.property_name && property.value_kind == reflect::ValueKind::ResourceRef) {
                Operation inv;
                inv.kind = OperationKind::SetResource;
                inv.node_id = op.node_id;
                inv.component_id = op.component_id;
                inv.property_name = op.property_name;
                inv.reference = property.resource_ref ? property.resource_ref(component) : std::string{};
                return inv;
            }
        }
        return Error("operation: cannot invert a resource write on a non-resource property");
    }
    case OperationKind::SetScriptProperty: {
        // The inverse reads the script's current value, so it needs the seam. A
        // caller that applies a script edit without one (a headless replay) can
        // apply the edit but not invert it, which is exactly the component case
        // when a target is missing.
        if (scripts == nullptr) {
            return Error("operation: cannot invert a script property write without a script target");
        }
        if (scripts->behaviour_of(op.node_id) != op.name) {
            return Error("operation: cannot invert a script property write on a node that runs another behaviour");
        }
        std::optional<reflect::Value> previous = scripts->read(op.node_id, op.path);
        if (!previous.has_value()) {
            return Error("operation: cannot invert a script property write with no readable value");
        }
        Operation inv;
        inv.kind = OperationKind::SetScriptProperty;
        inv.node_id = op.node_id;
        inv.name = op.name;
        inv.path = op.path;
        inv.value = std::move(previous.value());
        return inv;
    }
    }
    return Error("operation: unknown kind");
}
Error restore(AppliedObjects& captured, Scene& scene) {
    if (captured.component != nullptr) {
        Node* owner = scene.find_node(captured.owner_id);
        if (owner == nullptr) {
            return Error("operation: cannot restore a component whose node is gone");
        }
        // A component with this id already present means the restore is a
        // repeat; treat it as done rather than inserting a duplicate.
        if (scene.find_component(captured.component->id()) == nullptr) {
            owner->insert_component(captured.index, std::move(captured.component));
        }
        return Error::none();
    }
    if (captured.node != nullptr) {
        attach_node(scene, std::move(captured.node), captured.parent_id, captured.index);
        return Error::none();
    }
    return Error("operation: nothing captured to restore");
}

} // namespace fe::serialization
