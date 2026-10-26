// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Math.h"
#include "fe/core/Types.h"

#include <string>

namespace fe {

// A surface material: the metallic-roughness parameters of glTF 2.0.
//
// This is a plain value, not a `Resource`. A glTF material lives inside the
// mesh file that declares it, so there is nothing for the resource cache to key
// it on - a path would name the file, not the material. Keeping it a value also
// means it can be copied into a `Mesh`, compared in a test, and read by the
// renderer with no cache lookup and no GPU involvement.
//
// Texture references are stored as the same "<scheme>:<path>" keys every other
// component stores (`Texture2DLoader::reference_key`), so resolving one is the
// ordinary `ResourceManager::load` call. The material deliberately does not hold
// a `shared_ptr<Resource>`: the loader that produces it has no resource manager,
// and holding a texture here would make a material unable to exist in a
// headless test.
//
// The defaults are the glTF defaults: a fully rough, non-metallic, white-ish
// surface. A material that is default-constructed therefore describes the same
// look the engine drew before materials existed, which is what keeps a mesh with
// no imported material rendering exactly as it did.
struct Material {
    // Base colour factor, multiplied with the base colour texture when one is
    // set. Alpha is carried because glTF stores it here; the engine's opaque
    // pass ignores it, and a future transparent pass is where it becomes real.
    Vec4 base_color{0.8f, 0.8f, 0.85f, 1.0f};

    // Metallic and roughness factors, each in [0, 1]. glTF packs them into one
    // texture; the factors scale it.
    f32 metallic = 0.0f;
    f32 roughness = 0.6f;

    // Emissive colour, added after lighting. The glTF emissive factor is a
    // colour, so this is a `Vec3` rather than a scalar.
    Vec3 emissive{0.0f, 0.0f, 0.0f};

    // Texture references, as resource keys ("tex:assets/hero.png"). Empty means
    // the corresponding map is absent, which is the common case and is not an
    // error: a material may be entirely factors.
    std::string base_color_texture;
    std::string normal_texture;
    std::string emissive_texture;

    // glTF's `doubleSided`. False (back-face culling, the glTF default) is what
    // the engine has always done; the field is carried so an imported asset is
    // not silently rendered with the wrong side visible. The opaque pass does
    // not yet act on it, which is recorded in the ADR rather than implied.
    bool double_sided = false;

    [[nodiscard]] bool has_base_color_texture() const noexcept { return !base_color_texture.empty(); }
    [[nodiscard]] bool has_normal_texture() const noexcept { return !normal_texture.empty(); }
    [[nodiscard]] bool has_emissive_texture() const noexcept { return !emissive_texture.empty(); }

    // The Blinn-Phong specular strength this material implies. The renderer's
    // shading is still Blinn-Phong, so the metallic-roughness parameters are
    // projected onto it here, in one place, rather than in the shader: a
    // dielectric keeps a small specular lobe and a metal reflects almost all of
    // its base colour, which is the standard cheap mapping.
    [[nodiscard]] f32 specular_strength() const noexcept {
        const f32 dielectric = 0.04f;
        const f32 base = dielectric + (1.0f - dielectric) * metallic;
        return base * (1.0f - roughness);
    }

    // The Blinn-Phong shininess exponent implied by roughness: the standard
    // inverse-square mapping, clamped so a roughness of 0 does not produce an
    // infinite exponent and a roughness of 1 does not produce a zero one.
    [[nodiscard]] f32 shininess() const noexcept {
        const f32 clamped = roughness < 0.05f ? 0.05f : (roughness > 1.0f ? 1.0f : roughness);
        const f32 exponent = 2.0f / (clamped * clamped * clamped * clamped) - 2.0f;
        return exponent > 512.0f ? 512.0f : exponent;
    }

    // True when every field is at its default. Used to decide whether a mesh
    // carries a material worth preferring over a component's own fields, and to
    // keep an untextured, non-metallic, non-emissive import on the old path.
    [[nodiscard]] bool is_default() const noexcept {
        return base_color == Vec4{0.8f, 0.8f, 0.85f, 1.0f} && metallic == 0.0f && roughness == 0.6f &&
               emissive == Vec3{0.0f, 0.0f, 0.0f} && base_color_texture.empty() && normal_texture.empty() &&
               emissive_texture.empty() && !double_sided;
    }

    // Exact field equality. A material is a value, so a test comparing two
    // imports should be able to compare them directly.
    [[nodiscard]] bool operator==(const Material&) const noexcept = default;
};

} // namespace fe
