// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/systems/SceneSchedule.h"

#include "fe/core/Profiler.h"
#include "fe/scene/Scene.h"

#include <cstdio>
#include <string_view>

namespace fe {

void SceneSchedule::update(const FrameContext& context, Scene& scene) {
    // One timing sample per system, named "system/<name>", so a frame's cost
    // breaks down by system with no macro added by hand at the call site. The
    // scope is a no-op when no profiler is current, so this is free in a build
    // that does not profile. A system that renames itself per frame would create
    // a new name each time and eventually exhaust the name table; names are
    // expected to be stable, which is the contract System::name() already has.
    for (const auto& system : systems_) {
        const std::string_view name = system->name();
        char label[Profiler::kMaxNameLength + 1];
        const int written =
            std::snprintf(label, sizeof(label), "system/%.*s", static_cast<int>(name.size()), name.data());
        FE_PROFILE_SCOPE(std::string_view(label, written > 0 ? static_cast<usize>(written) : 0));
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
