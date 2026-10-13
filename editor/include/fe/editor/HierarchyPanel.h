// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/editor/Panel.h"

#include <array>

namespace fe::editor {

// The scene hierarchy panel: a tree of the scene's nodes.
//
// The panel is a thin projection of `HierarchyViewModel`: the view model decides
// which rows are visible, expanded, and selected, and this class only emits
// widgets for them. That split is what makes the hierarchy's behaviour testable
// without a window (see `HierarchyViewModel`) and keeps the ImGui code free of
// tree logic.
//
// It reads the scene through the public `Scene`/`Node` API only. No reflection,
// no private access.
class HierarchyPanel final : public Panel {
public:
    [[nodiscard]] const char* name() const noexcept override { return "Hierarchy"; }

    void draw(EditorContext& context) override;

private:
    std::array<char, 128> filter_buffer_{};
};

} // namespace fe::editor
