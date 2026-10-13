// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/editor/Panel.h"

namespace fe::editor {

// Play control and a small statistics read-out.
//
// Shows the Play/Stop button for play-in-editor, the play state, and the counts
// the runtime actually produced (frames and sprites drawn). Like every panel it
// only reads state and raises a request; the host that owns the embedded Engine
// performs the transition.
class PlayPanel final : public Panel {
public:
    [[nodiscard]] const char* name() const noexcept override { return "Play"; }

    void draw(EditorContext& context) override;
};

} // namespace fe::editor
