// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
//
// Tests for parallel extraction and culling (M9, ADR-0017, ADR-0069).
//
// The property that matters is equivalence: chunking the instance array across
// threads must produce exactly the array the serial path produces, because the
// draw step reads that array and a different array is a different image. These
// tests build a scene large enough to cross the parallel thresholds, run the
// step both ways, and compare the results with exact equality -- not approximate.
//
// They run with no display and no GL context: extraction and culling are pure
// CPU work over scene data, and a `JobSystem` is real threads with no device
// involved. The `JobSystem(0)` case proves the same code path a plain build takes.

#include <doctest/doctest.h>

#include "fe/core/FrameArena.h"
#include "fe/core/Frustum.h"
#include "fe/jobs/JobSystem.h"
#include "fe/mesh/Mesh.h"
#include "fe/render/RenderExtract.h"
#include "fe/renderer2d/Renderer2D.h"
#include "fe/renderer3d/Renderer3D.h"
#include "fe/resources/Texture2D.h"
#include "fe/rhi/Device.h"
#include "fe/rhi/Resources.h"
#include "fe/rhi/Types.h"
#include "fe/scene/Camera2D.h"
#include "fe/scene/Node.h"
#include "fe/scene/Scene.h"
#include "fe/scene/Sprite2D.h"
#include "fe/scene/Tilemap.h"
#include "fe/scene3d/Camera3D.h"
#include "fe/scene3d/MeshRenderer3D.h"

#include <memory>
#include <vector>

using namespace fe;

namespace {

[[nodiscard]] std::shared_ptr<Texture2D> make_solid_texture(rhi::Device& device, u32 width, u32 height) {
    std::vector<u8> pixels(static_cast<usize>(width) * height * 4, 255);
    rhi::TextureDesc desc;
    desc.name = "sprite";
    desc.width = width;
    desc.height = height;
    desc.pixels = pixels.data();
    auto texture = device.create_texture(desc);
    REQUIRE(texture.ok());
    return std::make_shared<Texture2D>("tex:solid.png", texture.value());
}

[[nodiscard]] Mat4 looking_at_origin(f32 aspect) {
    const Mat4 view = look_at(Vec3{0.0f, 0.0f, 5.0f}, Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.0f, 1.0f, 0.0f});
    const Mat4 projection = perspective(radians(60.0f), aspect, 0.1f, 100.0f);
    return projection * view;
}

// Fills `scene` with `count` sprites spread over a grid, so the extracted array
// is large and not all of it is in view. The spread also gives the cull both
// keep and drop cases to agree on. `Scene` is not copyable, so the helper fills
// a caller-owned scene rather than returning one.
void add_sprite_grid(Scene& scene, const std::shared_ptr<Texture2D>& texture, u32 count) {
    scene.create_node("Camera")->add_component<Camera2D>();
    for (u32 i = 0; i < count; ++i) {
        Node* node = scene.create_node("Sprite");
        auto* sprite = node->add_component<Sprite2D>();
        sprite->texture = texture;
        sprite->size = Vec2{16.0f, 16.0f};
        sprite->layer = static_cast<i32>(i % 4);
        node->transform().position =
            Vec3{static_cast<f32>(i % 64) * 40.0f - 1280.0f, static_cast<f32>(i / 64) * 40.0f - 1280.0f, 0.0f};
    }
}

// Fills `scene` with `count` unit cubes on a grid.
void add_mesh_grid(Scene& scene, u32 count) {
    scene.create_node("Camera")->add_component<Camera3D>();
    for (u32 i = 0; i < count; ++i) {
        Node* node = scene.create_node("Cube");
        auto* renderer = node->add_component<MeshRenderer3D>();
        renderer->mesh = Mesh::make_cube();
        node->transform().position =
            Vec3{static_cast<f32>(i % 32) * 3.0f - 48.0f, static_cast<f32>(i / 32) * 3.0f - 48.0f, 0.0f};
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Extraction: parallel output is identical to serial.
// ---------------------------------------------------------------------------

TEST_CASE("parallel sprite extraction is identical to serial") {
    auto created = rhi::create_null_device();
    REQUIRE(created.ok());
    rhi::Device& device = *created.value();
    auto texture = make_solid_texture(device, 8, 8);

    // Well past `kParallelExtractThreshold`, so the parallel path is taken.
    Scene scene;
    add_sprite_grid(scene, texture, 4000);

    renderer2d::Renderer2D renderer;
    REQUIRE(renderer.initialize(device).ok());

    std::vector<render::SpriteInstance> serial;
    renderer.extract(scene, serial, nullptr);

    JobSystem jobs(4);
    std::vector<render::SpriteInstance> parallel;
    renderer.extract(scene, parallel, &jobs);

    REQUIRE(parallel.size() == serial.size());
    CHECK(parallel.size() == 4000);
    // Exact equality: same instance in the same slot. An approximate compare
    // would not catch a reordered or lost instance, which is the bug this guards.
    for (usize i = 0; i < serial.size(); ++i) {
        CHECK(parallel[i] == serial[i]);
    }
}

TEST_CASE("parallel mesh extraction is identical to serial, including meshes_seen") {
    Scene scene;
    add_mesh_grid(scene, 2000);

    renderer3d::Renderer3D renderer;

    std::vector<render::MeshInstance> serial;
    std::vector<const Mesh*> serial_seen;
    renderer.extract(scene, serial, &serial_seen, nullptr);

    JobSystem jobs(4);
    std::vector<render::MeshInstance> parallel;
    std::vector<const Mesh*> parallel_seen;
    renderer.extract(scene, parallel, &parallel_seen, &jobs);

    REQUIRE(parallel.size() == serial.size());
    CHECK(parallel.size() == 2000);
    for (usize i = 0; i < serial.size(); ++i) {
        CHECK(parallel[i] == serial[i]);
    }
    // The liveness list must match too: the buffer sweep frees what it does not
    // see, so a reordered list would unload a mesh that is still drawn.
    REQUIRE(parallel_seen.size() == serial_seen.size());
    for (usize i = 0; i < serial_seen.size(); ++i) {
        CHECK((parallel_seen[i] == serial_seen[i]));
    }
}

TEST_CASE("a single-threaded JobSystem takes the serial path and matches it") {
    auto created = rhi::create_null_device();
    REQUIRE(created.ok());
    rhi::Device& device = *created.value();
    auto texture = make_solid_texture(device, 8, 8);

    Scene scene;
    add_sprite_grid(scene, texture, 4000);

    renderer2d::Renderer2D renderer;
    REQUIRE(renderer.initialize(device).ok());

    std::vector<render::SpriteInstance> serial;
    renderer.extract(scene, serial, nullptr);

    // worker_count == 0: `is_threaded()` is false, so the parallel branch is not
    // entered even though a JobSystem pointer is supplied.
    JobSystem inline_jobs;
    std::vector<render::SpriteInstance> from_inline;
    renderer.extract(scene, from_inline, &inline_jobs);

    REQUIRE(from_inline.size() == serial.size());
    for (usize i = 0; i < serial.size(); ++i) {
        CHECK(from_inline[i] == serial[i]);
    }
}

TEST_CASE("a scene below the threshold ignores the pool and stays serial") {
    auto created = rhi::create_null_device();
    REQUIRE(created.ok());
    rhi::Device& device = *created.value();
    auto texture = make_solid_texture(device, 8, 8);

    // Fewer nodes than `kParallelExtractThreshold`: the result must still match,
    // and this pins that the threshold does not change *what* is extracted.
    Scene scene;
    add_sprite_grid(scene, texture, 8);

    renderer2d::Renderer2D renderer;
    REQUIRE(renderer.initialize(device).ok());

    std::vector<render::SpriteInstance> serial;
    renderer.extract(scene, serial, nullptr);

    JobSystem jobs(4);
    std::vector<render::SpriteInstance> parallel;
    renderer.extract(scene, parallel, &jobs);

    REQUIRE(parallel.size() == serial.size());
    CHECK(parallel.size() == 8);
    for (usize i = 0; i < serial.size(); ++i) {
        CHECK(parallel[i] == serial[i]);
    }
}

TEST_CASE("parallel tile extraction is identical to serial") {
    auto created = rhi::create_null_device();
    REQUIRE(created.ok());
    rhi::Device& device = *created.value();
    auto texture = make_solid_texture(device, 64, 64);

    Scene scene;
    scene.create_node("Camera")->add_component<Camera2D>();
    Node* node = scene.create_node("Map");
    auto* map = node->add_component<Tilemap>();
    map->texture = texture;
    map->tile_size = Vec2{16.0f, 16.0f};
    // 96x96 = 9216 tiles, past `kParallelTileThreshold`, half of them filled.
    map->resize(96, 96, Tilemap::kEmpty);
    for (u32 y = 0; y < 96; ++y) {
        for (u32 x = 0; x < 96; ++x) {
            if ((x + y) % 2 == 0) {
                map->set(x, y, static_cast<i32>((x + y) % 4));
            }
        }
    }

    renderer2d::Renderer2D renderer;
    REQUIRE(renderer.initialize(device).ok());

    std::vector<render::SpriteInstance> serial;
    renderer.extract(scene, serial, nullptr);

    JobSystem jobs(4);
    std::vector<render::SpriteInstance> parallel;
    renderer.extract(scene, parallel, &jobs);

    REQUIRE(serial.size() == 96 * 96 / 2);
    REQUIRE(parallel.size() == serial.size());
    for (usize i = 0; i < serial.size(); ++i) {
        CHECK(parallel[i] == serial[i]);
    }
}

// ---------------------------------------------------------------------------
// Culling: parallel output is identical to serial.
// ---------------------------------------------------------------------------

TEST_CASE("parallel sprite culling keeps the same instances in the same order") {
    auto created = rhi::create_null_device();
    REQUIRE(created.ok());
    rhi::Device& device = *created.value();
    auto texture = make_solid_texture(device, 8, 8);

    // Past `kParallelCullThreshold` (16384), which is deliberately high: below it
    // the fan-out costs more than the test it saves.
    Scene scene;
    add_sprite_grid(scene, texture, 20000);

    renderer2d::Renderer2D renderer;
    REQUIRE(renderer.initialize(device).ok());

    std::vector<render::SpriteInstance> base;
    renderer.extract(scene, base, nullptr);
    REQUIRE(base.size() == 20000);

    const Frustum frustum = Frustum::from_view_projection(Mat4::orthographic(0.0f, 1280.0f, 720.0f, 0.0f, -1.0f, 1.0f));

    std::vector<render::SpriteInstance> serial = base;
    const usize serial_dropped = renderer2d::Renderer2D::cull(serial, &frustum, nullptr);

    JobSystem jobs(4);
    std::vector<render::SpriteInstance> parallel = base;
    const usize parallel_dropped = renderer2d::Renderer2D::cull(parallel, &frustum, &jobs);

    CHECK(parallel_dropped == serial_dropped);
    // Some were culled and some survived: the case is only meaningful if both
    // paths actually had work to do.
    CHECK(serial_dropped > 0);
    CHECK(serial.size() > 0);
    REQUIRE(parallel.size() == serial.size());
    for (usize i = 0; i < serial.size(); ++i) {
        CHECK(parallel[i] == serial[i]);
    }
}

TEST_CASE("parallel mesh culling keeps the same instances in the same order") {
    Scene scene;
    add_mesh_grid(scene, 20000);

    renderer3d::Renderer3D renderer;
    std::vector<render::MeshInstance> base;
    renderer.extract(scene, base, nullptr, nullptr);
    REQUIRE(base.size() == 20000);

    const Frustum frustum = Frustum::from_view_projection(looking_at_origin(1.0f));

    std::vector<render::MeshInstance> serial = base;
    const usize serial_dropped = renderer3d::Renderer3D::cull_flat(serial, &frustum, nullptr);

    JobSystem jobs(4);
    std::vector<render::MeshInstance> parallel = base;
    const usize parallel_dropped = renderer3d::Renderer3D::cull_flat(parallel, &frustum, &jobs);

    CHECK(parallel_dropped == serial_dropped);
    CHECK(serial_dropped > 0);
    REQUIRE(parallel.size() == serial.size());
    for (usize i = 0; i < serial.size(); ++i) {
        CHECK(parallel[i] == serial[i]);
    }
}

TEST_CASE("a null frustum culls nothing in parallel mode too") {
    JobSystem jobs(4);
    std::vector<render::MeshInstance> meshes(20000);
    CHECK(renderer3d::Renderer3D::cull_flat(meshes, nullptr, &jobs) == 0);
    CHECK(meshes.size() == 20000);

    std::vector<render::SpriteInstance> sprites(20000);
    CHECK(renderer2d::Renderer2D::cull(sprites, nullptr, &jobs) == 0);
    CHECK(sprites.size() == 20000);
}

// ---------------------------------------------------------------------------
// Through render(): the counts the caller sees do not change with a pool.
// ---------------------------------------------------------------------------

TEST_CASE("render() reports the same draw and cull counts with and without a pool") {
    auto created = rhi::create_null_device();
    REQUIRE(created.ok());
    rhi::Device& device = *created.value();
    auto texture = make_solid_texture(device, 8, 8);

    Scene scene;
    add_sprite_grid(scene, texture, 4000);

    renderer2d::Renderer2D serial_renderer;
    REQUIRE(serial_renderer.initialize(device).ok());
    auto serial = serial_renderer.render(device, scene, 640, 480, nullptr);
    REQUIRE(serial.ok());

    renderer2d::Renderer2D parallel_renderer;
    REQUIRE(parallel_renderer.initialize(device).ok());
    JobSystem jobs(4);
    auto parallel = parallel_renderer.render(device, scene, 640, 480, &jobs);
    REQUIRE(parallel.ok());

    // Identical outcome: the same sprites drawn, the same number culled. The
    // image is a function of these, and they do not depend on the pool.
    CHECK(parallel.value() == serial.value());
    CHECK(parallel_renderer.last_culled() == serial_renderer.last_culled());
    CHECK(parallel_renderer.last_draw_calls() == serial_renderer.last_draw_calls());
}

// ---------------------------------------------------------------------------
// Arena-backed extraction (ADR-0054 follow-on): identical output, no heap.
// ---------------------------------------------------------------------------

TEST_CASE("arena-backed extraction matches the vector path, serial and parallel") {
    auto created = rhi::create_null_device();
    REQUIRE(created.ok());
    rhi::Device& device = *created.value();
    auto texture = make_solid_texture(device, 8, 8);

    // Past the parallel threshold, so the arena path's count-then-write fan-out
    // is exercised, not just its serial branch.
    Scene scene;
    add_sprite_grid(scene, texture, 4000);

    renderer2d::Renderer2D renderer;
    REQUIRE(renderer.initialize(device).ok());

    std::vector<render::SpriteInstance> reference;
    renderer.extract(scene, reference, nullptr);
    REQUIRE(reference.size() == 4000);

    const usize bound = render::sprite_instance_bound(scene);
    REQUIRE(bound >= reference.size());

    FrameArena arena(sizeof(render::SpriteInstance) * bound + (1u << 16));
    std::vector<render::SpriteInstance> storage(bound);
    const usize written = render::extract_sprites_into(arena, scene, storage, nullptr);

    REQUIRE(written == reference.size());
    // Exact equality: the arena path must produce the same instance in the same
    // slot, or the draw reads different data.
    for (usize i = 0; i < reference.size(); ++i) {
        CHECK(storage[i] == reference[i]);
    }

    // The same must hold with a real pool, where the two-pass count/write runs.
    JobSystem jobs(4);
    FrameArena parallel_arena(sizeof(render::SpriteInstance) * bound + (1u << 16));
    std::vector<render::SpriteInstance> parallel_storage(bound);
    const usize parallel_written = render::extract_sprites_into(parallel_arena, scene, parallel_storage, &jobs);

    REQUIRE(parallel_written == reference.size());
    for (usize i = 0; i < reference.size(); ++i) {
        CHECK(parallel_storage[i] == reference[i]);
    }
}

TEST_CASE("arena-backed extraction respects a short output bound") {
    auto created = rhi::create_null_device();
    REQUIRE(created.ok());
    rhi::Device& device = *created.value();
    auto texture = make_solid_texture(device, 8, 8);

    Scene scene;
    add_sprite_grid(scene, texture, 64);

    FrameArena arena(1u << 16);
    std::vector<render::SpriteInstance> storage(10);
    const usize written = render::extract_sprites_into(arena, scene, storage, nullptr);

    // The bound is smaller than the scene, so exactly the bound is written and
    // nothing past it is touched. This is the documented "the caller sizes it"
    // contract, and it must not overrun.
    CHECK(written == 10);
}

TEST_CASE("arena-backed cull keeps the same survivors as the vector cull") {
    auto created = rhi::create_null_device();
    REQUIRE(created.ok());
    rhi::Device& device = *created.value();
    auto texture = make_solid_texture(device, 8, 8);

    Scene scene;
    add_sprite_grid(scene, texture, 4000);

    renderer2d::Renderer2D renderer;
    REQUIRE(renderer.initialize(device).ok());

    std::vector<render::SpriteInstance> extracted;
    renderer.extract(scene, extracted, nullptr);

    // The scene's own camera drives the view, so some sprites are in view and
    // some are not: the cull must have both keep and drop cases to agree on.
    const Frustum frustum = Frustum::from_view_projection(Camera2D{}.view_projection(640, 480));

    std::vector<render::SpriteInstance> vector_survivors = extracted;
    const usize dropped = renderer2d::Renderer2D::cull(vector_survivors, &frustum, nullptr);

    FrameArena arena(sizeof(render::SpriteInstance) * extracted.size() + (1u << 16));
    std::vector<render::SpriteInstance> storage = extracted;
    const usize kept = render::cull_sprites_in_place(arena, storage, storage.size(), &frustum, nullptr);

    REQUIRE(kept == vector_survivors.size());
    CHECK(extracted.size() - kept == dropped);
    for (usize i = 0; i < kept; ++i) {
        CHECK(storage[i] == vector_survivors[i]);
    }
}
