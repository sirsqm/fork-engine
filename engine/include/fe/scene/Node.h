// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Math.h"
#include "fe/core/Types.h"
#include "fe/core/Uuid.h"
#include "fe/scene/Component.h"
#include "fe/scene/Transform.h"

#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <typeinfo>
#include <vector>

namespace fe {

class Scene;

// A node in a scene graph.
//
// A Node has identity (a stable Uuid), a name, a local transform, and an
// ordered list of children. This is the *authoring* representation: it is what
// the editor shows and what the serializer writes. It is deliberately not the
// same thing as the runtime execution model. Rendering walks the graph to
// produce a flat draw list and batches it; physics and animation will read
// component data into their own layouts. The hierarchy organises data; it does
// not dictate how any system executes.
//
// Lifetime: a Node is owned by its parent (or by the Scene when it is a root).
// Nodes are created through Scene::create_node() or Node::create_child() and
// are never constructed directly by gameplay code.
class Node {
public:
    virtual ~Node();

    Node(const Node&) = delete;
    Node& operator=(const Node&) = delete;

    // --- Identity ----------------------------------------------------------
    [[nodiscard]] const Uuid& id() const noexcept { return id_; }

    // Overrides the node's identity. Deserialization uses this to restore the
    // id that was saved with the scene, so that references to a node survive a
    // save/load cycle. Callers outside the serialization layer should not need
    // it; identity is normally assigned when the node is created.
    void set_id(const Uuid& id) noexcept { id_ = id; }
    [[nodiscard]] const std::string& name() const noexcept { return name_; }
    void set_name(std::string name) { name_ = std::move(name); }

    // The registered type name used by the serializer ("Node", "Sprite2D", ...).
    [[nodiscard]] virtual const char* type_name() const noexcept { return "Node"; }

    // --- Hierarchy ---------------------------------------------------------
    [[nodiscard]] Node* parent() const noexcept { return parent_; }
    [[nodiscard]] const std::vector<std::unique_ptr<Node>>& children() const noexcept { return children_; }
    [[nodiscard]] usize child_count() const noexcept { return children_.size(); }

    // Attaches an existing node as a child, detaching it from any previous
    // parent. Returns the child.
    Node* add_child(std::unique_ptr<Node> child);

    // Creates and attaches a child of concrete type T.
    template <typename T, typename... Args> T* create_child(Args&&... args) {
        static_assert(std::is_base_of_v<Node, T>, "T must derive from fe::Node");
        auto child = std::unique_ptr<T>(new T(std::forward<Args>(args)...));
        T* raw = child.get();
        add_child(std::move(child));
        return raw;
    }

    // Detaches a direct child and returns ownership to the caller. Returns
    // nullptr if `child` is not a child of this node.
    [[nodiscard]] std::unique_ptr<Node> remove_child(Node* child);

    [[nodiscard]] bool is_ancestor_of(const Node& other) const noexcept;

    // --- Transform ---------------------------------------------------------
    [[nodiscard]] Transform& transform() noexcept { return transform_; }
    [[nodiscard]] const Transform& transform() const noexcept { return transform_; }

    // The node's own transform (local to its parent).
    [[nodiscard]] Mat4 local_matrix() const noexcept { return transform_.local_matrix(); }

    // World matrix = parent chain of local matrices. Computed on demand; the
    // renderer computes it once per frame and caches nothing, which is correct
    // and fast enough at this scale. Dirty-flag caching is a measured
    // optimisation, not a starting assumption.
    [[nodiscard]] Mat4 world_matrix() const noexcept;

    // --- Components --------------------------------------------------------
    // Adds a component of concrete type T, owned by this node. The node may
    // hold several components of different types; adding a second component of
    // the same type is allowed and both remain addressable by index.
    template <typename T, typename... Args> T* add_component(Args&&... args) {
        static_assert(std::is_base_of_v<Component, T>, "T must derive from fe::Component");
        auto component = std::unique_ptr<T>(new T(std::forward<Args>(args)...));
        T* raw = component.get();
        raw->set_node(this);
        if (scene_ != nullptr) {
            raw->on_attach(*this);
        }
        components_.push_back(std::move(component));
        return raw;
    }

    // Returns the first component of type T, or nullptr.
    template <typename T> [[nodiscard]] T* get_component() noexcept {
        for (const auto& component : components_) {
            if (component->type_info() == typeid(T)) {
                return static_cast<T*>(component.get());
            }
        }
        return nullptr;
    }

    template <typename T> [[nodiscard]] const T* get_component() const noexcept {
        for (const auto& component : components_) {
            if (component->type_info() == typeid(T)) {
                return static_cast<const T*>(component.get());
            }
        }
        return nullptr;
    }

    // Removes and destroys the first component of type T. Returns true if one
    // was found.
    template <typename T> bool remove_component() {
        for (auto it = components_.begin(); it != components_.end(); ++it) {
            if ((*it)->type_info() == typeid(T)) {
                (*it)->on_detach(*this);
                (*it)->set_node(nullptr);
                components_.erase(it);
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] const std::vector<std::unique_ptr<Component>>& components() const noexcept { return components_; }
    [[nodiscard]] usize component_count() const noexcept { return components_.size(); }

    // The scene that owns this node, or nullptr when detached.
    [[nodiscard]] Scene* scene() const noexcept { return scene_; }

protected:
    Node();
    explicit Node(std::string name);

    // Called after the node is attached to a scene, once for the node and then
    // for each descendant. Subclasses use it to acquire resources.
    virtual void on_attach(Scene& scene) {}
    // Called before the node leaves its scene, once for the node and then for
    // each descendant.
    virtual void on_detach(Scene& scene) {}

private:
    friend class Scene;

    void set_parent(Node* parent) noexcept { parent_ = parent; }
    void set_scene_recursive(Scene* scene) noexcept;
    void attach_recursive(Scene& scene);
    void detach_recursive(Scene& scene);

    Uuid id_;
    std::string name_;
    Transform transform_{};
    Node* parent_ = nullptr;
    Scene* scene_ = nullptr;
    std::vector<std::unique_ptr<Component>> components_;
    std::vector<std::unique_ptr<Node>> children_;
};

} // namespace fe
