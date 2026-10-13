// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Types.h"
#include "fe/core/Uuid.h"
#include "fe/editor/Command.h"
#include "fe/reflect/Value.h"

#include <memory>
#include <string>
#include <vector>

namespace fe {

class Node;
class Scene;
class Component;

namespace editor {

// The concrete commands the editor's panels perform. Every one addresses its
// scene objects by `Uuid` (never a raw pointer) and holds whatever it removed,
// so `undo()` restores the exact prior state - including sibling order and
// object identity. See docs/adr/0015-editor-undo-redo.md.

// Creates a node named `name` under `parent_id` (a nil id means a scene root)
// at sibling position `index`. On the first execute the node is created; on redo
// the same node instance is reattached, so its `Uuid` (and any children or
// components added to it before the undo) is preserved.
class CreateNodeCommand final : public Command {
public:
    CreateNodeCommand(std::string name, Uuid parent_id, usize index);
    ~CreateNodeCommand() override;

    void execute(Scene& scene) override;
    void undo(Scene& scene) override;
    [[nodiscard]] std::string_view label() const noexcept override { return "Create Node"; }

    // The id of the node, valid once the command has been executed. Lets the
    // caller select the new node.
    [[nodiscard]] const Uuid& node_id() const noexcept { return node_id_; }

private:
    std::string name_;
    Uuid parent_id_;
    usize index_;
    std::unique_ptr<Node> detached_;
    Uuid node_id_{};
};

// Deletes a node (and its subtree). The command takes ownership while undone,
// so undo reattaches the identical subtree at its original parent and index.
class DeleteNodeCommand final : public Command {
public:
    explicit DeleteNodeCommand(const Uuid& node_id);
    ~DeleteNodeCommand() override;

    void execute(Scene& scene) override;
    void undo(Scene& scene) override;
    [[nodiscard]] std::string_view label() const noexcept override { return "Delete Node"; }

private:
    Uuid node_id_;
    Uuid parent_id_{};
    usize index_ = 0;
    std::unique_ptr<Node> detached_;
};

// Moves a node under a new parent (a nil id means a scene root) at `index`.
// Undo returns it to the parent and index it came from.
class ReparentNodeCommand final : public Command {
public:
    ReparentNodeCommand(const Uuid& node_id, const Uuid& new_parent_id, usize new_index);

    void execute(Scene& scene) override;
    void undo(Scene& scene) override;
    [[nodiscard]] std::string_view label() const noexcept override { return "Reparent Node"; }

private:
    Uuid node_id_;
    Uuid new_parent_id_;
    usize new_index_;
    Uuid old_parent_id_{};
    usize old_index_ = 0;
    bool captured_ = false;
};

// Renames a node.
class RenameNodeCommand final : public Command {
public:
    RenameNodeCommand(const Uuid& node_id, std::string new_name);

    void execute(Scene& scene) override;
    void undo(Scene& scene) override;
    [[nodiscard]] std::string_view label() const noexcept override { return "Rename Node"; }

private:
    Uuid node_id_;
    std::string new_name_;
    std::string old_name_;
    bool captured_ = false;
};

// Adds a component of a registered type to a node at `index`. The component is
// default-constructed through `reflect::Registry`, so this names no component
// type. Undo removes it and keeps it, so redo restores the identical component.
class AddComponentCommand final : public Command {
public:
    AddComponentCommand(const Uuid& node_id, std::string type_name, usize index);
    ~AddComponentCommand() override;

    void execute(Scene& scene) override;
    void undo(Scene& scene) override;
    [[nodiscard]] std::string_view label() const noexcept override { return "Add Component"; }

private:
    Uuid node_id_;
    std::string type_name_;
    usize index_;
    std::unique_ptr<Component> detached_;
};

// Removes the component at `index` from a node. Undo reinserts it at the same
// index, so the node's component order is exactly restored.
class RemoveComponentCommand final : public Command {
public:
    RemoveComponentCommand(const Uuid& node_id, usize index);
    ~RemoveComponentCommand() override;

    void execute(Scene& scene) override;
    void undo(Scene& scene) override;
    [[nodiscard]] std::string_view label() const noexcept override { return "Remove Component"; }

private:
    Uuid node_id_;
    usize index_;
    std::unique_ptr<Component> detached_;
};

// Sets one reflected property of one component, resolved generically through
// `reflect::Registry`. `path` names the property within the component's
// `TypeDesc`: a single name for a leaf ("layer"), or a member path for a group
// ("color", "r"). This one class covers every reflected field of every
// component type - adding a reflected property makes it undoable with no new
// command.
//
// Consecutive edits of the same property coalesce: the key is the target plus
// the path, and merging adopts the newest value while keeping the original old
// value, so one undo reverts a whole drag.
class SetPropertyCommand final : public Command {
public:
    SetPropertyCommand(Uuid node_id, usize component_index, std::vector<std::string> path, reflect::Value value);

    void execute(Scene& scene) override;
    void undo(Scene& scene) override;
    [[nodiscard]] std::string_view label() const noexcept override { return "Set Property"; }

    [[nodiscard]] std::string coalesce_key() const override;
    [[nodiscard]] bool merge_with(const Command& newer) override;

private:
    Uuid node_id_;
    usize component_index_;
    std::vector<std::string> path_;
    reflect::Value new_value_;
    reflect::Value old_value_{};
    bool captured_ = false;
};

} // namespace editor
} // namespace fe
