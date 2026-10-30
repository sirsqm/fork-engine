// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

// The low-level glTF 2.0 reading helpers, shared by the two halves of the
// importer: `GltfLoader.cpp` (meshes) and `GltfScene.cpp` (nodes, skins, and
// animations).
//
// Why a shared header rather than one translation unit. The mesh half and the
// scene half read the same document through the same primitives -- a buffer
// view resolved to a byte range, an accessor flattened to floats, a GLB split
// into its chunks -- and neither owns them. Duplicating the byte readers would
// put the endianness and bounds rules in two places that could drift; folding
// both halves into one file would make a single 1600-line unit out of two
// concerns that are only coupled by the format. An internal header keeps one
// definition of each rule.
//
// Everything here is `inline` in `fe::gltf_detail`, so the header may be
// included by more than one translation unit. It is not installed and not part
// of the public API; the format rules it encodes are documented in ADR-0040 and
// ADR-0104.

#include "fe/anim/AnimationClip.h"
#include "fe/anim/Skeleton.h"
#include "fe/anim/SkinnedMesh.h"
#include "fe/core/Error.h"
#include "fe/core/Math.h"
#include "fe/core/Types.h"
#include "fe/mesh/SkinnedVertex3D.h"
#include "fe/mesh/Vertex3D.h"
#include "fe/scene/Transform.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstring>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fe::gltf_detail {

using json = nlohmann::json;

// Fills a flat normal for every triangle whose vertices arrived without one, so a
// file that omits NORMAL still lights. Shared by the static and skinned decoders,
// because the rule is about the triangle, not about which vertex struct holds it.
inline void apply_flat_normals(std::vector<Vertex3D>& vertices, const std::vector<u32>& indices) {
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

inline constexpr u32 kGlbMagic = 0x46546C67u;     // "glTF"
inline constexpr u32 kGlbJsonChunk = 0x4E4F534Au; // "JSON"
inline constexpr u32 kGlbBinChunk = 0x004E4942u;  // "BIN\0"

inline constexpr u32 kComponentI8 = 5120;
inline constexpr u32 kComponentU8 = 5121;
inline constexpr u32 kComponentI16 = 5122;
inline constexpr u32 kComponentU16 = 5123;
inline constexpr u32 kComponentU32 = 5125;
inline constexpr u32 kComponentF32 = 5126;

inline constexpr u32 kModeTriangles = 4;

// Reads a little-endian integer from raw bytes. glTF is little-endian on every
// platform, so assembling from bytes rather than reinterpreting is correct
// everywhere and avoids a host-endian assumption.
[[nodiscard]] inline u16 read_u16_le(const u8* p) noexcept {
    return static_cast<u16>(p[0]) | static_cast<u16>(static_cast<u16>(p[1]) << 8);
}

[[nodiscard]] inline u32 read_u32_le(const u8* p) noexcept {
    return static_cast<u32>(p[0]) | (static_cast<u32>(p[1]) << 8) | (static_cast<u32>(p[2]) << 16) |
           (static_cast<u32>(p[3]) << 24);
}

[[nodiscard]] inline bool is_gltf_extension(std::string_view extension) noexcept {
    return extension == ".gltf" || extension == ".glb";
}

// The number of components in an accessor type, or 0 for a type this loader does
// not decode.
[[nodiscard]] inline int type_components(std::string_view type) noexcept {
    if (type == "SCALAR") {
        return 1;
    }
    if (type == "VEC2") {
        return 2;
    }
    if (type == "VEC3") {
        return 3;
    }
    if (type == "VEC4") {
        return 4;
    }
    if (type == "MAT4") {
        return 16;
    }
    return 0;
}

[[nodiscard]] inline usize component_size(u32 component_type) noexcept {
    switch (component_type) {
    case kComponentI8:
    case kComponentU8:
        return 1;
    case kComponentI16:
    case kComponentU16:
        return 2;
    case kComponentU32:
    case kComponentF32:
        return 4;
    default:
        return 0;
    }
}

[[nodiscard]] inline bool read_component_as_f32(const u8* p, u32 component_type, bool normalized, f32& out) noexcept {
    switch (component_type) {
    case kComponentF32: {
        const u32 bits = read_u32_le(p);
        std::memcpy(&out, &bits, sizeof(f32));
        return true;
    }
    case kComponentI8: {
        const auto value = static_cast<i8>(p[0]);
        out = normalized ? std::max(static_cast<f32>(value) / 127.0f, -1.0f) : static_cast<f32>(value);
        return true;
    }
    case kComponentU8:
        out = normalized ? static_cast<f32>(p[0]) / 255.0f : static_cast<f32>(p[0]);
        return true;
    case kComponentI16: {
        const auto value = static_cast<i16>(read_u16_le(p));
        out = normalized ? std::max(static_cast<f32>(value) / 32767.0f, -1.0f) : static_cast<f32>(value);
        return true;
    }
    case kComponentU16:
        out = normalized ? static_cast<f32>(read_u16_le(p)) / 65535.0f : static_cast<f32>(read_u16_le(p));
        return true;
    case kComponentU32:
        out = static_cast<f32>(read_u32_le(p));
        return true;
    default:
        return false;
    }
}

[[nodiscard]] inline bool read_component_as_u32(const u8* p, u32 component_type, u32& out) noexcept {
    switch (component_type) {
    case kComponentU8:
        out = p[0];
        return true;
    case kComponentU16:
        out = read_u16_le(p);
        return true;
    case kComponentU32:
        out = read_u32_le(p);
        return true;
    default:
        return false;
    }
}

// Decodes a standard base64 string (with or without padding) into bytes.
[[nodiscard]] inline Result<std::vector<u8>> decode_base64(std::string_view text) {
    auto value_of = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') {
            return c - 'A';
        }
        if (c >= 'a' && c <= 'z') {
            return c - 'a' + 26;
        }
        if (c >= '0' && c <= '9') {
            return c - '0' + 52;
        }
        if (c == '+') {
            return 62;
        }
        if (c == '/') {
            return 63;
        }
        return -1;
    };

    std::vector<u8> out;
    out.reserve(text.size() / 4 * 3);
    u32 buffer = 0;
    int bits = 0;
    for (const char c : text) {
        if (c == '=') {
            break; // Padding: everything after is ignored.
        }
        if (c == '\n' || c == '\r' || c == ' ' || c == '\t') {
            continue; // Tolerate wrapped base64.
        }
        const int value = value_of(c);
        if (value < 0) {
            return Error("glTF data URI: invalid base64 character");
        }
        buffer = (buffer << 6) | static_cast<u32>(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<u8>((buffer >> bits) & 0xFFu));
        }
    }
    return out;
}

// Reads the bytes a glTF buffer URI refers to. A data URI is decoded here; any
// other URI is a relative path handed to `read_uri`.
[[nodiscard]] inline Result<std::vector<u8>>
read_buffer_uri(std::string_view uri, const std::function<Result<std::vector<u8>>(std::string_view)>& read_uri) {
    constexpr std::string_view prefix = "data:";
    if (uri.rfind(prefix, 0) == 0) {
        const usize comma = uri.find(',');
        if (comma == std::string_view::npos) {
            return Error("glTF data URI has no comma");
        }
        const std::string_view meta = uri.substr(prefix.size(), comma - prefix.size());
        const std::string_view payload = uri.substr(comma + 1);
        if (meta.find(";base64") == std::string_view::npos) {
            return Error("glTF data URI is not base64 (only base64 buffer data is supported)");
        }
        return decode_base64(payload);
    }
    return read_uri(uri);
}

// A buffer view and accessor resolved to a byte range inside a decoded buffer.
struct AccessorView {
    const std::vector<u8>* bytes = nullptr;
    usize base = 0;   // bufferView.byteOffset + accessor.byteOffset
    usize stride = 0; // Element stride in bytes.
    usize count = 0;
    usize element_size = 0;
    u32 component_type = 0;
    int components = 0;
    bool normalized = false;
};

[[nodiscard]] inline Result<AccessorView> resolve_accessor(const json& doc, const std::vector<std::vector<u8>>& buffers,
                                                           usize accessor_index) {
    if (!doc.contains("accessors") || accessor_index >= doc.at("accessors").size()) {
        return Error("glTF accessor index " + std::to_string(accessor_index) + " is out of range");
    }
    const json& accessor = doc.at("accessors").at(accessor_index);

    if (!accessor.contains("bufferView")) {
        return Error("glTF accessor has no bufferView (sparse and zero-filled accessors are not supported)");
    }
    const usize buffer_view_index = accessor.at("bufferView").get<usize>();
    if (!doc.contains("bufferViews") || buffer_view_index >= doc.at("bufferViews").size()) {
        return Error("glTF bufferView index " + std::to_string(buffer_view_index) + " is out of range");
    }
    const json& view = doc.at("bufferViews").at(buffer_view_index);

    const usize buffer_index = view.at("buffer").get<usize>();
    if (buffer_index >= buffers.size()) {
        return Error("glTF bufferView refers to buffer " + std::to_string(buffer_index) + " which does not exist");
    }

    AccessorView resolved;
    resolved.bytes = &buffers[buffer_index];
    resolved.component_type = accessor.at("componentType").get<u32>();
    resolved.components = type_components(accessor.at("type").get<std::string>());
    resolved.normalized = accessor.value("normalized", false);
    resolved.count = accessor.at("count").get<usize>();

    if (resolved.components == 0) {
        return Error("glTF accessor type is not a vertex-attribute type");
    }
    const usize component = component_size(resolved.component_type);
    if (component == 0) {
        return Error("glTF accessor has an unsupported componentType");
    }

    const usize view_offset = view.value("byteOffset", static_cast<usize>(0));
    const usize view_length = view.at("byteLength").get<usize>();
    const usize accessor_offset = accessor.value("byteOffset", static_cast<usize>(0));
    resolved.element_size = component * static_cast<usize>(resolved.components);
    resolved.stride = view.contains("byteStride") ? view.at("byteStride").get<usize>() : resolved.element_size;
    if (resolved.stride == 0) {
        resolved.stride = resolved.element_size;
    }
    resolved.base = view_offset + accessor_offset;

    if (resolved.bytes->size() < view_offset + view_length) {
        return Error("glTF bufferView reads past the end of its buffer");
    }
    // The last element must fit inside the view.
    if (resolved.count > 0) {
        const usize last = resolved.base + (resolved.count - 1) * resolved.stride + resolved.element_size;
        if (last > view_offset + view_length || last > resolved.bytes->size()) {
            return Error("glTF accessor reads past the end of its bufferView");
        }
    }
    return resolved;
}

// Flattens an accessor into `count * components` floats.
[[nodiscard]] inline Result<std::vector<f32>> read_accessor_floats(const json& doc,
                                                                   const std::vector<std::vector<u8>>& buffers,
                                                                   usize accessor_index, int expected_components) {
    auto view = resolve_accessor(doc, buffers, accessor_index);
    if (!view) {
        return view.error();
    }
    const AccessorView& resolved = view.value();
    if (resolved.components != expected_components) {
        return Error("glTF accessor has " + std::to_string(resolved.components) + " components, expected " +
                     std::to_string(expected_components));
    }

    std::vector<f32> out;
    out.reserve(resolved.count * static_cast<usize>(expected_components));
    for (usize i = 0; i < resolved.count; ++i) {
        const u8* element = resolved.bytes->data() + resolved.base + i * resolved.stride;
        for (int c = 0; c < expected_components; ++c) {
            f32 value = 0.0f;
            if (!read_component_as_f32(element + static_cast<usize>(c) * component_size(resolved.component_type),
                                       resolved.component_type, resolved.normalized, value)) {
                return Error("glTF accessor componentType cannot be decoded");
            }
            out.push_back(value);
        }
    }
    return out;
}

// Flattens an index accessor into `count` u32 indices.
[[nodiscard]] inline Result<std::vector<u32>>
read_accessor_indices(const json& doc, const std::vector<std::vector<u8>>& buffers, usize accessor_index) {
    auto view = resolve_accessor(doc, buffers, accessor_index);
    if (!view) {
        return view.error();
    }
    const AccessorView& resolved = view.value();
    if (resolved.components != 1) {
        return Error("glTF index accessor must be SCALAR");
    }

    std::vector<u32> out;
    out.reserve(resolved.count);
    for (usize i = 0; i < resolved.count; ++i) {
        const u8* element = resolved.bytes->data() + resolved.base + i * resolved.stride;
        u32 value = 0;
        if (!read_component_as_u32(element, resolved.component_type, value)) {
            return Error("glTF index accessor componentType must be UNSIGNED_BYTE, UNSIGNED_SHORT, or UNSIGNED_INT");
        }
        out.push_back(value);
    }
    return out;
}

// Splits a GLB container into its JSON text and (optional) binary chunk.
[[nodiscard]] inline Error parse_glb(std::string_view bytes, std::string& json_text, std::string& bin_chunk) {
    if (bytes.size() < 12) {
        return Error("glTF: GLB file is shorter than its 12-byte header");
    }
    const auto* base = reinterpret_cast<const u8*>(bytes.data());
    if (read_u32_le(base) != kGlbMagic) {
        return Error("glTF: not a GLB file (bad magic)");
    }
    const u32 version = read_u32_le(base + 4);
    if (version != 2) {
        return Error("glTF: unsupported GLB version " + std::to_string(version) + " (expected 2)");
    }
    const u32 declared_length = read_u32_le(base + 8);
    if (declared_length > bytes.size()) {
        return Error("glTF: GLB declares a length longer than the file");
    }

    usize offset = 12;
    bool found_json = false;
    while (offset + 8 <= bytes.size()) {
        const u32 chunk_length = read_u32_le(base + offset);
        const u32 chunk_type = read_u32_le(base + offset + 4);
        offset += 8;
        if (offset + chunk_length > bytes.size()) {
            return Error("glTF: GLB chunk runs past the end of the file");
        }
        if (chunk_type == kGlbJsonChunk) {
            json_text.assign(bytes.substr(offset, chunk_length));
            found_json = true;
        } else if (chunk_type == kGlbBinChunk) {
            bin_chunk.assign(bytes.substr(offset, chunk_length));
        }
        offset += chunk_length;
    }

    if (!found_json) {
        return Error("glTF: GLB file has no JSON chunk");
    }
    return Error::none();
}

// Reads a JSON array of `count` numbers into a Vec, or returns `fallback` when
// the key is absent or the array is the wrong length. glTF writes these as
// [r, g, b] / [r, g, b, a] number arrays, and a malformed one is treated as
// absent rather than failing the whole import: a colour is decoration, and
// refusing an otherwise-valid mesh over it would make the loader useless
// against a slightly-off exporter.
[[nodiscard]] inline Vec3 read_vec3(const json& object, const char* key, const Vec3& fallback) {
    if (!object.contains(key) || !object.at(key).is_array() || object.at(key).size() != 3) {
        return fallback;
    }
    return Vec3{object.at(key).at(0).get<f32>(), object.at(key).at(1).get<f32>(), object.at(key).at(2).get<f32>()};
}

[[nodiscard]] inline Vec4 read_vec4(const json& object, const char* key, const Vec4& fallback) {
    if (!object.contains(key) || !object.at(key).is_array() || object.at(key).size() != 4) {
        return fallback;
    }
    return Vec4{object.at(key).at(0).get<f32>(), object.at(key).at(1).get<f32>(), object.at(key).at(2).get<f32>(),
                object.at(key).at(3).get<f32>()};
}

// The texture key a glTF texture slot refers to, or "" when the slot is absent
// or names a texture this loader cannot resolve. `slot` is a glTF textureInfo
// object (`{"index": 0, "texCoord": 0}`); only `index` matters here.
[[nodiscard]] inline std::string texture_key_from_slot(const json& holder, const char* key,
                                                       const std::function<std::string(usize)>& texture_uri) {
    if (!holder.contains(key) || !holder.at(key).is_object()) {
        return {};
    }
    const json& info = holder.at(key);
    if (!info.contains("index")) {
        return {};
    }
    return texture_uri(info.at("index").get<usize>());
}

// A parsed glTF document: its JSON tree and every buffer decoded into bytes.
struct Document {
    json root;
    std::vector<std::vector<u8>> buffers;
};

// Parses glTF bytes into a document, resolving every buffer.
//
// `read_uri` supplies the bytes for an external buffer URI, resolved relative to
// the glTF's own directory by the caller. A data URI is decoded here and the GLB
// binary chunk needs no read at all. The version check accepts any 2.x and
// rejects an obvious 1.x, which is the same contract the mesh loader states.
[[nodiscard]] inline Result<Document>
decode_document(std::string_view path, std::string_view bytes,
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

    Document document;
    try {
        document.root = json::parse(json_text);
    } catch (const json::parse_error& e) {
        return Error(std::string("glTF parse error: ") + e.what());
    }

    if (document.root.contains("asset") && document.root.at("asset").contains("version")) {
        const std::string version = document.root.at("asset").at("version").get<std::string>();
        if (!version.empty() && version[0] != '2') {
            return Error("glTF asset version '" + version + "' is not supported (expected 2.x)");
        }
    }

    if (document.root.contains("buffers")) {
        document.buffers.reserve(document.root.at("buffers").size());
        for (const json& buffer : document.root.at("buffers")) {
            if (buffer.contains("uri")) {
                const std::string uri = buffer.at("uri").get<std::string>();
                auto read = read_buffer_uri(uri, read_uri);
                if (!read) {
                    return read.error();
                }
                document.buffers.push_back(std::move(read.value()));
            } else {
                if (bin_chunk.empty()) {
                    return Error("glTF buffer has no uri and the file has no binary chunk");
                }
                document.buffers.push_back(std::vector<u8>(bin_chunk.begin(), bin_chunk.end()));
            }
        }
    }

    (void)path;
    return document;
}

// The node's local transform: either the `matrix` property, or the TRS triple.
//
// glTF lets a node carry either a 4x4 column-major `matrix` or separate
// `translation`/`rotation`/`scale`, and a well-formed file uses exactly one.
// Both are read here so a joint's rest pose is right whichever the exporter
// chose. A `matrix` node is decomposed through `Transform::from_matrix`, which
// recovers translation, rotation, and scale exactly for the TRS-shaped matrix
// every exporter writes for a joint; a matrix carrying a shear would lose it,
// which `Transform` has no field for and a rig does not use.
[[nodiscard]] inline fe::Transform read_node_transform(const json& node) {
    fe::Transform transform;
    if (node.contains("matrix") && node.at("matrix").is_array() && node.at("matrix").size() == 16) {
        fe::Mat4 matrix;
        for (usize i = 0; i < 16; ++i) {
            matrix.m[i] = node.at("matrix").at(i).get<f32>();
        }
        return fe::Transform::from_matrix(matrix);
    }
    transform.position = read_vec3(node, "translation", transform.position);
    if (node.contains("rotation") && node.at("rotation").is_array() && node.at("rotation").size() == 4) {
        const fe::Quat rotation{node.at("rotation").at(0).get<f32>(), node.at("rotation").at(1).get<f32>(),
                                node.at("rotation").at(2).get<f32>(), node.at("rotation").at(3).get<f32>()};
        transform.rotation = rotation.normalized().to_euler();
    }
    transform.scale = read_vec3(node, "scale", transform.scale);
    return transform;
}

// Decodes one skinned glTF primitive into a `SkinnedVertex3D` list and its
// indices.
//
// Same attribute reads as the static path, plus `JOINTS_0` and `WEIGHTS_0`. The
// joint indices arrive as an integer accessor (UNSIGNED_BYTE or UNSIGNED_SHORT in
// practice) and are read through `read_component_as_u32`, so a `u16` joint index
// is not reinterpreted as a float. The weights are read as floats; a vertex whose
// weights do not sum to one is left as authored, because the skinner normalises
// (the CPU skinner and the shader both do) and the loader must not silently
// change what the artist wrote.
//
// A primitive with no `JOINTS_0`/`WEIGHTS_0` is not skinned even if its mesh
// names a skin; that is a valid glTF (a mesh's primitives may mix) and the caller
// treats it as static.
[[nodiscard]] inline Result<std::vector<SkinnedVertex3D>>
read_skinned_vertices(const json& doc, const std::vector<std::vector<u8>>& buffers, const json& primitive,
                      const std::vector<u32>& indices) {
    const json& attributes = primitive.at("attributes");
    if (!attributes.contains("JOINTS_0") || !attributes.contains("WEIGHTS_0")) {
        return Error("glTF primitive has no JOINTS_0/WEIGHTS_0");
    }

    auto positions = read_accessor_floats(doc, buffers, attributes.at("POSITION").get<usize>(), 3);
    if (!positions) {
        return positions.error();
    }
    auto joint_floats = read_accessor_floats(doc, buffers, attributes.at("JOINTS_0").get<usize>(), 4);
    if (!joint_floats) {
        return joint_floats.error();
    }
    auto weights = read_accessor_floats(doc, buffers, attributes.at("WEIGHTS_0").get<usize>(), 4);
    if (!weights) {
        return weights.error();
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
    std::vector<SkinnedVertex3D> vertices;
    vertices.reserve(vertex_count);
    for (usize i = 0; i < vertex_count; ++i) {
        SkinnedVertex3D vertex{};
        vertex.position = Vec3{positions.value()[i * 3], positions.value()[i * 3 + 1], positions.value()[i * 3 + 2]};
        if (i < normals.size()) {
            vertex.normal = normals[i];
        }
        if (i < texcoords.size()) {
            vertex.uv = texcoords[i];
        }
        for (usize c = 0; c < 4; ++c) {
            // A joint index past 65535 cannot be represented by the vertex type;
            // clamp rather than wrap, so a bad index reads as the last joint
            // instead of a small, plausible-looking one.
            const f32 joint = joint_floats.value()[i * 4 + c];
            vertex.joints[c] = joint < 0.0f ? 0 : static_cast<u16>(joint > 65535.0f ? 65535.0f : joint);
            vertex.weights[c] = weights.value()[i * 4 + c];
        }
        vertices.push_back(vertex);
    }

    // Flat-normal fill uses the static vertex form; it is the same rule and the
    // same function, so a skinned primitive with no NORMAL gets the same normals
    // a static one would.
    std::vector<Vertex3D> as_static;
    as_static.reserve(vertices.size());
    for (const SkinnedVertex3D& vertex : vertices) {
        as_static.push_back(to_static_vertex(vertex));
    }
    apply_flat_normals(as_static, indices);
    for (usize i = 0; i < vertices.size(); ++i) {
        vertices[i].normal = as_static[i].normal;
    }
    return vertices;
}

// Decodes every `skins` entry into an `anim::Skeleton`.
//
// The mapping from glTF to `Skeleton` is nearly direct, with two wrinkles the
// format forces:
//
//   * A skin's `joints` are *node* indices, and a joint's parent may be a node
//     that is not itself a joint (an armature root an exporter inserted above the
//     hips). `Skeleton` parents are indices into its own joint array, so a joint
//     whose glTF parent is not a joint becomes a root. That is the correct
//     reading: the non-joint ancestor contributes a static transform that is
//     already folded into the child's world matrix, and the palette walk needs a
//     parent that exists in the array.
//   * `inverseBindMatrices` is a separate accessor, one MAT4 per joint, in the
//     same order as `joints`. When it is absent the inverse bind matrices are
//     derived from the rest pose, which is what a skin authored in code expects
//     and what `compute_inverse_bind_matrices()` does.
//
// A skin with an empty `joints` array, or one whose node indices are out of
// range, is skipped rather than producing a rig that cannot be posed.
[[nodiscard]] inline Result<std::vector<std::shared_ptr<anim::Skeleton>>>
decode_skins(const json& doc, const std::vector<std::vector<u8>>& buffers, std::string_view path) {
    std::vector<std::shared_ptr<anim::Skeleton>> skeletons;
    if (!doc.contains("skins") || !doc.at("skins").is_array()) {
        return skeletons;
    }
    const json& nodes = doc.contains("nodes") ? doc.at("nodes") : json::array();

    usize skin_index = 0;
    for (const json& skin : doc.at("skins")) {
        const usize this_index = skin_index++;
        if (!skin.contains("joints") || !skin.at("joints").is_array() || skin.at("joints").empty()) {
            continue;
        }

        std::vector<usize> joint_nodes;
        joint_nodes.reserve(skin.at("joints").size());
        for (const json& joint : skin.at("joints")) {
            const usize node_index = joint.get<usize>();
            if (node_index >= nodes.size()) {
                return Error("glTF skin " + std::to_string(this_index) + " names node " + std::to_string(node_index) +
                             " which does not exist");
            }
            joint_nodes.push_back(node_index);
        }

        // node index -> position in the joint array, so a parent lookup is O(1)
        // and a parent that is not a joint is detectable.
        std::vector<i32> joint_of_node(nodes.size(), -1);
        for (usize i = 0; i < joint_nodes.size(); ++i) {
            joint_of_node[joint_nodes[i]] = static_cast<i32>(i);
        }

        std::vector<anim::Skeleton::Joint> joints;
        joints.reserve(joint_nodes.size());
        for (const usize node_index : joint_nodes) {
            anim::Skeleton::Joint joint;
            const json& node = nodes.at(node_index);
            joint.name = node.value("name", std::string{});
            if (joint.name.empty()) {
                joint.name = "joint_" + std::to_string(node_index);
            }
            joint.rest = read_node_transform(node);
            joints.push_back(std::move(joint));
        }

        // A joint's parent is the joint that lists it as a child. A node may be
        // listed by exactly one parent in a valid glTF; a malformed file that
        // lists it twice keeps the first, which the hierarchy check then reports.
        for (usize node_index = 0; node_index < nodes.size(); ++node_index) {
            if (!nodes.at(node_index).contains("children")) {
                continue;
            }
            for (const json& child : nodes.at(node_index).at("children")) {
                const usize child_index = child.get<usize>();
                if (child_index >= joint_of_node.size() || joint_of_node[child_index] < 0) {
                    continue; // The child is not a joint of this skin.
                }
                const i32 parent_joint = joint_of_node[node_index];
                if (parent_joint >= 0) {
                    anim::Skeleton::Joint& target = joints[static_cast<usize>(joint_of_node[child_index])];
                    if (target.parent == anim::Skeleton::kNoParent) {
                        target.parent = parent_joint;
                    }
                }
            }
        }

        if (skin.contains("inverseBindMatrices")) {
            const usize accessor_index = skin.at("inverseBindMatrices").get<usize>();
            auto matrices = read_accessor_floats(doc, buffers, accessor_index, 16);
            if (!matrices) {
                return matrices.error();
            }
            const std::vector<f32>& flat = matrices.value();
            if (flat.size() / 16 < joints.size()) {
                return Error("glTF skin " + std::to_string(this_index) +
                             " has fewer inverse bind matrices than joints");
            }
            for (usize i = 0; i < joints.size(); ++i) {
                Mat4 matrix;
                for (usize element = 0; element < 16; ++element) {
                    matrix.m[element] = flat[i * 16 + element];
                }
                joints[i].inverse_bind = matrix;
            }
        }

        std::string name(path);
        name += "#skin";
        name += std::to_string(this_index);
        auto skeleton = std::make_shared<anim::Skeleton>(std::move(name), std::move(joints));
        if (!skin.contains("inverseBindMatrices")) {
            skeleton->compute_inverse_bind_matrices();
        }
        skeletons.push_back(std::move(skeleton));
    }
    return skeletons;
}

// Decodes every `animations` entry into an `anim::AnimationClip`.
//
// A glTF animation is a flat list of channels, each naming a `(node, path)` pair
// and a sampler that holds the times and values. `AnimationClip` groups by joint:
// one `JointTrack` per animated node, holding up to three channels. So this
// inverts the format's shape -- channels to tracks -- which is the one piece of
// real translation in the import.
//
// The path strings are glTF's: "translation", "rotation", "scale". A path the
// engine does not model ("weights", for morph targets) is skipped, and a node
// whose channels are all skipped contributes no track.
//
// CUBICSPLINE is the one place the value accessor's shape changes: each key
// carries an in-tangent, the value, and an out-tangent, so the accessor holds
// three times as many elements and the value is the middle one. glTF also writes
// the tangents for a rotation as quaternions, which the track stores as such.
//
// A track's `joint_name` is the node's name, resolved to a joint index by
// `AnimationClip::bind` against whichever skin the caller pairs it with. That is
// deliberate: a glTF animation is independent of a skin, and one walk clip is
// routinely usable by several rigs that share joint names.
[[nodiscard]] inline Result<std::vector<std::shared_ptr<anim::AnimationClip>>>
decode_animations(const json& doc, const std::vector<std::vector<u8>>& buffers, std::string_view path) {
    std::vector<std::shared_ptr<anim::AnimationClip>> clips;
    if (!doc.contains("animations") || !doc.at("animations").is_array()) {
        return clips;
    }
    const json& nodes = doc.contains("nodes") ? doc.at("nodes") : json::array();

    const auto node_name = [&nodes](usize node_index) -> std::string {
        if (node_index >= nodes.size()) {
            return "node_" + std::to_string(node_index);
        }
        std::string name = nodes.at(node_index).value("name", std::string{});
        if (name.empty()) {
            name = "node_" + std::to_string(node_index);
        }
        return name;
    };

    usize animation_index = 0;
    for (const json& animation : doc.at("animations")) {
        const usize this_index = animation_index++;
        if (!animation.contains("channels") || !animation.contains("samplers")) {
            continue;
        }

        // One track per animated node, found by name so a node with three
        // channels (a fully animated joint) lands in one track.
        std::vector<anim::JointTrack> tracks;
        const auto track_for = [&tracks](std::string name) -> anim::JointTrack& {
            for (anim::JointTrack& track : tracks) {
                if (track.joint_name == name) {
                    return track;
                }
            }
            tracks.push_back(anim::JointTrack{});
            tracks.back().joint_name = std::move(name);
            return tracks.back();
        };

        for (const json& channel : animation.at("channels")) {
            if (!channel.contains("sampler") || !channel.contains("target")) {
                continue;
            }
            const json& target = channel.at("target");
            if (!target.contains("node") || !target.contains("path")) {
                continue;
            }
            const std::string path_name = target.at("path").get<std::string>();
            if (path_name != "translation" && path_name != "rotation" && path_name != "scale") {
                continue; // e.g. "weights": a morph target the engine does not model.
            }

            const usize sampler_index = channel.at("sampler").get<usize>();
            if (sampler_index >= animation.at("samplers").size()) {
                return Error("glTF animation " + std::to_string(this_index) + " channel names sampler " +
                             std::to_string(sampler_index) + " which does not exist");
            }
            const json& sampler = animation.at("samplers").at(sampler_index);
            if (!sampler.contains("input") || !sampler.contains("output")) {
                continue;
            }

            const std::string interpolation = sampler.value("interpolation", std::string("LINEAR"));
            anim::Interpolation mode = anim::Interpolation::Linear;
            bool cubic = false;
            if (interpolation == "STEP") {
                mode = anim::Interpolation::Step;
            } else if (interpolation == "CUBICSPLINE") {
                mode = anim::Interpolation::Cubic;
                cubic = true;
            } else if (interpolation != "LINEAR") {
                return Error("glTF animation sampler uses unsupported interpolation '" + interpolation + "'");
            }

            auto times = read_accessor_floats(doc, buffers, sampler.at("input").get<usize>(), 1);
            if (!times) {
                return times.error();
            }
            const usize key_count = times.value().size();

            const bool is_rotation = path_name == "rotation";
            const int components = is_rotation ? 4 : 3;
            // CUBICSPLINE writes in-tangent, value, out-tangent per key.
            auto values = read_accessor_floats(doc, buffers, sampler.at("output").get<usize>(), components);
            if (!values) {
                return values.error();
            }
            const std::vector<f32>& flat = values.value();
            const usize per_key = cubic ? 3u : 1u;
            if (key_count == 0 || flat.size() < key_count * per_key * static_cast<usize>(components)) {
                return Error("glTF animation sampler output is shorter than its input");
            }

            anim::JointTrack& track = track_for(node_name(target.at("node").get<usize>()));
            const auto value_at = [&flat, components](usize key, usize slot) {
                const usize base = (key * 3 + slot) * static_cast<usize>(components);
                if (components == 4) {
                    return Vec4{flat[base], flat[base + 1], flat[base + 2], flat[base + 3]};
                }
                return Vec4{flat[base], flat[base + 1], flat[base + 2], 0.0f};
            };

            if (is_rotation) {
                anim::QuaternionTrack rotation_track;
                rotation_track.interpolation = mode;
                rotation_track.rotation_interpolation =
                    interpolation == "STEP" ? anim::RotationInterpolation::Nlerp : anim::RotationInterpolation::Slerp;
                rotation_track.keys.reserve(key_count);
                for (usize key = 0; key < key_count; ++key) {
                    const Vec4 value = value_at(key, cubic ? 1 : 0);
                    anim::QuaternionKey quaternion_key;
                    quaternion_key.time = times.value()[key];
                    quaternion_key.value = Quat{value.x, value.y, value.z, value.w}.normalized();
                    if (cubic) {
                        const Vec4 in = value_at(key, 0);
                        const Vec4 out = value_at(key, 2);
                        quaternion_key.in_tangent = Quat{in.x, in.y, in.z, in.w};
                        quaternion_key.out_tangent = Quat{out.x, out.y, out.z, out.w};
                    }
                    rotation_track.keys.push_back(quaternion_key);
                }
                track.rotation = std::move(rotation_track);
            } else {
                anim::VectorTrack vector_track;
                vector_track.interpolation = mode;
                vector_track.keys.reserve(key_count);
                for (usize key = 0; key < key_count; ++key) {
                    const Vec4 value = value_at(key, cubic ? 1 : 0);
                    anim::VectorKey vector_key;
                    vector_key.time = times.value()[key];
                    vector_key.value = Vec3{value.x, value.y, value.z};
                    if (cubic) {
                        const Vec4 in = value_at(key, 0);
                        const Vec4 out = value_at(key, 2);
                        vector_key.in_tangent = Vec3{in.x, in.y, in.z};
                        vector_key.out_tangent = Vec3{out.x, out.y, out.z};
                    }
                    vector_track.keys.push_back(vector_key);
                }
                if (path_name == "translation") {
                    track.translation = std::move(vector_track);
                } else {
                    track.scale = std::move(vector_track);
                }
            }
        }

        std::string name = animation.value("name", std::string{});
        if (name.empty()) {
            name = "animation_" + std::to_string(this_index);
        }
        std::string resource_path(path);
        resource_path += "#animation";
        resource_path += std::to_string(this_index);
        clips.push_back(
            std::make_shared<anim::AnimationClip>(std::move(resource_path), std::move(name), std::move(tracks)));
    }
    return clips;
}

} // namespace fe::gltf_detail
