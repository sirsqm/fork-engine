// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/mesh/GltfLoader.h"

#include "fe/core/FileSystem.h"
#include "fe/core/Log.h"
#include "fe/mesh/Mesh.h"
#include "fe/resources/Texture2DLoader.h"

#include <nlohmann/json.hpp>

#include <cstring>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace fe {
namespace {

using json = nlohmann::json;

constexpr u32 kGlbMagic = 0x46546C67u;     // "glTF"
constexpr u32 kGlbJsonChunk = 0x4E4F534Au; // "JSON"
constexpr u32 kGlbBinChunk = 0x004E4942u;  // "BIN\0"

constexpr u32 kComponentI8 = 5120;
constexpr u32 kComponentU8 = 5121;
constexpr u32 kComponentI16 = 5122;
constexpr u32 kComponentU16 = 5123;
constexpr u32 kComponentU32 = 5125;
constexpr u32 kComponentF32 = 5126;

constexpr u32 kModeTriangles = 4;

// Reads a little-endian integer from raw bytes. glTF is little-endian on every
// platform, so assembling from bytes rather than reinterpreting is correct
// everywhere and avoids a host-endian assumption.
[[nodiscard]] u16 read_u16_le(const u8* p) noexcept {
    return static_cast<u16>(p[0]) | static_cast<u16>(static_cast<u16>(p[1]) << 8);
}

[[nodiscard]] u32 read_u32_le(const u8* p) noexcept {
    return static_cast<u32>(p[0]) | (static_cast<u32>(p[1]) << 8) | (static_cast<u32>(p[2]) << 16) |
           (static_cast<u32>(p[3]) << 24);
}

[[nodiscard]] bool is_gltf_extension(std::string_view extension) noexcept {
    return extension == ".gltf" || extension == ".glb";
}

// The number of components in an accessor type, or 0 for a type this loader does
// not decode. Only the vertex-attribute types are needed.
[[nodiscard]] int type_components(std::string_view type) noexcept {
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
    return 0;
}

[[nodiscard]] usize component_size(u32 component_type) noexcept {
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

[[nodiscard]] bool read_component_as_f32(const u8* p, u32 component_type, bool normalized, f32& out) noexcept {
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

[[nodiscard]] bool read_component_as_u32(const u8* p, u32 component_type, u32& out) noexcept {
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
[[nodiscard]] Result<std::vector<u8>> decode_base64(std::string_view text) {
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
[[nodiscard]] Result<std::vector<u8>>
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

[[nodiscard]] Result<AccessorView> resolve_accessor(const json& doc, const std::vector<std::vector<u8>>& buffers,
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
[[nodiscard]] Result<std::vector<f32>> read_accessor_floats(const json& doc,
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
[[nodiscard]] Result<std::vector<u32>>
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

// Fills a flat normal for every triangle whose vertices arrived without one,
// matching the OBJ loader's fallback so an unlit-looking asset still shades.
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

// Reads a JSON array of `count` numbers into a Vec, or returns `fallback` when
// the key is absent or the array is the wrong length. glTF writes these as
// [r, g, b] / [r, g, b, a] number arrays, and a malformed one is treated as
// absent rather than failing the whole import: a colour is decoration, and
// refusing an otherwise-valid mesh over it would make the loader useless
// against a slightly-off exporter.
[[nodiscard]] Vec3 read_vec3(const json& object, const char* key, const Vec3& fallback) {
    if (!object.contains(key) || !object.at(key).is_array() || object.at(key).size() != 3) {
        return fallback;
    }
    return Vec3{object.at(key).at(0).get<f32>(), object.at(key).at(1).get<f32>(), object.at(key).at(2).get<f32>()};
}

[[nodiscard]] Vec4 read_vec4(const json& object, const char* key, const Vec4& fallback) {
    if (!object.contains(key) || !object.at(key).is_array() || object.at(key).size() != 4) {
        return fallback;
    }
    return Vec4{object.at(key).at(0).get<f32>(), object.at(key).at(1).get<f32>(), object.at(key).at(2).get<f32>(),
                object.at(key).at(3).get<f32>()};
}

// The texture key a glTF texture slot refers to, or "" when the slot is absent
// or names a texture this loader cannot resolve. `slot` is a glTF textureInfo
// object (`{"index": 0, "texCoord": 0}`); only `index` matters here.
[[nodiscard]] std::string texture_key_from_slot(const json& holder, const char* key,
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

// Splits a GLB container into its JSON text and (optional) binary chunk.
[[nodiscard]] Error parse_glb(std::string_view bytes, std::string& json_text, std::string& bin_chunk) {
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
