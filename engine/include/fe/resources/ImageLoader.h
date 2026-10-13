// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/resources/Resource.h"

#include <string_view>

namespace fe {

class ImportCache;

// Decodes PNG/JPEG/BMP/TGA/GIF images into an Image resource.
//
// Backed by stb_image. A single loader covers the common formats the engine
// needs today; a format that stb_image cannot handle gets its own loader rather
// than a branch inside this one.
//
// The decoded Image is cached in an ImportCache when one is supplied: a
// re-import of unchanged bytes with unchanged settings is a cache read instead
// of a re-decode (ADR-0029). A null cache (the default) leaves the loader
// behaving exactly as before, so existing construction sites are unchanged.
class ImageLoader final : public ResourceLoader {
public:
    explicit ImageLoader(ImportCache* cache = nullptr) : cache_(cache) {}

    // Handles the default (unprefixed) scheme, so "assets/hero.png" decodes to
    // an Image while "tex:assets/hero.png" uploads to a Texture2D.
    [[nodiscard]] std::string_view scheme() const noexcept override { return {}; }

    [[nodiscard]] bool supports(std::string_view extension) const noexcept override;
    [[nodiscard]] Result<std::shared_ptr<Resource>> load(std::string_view path) override;

private:
    // Borrowed; may be null, which disables caching. Owned by the ResourceManager
    // the loader is registered with.
    ImportCache* cache_ = nullptr;
};

} // namespace fe
