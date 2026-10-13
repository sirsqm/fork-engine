// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/resources/Resource.h"
#include "fe/rhi/Resources.h"

#include <string>

namespace fe {

// A GPU-backed 2D texture.
//
// This is the bridge between the resource system and the RHI: the resource owns
// the decoded image's GPU representation and keeps it alive, while the renderer
// talks to the rhi::Texture it exposes. Nothing above the renderer needs to
// know which graphics API produced the handle.
class Texture2D final : public Resource {
public:
    static constexpr const char* kTypeName = "Texture2D";

    Texture2D(std::string path, rhi::TextureRef texture) : Resource(std::move(path)), texture_(std::move(texture)) {}

    [[nodiscard]] const char* type_name() const noexcept override { return kTypeName; }

    [[nodiscard]] rhi::Texture& texture() const noexcept { return *texture_; }

    [[nodiscard]] u32 width() const noexcept { return texture_->width(); }
    [[nodiscard]] u32 height() const noexcept { return texture_->height(); }

    [[nodiscard]] usize memory_usage() const noexcept override { return static_cast<usize>(width()) * height() * 4U; }

private:
    rhi::TextureRef texture_;
};

} // namespace fe
