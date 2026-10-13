// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/resources/ImageLoader.h"

#include "fe/core/FileSystem.h"
#include "fe/resources/Image.h"

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

namespace fe {
namespace {

[[nodiscard]] bool is_supported(std::string_view extension) noexcept {
    return extension == ".png" || extension == ".jpg" || extension == ".jpeg" || extension == ".bmp" ||
           extension == ".tga" || extension == ".gif";
}

} // namespace

bool ImageLoader::supports(std::string_view extension) const noexcept { return is_supported(extension); }

Result<std::shared_ptr<Resource>> ImageLoader::load(std::string_view path) {
    auto bytes = fs::read_binary_file(path);
    if (!bytes) {
        return bytes.error();
    }

    const std::vector<u8>& file_data = bytes.value();
    if (file_data.empty()) {
        return Error("image file is empty: " + std::string(path));
    }

    int width = 0;
    int height = 0;
    int source_channels = 0;
    // Force 4 channels so the rest of the engine always deals with RGBA8 and
    // never has to branch on channel count.
    stbi_uc* decoded = stbi_load_from_memory(file_data.data(), static_cast<int>(file_data.size()), &width, &height,
                                             &source_channels, 4);
    if (decoded == nullptr) {
        const char* reason = stbi_failure_reason();
        return Error("could not decode image '" + std::string(path) + "': " +
                     (reason != nullptr ? reason : "unknown error"));
    }

    const auto pixel_count = static_cast<usize>(width) * static_cast<usize>(height) * 4U;
    std::vector<u8> pixels(decoded, decoded + pixel_count);
    stbi_image_free(decoded);

    auto image = std::make_shared<Image>(std::string(path), static_cast<u32>(width), static_cast<u32>(height),
                                         std::move(pixels));
    return std::static_pointer_cast<Resource>(image);
}

} // namespace fe
