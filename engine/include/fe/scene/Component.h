// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include <memory>
#include <typeinfo>
#include <vector>

namespace fe {

class Node;

// Attachable data or behaviour that extends a Node.
//
// The engine's composition model is deliberately hybrid:
//
//   * A Node supplies identity, a name, a transform, and hierarchy. It is the
//     unit the editor manipulates and the serializer persists.
//   * A Component supplies a capability (a sprite, a camera, a collider). It
//     carries data and, when needed, logic, but it does not own hierarchy.
//
// This split keeps a single, predictable answer to "where does the transform
// live?" while still allowing arbitrary composition, and it lets systems read
// the data they care about without the hierarchy dictating how they execute.
//
// Components are owned by their Node and are move-only. Create them with
// Node::add_component<T>().
class Component {
public:
    virtual ~Component();

    Component(const Component&) = delete;
    Component& operator=(const Component&) = delete;
    Component(Component&&) = delete;
    Component& operator=(Component&&) = delete;

    // The registered type name used by the serializer and the inspector.
    [[nodiscard]] virtual const char* type_name() const noexcept = 0;

    // The owning node, or nullptr while detached.
    [[nodiscard]] Node* node() const noexcept { return node_; }

    // Runtime type tag. Comparing these is faster than a dynamic_cast and is
    // what Node::get_component<T>() uses.
    [[nodiscard]] const std::type_info& type_info() const noexcept { return typeid(*this); }

protected:
    Component() = default;

    // Called when the component is added to a node that is already in a scene.
    virtual void on_attach(Node& node) { (void)node; }
    // Called when the component is removed, before the node forgets it.
    virtual void on_detach(Node& node) { (void)node; }

private:
    friend class Node;
    void set_node(Node* node) noexcept { node_ = node; }

    Node* node_ = nullptr;
};

} // namespace fe
