// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Types.h"
#include "fe/resources/Resource.h"

#include <vector>

namespace fe {

// Decoded image data in main memory: RGBA8, tightly packed.
//
// This is the CPU-side half of the asset pipeline. Decoding is separated from
// GPU upload on purpose: the same decoded image can feed a texture, a thumbnail
// in the editor, or a future importer step, and it can be unit-tested without a
// graphics context.
class Image final : public Resource {
public:
    static constexpr const char* kTypeName = "Image";

    Image(std::string path, u32 width, u32 height, std::vector<u8> pixels)
        : Resource(std::move(path)), width_(width), height_(height), pixels_(std::move(pixels)) {}

    [[nodiscard]] const char* type_name() const noexcept override { return kTypeName; }

    [[nodiscard]] u32 width() const noexcept { return width_; }
    [[nodiscard]] u32 height() const noexcept { return height_; }

    // Row-major RGBA8. Size is always width() * height() * 4.
    [[nodiscard]] const std::vector<u8>& pixels() const noexcept { return pixels_; }

    [[nodiscard]] usize memory_usage() const noexcept override { return pixels_.size(); }

private:
    u32 width_;
    u32 height_;
    std::vector<u8> pixels_;
};

} // namespace fe
