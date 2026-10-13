// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/resources/Texture2DLoader.h"

#include "fe/core/FileSystem.h"
#include "fe/resources/Image.h"
#include "fe/resources/Texture2D.h"
#include "fe/rhi/Device.h"

namespace fe {
namespace {

[[nodiscard]] bool is_image_extension(std::string_view extension) noexcept {
    return extension == ".png" || extension == ".jpg" || extension == ".jpeg" || extension == ".bmp" ||
           extension == ".tga" || extension == ".gif";
}

} // namespace

Texture2DLoader::Texture2DLoader(rhi::Device& device, ResourceManager& manager)
    : device_(&device), manager_(&manager) {}

bool Texture2DLoader::supports(std::string_view extension) const noexcept {
    return is_image_extension(extension);
}

Result<std::shared_ptr<Resource>> Texture2DLoader::load(std::string_view path) {
    // Decode (or reuse a decoded) CPU image first. The ImageLoader registers
    // under the default scheme, so a bare path resolves to the Image.
    auto image_result = manager_->load_as<Image>(std::string(path));
    if (!image_result) {
        return image_result.error();
    }
    const std::shared_ptr<Image> image = image_result.value();

    rhi::TextureDesc desc;
    desc.name = std::string(fs::filename(path));
    desc.width = image->width();
    desc.height = image->height();
    desc.pixels = image->pixels().data();

    auto texture = device_->create_texture(desc);
    if (!texture) {
        return texture.error();
    }

    auto resource = std::make_shared<Texture2D>(std::string(path), texture.value());
    return std::static_pointer_cast<Resource>(resource);
}

std::string Texture2DLoader::reference_key(std::string_view file_path) {
    // An already-schemed path is returned unchanged so the helper is safe to
    // apply to either a bare file path or an existing reference.
    if (file_path.find(':') != std::string_view::npos) {
        return std::string(file_path);
    }
    return "tex:" + std::string(file_path);
}

} // namespace fe
