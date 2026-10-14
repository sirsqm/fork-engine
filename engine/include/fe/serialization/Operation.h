// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Error.h"
#include "fe/core/Types.h"
#include "fe/core/Uuid.h"
#include "fe/reflect/Value.h"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace fe {

class Node;
class Component;
class Scene;
class ResourceManager;

namespace serialization {

// A serializable edit to a scene: the transmitted, replayable form of an editor
// command.
//
// Where a `Command` (docs/adr/0015-editor-undo-redo.md) is a live object with
// captured undo state, an `Operation` is a small tagged value that names its
// objects by stable id, round-trips through JSON, and can be applied to any
// `Scene`. It is the unit the collaborative-editing log of
// docs/adr/0019-collaborative-editing.md carries, and it strengthens local
// undo/redo on its own because an operation's inverse is derived from the
// document rather than stashed in the editor.
//
// See docs/adr/0047-local-operation-model.md.
enum class OperationKind : u8 {
    CreateNode,      // make a node with a known id under a parent at an index
    DeleteNode,      // remove a node and its subtree
    ReparentNode,    // move a node under a new parent at an index
    RenameNode,      // set a node's name
    AddComponent,    // attach a default-constructed component with a known id
    RemoveComponent, // detach a component
    SetProperty,     // write one reflected property, resolved through the registry
    SetResource,     // set a resource-reference property through the manager
};

// The wire name of a kind ("create_node", ...). Stable; the reader rejects an
// unknown name rather than guessing.
[[nodiscard]] const char* to_string(OperationKind kind) noexcept;

// Parses a wire name back to a kind. Returns false on an unknown name.
[[nodiscard]] bool operation_kind_from_string(std::string_view text, OperationKind& out) noexcept;

// One edit. Only the fields its `kind` uses are meaningful; the rest are
// default. Every object is named by `Uuid`, never by pointer or index, so an
// operation resolves to the same object on any replica that shares it.
struct Operation {
    OperationKind kind = OperationKind::RenameNode;

    // The target node, for every kind that names one.
    Uuid node_id{};
    // CreateNode/ReparentNode: the destination parent (nil means the scene roots).
    Uuid parent_id{};
    // AddComponent/RemoveComponent/SetProperty/SetResource: the target component.
    Uuid component_id{};
    // CreateNode/ReparentNode/AddComponent: the sibling or component position.
    // Position is part of the intent (where the user dropped it) even though
    // identity is not positional; it is clamped on apply.
    usize index = 0;

    // CreateNode/RenameNode: the node name. AddComponent: the registered type name.
    std::string name;
    // SetProperty/SetResource: the property name (a single name for a leaf).
    std::string property_name;
    // SetProperty: the path within the component's TypeDesc, a single name for a
    // leaf or a member path for a group ("color", "r").
    std::vector<std::string> path;
    // SetProperty: the value to write.
    reflect::Value value{};
    // SetResource: the reference to resolve ("" clears the slot).
    std::string reference;
};

// Serializes `op` to a JSON object string with a "kind" discriminator.
[[nodiscard]] std::string to_json(const Operation& op);

// Parses an operation from a JSON object string. Strict: an unknown kind or a
// missing/malformed field is an error, not a silently dropped edit.
[[nodiscard]] Error from_json(std::string_view text, Operation& out);

// The objects an apply removed, so an undo can restore them with identity
// intact. A delete-like operation (DeleteNode, RemoveComponent) hands the
// removed object back here when the caller supplies a sink; a caller that
// supplies none (a log replay) lets it be destroyed.
struct AppliedObjects {
    // Out-of-line so a translation unit that only forward-declares Node and
    // Component can still hold an AppliedObjects by value.
    ~AppliedObjects();
    AppliedObjects();
    AppliedObjects(AppliedObjects&&) noexcept;
    AppliedObjects& operator=(AppliedObjects&&) noexcept;
    AppliedObjects(const AppliedObjects&) = delete;
    AppliedObjects& operator=(const AppliedObjects&) = delete;

    std::unique_ptr<Node> node;
    std::unique_ptr<Component> component;
    // DeleteNode: where the node came from, so it can be restored in place
    // (nil means the scene roots).
    Uuid parent_id{};
    // RemoveComponent: the node the component was detached from, so `restore`
    // can find it again.
    Uuid owner_id{};
    usize index = 0;
};

// Performs `op` on `scene`. `resources` may be null; a SetResource against a
// null manager is a no-op, matching the headless case. An operation whose
// target is absent does nothing (no error): the local editor has no other
// replica to be inconsistent with, and this matches the commands it replaces.
//
// When `captured` is non-null, a delete-like operation stores the removed
// object there instead of destroying it.
[[nodiscard]] Error apply(const Operation& op, Scene& scene, ResourceManager* resources = nullptr,
                          AppliedObjects* captured = nullptr);

// The operation that undoes `op` against the current `scene`, computed from the
// document rather than from any captured state. Returns an error for
// `DeleteNode` and `RemoveComponent`: their inverse is the object they removed,
// which is not a single operation, so the caller holds it (see `AppliedObjects`).
[[nodiscard]] Result<Operation> inverse(const Operation& op, const Scene& scene);

// Re-attaches the object `captured` holds, undoing a `DeleteNode` (its node) or
// a `RemoveComponent` (its component). The counterpart of the capture sink: an
// undo stack uses it to restore a removed object with identity intact.
//
// A `DeleteNode`'s node may carry child nodes of its own; the caller is
// responsible for restoring the rest of the subtree (its children were captured
// by the delete of each descendant). `index` is clamped to the parent's child
// count. Returns an error when `captured` holds nothing to restore.
[[nodiscard]] Error restore(AppliedObjects& captured, Scene& scene);

} // namespace serialization
} // namespace fe
