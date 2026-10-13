// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/resources/Resource.h"
#include "fe/resources/ResourceManager.h"

#include <string_view>

namespace fe {

namespace rhi {
class Device;
}

// Loads image files into a Texture2D by decoding them (via ImageLoader) and
// uploading the pixels through the RHI.
//
// The loader needs a graphics device, so it is constructed with one. It reuses
// the manager to fetch the decoded Image, which means the decode step is shared
// with any other consumer of the same file and is not repeated per texture.
class Texture2DLoader final : public ResourceLoader {
public:
    explicit Texture2DLoader(rhi::Device& device, ResourceManager& manager);

    // Handles the "tex" scheme, keeping GPU textures distinct from the CPU
    // images that share the same files.
    [[nodiscard]] std::string_view scheme() const noexcept override { return "tex"; }

    [[nodiscard]] bool supports(std::string_view extension) const noexcept override;
    [[nodiscard]] Result<std::shared_ptr<Resource>> load(std::string_view path) override;

    // Returns the cache key a reference should use to resolve to a GPU texture,
    // e.g. "assets/hero.png" -> "tex:assets/hero.png". A component that stores a
    // texture stores this key, so the reference survives serialization and
    // reloads through the ordinary ResourceManager path.
    [[nodiscard]] static std::string reference_key(std::string_view file_path);

private:
    rhi::Device* device_;
    ResourceManager* manager_;
};

} // namespace fe
