// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Types.h"
#include "fe/core/Uuid.h"
#include "fe/scene/Node.h"

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace fe {

// A scene: an owned tree of nodes plus a lookup index by identity.
//
// The scene is the unit of authoring and persistence. It owns its root nodes,
// keeps a map from Uuid to Node so that references survive renames and
// reorders, and notifies nodes as they enter and leave it.
//
// A scene does not render, simulate, or serialise itself. Those are separate
// concerns (renderer2d, future systems, serialization::SceneSerializer) that
// operate *on* a scene. This keeps the data model independent of any one
// consumer and lets the editor hold a scene without dragging in the runtime.
class Scene {
public:
    Scene();
    ~Scene();

    Scene(const Scene&) = delete;
    Scene& operator=(const Scene&) = delete;

    [[nodiscard]] const std::string& name() const noexcept { return name_; }
    void set_name(std::string name) { name_ = std::move(name); }

    // --- Node creation -----------------------------------------------------
    // Creates a root node of concrete type T and returns a borrowed pointer.
    // Ownership stays with the scene.
    template <typename T, typename... Args>
    T* create_node(Args&&... args) {
        static_assert(std::is_base_of_v<Node, T>, "T must derive from fe::Node");
        auto node = std::unique_ptr<T>(new T(std::forward<Args>(args)...));
        T* raw = node.get();
        attach_root(std::move(node));
        return raw;
    }

    // Creates a plain Node root.
    [[nodiscard]] Node* create_node(std::string name = "Node");

    [[nodiscard]] const std::vector<std::unique_ptr<Node>>& roots() const noexcept { return roots_; }
    [[nodiscard]] usize root_count() const noexcept { return roots_.size(); }

    // Total number of nodes in the scene, roots included.
    [[nodiscard]] usize node_count() const noexcept { return index_.size(); }

    // Detaches a root node and returns ownership. Returns nullptr if `node` is
    // not a root of this scene.
    [[nodiscard]] std::unique_ptr<Node> remove_root(Node* node);

    // Attaches an already-owned node as a root, detaching it from any previous
    // scene or parent. The counterpart to remove_root(), used to move a whole
    // subtree between scenes without recreating it (the editor uses it to load
    // a scene into a scratch instance and then adopt it). Returns the node.
    Node* add_root(std::unique_ptr<Node> node);

    // As add_root(), but inserts at `index` among the roots (clamped to
    // [0, root_count()]). Used by the editor to undo a delete or a reparent
    // without reordering the roots that were not touched.
    Node* add_root(std::unique_ptr<Node> node, usize index);

    // --- Lookup ------------------------------------------------------------
    [[nodiscard]] Node* find_node(const Uuid& id) const noexcept;
    [[nodiscard]] Node* find_node_by_name(std::string_view name) const noexcept;

    // Finds a component anywhere in the scene by its stable id, or nullptr.
    //
    // This is the counterpart of find_node() for the component identity added in
    // version 3 of the scene format (ADR-0043). It walks the tree rather than
    // keeping a second index: a component is created and destroyed far more
    // often than a node, and an index over it would have to be updated from
    // Node::attach_component/detach_component on every edit. The walk is the
    // correct first cut, and an index is a measured optimisation if a profile
    // ever shows this hot.
    [[nodiscard]] Component* find_component(const Uuid& id) const noexcept;

    // Rebuilds the id index from the current node tree. Deserialization assigns
    // saved ids to nodes after they are attached, so it must call this once the
    // whole tree is loaded for `find_node` (and any component that stores a node
    // reference, such as a joint) to resolve those ids.
    void rebuild_index() noexcept;

    // --- Traversal ---------------------------------------------------------
    // Visits every node depth-first (pre-order). The callback receives Node&.
    template <typename Fn>
    void for_each_node(Fn&& fn) const {
        for (const auto& root : roots_) {
            visit_recursive(*root, fn);
        }
    }

    // Clears the scene, detaching every node.
    void clear();

private:
    friend class Node;

    void attach_root(std::unique_ptr<Node> node);
    void register_node(Node& node);
    void unregister_node(Node& node);

    template <typename Fn>
    static void visit_recursive(Node& node, Fn& fn) {
        fn(node);
        for (const auto& child : node.children()) {
            visit_recursive(*child, fn);
        }
    }

    std::string name_ = "Scene";
    std::vector<std::unique_ptr<Node>> roots_;
    std::unordered_map<Uuid, Node*> index_;
};

} // namespace fe
