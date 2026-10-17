// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
//
// Tests for tilemaps (ADR-0062).
//
// A tile is a textured quad, so the tilemap reuses the sprite draw path: these
// tests exercise the grid, the cell-to-instance extraction, the tileset UV math,
// the shared cull, and the JSON round-trip. Extraction and culling are pure CPU
// work, so they run with no display and no GL context, against the null RHI
// device, exactly as the sprite tests do.

#include <doctest/doctest.h>

#include "fe/reflect/Registry.h"
#include "fe/render/RenderExtract.h"
#include "fe/renderer2d/Renderer2D.h"
#include "fe/resources/Texture2D.h"
#include "fe/rhi/Device.h"
#include "fe/rhi/Resources.h"
#include "fe/rhi/Types.h"
#include "fe/scene/Camera2D.h"
#include "fe/scene/Node.h"
#include "fe/scene/Scene.h"
#include "fe/scene/Sprite2D.h"
#include "fe/scene/Tilemap.h"
#include "fe/scene/TilemapTypes.h"
#include "fe/serialization/SceneSerializer.h"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

using namespace fe;

namespace {

// A `Texture2D` built on the null device. The 2D renderer only extracts tiles
// that reference a `Texture2D`, so this is the smallest tileset: no decode, no
// file, no GPU object.
[[nodiscard]] std::shared_ptr<Texture2D> make_solid_texture(rhi::Device& device, u32 width, u32 height) {
    std::vector<u8> pixels(static_cast<usize>(width) * height * 4, 255);
    rhi::TextureDesc desc;
    desc.name = "tileset";
    desc.width = width;
    desc.height = height;
    desc.pixels = pixels.data();
    auto texture = device.create_texture(desc);
    REQUIRE(texture.ok());
    return std::make_shared<Texture2D>("tex:tileset.png", texture.value());
}

} // namespace

// ---------------------------------------------------------------------------
// The grid: bounds, writes, resize, and the empty sentinel.
// ---------------------------------------------------------------------------

TEST_CASE("a fresh tilemap is empty and reads as kEmpty everywhere") {
    Tilemap map;
    CHECK(map.width == 0);
    CHECK(map.height == 0);
    CHECK(map.cell_count() == 0);
    CHECK(map.empty());
    CHECK(map.at(0, 0) == Tilemap::kEmpty);
}

TEST_CASE("resizing fills the grid and clear refills it") {
    Tilemap map;
    map.resize(4, 3, 7);

    CHECK(map.width == 4);
    CHECK(map.height == 3);
    CHECK(map.cell_count() == 12);
    CHECK(map.cells.size() == 12);
    for (u32 y = 0; y < map.height; ++y) {
        for (u32 x = 0; x < map.width; ++x) {
            CHECK(map.at(x, y) == 7);
        }
    }

    map.clear();
    CHECK(map.at(2, 1) == Tilemap::kEmpty);
    CHECK(map.cell_count() == 12);
}

TEST_CASE("a write inside the grid is read back and a write outside is ignored") {
    Tilemap map;
    map.resize(2, 2);

    map.set(1, 1, 5);
    CHECK(map.at(1, 1) == 5);

    // Out-of-bounds writes are ignored, and out-of-bounds reads are kEmpty: an
    // authoring query is defined, not a crash.
    map.set(2, 0, 9);
    map.set(0, 2, 9);
    CHECK(map.at(2, 0) == Tilemap::kEmpty);
    CHECK(map.at(0, 2) == Tilemap::kEmpty);
    CHECK(map.cells.size() == 4);
}

TEST_CASE("tile index 0 is a real tile, distinct from the empty sentinel") {
    Tilemap map;
    map.resize(2, 1, 0);

    CHECK(map.at(0, 0) == 0);
    CHECK(map.at(0, 0) != Tilemap::kEmpty);

    map.set(1, 0, Tilemap::kEmpty);
    CHECK(map.at(1, 0) == Tilemap::kEmpty);
}

// ---------------------------------------------------------------------------
// Tileset UVs: the cell math a tile is drawn from.
// ---------------------------------------------------------------------------

TEST_CASE("a tile index maps to its cell in the tileset grid") {
    Tilemap map;
    map.columns = 4;
    map.rows = 2;

    // Top-left cell.
    Vec4 uv = map.tile_uv(0);
    CHECK(uv.x == doctest::Approx(0.0f));
    CHECK(uv.y == doctest::Approx(0.0f));
    CHECK(uv.z == doctest::Approx(0.25f));
    CHECK(uv.w == doctest::Approx(0.5f));

    // The cell to its right.
    uv = map.tile_uv(1);
    CHECK(uv.x == doctest::Approx(0.25f));
    CHECK(uv.y == doctest::Approx(0.0f));
    CHECK(uv.z == doctest::Approx(0.5f));
    CHECK(uv.w == doctest::Approx(0.5f));

    // The first cell of the second row: the row wraps after `columns` tiles.
    uv = map.tile_uv(4);
    CHECK(uv.x == doctest::Approx(0.0f));
    CHECK(uv.y == doctest::Approx(0.5f));
    CHECK(uv.z == doctest::Approx(0.25f));
    CHECK(uv.w == doctest::Approx(1.0f));
}

TEST_CASE("an out-of-range tile and a degenerate tileset both yield the full texture") {
    Tilemap map;
    map.columns = 2;
    map.rows = 2;

    // Past the end of the grid.
    const Vec4 out_of_range = map.tile_uv(4);
    CHECK(out_of_range.x == doctest::Approx(0.0f));
    CHECK(out_of_range.y == doctest::Approx(0.0f));
    CHECK(out_of_range.z == doctest::Approx(1.0f));
    CHECK(out_of_range.w == doctest::Approx(1.0f));

    // Negative is the empty sentinel, not a tile.
    CHECK(map.tile_uv(Tilemap::kEmpty).z == doctest::Approx(1.0f));

    // A zero-dimension tileset cannot address a cell.
    Tilemap degenerate;
    CHECK(degenerate.tile_uv(0).z == doctest::Approx(1.0f));
}

// ---------------------------------------------------------------------------
// JSON: the grid as the reflection property's string.
// ---------------------------------------------------------------------------

TEST_CASE("a tilemap grid round-trips through JSON") {
    Tilemap original;
    original.resize(3, 2);
    original.set(0, 0, 1);
    original.set(2, 0, 4);
    original.set(1, 1, 7);

    const std::string text = original.to_json();

    Tilemap restored;
    REQUIRE(restored.from_json(text));
    CHECK(restored.width == 3);
    CHECK(restored.height == 2);
    CHECK(restored.at(0, 0) == 1);
    CHECK(restored.at(1, 0) == Tilemap::kEmpty);
    CHECK(restored.at(2, 0) == 4);
    CHECK(restored.at(1, 1) == 7);
}

TEST_CASE("a malformed grid document is rejected and leaves the map unchanged") {
    Tilemap map;
    map.resize(2, 2, 3);

    // Not JSON at all.
    CHECK_FALSE(map.from_json("{ not json"));
    // JSON, but not an object.
    CHECK_FALSE(map.from_json("[1, 2, 3]"));
    // Dimensions that disagree with the cell count: the invariant is enforced,
    // so a hand-edited scene keeps a valid grid rather than a partial one.
    CHECK_FALSE(map.from_json(R"({"width": 2, "height": 2, "cells": [1, 2]})"));

    // Every rejection left the original grid intact.
    CHECK(map.width == 2);
    CHECK(map.height == 2);
    CHECK(map.at(0, 0) == 3);
}

TEST_CASE("a grid document with a field of the wrong type is rejected, not thrown") {
    Tilemap map;
    map.resize(2, 2, 3);

    // These reach the JSON accessors, which throw on a type mismatch rather than
    // returning a default. The setter must not let that escape into the scene
    // deserializer, so each is caught and reported as a rejection.
    CHECK_FALSE(map.from_json(R"({"width": "two", "height": 2, "cells": []})"));
    CHECK_FALSE(map.from_json(R"({"width": 2, "height": 2, "cells": [1, "x", 3, 4]})"));
    CHECK_FALSE(map.from_json(R"({"width": 2, "height": 2, "cells": {"0": 1}})"));
    CHECK_FALSE(map.from_json(R"({"width": -1, "height": 2, "cells": []})"));

    CHECK(map.width == 2);
    CHECK(map.height == 2);
    CHECK(map.at(0, 0) == 3);
}

TEST_CASE("the tilemap type is registered with a grid property") {
    reflect::register_tilemap_types();

    const reflect::Registry& registry = reflect::Registry::instance();
    REQUIRE(registry.contains(Tilemap::kTypeName));

    const reflect::TypeDesc* type = registry.find(Tilemap::kTypeName);
    REQUIRE(type != nullptr);

    const reflect::Property* grid = nullptr;
    bool has_texture = false;
    bool has_tile_size = false;
    for (const auto& property : type->properties) {
        if (property.name == "grid") {
            grid = &property;
        } else if (property.name == "texture") {
            has_texture = true;
        } else if (property.name == "tile_size") {
            has_tile_size = true;
        }
    }
    REQUIRE(grid != nullptr);
    CHECK(grid->value_kind == reflect::ValueKind::String);
    CHECK(has_texture);
    CHECK(has_tile_size);

    // A fresh component produces a valid, parseable grid document.
    auto component = type->factory();
    REQUIRE(component != nullptr);
    const reflect::Value value = grid->getter(component.get());
    const auto* text = std::get_if<std::string>(&value);
    REQUIRE(text != nullptr);
    CHECK_FALSE(text->empty());
}

// ---------------------------------------------------------------------------
// Extraction: the grid becomes one quad per non-empty cell.
// ---------------------------------------------------------------------------

TEST_CASE("extracting a tilemap yields one instance per non-empty cell") {
    auto created = rhi::create_null_device();
    REQUIRE(created.ok());
    rhi::Device& device = *created.value();
    auto texture = make_solid_texture(device, 64, 32);

    Scene scene;
    scene.create_node("Camera")->add_component<Camera2D>();

    Node* node = scene.create_node("Ground");
    auto* map = node->add_component<Tilemap>();
    map->texture = texture;
    map->columns = 4;
    map->rows = 2;
    map->tile_size = Vec2{16.0f, 16.0f};
    map->resize(2, 2);
    map->set(0, 0, 0);
    map->set(1, 1, 5);
    // (1, 0) and (0, 1) stay empty and must not produce an instance.

    renderer2d::Renderer2D renderer;
    REQUIRE(renderer.initialize(device).ok());

    std::vector<render::SpriteInstance> instances;
    renderer.extract(scene, instances);

    REQUIRE(instances.size() == 2);
    for (const render::SpriteInstance& instance : instances) {
        CHECK((instance.texture == &texture->texture()));
        CHECK((instance.tilemap == map));
        CHECK((instance.sprite == nullptr));
        CHECK(instance.size.x == doctest::Approx(16.0f));
        CHECK(instance.size.y == doctest::Approx(16.0f));
        CHECK(instance.layer == 0);
    }
}

TEST_CASE("a tile's world position follows the top-left origin convention") {
    auto created = rhi::create_null_device();
    REQUIRE(created.ok());
    rhi::Device& device = *created.value();
    auto texture = make_solid_texture(device, 64, 32);

    Scene scene;
    scene.create_node("Camera")->add_component<Camera2D>();

    Node* node = scene.create_node("Ground");
    auto* map = node->add_component<Tilemap>();
    map->texture = texture;
    map->columns = 4;
    map->rows = 2;
    map->tile_size = Vec2{10.0f, 10.0f};
    map->resize(2, 2, 0);
    node->transform().position = Vec3{100.0f, 50.0f, 0.0f};

    std::vector<render::SpriteInstance> instances;
    render::extract_tiles(scene, instances);
    REQUIRE(instances.size() == 4);

    // Cell (0, 0) is the map's top-left, so its centre sits half a tile in from
    // the node origin. +Y goes down, so row 1 is *below* the origin.
    const Vec3 top_left = transform_point(instances[0].world, Vec3{});
    CHECK(top_left.x == doctest::Approx(105.0f));
    CHECK(top_left.y == doctest::Approx(45.0f));

    // Cell (1, 1): one tile right and one tile down from the first.
    const Vec3 bottom_right = transform_point(instances[3].world, Vec3{});
    CHECK(bottom_right.x == doctest::Approx(115.0f));
    CHECK(bottom_right.y == doctest::Approx(35.0f));
}

TEST_CASE("a tile's UV comes from its cell in the tileset") {
    auto created = rhi::create_null_device();
    REQUIRE(created.ok());
    rhi::Device& device = *created.value();
    auto texture = make_solid_texture(device, 64, 32);

    Scene scene;
    Node* node = scene.create_node("Ground");
    auto* map = node->add_component<Tilemap>();
    map->texture = texture;
    map->columns = 4;
    map->rows = 2;
    map->resize(2, 1);
    map->set(0, 0, 0);
    map->set(1, 0, 5);

    std::vector<render::SpriteInstance> instances;
    render::extract_tiles(scene, instances);
    REQUIRE(instances.size() == 2);

    CHECK(instances[0].uv.x == doctest::Approx(0.0f));
    CHECK(instances[0].uv.z == doctest::Approx(0.25f));

    // Tile 5 is column 1 of row 1.
    CHECK(instances[1].uv.x == doctest::Approx(0.25f));
    CHECK(instances[1].uv.y == doctest::Approx(0.5f));
    CHECK(instances[1].uv.w == doctest::Approx(1.0f));
}

TEST_CASE("a tilemap with no texture, an empty grid, or zero tiles contributes nothing") {
    auto created = rhi::create_null_device();
    REQUIRE(created.ok());
    rhi::Device& device = *created.value();
    auto texture = make_solid_texture(device, 64, 32);

    Scene scene;

    // No texture.
    Node* untextured = scene.create_node("Untextured");
    auto* map_a = untextured->add_component<Tilemap>();
    map_a->resize(2, 2, 0);

    // A texture but no grid.
    Node* empty = scene.create_node("Empty");
    auto* map_b = empty->add_component<Tilemap>();
    map_b->texture = texture;

    // A texture and a grid but a zero tile size: an authoring mistake draws
    // nothing rather than a degenerate quad.
    Node* zero = scene.create_node("Zero");
    auto* map_c = zero->add_component<Tilemap>();
    map_c->texture = texture;
    map_c->tile_size = Vec2{0.0f, 0.0f};
    map_c->resize(2, 2, 0);

    std::vector<render::SpriteInstance> instances;
    render::extract_tiles(scene, instances);
    CHECK(instances.empty());
}

TEST_CASE("extract_tiles appends to an array so tiles and sprites share a pass") {
    auto created = rhi::create_null_device();
    REQUIRE(created.ok());
    rhi::Device& device = *created.value();
    auto texture = make_solid_texture(device, 64, 32);

    Scene scene;
    Node* node = scene.create_node("Ground");
    auto* map = node->add_component<Tilemap>();
    map->texture = texture;
    map->columns = 4;
    map->rows = 2;
    map->resize(1, 1, 0);

    std::vector<render::SpriteInstance> instances;
    // A pre-existing entry must survive: extract_tiles appends, it does not
    // clear, which is what lets a caller merge tiles with sprites.
    instances.push_back(render::SpriteInstance{});
    render::extract_tiles(scene, instances);
    CHECK(instances.size() == 2);
}

// ---------------------------------------------------------------------------
// Culling: a tile is culled by the same quad test a sprite uses.
// ---------------------------------------------------------------------------

TEST_CASE("tiles outside the view are culled by the shared sprite cull") {
    auto created = rhi::create_null_device();
    REQUIRE(created.ok());
    rhi::Device& device = *created.value();
    auto texture = make_solid_texture(device, 64, 32);

    Scene scene;
    Node* camera = scene.create_node("Camera");
    auto* camera_component = camera->add_component<Camera2D>();
    // A 64x64 view at zoom 1 spans [-32, 32] in both axes.
    camera_component->zoom = 1.0f;

    // A map of 16x16 tiles starting at x = 0 spans [0, 128], so most of it is
    // off screen to the right.
    Node* node = scene.create_node("Ground");
    auto* map = node->add_component<Tilemap>();
    map->texture = texture;
    map->columns = 4;
    map->rows = 2;
    map->tile_size = Vec2{16.0f, 16.0f};
    map->resize(8, 1, 0);

    renderer2d::Renderer2D renderer;
    REQUIRE(renderer.initialize(device).ok());

    std::vector<render::SpriteInstance> instances;
    renderer.extract(scene, instances);
    REQUIRE(instances.size() == 8);

    const Frustum frustum = Frustum::from_view_projection(camera_component->view_projection(64, 64));
    const usize culled = renderer2d::Renderer2D::cull(instances, &frustum);

    // Tiles at x = 0, 16, 32 are visible; the rest are not.
    CHECK(culled == 5);
    CHECK(instances.size() == 3);
}

// ---------------------------------------------------------------------------
// Serialization: the grid survives a whole-scene round trip, not just its own
// JSON. This is the integration the reflection property exists for.
// ---------------------------------------------------------------------------

TEST_CASE("a scene with a malformed grid loads without throwing and keeps a valid map") {
    reflect::register_tilemap_types();

    // Serialize a real scene, then corrupt only the grid string inside it — the
    // edit a person makes by hand. Reloading must succeed with a usable
    // component rather than throwing out of the public API or leaving a
    // half-built grid, because the serializer treats a rejected setter as
    // "keep the default".
    Scene authored;
    Node* node = authored.create_node("Ground");
    node->add_component<Tilemap>()->resize(2, 2, 1);

    auto text = serialization::scene::to_json(authored);
    REQUIRE(text);

    std::string corrupted = text.value();
    // The grid is itself a JSON string, so its inner quotes are escaped in the
    // document. Match the cell list and inject a non-numeric cell.
    const std::string valid = "[1,1,1,1]";
    const usize at = corrupted.find(valid);
    REQUIRE(at != std::string::npos);
    corrupted.replace(at, valid.size(), R"([1,\"x\"])");

    Scene scene;
    REQUIRE_FALSE(serialization::scene::from_json(corrupted, scene));

    const Node* reloaded = scene.find_node_by_name("Ground");
    REQUIRE(reloaded != nullptr);
    const auto* map = reloaded->get_component<Tilemap>();
    REQUIRE(map != nullptr);
    // The default component is a valid empty grid, not a partially applied one.
    CHECK(map->cells.size() == static_cast<usize>(map->width) * map->height);
}

TEST_CASE("a tilemap round-trips through the scene serializer") {
    reflect::register_tilemap_types();

    Scene original;
    Node* node = original.create_node("Ground");
    auto* map = node->add_component<Tilemap>();
    map->columns = 4;
    map->rows = 2;
    map->tile_size = Vec2{8.0f, 8.0f};
    map->layer = 2;
    map->resize(3, 2);
    map->set(0, 0, 1);
    map->set(2, 1, 6);
    node->transform().position = Vec3{32.0f, 64.0f, 0.0f};

    auto text = serialization::scene::to_json(original);
    REQUIRE(text);

    Scene restored;
    REQUIRE_FALSE(serialization::scene::from_json(text.value(), restored));

    const Node* restored_node = restored.find_node_by_name("Ground");
    REQUIRE(restored_node != nullptr);
    const auto* restored_map = restored_node->get_component<Tilemap>();
    REQUIRE(restored_map != nullptr);

    CHECK(restored_map->width == 3);
    CHECK(restored_map->height == 2);
    CHECK(restored_map->columns == 4);
    CHECK(restored_map->rows == 2);
    CHECK(restored_map->layer == 2);
    CHECK(restored_map->tile_size.x == doctest::Approx(8.0f));
    CHECK(restored_map->at(0, 0) == 1);
    CHECK(restored_map->at(1, 0) == Tilemap::kEmpty);
    CHECK(restored_map->at(2, 1) == 6);
    CHECK(restored_node->transform().position.x == doctest::Approx(32.0f));
}

// ---------------------------------------------------------------------------
// The full render path: extract, cull, and draw agree, and a map and a sprite
// over it sort by layer.
// ---------------------------------------------------------------------------

TEST_CASE("the 2D render draws tiles and sprites through one path") {
    auto created = rhi::create_null_device();
    REQUIRE(created.ok());
    rhi::Device& device = *created.value();
    auto texture = make_solid_texture(device, 64, 32);

    Scene scene;
    Node* camera = scene.create_node("Camera");
    auto* camera_component = camera->add_component<Camera2D>();
    camera_component->zoom = 1.0f;

    Node* ground = scene.create_node("Ground");
    auto* map = ground->add_component<Tilemap>();
    map->texture = texture;
    map->columns = 4;
    map->rows = 2;
    map->tile_size = Vec2{16.0f, 16.0f};
    map->resize(2, 2, 0); // Four tiles, all within a 64x64 view at the origin.

    renderer2d::Renderer2D renderer;
    REQUIRE(renderer.initialize(device).ok());

    device.begin_frame(64, 64, rhi::ClearColor{});
    auto drawn = renderer.render(device, scene, 64, 64);
    REQUIRE(drawn.ok());

    // Every tile is on screen, so all four are drawn and none are culled.
    CHECK(drawn.value() == 4);
    CHECK(renderer.last_culled() == 0);
    device.end_frame();
}

TEST_CASE("a tilemap and a sprite over it sort by layer into one pass") {
    auto created = rhi::create_null_device();
    REQUIRE(created.ok());
    rhi::Device& device = *created.value();
    auto texture = make_solid_texture(device, 64, 32);

    Scene scene;
    Node* ground = scene.create_node("Ground");
    auto* map = ground->add_component<Tilemap>();
    map->texture = texture;
    map->columns = 4;
    map->rows = 2;
    map->resize(1, 1, 0);
    map->layer = 0;

    Node* hero = scene.create_node("Hero");
    auto* sprite = hero->add_component<Sprite2D>();
    sprite->texture = texture;
    sprite->size = Vec2{8.0f, 8.0f};
    sprite->layer = 5;

    renderer2d::Renderer2D renderer;
    REQUIRE(renderer.initialize(device).ok());

    std::vector<render::SpriteInstance> instances;
    renderer.extract(scene, instances);
    REQUIRE(instances.size() == 2);

    std::sort(instances.begin(), instances.end(), [](const render::SpriteInstance& a, const render::SpriteInstance& b) {
        if (a.layer != b.layer) {
            return a.layer < b.layer;
        }
        return a.texture < b.texture;
    });

    // The map is on layer 0 and the hero on layer 5, so the hero draws last:
    // layer is the primary key, which is what keeps artist intent over batching.
    CHECK(instances.front().layer == 0);
    CHECK((instances.front().tilemap == map));
    CHECK(instances.back().layer == 5);
    CHECK((instances.back().sprite == sprite));
}
