// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/resources/Resource.h"
#include "fe/resources/ResourceManager.h"

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace fe {

// Loads glTF 2.0 meshes into Mesh resources.
//
// Handles the "gltf" scheme ("gltf:assets/hero.glb"), keeping mesh loading
// distinct from image loading in the cache. Both forms are read:
//
//   * .gltf - the JSON scene description, with buffers as external URIs, as
//     base64 data URIs, or (rarely) absent.
//   * .glb  - the single-file binary container: a header, a JSON chunk, and a
//     binary chunk the buffers point into.
//
// The engine's Mesh is one indexed triangle list with a fixed
// position/normal/uv vertex (ADR-0010), so one glTF primitive maps to one Mesh.
// A file with several primitives yields several meshes; load() returns the
// first and load_all() returns them in document order. See ADR-0040 for what is
// supported and what is deliberately not (materials, skinning, animation).
class GltfLoader final : public ResourceLoader {
public:
    [[nodiscard]] std::string_view scheme() const noexcept override { return "gltf"; }

    [[nodiscard]] bool supports(std::string_view extension) const noexcept override;

    // Loads the file's first primitive as a Mesh.
    [[nodiscard]] Result<std::shared_ptr<Resource>> load(std::string_view path) override;

    // Loads every primitive in the file, in document order. Returns an error
    // when the file has no primitives the loader can decode.
    [[nodiscard]] Result<std::vector<std::shared_ptr<Resource>>> load_all(std::string_view path);

    // Returns the cache key a reference should use to resolve to a mesh, e.g.
    // "assets/hero.glb" -> "gltf:assets/hero.glb". A component that stores a
    // mesh stores this key, so the reference survives serialization and reloads
    // through the ordinary ResourceManager path.
    [[nodiscard]] static std::string reference_key(std::string_view file_path);

    // Parses glTF bytes. Exposed separately from load() so the parser is
    // testable without a file on disk; `path` is used only to resolve relative
    // external buffer URIs and to name the resulting resources. The bytes may be
    // either the JSON form or the GLB container; the magic number decides.
    [[nodiscard]] static Result<std::vector<std::shared_ptr<Resource>>> parse(std::string_view path,
                                                                              std::string_view bytes);

    // The files a glTF names besides itself, as paths relative to the glTF's own
    // directory: its external buffers (`.bin`) and its image sources. Data URIs
    // and the GLB binary chunk are self-contained and contribute nothing. An
    // empty list is returned for bytes that are not a glTF, so a caller can ask
    // about any asset.
    [[nodiscard]] static std::vector<std::string> external_dependencies(std::string_view bytes);

    // A reader for `pack::expand_asset_dependencies` that understands glTF and
    // ignores every other format: it resolves the glTF's external references
    // against the glTF's own directory, which is how the loader resolves them at
    // load time.
    //
    // Pruning keeps the assets a scene names, but a glTF's buffers and images are
    // named *inside the glTF*, not by the scene, so without this a pruned pack
    // would ship the glTF and drop its data - a mesh that loads and then fails to
    // decode, or a material whose maps never resolve.
    [[nodiscard]] static std::function<std::vector<std::string>(std::string_view, std::string_view)>
    dependency_reader();
};

} // namespace fe
