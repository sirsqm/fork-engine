// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/resources/Resource.h"

#include <string_view>

namespace fe {

// Decodes PNG/JPEG/BMP/TGA/GIF images into an Image resource.
//
// Backed by stb_image. A single loader covers the common formats the engine
// needs today; a format that stb_image cannot handle gets its own loader rather
// than a branch inside this one.
class ImageLoader final : public ResourceLoader {
public:
    // Handles the default (unprefixed) scheme, so "assets/hero.png" decodes to
    // an Image while "tex:assets/hero.png" uploads to a Texture2D.
    [[nodiscard]] std::string_view scheme() const noexcept override { return {}; }

    [[nodiscard]] bool supports(std::string_view extension) const noexcept override;
    [[nodiscard]] Result<std::shared_ptr<Resource>> load(std::string_view path) override;
};

} // namespace fe
