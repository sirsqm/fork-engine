// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/editor/Commands.h"

#include "fe/reflect/Registry.h"
#include "fe/reflect/TypeDesc.h"
#include "fe/resources/ResourceManager.h"
#include "fe/scene/Component.h"
#include "fe/scene/Node.h"
#include "fe/scene/Scene.h"

#include <utility>

namespace fe::editor {
namespace {

// The index of `node` among its parent's children, or among the scene's roots
// when it is a root. Returns 0 when the node is not found, which only happens
// for a node that has already been detached.
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

// Detaches `node` from its parent, or from the scene when it is a root, and
// returns ownership. Returns nullptr when the node is already detached.
[[nodiscard]] std::unique_ptr<Node> detach_node(Scene& scene, Node* node) {
    if (node == nullptr) {
        return nullptr;
    }
    if (Node* parent = node->parent(); parent != nullptr) {
        return parent->remove_child(node);
    }
    return scene.remove_root(node);
}

// Attaches `node` under `parent_id` (a nil id means the scene's roots) at
// `index`.
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

// The property named by `path` within `type`: a single name for a leaf, or a
// member path for a group ("color", "r"). Returns nullptr when the path does not
// resolve to a property with a setter.
[[nodiscard]] const reflect::Property* resolve_property(const reflect::TypeDesc& type,
                                                        const std::vector<std::string>& path) {
    const reflect::Property* current = nullptr;
    const std::vector<reflect::Property>* level = &type.properties;
    for (const std::string& name : path) {
        current = nullptr;
        for (const reflect::Property& candidate : *level) {
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

// The component at `index` on `node`, or nullptr when out of range.
[[nodiscard]] Component* component_at(Node& node, usize index) noexcept {
    if (index >= node.component_count()) {
        return nullptr;
    }
    return node.components()[index].get();
}

} // namespace

// ---------------------------------------------------------------------------
// CreateNodeCommand
// ---------------------------------------------------------------------------

CreateNodeCommand::CreateNodeCommand(std::string name, Uuid parent_id, usize index)
    : name_(std::move(name)), parent_id_(parent_id), index_(index) {}

// Out-of-line so the unique_ptr<Node> member sees the complete Node type.
CreateNodeCommand::~CreateNodeCommand() = default;

void CreateNodeCommand::execute(Scene& scene) {
    if (detached_ == nullptr) {
        // Scene owns the node constructor; create it as a root, take it back,
        // then place it where it belongs. This is the only way to build a plain
        // Node without naming the protected constructor.
        Node* created = scene.create_node(name_);
        node_id_ = created->id();
        detached_ = scene.remove_root(created);
    }
    attach_node(scene, std::move(detached_), parent_id_, index_);
}

void CreateNodeCommand::undo(Scene& scene) {
    Node* node = scene.find_node(node_id_);
    if (node == nullptr) {
        return;
    }
    detached_ = detach_node(scene, node);
}

// ---------------------------------------------------------------------------
// DeleteNodeCommand
// ---------------------------------------------------------------------------

DeleteNodeCommand::DeleteNodeCommand(const Uuid& node_id) : node_id_(node_id) {}

DeleteNodeCommand::~DeleteNodeCommand() = default;

void DeleteNodeCommand::execute(Scene& scene) {
    Node* node = scene.find_node(node_id_);
    if (node == nullptr) {
        return;
    }
    parent_id_ = node->parent() != nullptr ? node->parent()->id() : Uuid{};
    index_ = sibling_index(scene, *node);
    detached_ = detach_node(scene, node);
}

void DeleteNodeCommand::undo(Scene& scene) {
    attach_node(scene, std::move(detached_), parent_id_, index_);
}

// ---------------------------------------------------------------------------
// ReparentNodeCommand
// ---------------------------------------------------------------------------

ReparentNodeCommand::ReparentNodeCommand(const Uuid& node_id, const Uuid& new_parent_id, usize new_index)
    : node_id_(node_id), new_parent_id_(new_parent_id), new_index_(new_index) {}

void ReparentNodeCommand::execute(Scene& scene) {
    Node* node = scene.find_node(node_id_);
    if (node == nullptr) {
        return;
    }
    if (!captured_) {
        old_parent_id_ = node->parent() != nullptr ? node->parent()->id() : Uuid{};
        old_index_ = sibling_index(scene, *node);
        captured_ = true;
    }
    std::unique_ptr<Node> owned = detach_node(scene, node);
    attach_node(scene, std::move(owned), new_parent_id_, new_index_);
}

void ReparentNodeCommand::undo(Scene& scene) {
    Node* node = scene.find_node(node_id_);
    if (node == nullptr) {
        return;
    }
    std::unique_ptr<Node> owned = detach_node(scene, node);
    attach_node(scene, std::move(owned), old_parent_id_, old_index_);
}

// ---------------------------------------------------------------------------
// RenameNodeCommand
// ---------------------------------------------------------------------------

RenameNodeCommand::RenameNodeCommand(const Uuid& node_id, std::string new_name)
    : node_id_(node_id), new_name_(std::move(new_name)) {}

void RenameNodeCommand::execute(Scene& scene) {
    Node* node = scene.find_node(node_id_);
    if (node == nullptr) {
        return;
    }
    if (!captured_) {
        old_name_ = node->name();
        captured_ = true;
    }
    node->set_name(new_name_);
}

void RenameNodeCommand::undo(Scene& scene) {
    Node* node = scene.find_node(node_id_);
    if (node != nullptr) {
        node->set_name(old_name_);
    }
}

// ---------------------------------------------------------------------------
// AddComponentCommand
// ---------------------------------------------------------------------------

AddComponentCommand::AddComponentCommand(const Uuid& node_id, std::string type_name, usize index)
    : node_id_(node_id), type_name_(std::move(type_name)), index_(index) {}

AddComponentCommand::~AddComponentCommand() = default;

void AddComponentCommand::execute(Scene& scene) {
    Node* node = scene.find_node(node_id_);
    if (node == nullptr) {
        return;
    }
    if (detached_ == nullptr) {
        // Build through the registry so the command names no component type.
        detached_ = reflect::Registry::instance().default_construct(type_name_);
        if (detached_ == nullptr) {
            return;
        }
    }
    node->insert_component(index_, std::move(detached_));
}

void AddComponentCommand::undo(Scene& scene) {
    Node* node = scene.find_node(node_id_);
    if (node == nullptr) {
        return;
    }
    detached_ = node->detach_component(index_);
}

// ---------------------------------------------------------------------------
// RemoveComponentCommand
// ---------------------------------------------------------------------------

RemoveComponentCommand::RemoveComponentCommand(const Uuid& node_id, usize index) : node_id_(node_id), index_(index) {}

RemoveComponentCommand::~RemoveComponentCommand() = default;

void RemoveComponentCommand::execute(Scene& scene) {
    Node* node = scene.find_node(node_id_);
    if (node == nullptr) {
        return;
    }
    detached_ = node->detach_component(index_);
}

void RemoveComponentCommand::undo(Scene& scene) {
    Node* node = scene.find_node(node_id_);
    if (node == nullptr) {
        return;
    }
    node->insert_component(index_, std::move(detached_));
}

// ---------------------------------------------------------------------------
// SetPropertyCommand
// ---------------------------------------------------------------------------

SetPropertyCommand::SetPropertyCommand(Uuid node_id, usize component_index, std::vector<std::string> path,
                                       reflect::Value value)
    : node_id_(node_id), component_index_(component_index), path_(std::move(path)), new_value_(std::move(value)) {}

std::string SetPropertyCommand::coalesce_key() const {
    std::string key = node_id_.to_string();
    key += '#';
    key += std::to_string(component_index_);
    key += ':';
    for (usize i = 0; i < path_.size(); ++i) {
        if (i > 0) {
            key += '.';
        }
        key += path_[i];
    }
    return key;
}

bool SetPropertyCommand::merge_with(const Command& newer) {
    const auto* other = dynamic_cast<const SetPropertyCommand*>(&newer);
    if (other == nullptr || other->coalesce_key() != coalesce_key()) {
        return false;
    }
    // Adopt the newest value; keep the original old value, so one undo reverts
    // the entire gesture.
    new_value_ = other->new_value_;
    return true;
}

namespace {

// Applies `value` to the property at `path` on the command's target. Shared by
// execute() and undo() so the two can never diverge.
[[nodiscard]] bool write_property(Scene& scene, const Uuid& node_id, usize component_index,
                                  const std::vector<std::string>& path, const reflect::Value& value,
                                  reflect::Value* out_previous) {
    Node* node = scene.find_node(node_id);
    if (node == nullptr) {
        return false;
    }
    Component* component = component_at(*node, component_index);
    if (component == nullptr) {
        return false;
    }
    const reflect::TypeDesc* type = reflect::Registry::instance().find(component->type_name());
    if (type == nullptr) {
        return false;
    }
    const reflect::Property* property = resolve_property(*type, path);
    if (property == nullptr || !property->setter || !property->getter) {
        return false;
    }
    if (out_previous != nullptr) {
        *out_previous = property->getter(component);
    }
    return property->setter(component, value);
}

} // namespace

void SetPropertyCommand::execute(Scene& scene) {
    reflect::Value previous{};
    if (write_property(scene, node_id_, component_index_, path_, new_value_, &previous) && !captured_) {
        // Record the value to restore. Captured once, on the first execute, so a
        // coalesced drag still undoes to the value before the drag began.
        old_value_ = std::move(previous);
        captured_ = true;
    }
}

void SetPropertyCommand::undo(Scene& scene) {
    if (!captured_) {
        return;
    }
    reflect::Value ignored{};
    // The previous value is not needed on undo; a failure to write means the
    // target vanished and there is nothing to restore.
    (void)write_property(scene, node_id_, component_index_, path_, old_value_, &ignored);
}

// ---------------------------------------------------------------------------
// SetResourceCommand
// ---------------------------------------------------------------------------

SetResourceCommand::SetResourceCommand(Uuid node_id, usize component_index, std::string property_name,
                                       std::string reference, ResourceManager* resources)
    : node_id_(node_id), component_index_(component_index), property_name_(std::move(property_name)),
      reference_(std::move(reference)), resources_(resources) {}

std::string SetResourceCommand::coalesce_key() const {
    std::string key = node_id_.to_string();
    key += '#';
    key += std::to_string(component_index_);
    key += ':';
    key += property_name_;
    return key;
}

bool SetResourceCommand::merge_with(const Command& newer) {
    const auto* other = dynamic_cast<const SetResourceCommand*>(&newer);
    if (other == nullptr || other->coalesce_key() != coalesce_key()) {
        return false;
    }
    reference_ = other->reference_;
    return true;
}

namespace {

// The resource-reference property named `property_name` on the component at
// `index`, or nullptr when the target or the property does not exist. A property
// without a resolver (a value that is not a resource reference) is also rejected.
[[nodiscard]] const reflect::Property* resource_property(Scene& scene, const Uuid& node_id, usize component_index,
                                                         std::string_view property_name) {
    Node* node = scene.find_node(node_id);
    if (node == nullptr) {
        return nullptr;
    }
    Component* component = component_at(*node, component_index);
    if (component == nullptr) {
        return nullptr;
    }
    const reflect::TypeDesc* type = reflect::Registry::instance().find(component->type_name());
    if (type == nullptr) {
        return nullptr;
    }
    for (const reflect::Property& property : type->properties) {
        if (property.name == property_name && property.value_kind == reflect::ValueKind::ResourceRef) {
            return &property;
        }
    }
    return nullptr;
}

// Reads the current reference, resolves `reference` through the resource manager,
// and stores the result. Shared by execute() and undo() so they cannot diverge.
// An empty reference clears the slot. Returns false (without mutating) when the
// target is gone or the reference cannot be resolved.
[[nodiscard]] bool write_resource(Scene& scene, ResourceManager& resources, const Uuid& node_id, usize component_index,
                                  const std::string& property_name, const std::string& reference,
                                  std::string* out_previous) {
    Node* node = scene.find_node(node_id);
    if (node == nullptr) {
        return false;
    }
    Component* component = component_at(*node, component_index);
    if (component == nullptr) {
        return false;
    }
    const reflect::Property* property = resource_property(scene, node_id, component_index, property_name);
    if (property == nullptr || !property->resource_ref || !property->resolve_resource) {
        return false;
    }
    if (out_previous != nullptr) {
        *out_previous = property->resource_ref(component);
    }
    // A resolve failure returns nullptr and stores nothing, so a bad reference
    // never clobbers a good one. An empty reference is the explicit clear.
    (void)property->resolve_resource(component, reference, resources);
    return true;
}

} // namespace

void SetResourceCommand::execute(Scene& scene) {
    // The manager is reached through the scene's engine-independent seam: the
    // command carries only a reference, and the panel that builds it supplies
    // the manager via the document. A command with no manager cannot resolve, so
    // it is a no-op rather than a half-applied edit.
    if (resources_ == nullptr) {
        return;
    }
    std::string previous;
    if (write_resource(scene, *resources_, node_id_, component_index_, property_name_, reference_, &previous) &&
        !captured_) {
        old_reference_ = std::move(previous);
        captured_ = true;
    }
}

void SetResourceCommand::undo(Scene& scene) {
    if (!captured_ || resources_ == nullptr) {
        return;
    }
    std::string ignored;
    (void)write_resource(scene, *resources_, node_id_, component_index_, property_name_, old_reference_, &ignored);
}

} // namespace fe::editor
