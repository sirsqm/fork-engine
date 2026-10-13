// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Math.h"
#include "fe/core/Types.h"
#include "fe/resources/Resource.h"
#include "fe/scene/Component.h"

#include <memory>

namespace fe {

// A textured quad drawn in the 2D pass.
//
// The component holds a reference to a texture resource. Holding the
// shared_ptr is what keeps the texture loaded: the ResourceManager caches
// weakly, so a sprite that owns the last reference is also the reason the
// texture is not released. This is the concrete expression of the resource
// ownership rule described in ResourceManager.
class Sprite2D final : public Component {
public:
    static constexpr const char* kTypeName = "Sprite2D";

    [[nodiscard]] const char* type_name() const noexcept override { return kTypeName; }

    // Size of the quad in world units. Zero means "use the texture size", which
    // is resolved by the renderer at draw time.
    Vec2 size{0.0f, 0.0f};

    // Tint multiplied with the sampled texel.
    Vec4 color{1.0f, 1.0f, 1.0f, 1.0f};

    // UV sub-rectangle (u0, v0, u1, v1) for atlas and sprite-sheet use.
    Vec4 uv{0.0f, 0.0f, 1.0f, 1.0f};

    // Render order within the 2D pass. Higher values draw on top.
    i32 layer = 0;

    // The texture to draw. Null is allowed (the sprite draws nothing) so a
    // scene can be authored before its assets exist.
    std::shared_ptr<Resource> texture;

    [[nodiscard]] bool has_texture() const noexcept { return texture != nullptr; }
};

} // namespace fe
