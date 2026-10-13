// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/scene/Scene.h"

#include "fe/core/Assert.h"

#include <algorithm>

namespace fe {

Scene::Scene() = default;

Scene::~Scene() {
    // Detach explicitly so nodes observe on_detach before destruction, in the
    // same order a normal removal would produce.
    clear();
}

Node* Scene::create_node(std::string name) {
    return create_node<Node>(std::move(name));
}

void Scene::attach_root(std::unique_ptr<Node> node) {
    FE_CHECK(node != nullptr);
    node->set_parent(nullptr);
    node->attach_recursive(*this);
    roots_.push_back(std::move(node));
}

std::unique_ptr<Node> Scene::remove_root(Node* node) {
    const auto it = std::find_if(roots_.begin(), roots_.end(),
                                 [node](const std::unique_ptr<Node>& candidate) { return candidate.get() == node; });
    if (it == roots_.end()) {
        return nullptr;
    }
    std::unique_ptr<Node> owned = std::move(*it);
    roots_.erase(it);
    owned->detach_recursive(*this);
    owned->set_parent(nullptr);
    return owned;
}

Node* Scene::find_node(const Uuid& id) const noexcept {
    const auto it = index_.find(id);
    return it != index_.end() ? it->second : nullptr;
}

Node* Scene::find_node_by_name(std::string_view name) const noexcept {
    Node* found = nullptr;
    for_each_node([&](Node& node) {
        if (found == nullptr && node.name() == name) {
            found = &node;
        }
    });
    return found;
}

void Scene::clear() {
    while (!roots_.empty()) {
        auto node = std::move(roots_.back());
        roots_.pop_back();
        node->detach_recursive(*this);
    }
    index_.clear();
}

void Scene::register_node(Node& node) { index_[node.id()] = &node; }

void Scene::unregister_node(Node& node) {
    const auto it = index_.find(node.id());
    if (it != index_.end() && it->second == &node) {
        index_.erase(it);
    }
}

} // namespace fe
