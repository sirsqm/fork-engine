// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/editor/PlaySession.h"

#include "fe/core/Log.h"
#include "fe/physics2d/Physics2DSystem.h"
#include "fe/renderer2d/Renderer2D.h"
#include "fe/scene/Camera2D.h"
#include "fe/systems/SpriteAnimationSystem.h"

namespace fe::editor {

void PlaySession::register_default_systems() {
    // Idempotent: leave any systems a caller already registered (including ones
    // the editor added) in place, and only add the defaults that are missing.
    if (schedule_.find("SpriteAnimationSystem") == nullptr) {
        schedule_.add_system<SpriteAnimationSystem>();
    }
    // Physics runs after animation, so a body's transform is integrated against
    // the state the other systems produced this frame. The order is the
    // schedule's, and it is explicit here because it is semantically
    // significant (ADR-0008).
    if (schedule_.find("Physics2DSystem") == nullptr) {
        schedule_.add_system<physics2d::Physics2DSystem>();
    }
}

void PlaySession::register_component_factory(std::string type_name, ComponentFactory factory) {
    for (auto& entry : factories_) {
        if (entry.type_name == type_name) {
            entry.factory = std::move(factory);
            return;
        }
    }
    factories_.push_back(FactoryEntry{std::move(type_name), std::move(factory)});
}

void PlaySession::copy_components_of(const Node& source_node, Node& destination_node) const {
    for (const auto& component : source_node.components()) {
        const std::string type = component->type_name();
        bool handled = false;
        for (const auto& entry : factories_) {
            if (entry.type_name == type) {
                entry.factory(source_node, destination_node);
                handled = true;
                break;
            }
        }
        if (!handled && last_copy_warning_.empty()) {
            last_copy_warning_ = type;
        }
    }
}

void PlaySession::copy_scene(const Scene& authored_scene, Scene& out) const {
    last_copy_warning_.clear();
    out.clear();
    out.set_name(authored_scene.name());

    // Depth-first copy of the hierarchy. Node identity is not preserved: the
    // play scene is a throwaway world, and reusing ids would make the two trees
    // share `Uuid`s while being distinct objects, which is a trap for anything
    // that keys off identity (the hierarchy index, future node references).
    struct Pending {
        const Node* source;
        Node* destination;
    };

    std::vector<Pending> stack;
    for (const auto& root : authored_scene.roots()) {
        Node* copy = out.create_node(root->name());
        copy->transform() = root->transform();
        copy_components_of(*root, *copy);
        stack.push_back(Pending{root.get(), copy});
    }

    while (!stack.empty()) {
        const Pending pending = stack.back();
        stack.pop_back();
        for (const auto& child : pending.source->children()) {
            Node* child_copy = pending.destination->create_child<Node>(child->name());
            child_copy->transform() = child->transform();
            copy_components_of(*child, *child_copy);
            stack.push_back(Pending{child.get(), child_copy});
        }
    }
}

Error PlaySession::start(const Scene& authored_scene) {
    if (state_ == State::Playing) {
        return Error("PlaySession::start called while already playing");
    }

    copy_scene(authored_scene, play_scene_);
    if (!last_copy_warning_.empty()) {
        FE_LOG_WARN("play-in-editor: component type '%s' has no factory and was not copied into the play scene",
                    last_copy_warning_.c_str());
    }

    // Make play exercise real gameplay: register the default systems so the
    // schedule runs over the play scene each tick.
    register_default_systems();

    frames_played_ = 0;
    last_sprite_count_ = 0;
    play_time_ = 0.0;
    state_ = State::Playing;
    FE_LOG_INFO("play-in-editor: started (%zu nodes, %zu roots)", play_scene_.node_count(), play_scene_.root_count());
    return Error::none();
}

Error PlaySession::advance(const FrameContext& context) {
    if (state_ != State::Playing) {
        return Error("PlaySession::advance called while not playing");
    }
    schedule_.update(context, play_scene_);
    return Error::none();
}

Result<usize> PlaySession::tick(Engine& engine) {
    if (state_ != State::Playing) {
        return Error("PlaySession::tick called while not playing");
    }

    engine.tick();
    play_time_ = engine.elapsed_time();

    // Run gameplay before rendering: the systems advance component state (an
    // animator's current cell, a body's transform), and the renderer then draws
    // the world the systems produced. This is the same Scene + SceneSchedule
    // pairing `Application` uses, so play-in-editor is not a second execution
    // path.
    FrameContext frame;
    frame.delta_time = engine.delta_time();
    frame.elapsed_time = engine.elapsed_time();
    frame.resources = &engine.resources();
    if (Error error = advance(frame); error) {
        return error;
    }

    const u32 width = engine.window().framebuffer_width();
    const u32 height = engine.window().framebuffer_height();

    // Mirror the runtime's clear policy: the active camera decides the clear
    // colour, via the renderer's own camera selection so the two never diverge.
    rhi::ClearColor clear{};
    if (const Camera2D* camera = renderer2d::Renderer2D::find_active_camera(play_scene_); camera != nullptr) {
        clear = rhi::ClearColor{camera->clear_r, camera->clear_g, camera->clear_b, camera->clear_a};
    }

    engine.device().begin_frame(width, height, clear);
    auto result = engine.renderer_2d().render(engine.device(), play_scene_, width, height);

    if (!result) {
        last_sprite_count_ = 0;
        return result.error();
    }

    last_sprite_count_ = result.value();
    ++frames_played_;
    return last_sprite_count_;
}

void PlaySession::stop() {
    if (state_ == State::Stopped) {
        return;
    }

    play_scene_.clear();
    state_ = State::Stopped;
    frames_played_ = 0;
    last_sprite_count_ = 0;
    play_time_ = 0.0;
    FE_LOG_INFO("play-in-editor: stopped");
}

} // namespace fe::editor
