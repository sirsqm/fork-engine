// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
//
// Tests for the scene half of the glTF importer (ADR-0104): skins read into
// `anim::Skeleton` resources, `JOINTS_0`/`WEIGHTS_0` read into `anim::SkinnedMesh`
// resources, and `animations` read into bound `anim::AnimationClip` resources.
// Pure CPU tests; no graphics context and no file on disk are needed.

#include <doctest/doctest.h>

#include "fe/anim/AnimationClip.h"
#include "fe/anim/Skeleton.h"
#include "fe/anim/SkinnedMesh.h"
#include "fe/mesh/GltfLoader.h"
#include "fe/mesh/Mesh.h"

#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

using namespace fe;

namespace {

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

// Base64 of a byte vector, so a fixture can embed its buffer as a data URI and
// need no file on disk.
[[nodiscard]] std::string base64(const std::vector<u8>& bytes) {
    static constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    usize i = 0;
    for (; i + 2 < bytes.size(); i += 3) {
        const u32 chunk =
            (static_cast<u32>(bytes[i]) << 16) | (static_cast<u32>(bytes[i + 1]) << 8) | static_cast<u32>(bytes[i + 2]);
        out.push_back(kAlphabet[(chunk >> 18) & 0x3Fu]);
        out.push_back(kAlphabet[(chunk >> 12) & 0x3Fu]);
        out.push_back(kAlphabet[(chunk >> 6) & 0x3Fu]);
        out.push_back(kAlphabet[chunk & 0x3Fu]);
    }
    if (i < bytes.size()) {
        const usize remaining = bytes.size() - i;
        u32 chunk = static_cast<u32>(bytes[i]) << 16;
        if (remaining == 2) {
            chunk |= static_cast<u32>(bytes[i + 1]) << 8;
        }
        out.push_back(kAlphabet[(chunk >> 18) & 0x3Fu]);
        out.push_back(kAlphabet[(chunk >> 12) & 0x3Fu]);
        out.push_back(remaining == 2 ? kAlphabet[(chunk >> 6) & 0x3Fu] : '=');
        out.push_back('=');
    }
    return out;
}

[[nodiscard]] std::string data_uri(const std::vector<u8>& bytes) {
    return "data:application/octet-stream;base64," + base64(bytes);
}

// --- A two-joint skinned triangle -------------------------------------------
//
// One triangle whose vertices are split between two joints: the first two
// vertices are fully the root joint (index 0), the third is fully the child
// (index 1). That is enough to prove the vertex reader, the weight read, and the
// skin binding without a large fixture.
//
// Buffer layout:
//   0   POSITION     3 x vec3 f32  = 36 bytes
//   36  JOINTS_0     3 x vec4 u8   = 12 bytes
//   48  WEIGHTS_0    3 x vec4 f32  = 48 bytes
//   96  indices      3 x u16       = 6 bytes
//   -> 102 bytes, padded to 104 for 4-byte alignment.
[[nodiscard]] std::vector<u8> skinned_triangle_binary() {
    std::vector<u8> out;
    for (const f32 value : {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f}) {
        append_f32(out, value);
    }
    // JOINTS_0: (0,0,0,0), (0,0,0,0), (1,0,0,0)
    for (const u8 joint : {0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0}) {
        out.push_back(joint);
    }
    // WEIGHTS_0: (1,0,0,0), (1,0,0,0), (1,0,0,0)
    for (const f32 weight : {1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f}) {
        append_f32(out, weight);
    }
    out.push_back(0x00);
    out.push_back(0x00);
    out.push_back(0x01);
    out.push_back(0x00);
    out.push_back(0x02);
    out.push_back(0x00);
    return out;
}

// A .gltf naming one skin of two joints and a mesh that binds to it. The inverse
// bind matrices are identity, so a bind-pose palette is the identity, which the
// test asserts on.
[[nodiscard]] std::string skinned_triangle_gltf() {
    const std::string uri = data_uri(skinned_triangle_binary());
    const std::string json = R"JSON({
        "asset": {"version": "2.0"},
        "buffers": [{"uri": "PLACEHOLDER", "byteLength": 104}],
        "bufferViews": [
            {"buffer": 0, "byteOffset": 0, "byteLength": 36},
            {"buffer": 0, "byteOffset": 36, "byteLength": 12},
            {"buffer": 0, "byteOffset": 48, "byteLength": 48},
            {"buffer": 0, "byteOffset": 96, "byteLength": 6}
        ],
        "accessors": [
            {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3"},
            {"bufferView": 1, "componentType": 5121, "count": 3, "type": "VEC4"},
            {"bufferView": 2, "componentType": 5126, "count": 3, "type": "VEC4"},
            {"bufferView": 3, "componentType": 5123, "count": 3, "type": "SCALAR"}
        ],
        "nodes": [
            {"name": "root"},
            {"name": "child"},
            {"name": "skin_root", "children": [0, 1]}
        ],
        "skins": [{"joints": [0, 1]}],
        "meshes": [{"skin": 0, "primitives": [{
            "attributes": {"POSITION": 0, "JOINTS_0": 1, "WEIGHTS_0": 2},
            "indices": 3
        }]}]
    })JSON";
    const usize at = json.find("PLACEHOLDER");
    return json.substr(0, at) + uri + json.substr(at + std::string("PLACEHOLDER").size());
}

// A .gltf with one skin and one animation: the child joint translates from
// (0,0,0) at t=0 to (0,2,0) at t=1, LINEAR.
[[nodiscard]] std::string animated_gltf() {
    // Buffer: times (2 x f32 = 8 bytes), then values (2 x vec3 f32 = 24 bytes),
    // then the skinned triangle's geometry (104 bytes). 8 + 24 = 32.
    std::vector<u8> binary;
    append_f32(binary, 0.0f);
    append_f32(binary, 1.0f);
    append_f32(binary, 0.0f);
    append_f32(binary, 0.0f);
    append_f32(binary, 0.0f);
    append_f32(binary, 0.0f);
    append_f32(binary, 2.0f);
    append_f32(binary, 0.0f);
    const usize geometry_base = binary.size();
    const std::vector<u8> geometry = skinned_triangle_binary();
    binary.insert(binary.end(), geometry.begin(), geometry.end());

    const std::string uri = data_uri(binary);
    std::string json = R"JSON({
        "asset": {"version": "2.0"},
        "buffers": [{"uri": "PLACEHOLDER", "byteLength": 200}],
        "bufferViews": [
            {"buffer": 0, "byteOffset": 0, "byteLength": 8},
            {"buffer": 0, "byteOffset": 8, "byteLength": 24},
            {"buffer": 0, "byteOffset": GEOMETRY_BASE, "byteLength": 36},
            {"buffer": 0, "byteOffset": GEOMETRY_BASE+36, "byteLength": 12},
            {"buffer": 0, "byteOffset": GEOMETRY_BASE+48, "byteLength": 48},
            {"buffer": 0, "byteOffset": GEOMETRY_BASE+96, "byteLength": 6}
        ],
        "accessors": [
            {"bufferView": 0, "componentType": 5126, "count": 2, "type": "SCALAR"},
            {"bufferView": 1, "componentType": 5126, "count": 2, "type": "VEC3"},
            {"bufferView": 2, "componentType": 5126, "count": 3, "type": "VEC3"},
            {"bufferView": 3, "componentType": 5121, "count": 3, "type": "VEC4"},
            {"bufferView": 4, "componentType": 5126, "count": 3, "type": "VEC4"},
            {"bufferView": 5, "componentType": 5123, "count": 3, "type": "SCALAR"}
        ],
        "nodes": [{"name": "root"}, {"name": "child"}],
        "skins": [{"joints": [0, 1]}],
        "meshes": [{"skin": 0, "primitives": [{
            "attributes": {"POSITION": 2, "JOINTS_0": 3, "WEIGHTS_0": 4},
            "indices": 5
        }]}],
        "animations": [{
            "name": "Hop",
            "samplers": [{"input": 0, "output": 1, "interpolation": "LINEAR"}],
            "channels": [{"sampler": 0, "target": {"node": 1, "path": "translation"}}]
        }]
    })JSON";
    const auto replace_all = [](std::string haystack, std::string_view needle, std::string_view replacement) {
        for (usize at = haystack.find(needle); at != std::string::npos; at = haystack.find(needle, at)) {
            haystack.replace(at, needle.size(), replacement);
        }
        return haystack;
    };
    json = replace_all(json, "PLACEHOLDER", uri);
    json = replace_all(json, "GEOMETRY_BASE", std::to_string(geometry_base));
    return json;
}

// A .gltf with a skin but a primitive that carries no JOINTS_0/WEIGHTS_0, so the
// primitive must decode as a static Mesh even though its mesh names a skin.
[[nodiscard]] std::string skinned_mesh_static_primitive_gltf() {
    std::vector<u8> binary;
    for (const f32 value : {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f}) {
        append_f32(binary, value);
    }
    binary.push_back(0x00);
    binary.push_back(0x00);
    binary.push_back(0x01);
    binary.push_back(0x00);
    binary.push_back(0x02);
    binary.push_back(0x00);
    const std::string uri = data_uri(binary);
    const std::string json = R"JSON({
        "asset": {"version": "2.0"},
        "buffers": [{"uri": "PLACEHOLDER", "byteLength": 42}],
        "bufferViews": [
            {"buffer": 0, "byteOffset": 0, "byteLength": 36},
            {"buffer": 0, "byteOffset": 36, "byteLength": 6}
        ],
        "accessors": [
            {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3"},
            {"bufferView": 1, "componentType": 5123, "count": 3, "type": "SCALAR"}
        ],
        "nodes": [{"name": "root"}],
        "skins": [{"joints": [0]}],
        "meshes": [{"skin": 0, "primitives": [{"attributes": {"POSITION": 0}, "indices": 1}]}]
    })JSON";
    const usize at = json.find("PLACEHOLDER");
    return json.substr(0, at) + uri + json.substr(at + std::string("PLACEHOLDER").size());
}

// A .gltf with a skin whose joint is parented under a non-joint node: the joint
// must become a root of the skeleton, not a child of an index that does not exist.
[[nodiscard]] std::string nested_skin_gltf() {
    const std::string uri = data_uri(skinned_triangle_binary());
    const std::string json = R"JSON({
        "asset": {"version": "2.0"},
        "buffers": [{"uri": "PLACEHOLDER", "byteLength": 104}],
        "bufferViews": [
            {"buffer": 0, "byteOffset": 0, "byteLength": 36},
            {"buffer": 0, "byteOffset": 36, "byteLength": 12},
            {"buffer": 0, "byteOffset": 48, "byteLength": 48},
            {"buffer": 0, "byteOffset": 96, "byteLength": 6}
        ],
        "accessors": [
            {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3"},
            {"bufferView": 1, "componentType": 5121, "count": 3, "type": "VEC4"},
            {"bufferView": 2, "componentType": 5126, "count": 3, "type": "VEC4"},
            {"bufferView": 3, "componentType": 5123, "count": 3, "type": "SCALAR"}
        ],
        "nodes": [
            {"name": "armature", "children": [1]},
            {"name": "hip", "translation": [0.0, 1.0, 0.0], "children": [2]},
            {"name": "knee", "translation": [0.0, 0.5, 0.0]}
        ],
        "skins": [{"joints": [1, 2]}],
        "meshes": [{"skin": 0, "primitives": [{
            "attributes": {"POSITION": 0, "JOINTS_0": 1, "WEIGHTS_0": 2},
            "indices": 3
        }]}]
    })JSON";
    const usize at = json.find("PLACEHOLDER");
    return json.substr(0, at) + uri + json.substr(at + std::string("PLACEHOLDER").size());
}

} // namespace

TEST_CASE("GltfLoader::parse_scene reads a skin into a Skeleton resource") {
    auto parsed = GltfLoader::parse_scene("hero.gltf", skinned_triangle_gltf());
    REQUIRE(parsed);
    REQUIRE(parsed.value().skeletons.size() == 1);

    const anim::Skeleton& skeleton = *parsed.value().skeletons.front();
    CHECK(skeleton.joint_count() == 2);
    CHECK(skeleton.has_valid_hierarchy());
    CHECK(skeleton.joint(0).name == "root");
    CHECK(skeleton.joint(1).name == "child");
    CHECK(skeleton.find_joint("child") == 1);
    CHECK(skeleton.find_joint("nope") == anim::Skeleton::kNoParent);

    // No inverseBindMatrices in the fixture, so they are derived from the rest
    // pose, which is identity here.
    CHECK(skeleton.joint(0).inverse_bind == Mat4::identity());
}

TEST_CASE("GltfLoader::parse_scene reads JOINTS_0 and WEIGHTS_0 into a SkinnedMesh") {
    auto parsed = GltfLoader::parse_scene("hero.gltf", skinned_triangle_gltf());
    REQUIRE(parsed);
    REQUIRE(parsed.value().meshes.size() == 1);

    const auto mesh = std::dynamic_pointer_cast<anim::SkinnedMesh>(parsed.value().meshes.front());
    REQUIRE((mesh != nullptr));
    CHECK(mesh->vertex_count() == 3);
    CHECK(mesh->index_count() == 3);
    CHECK(mesh->has_skeleton());

    // The third vertex is fully the child joint; the first two are the root.
    CHECK(mesh->vertices()[0].joints[0] == 0);
    CHECK(mesh->vertices()[0].weights[0] == doctest::Approx(1.0f));
    CHECK(mesh->vertices()[2].joints[0] == 1);
    CHECK(mesh->vertices()[2].weights[0] == doctest::Approx(1.0f));

    // The mesh's skeleton is the one the document declared.
    REQUIRE((mesh->skeleton() != nullptr));
    CHECK(mesh->skeleton()->joint_count() == 2);
}

TEST_CASE("GltfLoader::parse_scene leaves a primitive without joint data static") {
    auto parsed = GltfLoader::parse_scene("hero.gltf", skinned_mesh_static_primitive_gltf());
    REQUIRE(parsed);
    REQUIRE(parsed.value().meshes.size() == 1);

    // A mesh that names a skin, but whose only primitive has no JOINTS_0, must
    // decode as a static Mesh: the skin is irrelevant to a primitive that carries
    // no joint data.
    CHECK(std::dynamic_pointer_cast<Mesh>(parsed.value().meshes.front()) != nullptr);
    CHECK(std::dynamic_pointer_cast<anim::SkinnedMesh>(parsed.value().meshes.front()) == nullptr);
}

TEST_CASE("GltfLoader::parse_scene makes a joint under a non-joint node a root") {
    auto parsed = GltfLoader::parse_scene("hero.gltf", nested_skin_gltf());
    REQUIRE(parsed);
    REQUIRE(parsed.value().skeletons.size() == 1);

    const anim::Skeleton& skeleton = *parsed.value().skeletons.front();
    REQUIRE(skeleton.joint_count() == 2);
    // "hip" is under "armature", which is not a joint, so hip is a root of the
    // skeleton and knee is its child.
    CHECK(skeleton.joint(0).name == "hip");
    CHECK(skeleton.joint(0).parent == anim::Skeleton::kNoParent);
    CHECK(skeleton.joint(1).name == "knee");
    CHECK(skeleton.joint(1).parent == 0);
    CHECK(skeleton.has_valid_hierarchy());
    CHECK(skeleton.root_count() == 1);
    // The hip's rest translation is read from the node.
    CHECK(skeleton.joint(0).rest.position.y == doctest::Approx(1.0f));
}

TEST_CASE("GltfLoader::parse_scene reads an animation into a bound AnimationClip") {
    auto parsed = GltfLoader::parse_scene("hero.gltf", animated_gltf());
    REQUIRE(parsed);
    REQUIRE(parsed.value().animations.size() == 1);

    const anim::AnimationClip& clip = *parsed.value().animations.front();
    CHECK(clip.name() == "Hop");
    CHECK(clip.duration() == doctest::Approx(1.0f));
    REQUIRE(clip.track_count() == 1);
    CHECK(clip.tracks().front().joint_name == "child");
    // Bound against the document's first skin: the track resolved to joint 1.
    CHECK(clip.tracks().front().joint == 1);
    REQUIRE(clip.tracks().front().translation.has_value());

    // Sampling at the midpoint gives the midpoint translation.
    anim::SkeletonPose pose;
    pose.reset_to_rest(*parsed.value().skeletons.front());
    clip.sample(0.5f, pose);
    CHECK(pose.at(1).translation.y == doctest::Approx(1.0f));
}

TEST_CASE("GltfLoader::parse_scene reports a document with no meshes") {
    const std::string doc = R"JSON({
        "asset": {"version": "2.0"},
        "nodes": [{"name": "a"}]
    })JSON";
    auto parsed = GltfLoader::parse_scene("empty.gltf", doc);
    CHECK_FALSE(parsed);
}

TEST_CASE("GltfLoader::parse_scene rejects a skin that names a missing node") {
    const std::string doc = R"JSON({
        "asset": {"version": "2.0"},
        "nodes": [{"name": "a"}],
        "skins": [{"joints": [0, 5]}],
        "meshes": [{"primitives": [{"attributes": {"POSITION": 0}}]}]
    })JSON";
    auto parsed = GltfLoader::parse_scene("bad.gltf", doc);
    CHECK_FALSE(parsed);
}

TEST_CASE("GltfLoader::parse_scene reads a document with no skins or animations") {
    // A plain static glTF still parses through the scene path, producing one
    // static mesh and no skeletons or clips.
    const std::vector<u8> binary = [] {
        std::vector<u8> out;
        for (const f32 value : {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f}) {
            append_f32(out, value);
        }
        out.push_back(0x00);
        out.push_back(0x00);
        out.push_back(0x01);
        out.push_back(0x00);
        out.push_back(0x02);
        out.push_back(0x00);
        return out;
    }();
    const std::string uri = data_uri(binary);
    const std::string json = R"JSON({
        "asset": {"version": "2.0"},
        "buffers": [{"uri": "PLACEHOLDER", "byteLength": 42}],
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
    const usize at = json.find("PLACEHOLDER");
    const std::string doc = json.substr(0, at) + uri + json.substr(at + std::string("PLACEHOLDER").size());

    auto parsed = GltfLoader::parse_scene("static.gltf", doc);
    REQUIRE(parsed);
    CHECK(parsed.value().meshes.size() == 1);
    CHECK(parsed.value().skeletons.empty());
    CHECK(parsed.value().animations.empty());
    CHECK(std::dynamic_pointer_cast<Mesh>(parsed.value().meshes.front()) != nullptr);
}

TEST_CASE("GltfLoader::parse_scene reads CUBICSPLINE tangents") {
    // Times 0 and 1; per key an in-tangent, the value, and an out-tangent (3 vec3
    // each = 18 floats). The values are 0 and 2 on y, tangents zero.
    std::vector<u8> binary;
    append_f32(binary, 0.0f);
    append_f32(binary, 1.0f);
    const auto append_vec3 = [&binary](f32 x, f32 y, f32 z) {
        append_f32(binary, x);
        append_f32(binary, y);
        append_f32(binary, z);
    };
    // key 0: in, value, out
    append_vec3(0.0f, 0.0f, 0.0f);
    append_vec3(0.0f, 0.0f, 0.0f);
    append_vec3(0.0f, 1.0f, 0.0f);
    // key 1: in, value, out
    append_vec3(0.0f, 1.0f, 0.0f);
    append_vec3(0.0f, 2.0f, 0.0f);
    append_vec3(0.0f, 0.0f, 0.0f);

    const std::string uri = data_uri(binary);
    const std::string json = R"JSON({
        "asset": {"version": "2.0"},
        "buffers": [{"uri": "PLACEHOLDER", "byteLength": 80}],
        "bufferViews": [
            {"buffer": 0, "byteOffset": 0, "byteLength": 8},
            {"buffer": 0, "byteOffset": 8, "byteLength": 72}
        ],
        "accessors": [
            {"bufferView": 0, "componentType": 5126, "count": 2, "type": "SCALAR"},
            {"bufferView": 1, "componentType": 5126, "count": 6, "type": "VEC3"}
        ],
        "nodes": [{"name": "root"}],
        "skins": [{"joints": [0]}],
        "meshes": [{"primitives": [{"attributes": {"POSITION": 0}}]}],
        "animations": [{
            "name": "Ease",
            "samplers": [{"input": 0, "output": 1, "interpolation": "CUBICSPLINE"}],
            "channels": [{"sampler": 0, "target": {"node": 0, "path": "translation"}}]
        }]
    })JSON";
    const usize at = json.find("PLACEHOLDER");
    const std::string doc = json.substr(0, at) + uri + json.substr(at + std::string("PLACEHOLDER").size());

    auto parsed = GltfLoader::parse_scene("ease.gltf", doc);
    REQUIRE(parsed);
    REQUIRE(parsed.value().animations.size() == 1);
    const anim::AnimationClip& clip = *parsed.value().animations.front();
    REQUIRE(clip.track_count() == 1);
    REQUIRE(clip.tracks().front().translation.has_value());
    CHECK(clip.tracks().front().translation->interpolation == anim::Interpolation::Cubic);
    REQUIRE(clip.tracks().front().translation->keys.size() == 2);
    CHECK(clip.tracks().front().translation->keys[0].value.y == doctest::Approx(0.0f));
    CHECK(clip.tracks().front().translation->keys[1].value.y == doctest::Approx(2.0f));
}

TEST_CASE("GltfLoader::parse_scene skips an unsupported animation path") {
    const std::string doc = R"JSON({
        "asset": {"version": "2.0"},
        "nodes": [{"name": "root"}],
        "skins": [{"joints": [0]}],
        "meshes": [{"primitives": [{"attributes": {"POSITION": 0}}]}],
        "animations": [{
            "name": "Morph",
            "samplers": [{"input": 0, "output": 1}],
            "channels": [{"sampler": 0, "target": {"node": 0, "path": "weights"}}]
        }]
    })JSON";
    auto parsed = GltfLoader::parse_scene("morph.gltf", doc);
    // No accessors exist, so the sampler cannot be read; but the channel's path
    // is skipped before the sampler is touched, so the clip binds zero tracks and
    // the document still parses.
    REQUIRE(parsed);
    REQUIRE(parsed.value().animations.size() == 1);
    CHECK(parsed.value().animations.front()->track_count() == 0);
}
