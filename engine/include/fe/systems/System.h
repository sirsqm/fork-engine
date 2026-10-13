// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/scene/Scene.h"
#include "fe/systems/FrameContext.h"

#include <string_view>

namespace fe {

// A unit of per-frame behaviour.
//
// The composition of behaviour belongs to systems, not to nodes. A system
// declares the component types it reads and processes them together, choosing
// its own internal layout rather than being called once per object. That is the
// execution half of the hybrid model (ADR-0002): the scene graph is the
// authoring representation, and a system reads the data it cares about into its
// own structure each frame.
//
// This is deliberately the opposite of a per-node `update()` virtual. There is
// no callback on Node or Component that a frame loop invokes; a system owns its
// traversal, so an animation system can copy the animators it will advance into
// a flat list while a future physics system lays colliders out in a grid. The
// engine never dictates the shape of a system's inner loop.
//
// Lifetime: a system is created and owned by a SceneSchedule. It must not
// outlive the schedule. Systems run on the thread that drives the schedule.
class System {
public:
    virtual ~System();

    System(const System&) = delete;
    System& operator=(const System&) = delete;
    System(System&&) = delete;
    System& operator=(System&&) = delete;

    // The name reported by the runtime and used to identify the system in logs.
    [[nodiscard]] virtual std::string_view name() const noexcept = 0;

    // Invoked once per frame with the scene the schedule is bound to. A system
    // should be a no-op when the scene holds nothing it processes.
    virtual void update(const FrameContext& context, Scene& scene) = 0;

protected:
    System() = default;
};

} // namespace fe
