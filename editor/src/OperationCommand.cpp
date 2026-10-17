// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/editor/OperationCommand.h"

#include "fe/scene/Component.h"
#include "fe/scene/Node.h"
#include "fe/scene/Scene.h"

#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace fe::editor {
namespace {

using serialization::Operation;
using serialization::OperationKind;

// The two families whose forward and inverse edits move an *object* between the
// scene and the command. A create makes one, a delete removes one; undoing a
// create removes the object (and it must be kept to redo), undoing a delete
// restores the removed object. The remaining kinds only rewrite state in place,
// so their inverse is a plain operation.
[[nodiscard]] bool is_create_like(OperationKind kind) noexcept {
    return kind == OperationKind::CreateNode || kind == OperationKind::AddComponent;
}

[[nodiscard]] bool is_delete_like(OperationKind kind) noexcept {
    return kind == OperationKind::DeleteNode || kind == OperationKind::RemoveComponent;
}

void attach_node(Scene& scene, std::unique_ptr<Node> node, const Uuid& parent_id, usize index) {
    if (node == nullptr) {
        return;
    }
    if (Node* parent = parent_id.is_nil() ? nullptr : scene.find_node(parent_id); parent != nullptr) {
        parent->add_child(std::move(node), index);
    } else {
        scene.add_root(std::move(node), index);
    }
}

// Puts back an object a previous step removed, at the position it was removed
// from, so identity and order are preserved.
void restore_captured(Scene& scene, const Operation& op, serialization::AppliedObjects& captured) {
    if (captured.node != nullptr) {
        attach_node(scene, std::move(captured.node), captured.parent_id, captured.index);
        return;
    }
    if (captured.component != nullptr) {
        if (Node* node = scene.find_node(op.node_id); node != nullptr) {
            node->insert_component(captured.index, std::move(captured.component));
        }
    }
}

} // namespace

OperationCommand::OperationCommand(Operation operation, ResourceManager* resources,
                                   serialization::ScriptTarget* scripts)
    : operation_(std::move(operation)), resources_(resources), scripts_(scripts) {}

OperationCommand::~OperationCommand() = default;

void OperationCommand::execute(Scene& scene) {
    // Redo of a create/delete: the object is held, not in the scene. Put it back.
    if (has_capture_) {
        restore_captured(scene, operation_, captured_);
        has_capture_ = false;
        return;
    }

    if (!is_create_like(operation_.kind) && !is_delete_like(operation_.kind) && !has_inverse_) {
        // A modify: its inverse is read from the document once, before the first
        // forward apply, so undo restores the state this edit started from.
        if (Result<Operation> inv = serialization::inverse(operation_, scene, scripts_); inv) {
            inverse_ = inv.value();
            has_inverse_ = true;
        }
    }

    if (is_delete_like(operation_.kind)) {
        // The forward edit removes an object; keep it so undo can restore it.
        (void)serialization::apply(operation_, scene, resources_, &captured_, scripts_);
    } else {
        (void)serialization::apply(operation_, scene, resources_, nullptr, scripts_);
    }
}

void OperationCommand::undo(Scene& scene) {
    if (is_create_like(operation_.kind)) {
        // Undoing a create removes the object it made; keep it to redo.
        if (Result<Operation> inv = serialization::inverse(operation_, scene, scripts_); inv) {
            (void)serialization::apply(inv.value(), scene, resources_, &captured_, scripts_);
            has_capture_ = true;
        }
        return;
    }
    if (is_delete_like(operation_.kind)) {
        restore_captured(scene, operation_, captured_);
        has_capture_ = false;
        return;
    }
    if (has_inverse_) {
        (void)serialization::apply(inverse_, scene, resources_, nullptr, scripts_);
    }
}

std::string_view OperationCommand::label() const noexcept {
    switch (operation_.kind) {
    case OperationKind::CreateNode:
        return "Create Node";
    case OperationKind::DeleteNode:
        return "Delete Node";
    case OperationKind::ReparentNode:
        return "Reparent Node";
    case OperationKind::RenameNode:
        return "Rename Node";
    case OperationKind::AddComponent:
        return "Add Component";
    case OperationKind::RemoveComponent:
        return "Remove Component";
    case OperationKind::SetProperty:
        return "Set Property";
    case OperationKind::SetResource:
        return "Set Resource";
    case OperationKind::SetScriptProperty:
        return "Set Script Property";
    }
    return "Edit";
}

std::string OperationCommand::coalesce_key() const {
    // Only the continuous edits coalesce: a drag that changes one property or one
    // resource slot many times. The key names the target by id and the property,
    // so two different gestures never merge.
    switch (operation_.kind) {
    case OperationKind::SetProperty: {
        std::string key = operation_.node_id.to_string();
        key += '#';
        key += operation_.component_id.to_string();
        key += ':';
        for (usize i = 0; i < operation_.path.size(); ++i) {
            if (i > 0) {
                key += '.';
            }
            key += operation_.path[i];
        }
        return key;
    }
    case OperationKind::SetResource: {
        std::string key = operation_.node_id.to_string();
        key += '#';
        key += operation_.component_id.to_string();
        key += ':';
        key += operation_.property_name;
        return key;
    }
    case OperationKind::SetScriptProperty: {
        // A script has no component id; the behaviour name plus the path is what
        // identifies the field, so two gestures on the same script property merge
        // and two different scripts never do.
        std::string key = operation_.node_id.to_string();
        key += '@';
        key += operation_.name;
        key += ':';
        for (usize i = 0; i < operation_.path.size(); ++i) {
            if (i > 0) {
                key += '.';
            }
            key += operation_.path[i];
        }
        return key;
    }
    default:
        return {};
    }
}

bool OperationCommand::merge_with(const Command& newer) {
    const auto* other = dynamic_cast<const OperationCommand*>(&newer);
    if (other == nullptr || other->coalesce_key() != coalesce_key()) {
        return false;
    }
    // Adopt the newest value; keep the original inverse, so one undo reverts the
    // whole gesture. The stack re-executes this command after a merge, which
    // applies the adopted value.
    operation_.value = other->operation_.value;
    operation_.reference = other->operation_.reference;
    return true;
}

} // namespace fe::editor
