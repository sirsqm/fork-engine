// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/systems/SceneSchedule.h"

#include "fe/scene/Scene.h"

namespace fe {

void SceneSchedule::update(const FrameContext& context, Scene& scene) {
    for (const auto& system : systems_) {
        system->update(context, scene);
    }
}

System* SceneSchedule::find(std::string_view name) noexcept {
    for (const auto& system : systems_) {
        if (system->name() == name) {
            return system.get();
        }
    }
    return nullptr;
}

} // namespace fe
