// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/scene/Node.h"

#include "fe/core/Assert.h"
#include "fe/scene/Scene.h"

#include <algorithm>

namespace fe {

Node::Node() : id_(Uuid::generate()), name_("Node") {}

Node::Node(std::string name) : id_(Uuid::generate()), name_(std::move(name)) {}

Node::~Node() = default;

Node* Node::add_child(std::unique_ptr<Node> child) {
    FE_CHECK(child != nullptr);
    FE_CHECK(child.get() != this);
    FE_CHECK(!is_ancestor_of(*child));

    // Detach from the previous parent so a node has exactly one owner.
    if (child->parent_ != nullptr) {
        Node* previous = child->parent_;
        auto owned = previous->remove_child(child.get());
        // `owned` now holds the child; continue with it.
        child = std::move(owned);
    }

    Node* raw = child.get();
    raw->set_parent(this);

    if (scene_ != nullptr) {
        raw->attach_recursive(*scene_);
    }

    children_.push_back(std::move(child));
    return raw;
}

std::unique_ptr<Node> Node::remove_child(Node* child) {
    const auto it = std::find_if(children_.begin(), children_.end(),
                                 [child](const std::unique_ptr<Node>& candidate) { return candidate.get() == child; });
    if (it == children_.end()) {
        return nullptr;
    }

    std::unique_ptr<Node> owned = std::move(*it);
    children_.erase(it);

    if (scene_ != nullptr) {
        owned->detach_recursive(*scene_);
    }
    owned->set_parent(nullptr);
    return owned;
}

bool Node::is_ancestor_of(const Node& other) const noexcept {
    for (const Node* current = other.parent_; current != nullptr; current = current->parent_) {
        if (current == this) {
            return true;
        }
    }
    return false;
}

Mat4 Node::world_matrix() const noexcept {
    if (parent_ == nullptr) {
        return local_matrix();
    }
    return parent_->world_matrix() * local_matrix();
}

void Node::set_scene_recursive(Scene* scene) noexcept {
    scene_ = scene;
    for (const auto& child : children_) {
        child->set_scene_recursive(scene);
    }
}

void Node::attach_recursive(Scene& scene) {
    scene_ = &scene;
    on_attach(scene);
    for (const auto& component : components_) {
        component->on_attach(*this);
    }
    scene.register_node(*this);
    for (const auto& child : children_) {
        child->attach_recursive(scene);
    }
}

void Node::detach_recursive(Scene& scene) {
    for (const auto& child : children_) {
        child->detach_recursive(scene);
    }
    scene.unregister_node(*this);
    for (const auto& component : components_) {
        component->on_detach(*this);
    }
    on_detach(scene);
    scene_ = nullptr;
}

} // namespace fe
