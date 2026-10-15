// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
//
// Tests for the extraction seam (ADR-0052).
//
// These run with no display and no GL context: extraction and culling are pure
// CPU work over scene data, and the draw step is exercised against the null RHI
// device, which proves submission without a rasteriser. The point of the seam is
// that this split exists, so the tests assert on the extracted arrays and the
// reported counters, not on pixels -- pixel assertions belong in the graphics
// suite, which needs a real context.

#include <doctest/doctest.h>

#include "fe/core/Frustum.h"
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
#include "fe/scene3d/Camera3D.h"
#include "fe/scene3d/DirectionalLight3D.h"
#include "fe/scene3d/MeshRenderer3D.h"

#include <memory>
#include <vector>

using namespace fe;

namespace {

// A `Texture2D` built on the null device. The 2D renderer only extracts sprites
// that reference a `Texture2D`, so this is the smallest sprite the seam will
// collect: no decode, no file, no GPU object.
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

// A view-projection looking straight down the -Z axis at the origin, so a box at
// the origin is visible and a box far along +X is not. Built from `look_at` so
// the test does not depend on the camera component's own conventions.
[[nodiscard]] Mat4 looking_at_origin(f32 aspect) {
    const Mat4 view = look_at(Vec3{0.0f, 0.0f, 5.0f}, Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.0f, 1.0f, 0.0f});
    const Mat4 projection = perspective(radians(60.0f), aspect, 0.1f, 100.0f);
    return projection * view;
}

} // namespace

// ---------------------------------------------------------------------------
// Extraction: the array is the seam.
// ---------------------------------------------------------------------------

TEST_CASE("extracting a 2D scene yields one instance per textured sprite") {
    auto created = rhi::create_null_device();
    REQUIRE(created.ok());
    rhi::Device& device = *created.value();
    auto texture = make_solid_texture(device, 8, 8);

    Scene scene;
    scene.create_node("Camera")->add_component<Camera2D>();

    Node* node = scene.create_node("Sprite");
    auto* sprite = node->add_component<Sprite2D>();
    sprite->texture = texture;
    sprite->size = Vec2{4.0f, 6.0f};
    sprite->layer = 3;
    node->transform().position = Vec3{1.0f, 2.0f, 0.0f};

    // A sprite with no texture is an authoring state, not content: it must not
    // appear in the array.
    scene.create_node("Untextured")->add_component<Sprite2D>();

    renderer2d::Renderer2D renderer;
    REQUIRE(renderer.initialize(device).ok());

    std::vector<render::SpriteInstance> instances;
    renderer.extract(scene, instances);

    REQUIRE(instances.size() == 1);
    CHECK((instances[0].texture == &texture->texture()));
    CHECK((instances[0].sprite == sprite));
    CHECK(instances[0].size.x == doctest::Approx(4.0f));
    CHECK(instances[0].size.y == doctest::Approx(6.0f));
    // The world matrix carries the node's translation; extraction does not bake
    // the size into it.
    const Vec3 origin = transform_point(instances[0].world, Vec3{});
    CHECK(origin.x == doctest::Approx(1.0f));
    CHECK(origin.y == doctest::Approx(2.0f));
}

TEST_CASE("a sprite with no explicit size takes its size from the texture") {
    auto created = rhi::create_null_device();
    REQUIRE(created.ok());
    rhi::Device& device = *created.value();
    auto texture = make_solid_texture(device, 32, 16);

    Scene scene;
    scene.create_node("Camera")->add_component<Camera2D>();
    Node* node = scene.create_node("Sprite");
    auto* sprite = node->add_component<Sprite2D>();
    sprite->texture = texture;
    sprite->size = Vec2{0.0f, 0.0f}; // "use the texture size".

    renderer2d::Renderer2D renderer;
    REQUIRE(renderer.initialize(device).ok());

    std::vector<render::SpriteInstance> instances;
    renderer.extract(scene, instances);

    REQUIRE(instances.size() == 1);
    CHECK(instances[0].size.x == doctest::Approx(32.0f));
    CHECK(instances[0].size.y == doctest::Approx(16.0f));
}

TEST_CASE("extracting a 3D scene bakes each mesh's world AABB into its instance") {
    Scene scene;
    scene.create_node("Camera")->add_component<Camera3D>();

    Node* node = scene.create_node("Cube");
    auto* mesh_renderer = node->add_component<MeshRenderer3D>();
    mesh_renderer->mesh = Mesh::make_cube(); // A unit cube, half extent 0.5.
    node->transform().position = Vec3{10.0f, 0.0f, 0.0f};

    renderer3d::Renderer3D renderer;
    std::vector<render::MeshInstance> instances;
    std::vector<const Mesh*> meshes_seen;
    renderer.extract(scene, instances, &meshes_seen);

    REQUIRE(instances.size() == 1);
    CHECK((instances[0].mesh == mesh_renderer->mesh.get()));
    CHECK((instances[0].renderer == mesh_renderer));
    // The cube's local AABB is [-0.5, 0.5]^3; translated by 10 on X its world
    // AABB is [9.5, 10.5] x [-0.5, 0.5]^2. The AABB is precomputed so culling
    // needs neither the mesh nor the scene.
    CHECK(instances[0].world_min.x == doctest::Approx(9.5f));
    CHECK(instances[0].world_max.x == doctest::Approx(10.5f));
    CHECK(instances[0].world_min.y == doctest::Approx(-0.5f));
    CHECK(instances[0].world_max.z == doctest::Approx(0.5f));

    // Extraction records the mesh for the buffer-liveness sweep.
    REQUIRE(meshes_seen.size() == 1);
    CHECK((meshes_seen[0] == mesh_renderer->mesh.get()));
}

TEST_CASE("extraction composes a child's world transform through its parent") {
    Scene scene;
    scene.create_node("Camera")->add_component<Camera3D>();

    Node* parent = scene.create_node("Parent");
    parent->transform().position = Vec3{4.0f, 0.0f, 0.0f};

    Node* child = parent->create_child<Node>("Child");
    auto* mesh_renderer = child->add_component<MeshRenderer3D>();
    mesh_renderer->mesh = Mesh::make_cube();
    child->transform().position = Vec3{0.0f, 3.0f, 0.0f};

    renderer3d::Renderer3D renderer;
    std::vector<render::MeshInstance> instances;
    renderer.extract(scene, instances, nullptr);

    REQUIRE(instances.size() == 1);
    // The world matrix is parent * child, so the cube sits at (4, 3, 0).
    const Vec3 origin = transform_point(instances[0].model, Vec3{});
    CHECK(origin.x == doctest::Approx(4.0f));
    CHECK(origin.y == doctest::Approx(3.0f));
}

// ---------------------------------------------------------------------------
// Culling: pure over the array, conservative, and testable with no scene.
// ---------------------------------------------------------------------------

TEST_CASE("a null frustum culls nothing") {
    std::vector<render::MeshInstance> instances(3);
    CHECK(renderer3d::Renderer3D::cull_flat(instances, nullptr) == 0);
    CHECK(instances.size() == 3);

    std::vector<render::SpriteInstance> sprites(2);
    CHECK(renderer2d::Renderer2D::cull(sprites, nullptr) == 0);
    CHECK(sprites.size() == 2);
}

TEST_CASE("culling keeps a box inside the frustum and drops one wholly outside") {
    const Frustum frustum = Frustum::from_view_projection(looking_at_origin(1.0f));

    std::vector<render::MeshInstance> instances(2);
    // Instance 0: a unit cube at the origin, dead centre of the view.
    instances[0].world_min = Vec3{-0.5f, -0.5f, -0.5f};
    instances[0].world_max = Vec3{0.5f, 0.5f, 0.5f};
    // Instance 1: a unit cube far to the right, outside the frustum.
    instances[1].world_min = Vec3{100.0f, -0.5f, -0.5f};
    instances[1].world_max = Vec3{101.0f, 0.5f, 0.5f};

    CHECK(renderer3d::Renderer3D::cull_flat(instances, &frustum) == 1);
    REQUIRE(instances.size() == 1);
    CHECK(instances[0].world_min.x == doctest::Approx(-0.5f));
}

TEST_CASE("culling keeps a box that straddles a frustum plane") {
    const Frustum frustum = Frustum::from_view_projection(looking_at_origin(1.0f));

    // A huge box that contains the camera and spills past every plane. A
    // conservative test must keep it: the box is not *wholly* outside anything.
    std::vector<render::MeshInstance> instances(1);
    instances[0].world_min = Vec3{-1000.0f, -1000.0f, -1000.0f};
    instances[0].world_max = Vec3{1000.0f, 1000.0f, 1000.0f};

    CHECK(renderer3d::Renderer3D::cull_flat(instances, &frustum) == 0);
    CHECK(instances.size() == 1);
}

TEST_CASE("culling a sprite uses its quad size, not a point") {
    const Frustum frustum = Frustum::from_view_projection(looking_at_origin(1.0f));

    // One sprite centred at the origin, one far to the right. Both are drawn as
    // quads; the right-hand one is out of view and must be dropped.
    std::vector<render::SpriteInstance> instances(2);
    instances[0].size = Vec2{1.0f, 1.0f};
    instances[0].world = Mat4::identity();
    instances[1].size = Vec2{1.0f, 1.0f};
    instances[1].world = Mat4::translation(Vec3{100.0f, 0.0f, 0.0f});

    CHECK(renderer2d::Renderer2D::cull(instances, &frustum) == 1);
    REQUIRE(instances.size() == 1);
    CHECK(instances[0].world.at(0, 3) == doctest::Approx(0.0f));
}

// ---------------------------------------------------------------------------
// Culling through the spatial hierarchy: the BVH path must match the flat
// reference and must rebuild the tree only when the geometry moves.
// ---------------------------------------------------------------------------

TEST_CASE("the BVH cull matches the flat cull over a grid of boxes") {
    const Frustum frustum = Frustum::from_view_projection(looking_at_origin(1.0f));

    // A spread of boxes: some dead centre, some far outside on every axis, so
    // the two paths have both keep and drop cases to agree on.
    std::vector<render::MeshInstance> source;
    for (int x = -3; x <= 3; ++x) {
        for (int y = -3; y <= 3; ++y) {
            for (int z = -3; z <= 3; ++z) {
                render::MeshInstance instance;
                const Vec3 centre{static_cast<f32>(x) * 3.0f, static_cast<f32>(y) * 3.0f, static_cast<f32>(z) * 3.0f};
                instance.world_min = centre - Vec3{0.5f, 0.5f, 0.5f};
                instance.world_max = centre + Vec3{0.5f, 0.5f, 0.5f};
                source.push_back(instance);
            }
        }
    }

    std::vector<render::MeshInstance> flat = source;
    const usize flat_dropped = renderer3d::Renderer3D::cull_flat(flat, &frustum);

    renderer3d::Renderer3D renderer;
    std::vector<render::MeshInstance> tree = source;
    const usize tree_dropped = renderer.cull(tree, &frustum);

    // Same count dropped, same survivors, in the same order: the tree changes
    // how the visible set is found, never which set it is.
    CHECK(tree_dropped == flat_dropped);
    REQUIRE(tree.size() == flat.size());
    for (usize i = 0; i < tree.size(); ++i) {
        CHECK(tree[i].world_min.x == doctest::Approx(flat[i].world_min.x));
        CHECK(tree[i].world_max.z == doctest::Approx(flat[i].world_max.z));
    }
}

TEST_CASE("the cull BVH is reused while the geometry is static") {
    const Frustum frustum = Frustum::from_view_projection(looking_at_origin(1.0f));

    std::vector<render::MeshInstance> source(8);
    for (usize i = 0; i < source.size(); ++i) {
        const Vec3 centre{static_cast<f32>(i), 0.0f, 0.0f};
        source[i].world_min = centre - Vec3{0.5f, 0.5f, 0.5f};
        source[i].world_max = centre + Vec3{0.5f, 0.5f, 0.5f};
    }

    renderer3d::Renderer3D renderer;
    for (int frame = 0; frame < 5; ++frame) {
        std::vector<render::MeshInstance> instances = source;
        (void)renderer.cull(instances, &frustum);
    }

    // The geometry never changed, so the tree was built once and queried five
    // times. This is the point of the cache: a moving camera over static
    // geometry does not rebuild.
    CHECK(renderer.bvh_rebuilds() == 1);
}

TEST_CASE("the cull BVH rebuilds when an instance moves") {
    const Frustum frustum = Frustum::from_view_projection(looking_at_origin(1.0f));

    std::vector<render::MeshInstance> instances(4);
    for (usize i = 0; i < instances.size(); ++i) {
        instances[i].world_min = Vec3{static_cast<f32>(i), 0.0f, 0.0f};
        instances[i].world_max = Vec3{static_cast<f32>(i) + 0.5f, 0.5f, 0.5f};
    }

    renderer3d::Renderer3D renderer;
    (void)renderer.cull(instances, &frustum);
    CHECK(renderer.bvh_rebuilds() == 1);

    // Move one instance: its key changes, so the tree must be rebuilt.
    std::vector<render::MeshInstance> moved = instances;
    moved[0].world_min.x += 10.0f;
    moved[0].world_max.x += 10.0f;
    (void)renderer.cull(moved, &frustum);
    CHECK(renderer.bvh_rebuilds() == 2);

    // A null frustum keeps everything and does not touch the cache.
    std::vector<render::MeshInstance> again = moved;
    CHECK(renderer.cull(again, nullptr) == 0);
    CHECK(again.size() == moved.size());
    CHECK(renderer.bvh_rebuilds() == 2);
}

// ---------------------------------------------------------------------------
// Draw: the seam's steps compose back into render(), against a null device.
// ---------------------------------------------------------------------------

TEST_CASE("the 2D render() extracts, culls, and draws in agreement with its steps") {
    auto created = rhi::create_null_device();
    REQUIRE(created.ok());
    rhi::Device& device = *created.value();
    auto texture = make_solid_texture(device, 4, 4);

    Scene scene;
    Node* camera = scene.create_node("Camera");
    auto* camera_component = camera->add_component<Camera2D>();
    // A 64x64 view at zoom 1 spans [-32, 32] in both axes, so a sprite at
    // x = 1000 is off screen.
    camera_component->zoom = 1.0f;

    Node* on_screen = scene.create_node("OnScreen");
    auto* on_sprite = on_screen->add_component<Sprite2D>();
    on_sprite->texture = texture;
    on_sprite->size = Vec2{4.0f, 4.0f};

    Node* off_screen = scene.create_node("OffScreen");
    auto* off_sprite = off_screen->add_component<Sprite2D>();
    off_sprite->texture = texture;
    off_sprite->size = Vec2{4.0f, 4.0f};
    off_screen->transform().position = Vec3{1000.0f, 0.0f, 0.0f};

    renderer2d::Renderer2D renderer;
    REQUIRE(renderer.initialize(device).ok());

    device.begin_frame(64, 64, rhi::ClearColor{});
    auto drawn = renderer.render(device, scene, 64, 64);
    REQUIRE(drawn.ok());

    // One sprite is visible, one is culled. Drawn + culled is the extracted
    // count, which is how the counters prove the seam ran.
    CHECK(drawn.value() == 1);
    CHECK(renderer.last_draw_calls() == 1);
    CHECK(renderer.last_culled() == 1);
    device.end_frame();
}

TEST_CASE("the 2D draw step alone does no culling") {
    auto created = rhi::create_null_device();
    REQUIRE(created.ok());
    rhi::Device& device = *created.value();
    auto texture = make_solid_texture(device, 4, 4);

    Scene scene;
    scene.create_node("Camera")->add_component<Camera2D>();

    std::vector<render::SpriteInstance> instances;
    for (int i = 0; i < 3; ++i) {
        Node* node = scene.create_node("Sprite");
        auto* sprite = node->add_component<Sprite2D>();
        sprite->texture = texture;
        sprite->size = Vec2{4.0f, 4.0f};
        node->transform().position = Vec3{static_cast<f32>(i) * 1000.0f, 0.0f, 0.0f};
    }

    renderer2d::Renderer2D renderer;
    REQUIRE(renderer.initialize(device).ok());

    renderer.extract(scene, instances);
    REQUIRE(instances.size() == 3);

    device.begin_frame(64, 64, rhi::ClearColor{});
    // Handed the array directly, draw() draws every instance it is given,
    // including the off-screen ones: culling is a separate, explicit step.
    auto drawn = renderer.draw(device, scene, 64, 64, instances);
    REQUIRE(drawn.ok());
    CHECK(drawn.value() == 3);
    device.end_frame();
}

TEST_CASE("the 3D render() extracts, culls, and draws in agreement with its steps") {
    auto created = rhi::create_null_device();
    REQUIRE(created.ok());
    rhi::Device& device = *created.value();

    Scene scene;
    scene.create_node("Camera")->add_component<Camera3D>();
    scene.create_node("Sun")->add_component<DirectionalLight3D>();

    // A visible cube at the origin and one a long way down +X. The camera looks
    // down -Z from the origin, so the far cube is about 90 degrees to the right
    // and outside the view.
    Node* visible = scene.create_node("Visible");
    auto* visible_renderer = visible->add_component<MeshRenderer3D>();
    visible_renderer->mesh = Mesh::make_cube();

    Node* hidden = scene.create_node("Hidden");
    auto* hidden_renderer = hidden->add_component<MeshRenderer3D>();
    hidden_renderer->mesh = Mesh::make_cube();
    hidden->transform().position = Vec3{5000.0f, 0.0f, 0.0f};

    renderer3d::Renderer3D renderer;
    REQUIRE(renderer.initialize(device).ok());

    device.begin_frame(320, 200, rhi::ClearColor{});
    auto drawn = renderer.render(device, scene, 320, 200);
    REQUIRE(drawn.ok());

    CHECK(drawn.value() == 1);
    CHECK(renderer.last_draw_calls() == 1);
    CHECK(renderer.last_culled() == 1);
    // Only the drawn mesh was uploaded. The culled mesh is seen (so its buffers
    // would be kept if it had any) but never reaches the GPU.
    CHECK(renderer.cached_mesh_count() == 1);
    device.end_frame();
}

TEST_CASE("extraction and culling run with no renderer and no device") {
    // The seam's first two steps are pure CPU work. A scene can be extracted and
    // culled with no RHI device at all, which is what makes them unit-testable
    // and, later, runnable in parallel chunks.
    Scene scene;
    scene.create_node("Camera")->add_component<Camera3D>();

    Node* cube = scene.create_node("Cube");
    auto* mesh_renderer = cube->add_component<MeshRenderer3D>();
    mesh_renderer->mesh = Mesh::make_cube();
    cube->transform().position = Vec3{1000.0f, 0.0f, 0.0f};

    std::vector<render::MeshInstance> instances;
    render::extract_meshes(scene, instances, nullptr);
    REQUIRE(instances.size() == 1);

    const Frustum frustum = Frustum::from_view_projection(looking_at_origin(1.0f));
    CHECK(render::cull_meshes(instances, &frustum) == 1);
    CHECK(instances.empty());
}
