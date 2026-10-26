// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
//
// Tests for the glTF 2.0 importer: the JSON (.gltf) form with a base64 data URI,
// the binary (.glb) container, the vertex-attribute and index decoders, the
// supported/unsupported contract from ADR-0040, and the resource-manager load
// path. These are pure CPU tests; no graphics context is needed.

#include <doctest/doctest.h>

#include "fe/core/FileSystem.h"
#include "fe/mesh/GltfLoader.h"
#include "fe/mesh/Mesh.h"
#include "fe/resources/ResourceManager.h"

#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

using namespace fe;

namespace {

// A one-triangle buffer as base64: POSITION (3x f32, offset 0), indices (3x u16,
// offset 36), NORMAL (3x f32, offset 42), TEXCOORD_0 (2x f32 per vertex, offset
// 78). Encoded so a .gltf fixture needs no file on disk.
constexpr std::string_view kFullUri =
    "data:application/octet-stream;base64,"
    "AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAABAAIAAAAAAAAAAAAAAIA/AAAAAAAAAAAAAIA/AAAAAAAAAAAA"
    "AIA/AAAAAAAAAAAAAIA/AAAAAAAAAAAAAIA/";

// The same buffer's first 42 bytes (positions + indices), as base64. Used by the
// fixtures that carry no normals or UVs.
constexpr std::string_view kPosIndexUri =
    "data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAABAAIA";

// The URI placeholder in the JSON templates below. Substituting after the fact
// keeps the raw string literals free of the `)"` sequence that would otherwise
// close them.
constexpr std::string_view kPlaceholder = "PLACEHOLDER_URI";

[[nodiscard]] std::string fill_uri(std::string json, std::string_view uri) {
    const usize at = json.find(kPlaceholder);
    if (at != std::string::npos) {
        json.replace(at, kPlaceholder.size(), uri);
    }
    return json;
}

// A .gltf with positions, indices, normals, and UVs.
[[nodiscard]] std::string triangle_gltf() {
    return fill_uri(R"JSON({
        "asset": {"version": "2.0"},
        "buffers": [{"uri": "PLACEHOLDER_URI", "byteLength": 102}],
        "bufferViews": [
            {"buffer": 0, "byteOffset": 0, "byteLength": 36},
            {"buffer": 0, "byteOffset": 36, "byteLength": 6},
            {"buffer": 0, "byteOffset": 42, "byteLength": 36},
            {"buffer": 0, "byteOffset": 78, "byteLength": 24}
        ],
        "accessors": [
            {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3"},
            {"bufferView": 1, "componentType": 5123, "count": 3, "type": "SCALAR"},
            {"bufferView": 2, "componentType": 5126, "count": 3, "type": "VEC3"},
            {"bufferView": 3, "componentType": 5126, "count": 3, "type": "VEC2"}
        ],
        "meshes": [{"primitives": [{"attributes": {"POSITION": 0, "NORMAL": 2, "TEXCOORD_0": 3}, "indices": 1}]}]
    })JSON",
                    kFullUri);
}

// A .gltf with positions + indices only (no normals, no UVs).
[[nodiscard]] std::string pos_index_gltf() {
    return fill_uri(R"JSON({
        "asset": {"version": "2.0"},
        "buffers": [{"uri": "PLACEHOLDER_URI", "byteLength": 42}],
        "bufferViews": [
            {"buffer": 0, "byteOffset": 0, "byteLength": 36},
            {"buffer": 0, "byteOffset": 36, "byteLength": 6}
        ],
        "accessors": [
            {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3"},
            {"bufferView": 1, "componentType": 5123, "count": 3, "type": "SCALAR"}
        ],
        "meshes": [{"primitives": [{"attributes": {"POSITION": 0}, "indices": 1}]}]
    })JSON",
                    kPosIndexUri);
}

// A .gltf triangle whose primitive references material 0, a fully-populated
// metallic-roughness material with a base-colour texture, a normal map, an
// emissive factor/texture, and double-sided set.
[[nodiscard]] std::string material_gltf() {
    return fill_uri(R"JSON({
        "asset": {"version": "2.0"},
        "buffers": [{"uri": "PLACEHOLDER_URI", "byteLength": 102}],
        "bufferViews": [
            {"buffer": 0, "byteOffset": 0, "byteLength": 36},
            {"buffer": 0, "byteOffset": 36, "byteLength": 6},
            {"buffer": 0, "byteOffset": 42, "byteLength": 36},
            {"buffer": 0, "byteOffset": 78, "byteLength": 24}
        ],
        "accessors": [
            {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3"},
            {"bufferView": 1, "componentType": 5123, "count": 3, "type": "SCALAR"},
            {"bufferView": 2, "componentType": 5126, "count": 3, "type": "VEC3"},
            {"bufferView": 3, "componentType": 5126, "count": 3, "type": "VEC2"}
        ],
        "images": [
            {"uri": "textures/albedo.png"},
            {"uri": "textures/normal.png"},
            {"uri": "textures/glow.png"}
        ],
        "textures": [{"source": 0}, {"source": 1}, {"source": 2}],
        "materials": [{
            "pbrMetallicRoughness": {
                "baseColorFactor": [0.25, 0.5, 0.75, 1.0],
                "baseColorTexture": {"index": 0},
                "metallicFactor": 0.8,
                "roughnessFactor": 0.2
            },
            "normalTexture": {"index": 1},
            "emissiveFactor": [0.1, 0.2, 0.3],
            "emissiveTexture": {"index": 2},
            "doubleSided": true
        }],
        "meshes": [{"primitives": [{"attributes": {"POSITION": 0, "NORMAL": 2, "TEXCOORD_0": 3}, "indices": 1,
                                    "material": 0}]}]
    })JSON",
                    kFullUri);
}

// A .gltf triangle with a material that sets only a base colour, so every other
// field keeps the glTF default.
[[nodiscard]] std::string base_color_only_gltf() {
    return fill_uri(R"JSON({
        "asset": {"version": "2.0"},
        "buffers": [{"uri": "PLACEHOLDER_URI", "byteLength": 42}],
        "bufferViews": [
            {"buffer": 0, "byteOffset": 0, "byteLength": 36},
            {"buffer": 0, "byteOffset": 36, "byteLength": 6}
        ],
        "accessors": [
            {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3"},
            {"bufferView": 1, "componentType": 5123, "count": 3, "type": "SCALAR"}
        ],
        "materials": [{"pbrMetallicRoughness": {"baseColorFactor": [1.0, 0.0, 0.0, 1.0]}}],
        "meshes": [{"primitives": [{"attributes": {"POSITION": 0}, "indices": 1, "material": 0}]}]
    })JSON",
                    kPosIndexUri);
}

// A .gltf triangle whose material is index 7, which no document declares. The
// import must still succeed, just without a material.
[[nodiscard]] std::string out_of_range_material_gltf() {
    return fill_uri(R"JSON({
        "asset": {"version": "2.0"},
        "buffers": [{"uri": "PLACEHOLDER_URI", "byteLength": 42}],
        "bufferViews": [
            {"buffer": 0, "byteOffset": 0, "byteLength": 36},
            {"buffer": 0, "byteOffset": 36, "byteLength": 6}
        ],
        "accessors": [
            {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3"},
            {"bufferView": 1, "componentType": 5123, "count": 3, "type": "SCALAR"}
        ],
        "materials": [{"pbrMetallicRoughness": {"baseColorFactor": [1.0, 0.0, 0.0, 1.0]}}],
        "meshes": [{"primitives": [{"attributes": {"POSITION": 0}, "indices": 1, "material": 7}]}]
    })JSON",
                    kPosIndexUri);
}

[[nodiscard]] std::shared_ptr<Mesh> as_mesh(const std::shared_ptr<Resource>& resource) {
    return std::dynamic_pointer_cast<Mesh>(resource);
}

void append_u32(std::vector<u8>& out, u32 value) {
    out.push_back(static_cast<u8>(value & 0xFFu));
    out.push_back(static_cast<u8>((value >> 8) & 0xFFu));
    out.push_back(static_cast<u8>((value >> 16) & 0xFFu));
    out.push_back(static_cast<u8>((value >> 24) & 0xFFu));
}

void append_f32(std::vector<u8>& out, f32 value) {
    u32 bits = 0;
    std::memcpy(&bits, &value, sizeof(f32));
    append_u32(out, bits);
}

// Positions (0,0,0),(1,0,0),(0,1,0) as f32 LE, then u16 indices 0,1,2: 42 bytes.
[[nodiscard]] std::vector<u8> triangle_binary() {
    std::vector<u8> out;
    for (const f32 value : {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f}) {
        append_f32(out, value);
    }
    out.push_back(0x00);
    out.push_back(0x00); // index 0
    out.push_back(0x01);
    out.push_back(0x00); // index 1
    out.push_back(0x02);
    out.push_back(0x00); // index 2
    return out;
}

// Builds a .glb: header, JSON chunk (padded to 4 bytes), binary chunk.
[[nodiscard]] std::vector<u8> make_glb(std::string_view json_text, const std::vector<u8>& binary) {
    std::vector<u8> out;
    append_u32(out, 0x46546C67u); // "glTF"
    append_u32(out, 2);
    const u32 json_len = static_cast<u32>(json_text.size());
    const u32 padded_json = (json_len + 3u) & ~3u;
    const u32 bin_len = static_cast<u32>(binary.size());
    append_u32(out, 12 + 8 + padded_json + 8 + bin_len);

    append_u32(out, padded_json);
    append_u32(out, 0x4E4F534Au); // "JSON"
    out.insert(out.end(), json_text.begin(), json_text.end());
    out.resize(out.size() + (padded_json - json_len), ' ');

    append_u32(out, bin_len);
    append_u32(out, 0x004E4942u); // "BIN\0"
    out.insert(out.end(), binary.begin(), binary.end());
    return out;
}

// The JSON for a GLB whose single buffer is the binary chunk (no uri).
[[nodiscard]] std::string glb_json() {
    return R"JSON({
        "asset": {"version": "2.0"},
        "buffers": [{"byteLength": 42}],
        "bufferViews": [
            {"buffer": 0, "byteOffset": 0, "byteLength": 36},
            {"buffer": 0, "byteOffset": 36, "byteLength": 6}
        ],
        "accessors": [
            {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3"},
            {"bufferView": 1, "componentType": 5123, "count": 3, "type": "SCALAR"}
        ],
        "meshes": [{"primitives": [{"attributes": {"POSITION": 0}, "indices": 1}]}]
    })JSON";
}

} // namespace

TEST_CASE("GltfLoader::reference_key adds the gltf scheme once") {
    CHECK(GltfLoader::reference_key("assets/hero.glb") == "gltf:assets/hero.glb");
    CHECK(GltfLoader::reference_key("gltf:assets/hero.glb") == "gltf:assets/hero.glb");
    CHECK(GltfLoader::reference_key("assets/hero.gltf") == "gltf:assets/hero.gltf");
}

TEST_CASE("GltfLoader supports .gltf and .glb, not other extensions") {
    GltfLoader loader;
    CHECK(loader.supports(".gltf"));
    CHECK(loader.supports(".glb"));
    CHECK_FALSE(loader.supports(".obj"));
    CHECK_FALSE(loader.supports(".png"));
}

TEST_CASE("GltfLoader parses a .gltf triangle with positions, normals, and uvs") {
    auto parsed = GltfLoader::parse("tri.gltf", triangle_gltf());
    REQUIRE(parsed);
    REQUIRE(parsed.value().size() == 1);

    const std::shared_ptr<Mesh> mesh = as_mesh(parsed.value().front());
    REQUIRE(mesh != nullptr);
    CHECK(mesh->vertex_count() == 3);
    CHECK(mesh->index_count() == 3);
    CHECK(mesh->triangle_count() == 1);

    CHECK(mesh->vertices()[0].position.x == doctest::Approx(0.0f));
    CHECK(mesh->vertices()[1].position.x == doctest::Approx(1.0f));
    CHECK(mesh->vertices()[2].position.y == doctest::Approx(1.0f));
    // The supplied normal and UV are preserved.
    CHECK(mesh->vertices()[0].normal.z == doctest::Approx(1.0f));
    CHECK(mesh->vertices()[1].uv.x == doctest::Approx(1.0f));

    CHECK(mesh->indices()[0] == 0);
    CHECK(mesh->indices()[1] == 1);
    CHECK(mesh->indices()[2] == 2);
}

TEST_CASE("GltfLoader parses a .glb binary container") {
    const std::vector<u8> glb = make_glb(glb_json(), triangle_binary());
    auto parsed = GltfLoader::parse("tri.glb", std::string_view(reinterpret_cast<const char*>(glb.data()), glb.size()));
    REQUIRE(parsed);
    REQUIRE(parsed.value().size() == 1);

    const std::shared_ptr<Mesh> mesh = as_mesh(parsed.value().front());
    REQUIRE(mesh != nullptr);
    CHECK(mesh->vertex_count() == 3);
    CHECK(mesh->triangle_count() == 1);
    // No NORMAL in this fixture, so a flat normal is computed and must be unit.
    CHECK(mesh->vertices()[0].normal.length_squared() == doctest::Approx(1.0f));
}

TEST_CASE("GltfLoader synthesises indices for a non-indexed primitive") {
    // Drop the "indices" member: the loader produces 0,1,2.
    std::string doc = pos_index_gltf();
    const std::string needle = R"(, "indices": 1)";
    const usize at = doc.find(needle);
    REQUIRE(at != std::string::npos);
    doc.erase(at, needle.size());

    auto parsed = GltfLoader::parse("plain.gltf", doc);
    REQUIRE(parsed);
    const std::shared_ptr<Mesh> mesh = as_mesh(parsed.value().front());
    REQUIRE(mesh != nullptr);
    CHECK(mesh->index_count() == 3);
    CHECK(mesh->indices()[0] == 0);
    CHECK(mesh->indices()[2] == 2);
}

TEST_CASE("GltfLoader maps each primitive to its own mesh") {
    // Duplicate the single primitive by repeating the "meshes" entry.
    std::string doc = pos_index_gltf();
    const std::string needle = R"("meshes": [{"primitives": [{"attributes": {"POSITION": 0}, "indices": 1}]}])";
    const usize at = doc.find(needle);
    REQUIRE(at != std::string::npos);
    doc.replace(
        at, needle.size(),
        R"("meshes": [{"primitives": [{"attributes": {"POSITION": 0}, "indices": 1}, {"attributes": {"POSITION": 0}, "indices": 1}]}])");

    auto parsed = GltfLoader::parse("two.gltf", doc);
    REQUIRE(parsed);
    CHECK(parsed.value().size() == 2);
    CHECK(as_mesh(parsed.value()[0]) != nullptr);
    CHECK(as_mesh(parsed.value()[1]) != nullptr);
}

TEST_CASE("GltfLoader skips non-triangle primitives rather than failing") {
    // A file whose only primitive is POINTS has nothing this loader decodes.
    std::string points = pos_index_gltf();
    const std::string needle = R"("indices": 1})";
    const usize at = points.find(needle);
    REQUIRE(at != std::string::npos);
    points.replace(at, needle.size(), R"("indices": 1, "mode": 0})");

    auto only_points = GltfLoader::parse("points.gltf", points);
    REQUIRE_FALSE(only_points);
    CHECK(only_points.error().message().find("does not support") != std::string::npos);

    // A point primitive beside a triangle primitive: exactly one mesh comes back.
    std::string mixed = pos_index_gltf();
    const usize mixed_at = mixed.find(needle);
    REQUIRE(mixed_at != std::string::npos);
    mixed.replace(mixed_at, needle.size(),
                  R"("indices": 1, "mode": 0}, {"attributes": {"POSITION": 0}, "indices": 1, "mode": 4})");

    auto kept = GltfLoader::parse("mixed.gltf", mixed);
    REQUIRE(kept);
    CHECK(kept.value().size() == 1);
}

TEST_CASE("GltfLoader reports a malformed JSON file without crashing") {
    auto parsed = GltfLoader::parse("bad.gltf", "{ this is not json");
    REQUIRE_FALSE(parsed);
    CHECK_FALSE(parsed.error().message().empty());
}

TEST_CASE("GltfLoader reports a file with no meshes") {
    auto parsed = GltfLoader::parse("empty.gltf", R"({"asset": {"version": "2.0"}})");
    REQUIRE_FALSE(parsed);
    CHECK(parsed.error().message().find("no meshes") != std::string::npos);
}

TEST_CASE("GltfLoader rejects a bufferView that reads past its buffer") {
    std::string doc = pos_index_gltf();
    const std::string needle = R"({"buffer": 0, "byteOffset": 0, "byteLength": 36})";
    const usize at = doc.find(needle);
    REQUIRE(at != std::string::npos);
    doc.replace(at, needle.size(), R"({"buffer": 0, "byteOffset": 0, "byteLength": 4096})");

    auto parsed = GltfLoader::parse("oob.gltf", doc);
    REQUIRE_FALSE(parsed);
    CHECK(parsed.error().message().find("past the end") != std::string::npos);
}

TEST_CASE("GltfLoader rejects an index that is out of range for its vertices") {
    // Shrink POSITION to one vertex; the three indices then run off the end.
    std::string doc = pos_index_gltf();
    const std::string needle = R"({"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3"})";
    const usize at = doc.find(needle);
    REQUIRE(at != std::string::npos);
    doc.replace(at, needle.size(), R"({"bufferView": 0, "componentType": 5126, "count": 1, "type": "VEC3"})");

    auto parsed = GltfLoader::parse("idx.gltf", doc);
    REQUIRE_FALSE(parsed);
    CHECK(parsed.error().message().find("out of range") != std::string::npos);
}

TEST_CASE("GltfLoader rejects an unsupported asset version") {
    auto parsed = GltfLoader::parse("old.gltf", R"({"asset": {"version": "1.0"}, "meshes": []})");
    REQUIRE_FALSE(parsed);
    CHECK(parsed.error().message().find("2.x") != std::string::npos);
}

TEST_CASE("GltfLoader rejects a truncated GLB header") {
    // The full magic but fewer than the 12 header bytes.
    auto parsed = GltfLoader::parse("short.glb", "glTF");
    REQUIRE_FALSE(parsed);
    CHECK(parsed.error().message().find("GLB") != std::string::npos);
}

TEST_CASE("a glTF asset loads through the ResourceManager by scheme and extension") {
    ResourceManager manager;
    manager.register_loader(std::make_unique<GltfLoader>());

    const std::string dir = fs::join(std::filesystem::temp_directory_path().string(), "fe_gltf_test");
    REQUIRE_FALSE(fs::create_directories(dir));
    const std::string file = fs::join(dir, "tri.gltf");
    REQUIRE_FALSE(fs::write_text_file(file, triangle_gltf()));

    auto loaded = manager.load_as<Mesh>("gltf:" + file);
    REQUIRE(loaded);
    CHECK(loaded.value()->vertex_count() == 3);
    CHECK(loaded.value()->triangle_count() == 1);

    // A second load of the same reference is served from the cache.
    auto again = manager.load_as<Mesh>("gltf:" + file);
    REQUIRE(again);
    CHECK(again.value().get() == loaded.value().get());

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST_CASE("GltfLoader imports a primitive's metallic-roughness material") {
    auto parsed = GltfLoader::parse("mat.gltf", material_gltf());
    REQUIRE(parsed);
    REQUIRE(parsed.value().size() == 1);

    auto mesh = as_mesh(parsed.value().front());
    REQUIRE(mesh);
    REQUIRE(mesh->has_material());

    const Material& material = mesh->material().value();
    CHECK(material.base_color.x == doctest::Approx(0.25f));
    CHECK(material.base_color.y == doctest::Approx(0.5f));
    CHECK(material.base_color.z == doctest::Approx(0.75f));
    CHECK(material.base_color.w == doctest::Approx(1.0f));
    CHECK(material.metallic == doctest::Approx(0.8f));
    CHECK(material.roughness == doctest::Approx(0.2f));
    CHECK(material.emissive.x == doctest::Approx(0.1f));
    CHECK(material.emissive.y == doctest::Approx(0.2f));
    CHECK(material.emissive.z == doctest::Approx(0.3f));

    // Each texture resolves through the texture loader's key, so a component
    // storing it reloads through the ordinary resource path.
    CHECK(material.base_color_texture == "tex:textures/albedo.png");
    CHECK(material.normal_texture == "tex:textures/normal.png");
    CHECK(material.emissive_texture == "tex:textures/glow.png");
    CHECK(material.double_sided);
    CHECK_FALSE(material.is_default());
}

TEST_CASE("a glTF material keeps the glTF defaults for the fields it omits") {
    auto parsed = GltfLoader::parse("base.gltf", base_color_only_gltf());
    REQUIRE(parsed);
    auto mesh = as_mesh(parsed.value().front());
    REQUIRE(mesh);
    REQUIRE(mesh->has_material());

    const Material& material = mesh->material().value();
    CHECK(material.base_color.x == doctest::Approx(1.0f));
    CHECK(material.base_color.y == doctest::Approx(0.0f));
    // The glTF defaults, not zeros: fully rough, non-metallic, unlit-emissive.
    CHECK(material.metallic == doctest::Approx(0.0f));
    CHECK(material.roughness == doctest::Approx(0.6f));
    CHECK(material.emissive == Vec3{0.0f, 0.0f, 0.0f});
    CHECK(material.base_color_texture.empty());
    CHECK(material.normal_texture.empty());
    CHECK_FALSE(material.double_sided);
}

TEST_CASE("a primitive with no material carries no material") {
    auto parsed = GltfLoader::parse("plain.gltf", triangle_gltf());
    REQUIRE(parsed);
    auto mesh = as_mesh(parsed.value().front());
    REQUIRE(mesh);
    CHECK_FALSE(mesh->has_material());
}

TEST_CASE("an out-of-range material index imports without a material") {
    auto parsed = GltfLoader::parse("oob.gltf", out_of_range_material_gltf());
    REQUIRE(parsed);
    auto mesh = as_mesh(parsed.value().front());
    REQUIRE(mesh);
    CHECK_FALSE(mesh->has_material());
}

TEST_CASE("a glTF material texture with a data-uri image resolves to no texture") {
    // The image is embedded, so there is no file to load. The import still
    // succeeds and keeps the material's factors, with an empty texture key.
    const std::string doc = R"JSON({
        "asset": {"version": "2.0"},
        "buffers": [{"uri": "data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAABAAIA", "byteLength": 42}],
        "bufferViews": [
            {"buffer": 0, "byteOffset": 0, "byteLength": 36},
            {"buffer": 0, "byteOffset": 36, "byteLength": 6}
        ],
        "accessors": [
            {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3"},
            {"bufferView": 1, "componentType": 5123, "count": 3, "type": "SCALAR"}
        ],
        "images": [{"uri": "data:image/png;base64,AAAA"}],
        "textures": [{"source": 0}],
        "materials": [{"pbrMetallicRoughness": {"baseColorTexture": {"index": 0}, "metallicFactor": 0.5}}],
        "meshes": [{"primitives": [{"attributes": {"POSITION": 0}, "indices": 1, "material": 0}]}]
    })JSON";

    auto parsed = GltfLoader::parse("embed.gltf", doc);
    REQUIRE(parsed);
    auto mesh = as_mesh(parsed.value().front());
    REQUIRE(mesh);
    REQUIRE(mesh->has_material());
    CHECK(mesh->material().value().base_color_texture.empty());
    CHECK(mesh->material().value().metallic == doctest::Approx(0.5f));
}

TEST_CASE("a glTF in a subdirectory keys its material's maps under that directory") {
    // A glTF image URI is relative to the glTF file, so a document that lives in
    // a subdirectory names its maps there too. Keying the bare URI would produce
    // a reference the file is not at, and the map would never load.
    auto parsed = GltfLoader::parse("assets/models/prop.gltf", material_gltf());
    REQUIRE(parsed);
    REQUIRE(parsed.value().size() == 1);

    auto mesh = as_mesh(parsed.value().front());
    REQUIRE(mesh);
    REQUIRE(mesh->has_material());

    const Material& material = mesh->material().value();
    CHECK(material.base_color_texture == "tex:assets/models/textures/albedo.png");
    CHECK(material.normal_texture == "tex:assets/models/textures/normal.png");
    CHECK(material.emissive_texture == "tex:assets/models/textures/glow.png");
}
