// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/runtime/AudioListenerSystem.h"

#include "fe/audio/AudioSystem.h"
#include "fe/core/Math.h"
#include "fe/renderer2d/Renderer2D.h"
#include "fe/renderer3d/Renderer3D.h"
#include "fe/scene/Camera2D.h"
#include "fe/scene/Node.h"
#include "fe/scene/Scene.h"
#include "fe/scene3d/Camera3D.h"

namespace fe {

void AudioListenerSystem::update(const FrameContext& context, Scene& scene) {
    (void)context;
    if (audio_ == nullptr) {
        return;
    }

    // A 3D camera is preferred when the scene has one: it is the camera the 3D
    // pass draws through, so it is the one the player hears from. A scene with
    // only a 2D camera falls back to that, which is every 2D game. The two
    // lookups are the renderers' own, so this agrees with what was drawn.
    if (const Camera3D* camera = renderer3d::Renderer3D::find_active_camera(scene); camera != nullptr) {
        // `eye()` is the owner's world position, and already handles a detached
        // camera (returns the origin); skip the write rather than move the
        // listener to a position nobody asked for.
        if (camera->node() != nullptr) {
            audio_->set_listener_position(camera->eye());
        }
        return;
    }

    // A 2D camera has no `eye()` accessor, but its world position is the same
    // computation: the owner's translation. Reading the world matrix (not the
    // local transform) is what drags the listener with a parented camera.
    if (const Camera2D* camera = renderer2d::Renderer2D::find_active_camera(scene); camera != nullptr) {
        if (const Node* owner = camera->node(); owner != nullptr) {
            audio_->set_listener_position(transform_point(owner->world_matrix(), Vec3{}));
        }
    }
}

} // namespace fe
