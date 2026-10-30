// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/mesh/GltfLoader.h"

#include "GltfInternal.h"
#include "fe/core/FileSystem.h"
#include "fe/core/Log.h"
#include "fe/mesh/Mesh.h"
#include "fe/resources/Texture2DLoader.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstring>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace fe {
namespace {

using gltf_detail::AccessorView;
using gltf_detail::decode_base64;
using gltf_detail::is_gltf_extension;
using gltf_detail::kGlbBinChunk;
using gltf_detail::kGlbJsonChunk;
using gltf_detail::kGlbMagic;
using gltf_detail::kModeTriangles;
using gltf_detail::parse_glb;
using gltf_detail::read_accessor_floats;
using gltf_detail::read_accessor_indices;
using gltf_detail::read_buffer_uri;
using gltf_detail::read_u32_le;
using gltf_detail::read_vec3;
using gltf_detail::read_vec4;
using gltf_detail::resolve_accessor;
using gltf_detail::texture_key_from_slot;
using json = nlohmann::json;

void apply_flat_normals(std::vector<Vertex3D>& vertices, const std::vector<u32>& indices) {
    for (usize i = 0; i + 2 < indices.size(); i += 3) {
        const u32 a = indices[i];
        const u32 b = indices[i + 1];
        const u32 c = indices[i + 2];
        if (vertices[a].normal.length_squared() > 0.0f && vertices[b].normal.length_squared() > 0.0f &&
            vertices[c].normal.length_squared() > 0.0f) {
            continue;
        }
        const Vec3 edge1 = vertices[b].position - vertices[a].position;
        const Vec3 edge2 = vertices[c].position - vertices[a].position;
        const Vec3 normal = edge1.cross(edge2).normalized();
        for (const u32 index : {a, b, c}) {
            if (vertices[index].normal.length_squared() == 0.0f) {
                vertices[index].normal = normal;
            }
        }
    }
}

// Decodes a glTF material into the engine's `Material`.
//
// The mapping is direct because both sides are metallic-roughness:
//
//   pbrMetallicRoughness.baseColorFactor  -> base_color
//   pbrMetallicRoughness.baseColorTexture -> base_color_texture
//   pbrMetallicRoughness.metallicFactor   -> metallic
//   pbrMetallicRoughness.roughnessFactor  -> roughness
//   emissiveFactor / emissiveTexture      -> emissive / emissive_texture
//   normalTexture                         -> normal_texture
//   doubleSided                           -> double_sided
//
// Anything the engine has no field for (occlusion, alphaMode, the KHR_*
// extensions) is ignored rather than misread into a field that means something
// else. A material index that is out of range, or a document with no materials,
// yields "no material" so the caller keeps the component's authored look.
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
        material.base_color = read_vec4(pbr, "baseColorFactor", material.base_color);
        material.metallic = pbr.value("metallicFactor", material.metallic);
        material.roughness = pbr.value("roughnessFactor", material.roughness);
        material.base_color_texture = texture_key_from_slot(pbr, "baseColorTexture", texture_uri);
    }
    material.emissive = read_vec3(source, "emissiveFactor", material.emissive);
    material.normal_texture = texture_key_from_slot(source, "normalTexture", texture_uri);
    material.emissive_texture = texture_key_from_slot(source, "emissiveTexture", texture_uri);
    material.double_sided = source.value("doubleSided", false);
    return material;
}

// Decodes one glTF primitive into a Mesh, or returns nullptr when the primitive
// is a kind this loader deliberately skips (a non-triangle mode).
//
// `texture_uri` maps a glTF texture index to a resource key ("tex:<path>"), so
// a material's texture references resolve the same way every other component's
// do. It is a callback because the mapping (index -> image URI -> resolved
// path) is a property of the document, not of the primitive.
[[nodiscard]] Result<std::shared_ptr<Mesh>> decode_primitive(const json& doc,
                                                             const std::vector<std::vector<u8>>& buffers,
                                                             const json& primitive, std::string name,
                                                             const std::function<std::string(usize)>& texture_uri) {
    const u32 mode = primitive.value("mode", kModeTriangles);
    if (mode != kModeTriangles) {
        return Result<std::shared_ptr<Mesh>>(std::shared_ptr<Mesh>{});
    }

    if (!primitive.contains("attributes") || !primitive.at("attributes").contains("POSITION")) {
        return Error("glTF primitive has no POSITION attribute");
    }
    const json& attributes = primitive.at("attributes");

    auto positions = read_accessor_floats(doc, buffers, attributes.at("POSITION").get<usize>(), 3);
    if (!positions) {
        return positions.error();
    }

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

    const usize vertex_count = positions.value().size() / 3;
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

    apply_flat_normals(vertices, indices);

    if (vertices.empty()) {
        return Error("glTF primitive has no vertices");
    }

    auto mesh = std::make_shared<Mesh>(std::move(name), std::move(vertices), std::move(indices));
    // The material lives on the primitive in glTF, so it rides along with the
    // mesh rather than becoming a separate resource.
    if (std::optional<Material> material = decode_material(doc, primitive, texture_uri); material.has_value()) {
        mesh->set_material(std::move(material));
    }
    return Result<std::shared_ptr<Mesh>>(std::move(mesh));
}

[[nodiscard]] Result<std::vector<std::shared_ptr<Resource>>>
parse_impl(std::string_view path, std::string_view bytes,
           const std::function<Result<std::vector<u8>>(std::string_view)>& read_uri) {
    std::string json_text;
    std::string bin_chunk;

    const bool is_glb = bytes.size() >= 4 && read_u32_le(reinterpret_cast<const u8*>(bytes.data())) == kGlbMagic;
    if (is_glb) {
        if (Error error = parse_glb(bytes, json_text, bin_chunk); error) {
            return error;
        }
    } else {
        json_text.assign(bytes);
    }

    json doc;
    try {
        doc = json::parse(json_text);
    } catch (const json::parse_error& e) {
        return Error(std::string("glTF parse error: ") + e.what());
    }

    // The spec version is "2.0". Accept any 2.x; reject an obvious 1.x.
    if (doc.contains("asset") && doc.at("asset").contains("version")) {
        const std::string version = doc.at("asset").at("version").get<std::string>();
        if (!version.empty() && version[0] != '2') {
            return Error("glTF asset version '" + version + "' is not supported (expected 2.x)");
        }
    }

    // Resolve every buffer: the GLB binary chunk (no uri), a data uri, or an
    // external file read relative to the glTF file's directory.
    const std::string base = fs::parent_path(path);
    std::vector<std::vector<u8>> buffers;
    if (doc.contains("buffers")) {
        buffers.reserve(doc.at("buffers").size());
        for (const json& buffer : doc.at("buffers")) {
            if (buffer.contains("uri")) {
                const std::string uri = buffer.at("uri").get<std::string>();
                auto read = read_buffer_uri(uri, [&](std::string_view relative) {
                    const std::string resolved = base.empty() ? std::string(relative) : fs::join(base, relative);
                    return read_uri(resolved);
                });
                if (!read) {
                    return read.error();
                }
                buffers.push_back(std::move(read.value()));
            } else {
                if (bin_chunk.empty()) {
                    return Error("glTF buffer has no uri and the file has no binary chunk");
                }
                buffers.push_back(std::vector<u8>(bin_chunk.begin(), bin_chunk.end()));
            }
        }
    }

    if (!doc.contains("meshes")) {
        return Error("glTF '" + std::string(path) + "' contains no meshes");
    }

    // Resolve a glTF texture index to the resource key the engine stores on a
    // component: texture -> source -> image -> uri, then the texture loader's
    // reference key. A data-URI image (a `.glb` commonly embeds its textures)
    // has no file to load, so it resolves to "" and the material falls back to
    // its factors, which is the honest outcome rather than a broken reference.
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
            return {}; // A bufferView-backed image; see the ADR.
        }
        const std::string uri = image.at("uri").get<std::string>();
        if (uri.rfind("data:", 0) == 0) {
            return {};
        }
        // A glTF image URI is relative to the glTF file, exactly as a buffer URI
        // is, so a document in a subdirectory keys its maps under that
        // subdirectory too. Keying the bare URI would name a path the file is
        // not at, and the material's map would never resolve.
        const std::string relative = base.empty() ? uri : fs::join(base, uri);
        return Texture2DLoader::reference_key(relative);
    };

    std::vector<std::shared_ptr<Resource>> meshes;
    usize skipped = 0;
    usize primitive_index = 0;
    for (const json& mesh : doc.at("meshes")) {
        if (!mesh.contains("primitives")) {
            continue;
        }
        for (const json& primitive : mesh.at("primitives")) {
            std::string name(path);
            name += "#";
            name += std::to_string(primitive_index++);
            auto decoded = decode_primitive(doc, buffers, primitive, std::move(name), texture_uri);
            if (!decoded) {
                return decoded.error();
            }
            if (!decoded.value()) {
                ++skipped; // A non-triangle primitive; deliberately skipped.
                continue;
            }
            meshes.push_back(std::static_pointer_cast<Resource>(decoded.value()));
        }
    }

    if (meshes.empty()) {
        if (skipped > 0) {
            return Error("glTF '" + std::string(path) + "' has only primitives this loader does not support");
        }
        return Error("glTF '" + std::string(path) + "' contains no primitives");
    }
    if (skipped > 0) {
        FE_LOG_DEBUG("gltf '%s': skipped %zu unsupported primitive(s)", std::string(path).c_str(), skipped);
    }
    return meshes;
}

} // namespace

bool GltfLoader::supports(std::string_view extension) const noexcept {
    return is_gltf_extension(extension);
}

std::string GltfLoader::reference_key(std::string_view file_path) {
    if (file_path.find(':') != std::string_view::npos) {
        return std::string(file_path);
    }
    return "gltf:" + std::string(file_path);
}

Result<std::vector<std::shared_ptr<Resource>>> GltfLoader::parse(std::string_view path, std::string_view bytes) {
    return parse_impl(path, bytes, [](std::string_view relative) { return fs::read_binary_file(relative); });
}

std::vector<std::string> GltfLoader::external_dependencies(std::string_view bytes) {
    std::string json_text;
    std::string bin_chunk;
    const bool is_glb = bytes.size() >= 4 && read_u32_le(reinterpret_cast<const u8*>(bytes.data())) == kGlbMagic;
    if (is_glb) {
        if (Error error = parse_glb(bytes, json_text, bin_chunk); error) {
            return {};
        }
    } else {
        json_text.assign(bytes);
    }

    // No-exceptions parse: this runs under Emscripten, where exceptions are
    // disabled, and it is asked about every asset under the pack root - most of
    // which are not JSON at all. A discarded value means "not a glTF".
    const json doc = json::parse(json_text, nullptr, /*allow_exceptions=*/false);
    if (doc.is_discarded()) {
        return {};
    }

    // Only external URIs count. A data URI is decoded from the document and the
    // GLB binary chunk is in the same file, so neither is a file to ship.
    std::vector<std::string> out;
    const auto add_uri = [&out](const json& holder) {
        if (!holder.is_object() || !holder.contains("uri") || !holder.at("uri").is_string()) {
            return;
        }
        const std::string uri = holder.at("uri").get<std::string>();
        if (uri.empty() || uri.rfind("data:", 0) == 0) {
            return;
        }
        out.push_back(uri);
    };

    if (doc.contains("buffers")) {
        for (const json& buffer : doc.at("buffers")) {
            add_uri(buffer);
        }
    }
    if (doc.contains("images")) {
        for (const json& image : doc.at("images")) {
            add_uri(image);
        }
    }

    // De-duplicate while preserving order, so a texture shared by two materials
    // is listed once.
    std::vector<std::string> unique;
    unique.reserve(out.size());
    for (std::string& uri : out) {
        if (std::find(unique.begin(), unique.end(), uri) == unique.end()) {
            unique.push_back(std::move(uri));
        }
    }
    return unique;
}

std::function<std::vector<std::string>(std::string_view, std::string_view)> GltfLoader::dependency_reader() {
    return [](std::string_view path, std::string_view bytes) {
        const std::string base = fs::parent_path(path);
        std::vector<std::string> resolved;
        for (std::string& uri : external_dependencies(bytes)) {
            resolved.push_back(fs::normalize(fs::join(base, uri)));
        }
        return resolved;
    };
}

Result<std::vector<std::shared_ptr<Resource>>> GltfLoader::load_all(std::string_view path) {
    auto bytes = read_bytes(path);
    if (!bytes) {
        return bytes.error();
    }
    const std::string_view view(reinterpret_cast<const char*>(bytes.value().data()), bytes.value().size());
    return parse_impl(path, view, [this](std::string_view relative) { return read_bytes(relative); });
}

Result<std::shared_ptr<Resource>> GltfLoader::load(std::string_view path) {
    auto all = load_all(path);
    if (!all) {
        return all.error();
    }
    if (all.value().empty()) {
        return Error("glTF '" + std::string(path) + "' produced no mesh");
    }
    return all.value().front();
}

} // namespace fe
