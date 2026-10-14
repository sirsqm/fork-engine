// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/script/ScriptHost.h"
#include "fe/systems/System.h"

#include <string_view>

namespace fe {

// Drives a ScriptHost from the ordinary SceneSchedule.
//
// This is the whole bridge between the scripting seam and the frame loop: it is
// an ordinary System, so scripts run wherever a schedule runs — the sandbox,
// play-in-editor, the headless example, and a test — through the identical
// path, with no new execution model and no editor or RHI dependency.
//
// The host is owned by the game (or the editor), not by this system, because
// gameplay attaches and detaches scripts through the host between frames while
// the schedule owns only its systems. The system borrows the host and must not
// outlive it.
class ScriptSystem final : public System {
public:
    explicit ScriptSystem(ScriptHost& host) : host_(&host) {}

    [[nodiscard]] std::string_view name() const noexcept override { return "ScriptSystem"; }

    void update(const FrameContext& context, Scene& scene) override {
        // The scene is unused: a script reaches its node through the host's
        // binding. The parameter is part of the System contract.
        (void)scene;
        host_->update(context);
    }

    [[nodiscard]] ScriptHost& host() noexcept { return *host_; }
    [[nodiscard]] const ScriptHost& host() const noexcept { return *host_; }

private:
    ScriptHost* host_;
};

} // namespace fe
