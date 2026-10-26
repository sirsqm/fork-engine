// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/resources/Resource.h"
#include "fe/resources/ResourceManager.h"

#include <string_view>

namespace fe {

// Loads a bitmap font: a metrics document plus the atlas texture it names.
//
// The font file is JSON written by `tools/gen_font_atlas.py`. It names the atlas
// by path, and the loader resolves that through the manager's GPU texture scheme
// so the atlas is one `Texture2D` shared with anything else that draws it. The
// atlas is therefore an ordinary texture the existing loader already handles;
// only the metrics need this loader.
class FontLoader final : public ResourceLoader {
public:
    explicit FontLoader(ResourceManager& manager) : manager_(&manager) {}

    // Handles the "font" scheme, so a font reference reads as
    // "font:assets/hud.fefont" and is distinct from the bare path of the metrics
    // file.
    [[nodiscard]] std::string_view scheme() const noexcept override { return "font"; }

    [[nodiscard]] bool supports(std::string_view extension) const noexcept override;
    [[nodiscard]] Result<std::shared_ptr<Resource>> load(std::string_view path) override;

    // The cache key a component should store for a font file, e.g.
    // "assets/hud.fefont" -> "font:assets/hud.fefont". Mirrors
    // Texture2DLoader::reference_key, so a reference survives serialization and
    // resolves back through the manager.
    [[nodiscard]] static std::string reference_key(std::string_view file_path);

private:
    ResourceManager* manager_;
};

} // namespace fe
