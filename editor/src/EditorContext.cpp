// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/editor/EditorContext.h"

#include "fe/core/Log.h"
#include "fe/physics2d/Collider2D.h"
#include "fe/physics2d/RigidBody2D.h"
#include "fe/resources/ResourceManager.h"
#include "fe/scene/Camera2D.h"
#include "fe/scene/Node.h"
#include "fe/scene/Sprite2D.h"
#include "fe/scene/SpriteAnimator.h"
#include "fe/serialization/SceneSerializer.h"

#include <memory>
#include <string>
#include <utility>

namespace fe::editor {
namespace {

// Component copy factories for the built-in types. These are the scene-specific
// knowledge that lets play-in-editor clone a scene without the editor depending
// on reflection: the serializer's per-type switch (ADR-0005) has the same shape
// and is deliberately not shared, because serialization and cloning have
// different targets and will diverge.

void copy_camera(const Node& source_node, Node& destination_node) {
    const auto* source = source_node.get_component<Camera2D>();
    if (source == nullptr) {
        return;
    }
    // Components are move-only (ADR-0002), so the fields are copied explicitly.
    auto* destination = destination_node.add_component<Camera2D>();
    destination->zoom = source->zoom;
    destination->clear_r = source->clear_r;
    destination->clear_g = source->clear_g;
    destination->clear_b = source->clear_b;
    destination->clear_a = source->clear_a;
    destination->primary = source->primary;
}

void copy_sprite(const Node& source_node, Node& destination_node) {
    const auto* source = source_node.get_component<Sprite2D>();
    if (source == nullptr) {
        return;
    }
    auto* destination = destination_node.add_component<Sprite2D>();
    destination->size = source->size;
    destination->color = source->color;
    destination->uv = source->uv;
    destination->layer = source->layer;
    // The texture is a shared_ptr<Resource>; copying it shares the resource,
    // which is correct under ADR-0004. Reloading per-session is deferred until
    // the asset pipeline lands.
    destination->texture = source->texture;
}

void copy_animator(const Node& source_node, Node& destination_node) {
    const auto* source = source_node.get_component<SpriteAnimator>();
    if (source == nullptr) {
        return;
    }
    auto* destination = destination_node.add_component<SpriteAnimator>();
    destination->frames_x = source->frames_x;
    destination->frames_y = source->frames_y;
    destination->fps = source->fps;
    destination->playing = source->playing;
    destination->loop = source->loop;
    destination->current_frame = source->current_frame;
}

void copy_collider(const Node& source_node, Node& destination_node) {
    const auto* source = source_node.get_component<physics2d::Collider2D>();
    if (source == nullptr) {
        return;
    }
    auto* destination = destination_node.add_component<physics2d::Collider2D>();
    destination->shape = source->shape;
    destination->half_extent = source->half_extent;
    destination->radius = source->radius;
    destination->restitution = source->restitution;
    destination->friction = source->friction;
}

void copy_rigid_body(const Node& source_node, Node& destination_node) {
    const auto* source = source_node.get_component<physics2d::RigidBody2D>();
    if (source == nullptr) {
        return;
    }
    auto* destination = destination_node.add_component<physics2d::RigidBody2D>();
    destination->velocity = source->velocity;
    destination->mass = source->mass;
    destination->linear_damping = source->linear_damping;
    destination->kinematic = source->kinematic;
    destination->affected_by_gravity = source->affected_by_gravity;
}

} // namespace

EditorContext::EditorContext() = default;

void EditorContext::register_default_component_factories() {
    play_.register_component_factory(Camera2D::kTypeName, copy_camera);
    play_.register_component_factory(Sprite2D::kTypeName, copy_sprite);
    play_.register_component_factory(SpriteAnimator::kTypeName, copy_animator);
    // Physics components copy too, so a play session simulates the authored
    // colliders and bodies (Physics2DSystem runs in the play schedule).
    play_.register_component_factory(physics2d::Collider2D::kTypeName, copy_collider);
    play_.register_component_factory(physics2d::RigidBody2D::kTypeName, copy_rigid_body);
}

Error EditorContext::new_scene(ResourceManager* resources) {
    scene_.clear();
    scene_.set_name("Untitled");
    scene_path_.clear();
    // A new document has no history: undo must not reach back across documents.
    commands_.clear();

    // A camera plus a small sprite cluster. When a resource manager is present,
    // load the sample checker texture so the scene renders something; otherwise
    // the sprites are texture-less and the renderer skips them, which is a valid
    // authoring state.
    scene_.create_node("Camera")->add_component<Camera2D>();

    std::shared_ptr<Resource> texture;
    if (resources != nullptr) {
        auto loaded = resources->load("tex:assets/textures/checker.png");
        if (loaded) {
            texture = loaded.value();
        } else {
            FE_LOG_WARN("editor: sample texture unavailable: %s", loaded.error().message().c_str());
        }
    }

    for (i32 i = 0; i < 3; ++i) {
        Node* node = scene_.create_node("Sprite" + std::to_string(i));
        auto* sprite = node->add_component<Sprite2D>();
        sprite->texture = texture;
        sprite->size = Vec2{96.0f, 96.0f};
        node->transform().position = Vec3{static_cast<f32>(i) * 120.0f - 120.0f, 0.0f, 0.0f};
    }

    // A parent/child pair so the hierarchy has depth to show.
    Node* parent = scene_.create_node("Group");
    parent->transform().position = Vec3{0.0f, 120.0f, 0.0f};
    Node* child = parent->create_child<Node>("GroupChild");
    auto* child_sprite = child->add_component<Sprite2D>();
    child_sprite->texture = texture;
    child_sprite->size = Vec2{48.0f, 48.0f};

    refresh_hierarchy();
    FE_LOG_INFO("editor: new scene with %zu nodes", scene_.node_count());
    return Error::none();
}

Error EditorContext::load_scene(std::string_view path, ResourceManager* resources) {
    // Load into a scratch scene so a malformed file cannot leave the document
    // half-replaced.
    Scene loaded;
    if (Error error = serialization::scene::load(path, loaded, resources); error) {
        return error;
    }

    scene_.clear();
    // Move the fully formed scene into the document. There is no Scene move
    // constructor, so rebuild it by detaching and reattaching the roots, which
    // preserves node identity and every loaded component.
    scene_.set_name(loaded.name());
    while (!loaded.roots().empty()) {
        Node* root = loaded.roots().front().get();
        std::unique_ptr<Node> owned = loaded.remove_root(root);
        scene_.add_root(std::move(owned));
    }

    scene_path_ = std::string(path);
    // Loading replaces the document, so the previous document's history no
    // longer applies to what is on screen.
    commands_.clear();
    refresh_hierarchy();
    FE_LOG_INFO("editor: loaded '%s' (%zu nodes)", scene_path_.c_str(), scene_.node_count());
    return Error::none();
}

Error EditorContext::save_scene(std::string_view path) {
    const std::string target = path.empty() ? scene_path_ : std::string(path);
    if (target.empty()) {
        return Error("EditorContext::save_scene: no path given and the scene has never been saved");
    }
    if (Error error = serialization::scene::save(scene_, target); error) {
        return error;
    }
    scene_path_ = target;
    FE_LOG_INFO("editor: saved '%s'", scene_path_.c_str());
    return Error::none();
}

void EditorContext::refresh_hierarchy() {
    hierarchy_.rebuild(scene_);
}

bool EditorContext::execute(std::unique_ptr<Command> command) {
    if (!is_editable() || command == nullptr) {
        return false;
    }
    if (!commands_.execute(scene_, std::move(command))) {
        return false;
    }
    // Structural commands change the tree, and property commands can change a
    // node's name, so the hierarchy is rebuilt unconditionally: it is cheap and
    // the alternative is a stale panel.
    refresh_hierarchy();
    return true;
}

bool EditorContext::undo() {
    if (!can_undo()) {
        return false;
    }
    const bool undone = commands_.undo(scene_);
    if (undone) {
        refresh_hierarchy();
    }
    return undone;
}

bool EditorContext::redo() {
    if (!can_redo()) {
        return false;
    }
    const bool redone = commands_.redo(scene_);
    if (redone) {
        refresh_hierarchy();
    }
    return redone;
}

void EditorContext::set_project_root(std::string root, ResourceManager* resources) {
    project_root_ = std::move(root);
    if (resources == nullptr) {
        return;
    }
    // Setting the root rescans the database, so the asset browser and GUID
    // reference resolution see the new project immediately. An empty root clears
    // the index (set_root on the database handles that).
    resources->assets().set_root(project_root_);
}

} // namespace fe::editor
