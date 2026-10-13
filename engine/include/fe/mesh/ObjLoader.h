// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/resources/Resource.h"
#include "fe/resources/ResourceManager.h"

#include <string_view>

namespace fe {

// Loads Wavefront OBJ meshes into a Mesh resource.
//
// Handles the "obj" scheme ("obj:assets/crate.obj"), keeping mesh loading
// distinct from image loading in the cache. The parser covers the subset the
// engine needs today: vertex positions, texture coordinates, normals, faces
// (triangles and polygons, triangulated as a fan), and negative (relative)
// indices. It ignores material and group statements rather than failing on
// them, because an OBJ with a companion .mtl is still a valid mesh.
//
// If a face has no normals, a flat normal is computed per triangle. If it has
// no texture coordinates, UVs are zeroed.
class ObjLoader final : public ResourceLoader {
public:
    [[nodiscard]] std::string_view scheme() const noexcept override { return "obj"; }

    [[nodiscard]] bool supports(std::string_view extension) const noexcept override;
    [[nodiscard]] Result<std::shared_ptr<Resource>> load(std::string_view path) override;

    // Returns the cache key a reference should use to resolve to a mesh, e.g.
    // "assets/crate.obj" -> "obj:assets/crate.obj". A component that stores a
    // mesh stores this key, so the reference survives serialization and reloads
    // through the ordinary ResourceManager path.
    [[nodiscard]] static std::string reference_key(std::string_view file_path);

    // Parses OBJ text. Exposed separately from load() so the parser is testable
    // without a file on disk.
    [[nodiscard]] static Result<std::shared_ptr<Resource>> parse(std::string_view path, std::string_view text);
};

} // namespace fe
