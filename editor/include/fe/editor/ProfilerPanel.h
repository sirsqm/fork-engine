// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/editor/Panel.h"
#include "fe/editor/ProfilerPanelViewModel.h"

namespace fe::editor {

// The profiler panel: the frame's cost breakdown, read from the editor's
// profiler.
//
// It is a thin projection of `ProfilerPanelViewModel`, which does the reading
// and reshaping; this class only emits widgets. Like every panel it only reads
// state -- it never starts or stops recording, because the host owns the frame
// loop and installs the profiler. Its one piece of local state is a Pause
// toggle, which freezes the *view* (so a spike can be inspected) without
// touching what is recorded.
//
// The panel is what the M9 roadmap item asks for: an in-editor read-out fed by
// the core profiler (ADR-0022), showing the same per-name breakdown the profiler
// records, so the numbers a developer sees in the editor are the numbers CI
// gates on rather than a second measurement.
class ProfilerPanel final : public Panel {
public:
    [[nodiscard]] const char* name() const noexcept override { return "Profiler"; }

    void draw(EditorContext& context) override;

    // The current snapshot. Exposed read-only so the headless part of the panel
    // can be inspected without ImGui.
    [[nodiscard]] const ProfilerPanelViewModel& view_model() const noexcept { return view_model_; }
    [[nodiscard]] bool paused() const noexcept { return paused_; }

private:
    ProfilerPanelViewModel view_model_;
    bool paused_ = false;
};

} // namespace fe::editor
