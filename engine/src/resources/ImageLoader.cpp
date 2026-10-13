// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/resources/ImageLoader.h"

#include "fe/core/FileSystem.h"
#include "fe/core/Log.h"
#include "fe/resources/Image.h"
#include "fe/resources/ImportCache.h"

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <span>
#include <string_view>
#include <vector>

namespace fe {
namespace {

[[nodiscard]] bool is_supported(std::string_view extension) noexcept {
    return extension == ".png" || extension == ".jpg" || extension == ".jpeg" || extension == ".bmp" ||
           extension == ".tga" || extension == ".gif";
}

// The decoded image is cached as width, height, then tightly packed RGBA8. It
// deliberately does not store the source path: the artifact is a function of the
// bytes, so the same image decoded from two paths is one cache entry, and a hit
// is reconstructed under the path the caller asked for.
[[nodiscard]] std::vector<u8> encode_image_artifact(const Image& image) {
    std::vector<u8> out;
    const std::vector<u8>& pixels = image.pixels();
    out.reserve(8 + pixels.size());
    const u32 width = image.width();
    const u32 height = image.height();
    for (int shift = 0; shift < 32; shift += 8) {
        out.push_back(static_cast<u8>(width >> shift));
    }
    for (int shift = 0; shift < 32; shift += 8) {
        out.push_back(static_cast<u8>(height >> shift));
    }
    out.insert(out.end(), pixels.begin(), pixels.end());
    return out;
}

// Rebuilds an Image from a cached artifact, or returns nullptr when the artifact
// does not have the shape a decoded image must have. The cache validates its own
// header; this validates the payload, so a corrupt payload is a miss rather than
// a wrong image.
[[nodiscard]] std::shared_ptr<Image> decode_image_artifact(std::string_view path, std::span<const u8> artifact) {
    if (artifact.size() < 8) {
        return nullptr;
    }
    u32 width = 0;
    u32 height = 0;
    for (int i = 0; i < 4; ++i) {
        width |= static_cast<u32>(artifact[static_cast<std::size_t>(i)]) << (8 * i);
        height |= static_cast<u32>(artifact[static_cast<std::size_t>(i) + 4]) << (8 * i);
    }
    const usize expected = static_cast<usize>(width) * static_cast<usize>(height) * 4U;
    if (artifact.size() - 8 != expected) {
        return nullptr;
    }
    std::vector<u8> pixels(artifact.begin() + 8, artifact.end());
    return std::make_shared<Image>(std::string(path), width, height, std::move(pixels));
}

} // namespace

bool ImageLoader::supports(std::string_view extension) const noexcept {
    return is_supported(extension);
}

Result<std::shared_ptr<Resource>> ImageLoader::load(std::string_view path) {
    auto bytes = fs::read_binary_file(path);
    if (!bytes) {
        return bytes.error();
    }

    const std::vector<u8>& file_data = bytes.value();
    if (file_data.empty()) {
        return Error("image file is empty: " + std::string(path));
    }

    // The CPU decode is independent of the texture import settings (filter,
    // wrap, and mipmaps affect the upload, not the RGBA8 bytes), so the artifact
    // keys on the source bytes and the importer identity alone. That is what lets
    // two textures with different filters share one decoded image. A future
    // setting that *does* change the decode would be hashed here.
    const std::string cache_key =
        cache_ != nullptr ? ImportCache::key_for(std::span<const u8>(file_data), nullptr, "image") : std::string{};
    if (cache_ != nullptr && cache_->contains(cache_key)) {
        if (auto cached = cache_->fetch(cache_key)) {
            if (auto image = decode_image_artifact(path, cached.value())) {
                return std::static_pointer_cast<Resource>(image);
            }
        }
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
        return Error("could not decode image '" + std::string(path) +
                     "': " + (reason != nullptr ? reason : "unknown error"));
    }

    const auto pixel_count = static_cast<usize>(width) * static_cast<usize>(height) * 4U;
    std::vector<u8> pixels(decoded, decoded + pixel_count);
    stbi_image_free(decoded);

    auto image = std::make_shared<Image>(std::string(path), static_cast<u32>(width), static_cast<u32>(height),
                                         std::move(pixels));
    if (cache_ != nullptr) {
        const std::vector<u8> artifact = encode_image_artifact(*image);
        if (Error error = cache_->store(cache_key, artifact); error) {
            // A failed cache write must not fail the load: the image decoded
            // fine, and the cache is only a speedup.
            FE_LOG_DEBUG("import cache store failed: %s", error.message().c_str());
        }
    }
    return std::static_pointer_cast<Resource>(image);
}

} // namespace fe
