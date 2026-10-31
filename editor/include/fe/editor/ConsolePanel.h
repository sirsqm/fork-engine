// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/editor/ConsolePanelViewModel.h"
#include "fe/editor/Panel.h"

namespace fe::editor {

// The console panel: the engine log and the contained script failures, in one
// place.
//
// It is a thin projection of `ConsolePanelViewModel`, which does the polling and
// filtering; this class only emits widgets. Like every panel it reads state and
// never owns it: the log buffer is installed by the host, and the script
// failures belong to the script host, so the panel can be opened and closed
// without losing a record.
//
// This is the read-out M7's debugging item asks for: a game author without a
// debugger attached can see what the engine and their gameplay code logged, and
// -- more useful still -- which script failed, on which node, and why. A failing
// script is disabled by the host, so the failure is recorded once and would
// otherwise scroll out of view.
class ConsolePanel final : public Panel {
public:
    [[nodiscard]] const char* name() const noexcept override { return "Console"; }

    void draw(EditorContext& context) override;

    // The current snapshot. Exposed read-only so the headless part of the panel
    // can be inspected without ImGui.
    [[nodiscard]] const ConsolePanelViewModel& view_model() const noexcept { return view_model_; }

private:
    ConsolePanelViewModel view_model_;
    // When true the panel keeps showing the newest entry as records arrive.
    bool auto_scroll_ = true;
};

} // namespace fe::editor
