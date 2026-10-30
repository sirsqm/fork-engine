// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
//
// The scene half of the glTF importer: nodes, skins, and animations.
//
// The mesh half (`GltfLoader.cpp`) reads a primitive's geometry. This half reads
// everything else a glTF document carries and that a skinned character needs: the
// `skins` array as `anim::Skeleton` resources, the `animations` array as
// `anim::AnimationClip` resources, and a primitive that names a skin as an
// `anim::SkinnedMesh` bound to that skeleton. The two halves share the byte-level
// readers through `GltfInternal.h`.
//
// Why this is a separate translation unit rather than more of `GltfLoader.cpp`.
// The mesh loader is about a vertex stream and a material; this is about a rig, a
// clip, and the relationships between top-level arrays. They are read together
// (one document) but reasoned about separately, and keeping them apart keeps each
// file about one thing. `GltfLoader::parse_scene` is the entry point that runs
// both.

#include "fe/mesh/GltfLoader.h"

#include "fe/core/FileSystem.h"
#include "fe/core/Log.h"
#include "fe/mesh/Mesh.h"
#include "fe/resources/Texture2DLoader.h"

#include "GltfInternal.h"

#include <algorithm>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace fe {
namespace {

using gltf_detail::decode_animations;
using gltf_detail::decode_document;
using gltf_detail::decode_skins;
using gltf_detail::json;
using gltf_detail::kModeTriangles;
using gltf_detail::read_accessor_floats;
using gltf_detail::read_accessor_indices;
using gltf_detail::read_skinned_vertices;
using gltf_detail::texture_key_from_slot;

// Decodes a glTF material into the engine's `Material`. Mirrors the mesh
// loader's, because a skinned primitive's material is the same object with the
// same mapping; duplicated rather than shared because it is a pure function of
// the document and moving it into the internal header would make that header
// depend on `Material` for one caller.
[[nodiscard]] std::optional<Material> decode_material(const json& doc, const json& primitive,
                                                      const std::function<std::string(usize)>& texture_uri) {
    if (!primitive.contains("material") || !doc.contains("materials")) {
        return std::nullopt;
    }
    const usize index = primitive.at("material").get<usize>();
    if (index >= doc.at("materials").size()) {
        return std::nullopt;
    }
    const json& source = doc.at("materials").at(index);

    Material material;
    if (source.contains("pbrMetallicRoughness") && source.at("pbrMetallicRoughness").is_object()) {
        const json& pbr = source.at("pbrMetallicRoughness");
        material.base_color = gltf_detail::read_vec4(pbr, "baseColorFactor", material.base_color);
        material.metallic = pbr.value("metallicFactor", material.metallic);
        material.roughness = pbr.value("roughnessFactor", material.roughness);
        material.base_color_texture = texture_key_from_slot(pbr, "baseColorTexture", texture_uri);
    }
    material.emissive = gltf_detail::read_vec3(source, "emissiveFactor", material.emissive);
    material.normal_texture = texture_key_from_slot(source, "normalTexture", texture_uri);
    material.emissive_texture = texture_key_from_slot(source, "emissiveTexture", texture_uri);
    material.double_sided = source.value("doubleSided", false);
    return material;
}

// Decodes one primitive, choosing the static or the skinned decode by whether the
// primitive carries skinning attributes *and* its mesh names a skin.
//
// The two conditions are checked together on purpose. A primitive with
// JOINTS_0/WEIGHTS_0 but no skin is malformed and decodes as static (the joint
// data is dropped rather than bound to a rig that does not exist); a primitive
// with a skin but no joint attributes is a valid, unskinned part of a skinned
// mesh and decodes as static. Both are the honest reading, and neither is an
// error the importer should refuse the whole file over.
[[nodiscard]] Result<std::shared_ptr<Resource>>
decode_scene_primitive(const json& doc, const std::vector<std::vector<u8>>& buffers, const json& primitive,
                       std::string name, const std::shared_ptr<anim::Skeleton>& skeleton,
                       const std::function<std::string(usize)>& texture_uri) {
    const u32 mode = primitive.value("mode", kModeTriangles);
    if (mode != kModeTriangles) {
        return Result<std::shared_ptr<Resource>>(std::shared_ptr<Resource>{});
    }
    if (!primitive.contains("attributes") || !primitive.at("attributes").contains("POSITION")) {
        return Error("glTF primitive has no POSITION attribute");
    }
    const json& attributes = primitive.at("attributes");

    auto positions = read_accessor_floats(doc, buffers, attributes.at("POSITION").get<usize>(), 3);
    if (!positions) {
        return positions.error();
    }
    const usize vertex_count = positions.value().size() / 3;
    if (vertex_count == 0) {
        return Error("glTF primitive has no vertices");
    }

    std::vector<u32> indices;
    if (primitive.contains("indices")) {
        auto read = read_accessor_indices(doc, buffers, primitive.at("indices").get<usize>());
        if (!read) {
            return read.error();
        }
        indices = std::move(read.value());
        for (const u32 index : indices) {
            if (index >= vertex_count) {
                return Error("glTF index " + std::to_string(index) + " is out of range for " +
                             std::to_string(vertex_count) + " vertices");
            }
        }
    } else {
        indices.resize(vertex_count);
        for (usize i = 0; i < vertex_count; ++i) {
            indices[i] = static_cast<u32>(i);
        }
    }

    const bool skinned = skeleton != nullptr && attributes.contains("JOINTS_0") && attributes.contains("WEIGHTS_0");
    if (skinned) {
        auto vertices = read_skinned_vertices(doc, buffers, primitive, indices);
        if (!vertices) {
            return vertices.error();
        }
        auto mesh = std::make_shared<anim::SkinnedMesh>(std::move(name), std::move(vertices.value()),
                                                        std::move(indices), skeleton);
        if (std::optional<Material> material = decode_material(doc, primitive, texture_uri); material.has_value()) {
            mesh->set_material(std::move(material));
        }
        return Result<std::shared_ptr<Resource>>(std::static_pointer_cast<Resource>(std::move(mesh)));
    }

    // Static fallback: the same vertex reads the mesh loader does, so a primitive
    // of a skinned mesh that carries no joint data is byte-identical to a static
    // one.
    std::vector<Vec3> normals;
    if (attributes.contains("NORMAL")) {
        auto read = read_accessor_floats(doc, buffers, attributes.at("NORMAL").get<usize>(), 3);
        if (!read) {
            return read.error();
        }
        const std::vector<f32>& flat = read.value();
        normals.reserve(flat.size() / 3);
        for (usize i = 0; i + 2 < flat.size(); i += 3) {
            normals.push_back(Vec3{flat[i], flat[i + 1], flat[i + 2]});
        }
    }
    std::vector<Vec2> texcoords;
    if (attributes.contains("TEXCOORD_0")) {
        auto read = read_accessor_floats(doc, buffers, attributes.at("TEXCOORD_0").get<usize>(), 2);
        if (!read) {
            return read.error();
        }
        const std::vector<f32>& flat = read.value();
        texcoords.reserve(flat.size() / 2);
        for (usize i = 0; i + 1 < flat.size(); i += 2) {
            texcoords.push_back(Vec2{flat[i], flat[i + 1]});
        }
    }

    std::vector<Vertex3D> vertices;
    vertices.reserve(vertex_count);
    for (usize i = 0; i < vertex_count; ++i) {
        Vertex3D vertex{};
        vertex.position = Vec3{positions.value()[i * 3], positions.value()[i * 3 + 1], positions.value()[i * 3 + 2]};
        if (i < normals.size()) {
            vertex.normal = normals[i];
        }
        if (i < texcoords.size()) {
            vertex.uv = texcoords[i];
        }
        vertices.push_back(vertex);
    }
    gltf_detail::apply_flat_normals(vertices, indices);

    auto mesh = std::make_shared<Mesh>(std::move(name), std::move(vertices), std::move(indices));
    if (std::optional<Material> material = decode_material(doc, primitive, texture_uri); material.has_value()) {
        mesh->set_material(std::move(material));
    }
    return Result<std::shared_ptr<Resource>>(std::static_pointer_cast<Resource>(std::move(mesh)));
}

} // namespace

Result<GltfLoader::Scene> GltfLoader::parse_scene(std::string_view path, std::string_view bytes) {
    return parse_scene_impl(path, bytes, [](std::string_view relative) { return fs::read_binary_file(relative); });
}

Result<GltfLoader::Scene> GltfLoader::load_scene(std::string_view path) {
    auto bytes = read_bytes(path);
    if (!bytes) {
        return bytes.error();
    }
    const std::string_view view(reinterpret_cast<const char*>(bytes.value().data()), bytes.value().size());
    return parse_scene_impl(path, view, [this](std::string_view relative) { return read_bytes(relative); });
}

Result<GltfLoader::Scene>
GltfLoader::parse_scene_impl(std::string_view path, std::string_view bytes,
                             const std::function<Result<std::vector<u8>>(std::string_view)>& read_uri) {
    auto document = decode_document(path, bytes, read_uri);
    if (!document) {
        return document.error();
    }
    const json& doc = document.value().root;
    const std::vector<std::vector<u8>>& buffers = document.value().buffers;

    Scene scene;

    auto skeletons = decode_skins(doc, buffers, path);
    if (!skeletons) {
        return skeletons.error();
    }
    scene.skeletons = std::move(skeletons.value());

    auto animations = decode_animations(doc, buffers, path);
    if (!animations) {
        return animations.error();
    }
    scene.animations = std::move(animations.value());

    if (!doc.contains("meshes")) {
        return Error("glTF '" + std::string(path) + "' contains no meshes");
    }

    const std::string base = fs::parent_path(path);
    const auto texture_uri = [&](usize texture_index) -> std::string {
        if (!doc.contains("textures") || texture_index >= doc.at("textures").size()) {
            return {};
        }
        const json& texture = doc.at("textures").at(texture_index);
        if (!texture.contains("source")) {
            return {};
        }
        const usize source_index = texture.at("source").get<usize>();
        if (!doc.contains("images") || source_index >= doc.at("images").size()) {
            return {};
        }
        const json& image = doc.at("images").at(source_index);
        if (!image.contains("uri")) {
            return {};
        }
        const std::string uri = image.at("uri").get<std::string>();
        if (uri.rfind("data:", 0) == 0) {
            return {};
        }
        const std::string relative = base.empty() ? uri : fs::join(base, uri);
        return Texture2DLoader::reference_key(relative);
    };

    usize primitive_index = 0;
    for (const json& mesh : doc.at("meshes")) {
        if (!mesh.contains("primitives")) {
            continue;
        }
        // A mesh's `skin` names the skin every skinned primitive of the mesh binds
        // to. A mesh with no `skin`, or a skin index out of range, yields static
        // primitives.
        std::shared_ptr<anim::Skeleton> skeleton;
        if (mesh.contains("skin")) {
            const usize skin_index = mesh.at("skin").get<usize>();
            if (skin_index < scene.skeletons.size()) {
                skeleton = scene.skeletons[skin_index];
            }
        }
        for (const json& primitive : mesh.at("primitives")) {
            std::string name(path);
            name += "#";
            name += std::to_string(primitive_index++);
            auto decoded = decode_scene_primitive(doc, buffers, primitive, std::move(name), skeleton, texture_uri);
            if (!decoded) {
                return decoded.error();
            }
            if (!decoded.value()) {
                continue; // A non-triangle primitive; deliberately skipped.
            }
            scene.meshes.push_back(std::move(decoded.value()));
        }
    }

    // Bind every clip to the first skeleton, by joint name, so a clip returned
    // from here is ready to sample. A clip whose joint names match no skin binds
    // zero tracks; that is reported, not silently dropped, because it means the
    // file's animation names nodes the skin does not contain.
    if (!scene.skeletons.empty()) {
        for (const std::shared_ptr<anim::AnimationClip>& clip : scene.animations) {
            const usize bound = clip->bind(*scene.skeletons.front());
            if (bound == 0 && clip->track_count() > 0) {
                FE_LOG_DEBUG("gltf '%s': animation '%s' bound no joints to the first skin", std::string(path).c_str(),
                             clip->name().c_str());
            }
        }
    }

    return scene;
}

} // namespace fe
