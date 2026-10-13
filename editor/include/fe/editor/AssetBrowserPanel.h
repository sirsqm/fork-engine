// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/editor/AssetBrowserViewModel.h"
#include "fe/editor/Panel.h"

#include <array>

namespace fe::editor {

// The asset browser panel: a flat, filterable listing of the project's assets.
//
// It is a thin projection of `AssetBrowserViewModel`, which holds the listing,
// the filter, and the selection; this class only emits widgets. The listing
// comes from the asset database that the resource system already maintains, so
// the browser shows exactly the assets the engine can load, with their stable
// GUIDs.
//
// Rebuilds the listing from the resource manager every frame: scanning is
// cheap and the browser must reflect a rescan (a file move, an import) without
// an explicit refresh. The ImGui code holds only the filter text buffer.
class AssetBrowserPanel final : public Panel {
public:
    [[nodiscard]] const char* name() const noexcept override { return "Assets"; }

    void draw(EditorContext& context) override;

private:
    AssetBrowserViewModel model_;
    std::array<char, 128> filter_buffer_{};
};

} // namespace fe::editor
