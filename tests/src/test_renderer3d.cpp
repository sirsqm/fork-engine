// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
//
// End-to-end tests for the 3D renderer. Like the 2D graphics tests, these
// create a hidden window and a real OpenGL context, then assert on pixels read
// back from the framebuffer, which is the only way to prove that a mesh was
// actually rasterised. Skipped (with a warning) when no context is available.

#include <doctest/doctest.h>

#include "fe/mesh/Mesh.h"
#include "fe/platform/Window.h"
#include "fe/renderer3d/Renderer3D.h"
#include "fe/rhi/Device.h"
#include "fe/scene/Node.h"
#include "fe/scene/Scene.h"
#include "fe/scene3d/Camera3D.h"
#include "fe/scene3d/DirectionalLight3D.h"
#include "fe/scene3d/MeshRenderer3D.h"

#include <memory>
#include <span>
#include <vector>

using namespace fe;

namespace {

struct GraphicsContext3D {
    std::unique_ptr<Window> window;
    std::unique_ptr<rhi::Device> device;
    bool available = false;

    GraphicsContext3D() {
        WindowDesc desc;
        desc.title = "fork engine 3d test";
        desc.width = 64;
        desc.height = 64;
        desc.visible = false;
        desc.vsync = false;

        auto created = Window::create(desc);
        if (!created) {
            MESSAGE("graphics context unavailable: " << created.error().message());
            return;
        }
        window = std::move(created.value());

        auto created_device = rhi::create_device(*window);
        if (!created_device) {
            MESSAGE("graphics device unavailable: " << created_device.error().message());
            window.reset();
            return;
        }
        device = std::move(created_device.value());
        available = true;
    }

    ~GraphicsContext3D() {
        device.reset();
        window.reset();
    }
};

struct Pixel {
    u8 r = 0;
    u8 g = 0;
    u8 b = 0;
    u8 a = 0;
};

// Renders `scene` with a Renderer3D and reads the centre pixel back.
[[nodiscard]] Pixel render_and_sample_centre(GraphicsContext3D& context, const Scene& scene, u32 width, u32 height) {
    context.window->poll_events();
    context.window->swap_buffers();

    rhi::ClearColor clear{};
    if (const Camera3D* camera = renderer3d::Renderer3D::find_active_camera(scene); camera != nullptr) {
        clear = rhi::ClearColor{camera->clear_r, camera->clear_g, camera->clear_b, camera->clear_a};
    }

    context.device->begin_frame(width, height, clear);

    renderer3d::Renderer3D renderer;
    REQUIRE_FALSE(renderer.initialize(*context.device));
    auto result = renderer.render(*context.device, scene, width, height);
    REQUIRE(result);

    std::vector<u8> pixels(static_cast<usize>(width) * height * 4U, 0);
    context.device->read_pixels(width / 2, height / 2, 1, 1, std::span<u8>(pixels.data(), pixels.size()));

    context.device->end_frame();
    return Pixel{pixels[0], pixels[1], pixels[2], pixels[3]};
}

// A camera at +Z looking down -Z, and a light aimed straight at the viewer so a
// face pointing at the camera receives full diffuse lighting.
void add_camera_and_key_light(Scene& scene) {
    Node* camera_node = scene.create_node("Camera");
    camera_node->transform().position = Vec3{0.0f, 0.0f, 3.0f};
    camera_node->add_component<Camera3D>();

    Node* light_node = scene.create_node("Sun");
    auto* light = light_node->add_component<DirectionalLight3D>();
    light->direction = Vec3{0.0f, 0.0f, -1.0f}; // Travelling away from the camera.
    light->color = Vec3{1.0f, 1.0f, 1.0f};
    light->intensity = 1.0f;
    light->ambient = 0.0f;
}

} // namespace

TEST_CASE("renderer3d draws a lit mesh covering the centre of the view") {
    GraphicsContext3D context;
    if (!context.available) {
        MESSAGE("skipping: no graphics context");
        return;
    }

    Scene scene;
    add_camera_and_key_light(scene);

    Node* cube_node = scene.create_node("Cube");
    auto* renderer = cube_node->add_component<MeshRenderer3D>();
    renderer->mesh = Mesh::make_cube();
    renderer->base_color = Vec4{1.0f, 0.0f, 0.0f, 1.0f}; // Red.

    const Pixel centre = render_and_sample_centre(context, scene, 64, 64);
    CHECK(centre.r > 200);
    CHECK(centre.g < 60);
    CHECK(centre.b < 60);
}

TEST_CASE("renderer3d reports the number of instances drawn") {
    GraphicsContext3D context;
    if (!context.available) {
        MESSAGE("skipping: no graphics context");
        return;
    }

    Scene scene;
    add_camera_and_key_light(scene);

    auto cube = Mesh::make_cube();
    for (int i = 0; i < 3; ++i) {
        Node* node = scene.create_node("Cube");
        auto* renderer = node->add_component<MeshRenderer3D>();
        renderer->mesh = cube;
        node->transform().position = Vec3{static_cast<f32>(i) * 0.2f - 0.2f, 0.0f, 0.0f};
    }

    context.device->begin_frame(64, 64, rhi::ClearColor{0.0f, 0.0f, 0.0f, 1.0f});
    renderer3d::Renderer3D renderer;
    REQUIRE_FALSE(renderer.initialize(*context.device));
    auto result = renderer.render(*context.device, scene, 64, 64);
    REQUIRE(result);
    CHECK(result.value() == 3);
    CHECK(renderer.last_draw_calls() == 3);
    // The shared mesh is uploaded once, not per instance.
    CHECK(renderer.cached_mesh_count() == 1);
    context.device->end_frame();
}

TEST_CASE("renderer3d draws nothing without a camera") {
    GraphicsContext3D context;
    if (!context.available) {
        MESSAGE("skipping: no graphics context");
        return;
    }

    Scene scene;
    Node* cube_node = scene.create_node("Cube");
    auto* renderer = cube_node->add_component<MeshRenderer3D>();
    renderer->mesh = Mesh::make_cube();

    context.device->begin_frame(64, 64, rhi::ClearColor{0.0f, 0.0f, 0.0f, 1.0f});
    renderer3d::Renderer3D r;
    REQUIRE_FALSE(r.initialize(*context.device));
    auto result = r.render(*context.device, scene, 64, 64);
    REQUIRE(result);
    CHECK(result.value() == 0);
    CHECK(r.last_draw_calls() == 0);
    context.device->end_frame();
}

TEST_CASE("renderer3d frustum-culls a mesh behind the camera") {
    GraphicsContext3D context;
    if (!context.available) {
        MESSAGE("skipping: no graphics context");
        return;
    }

    Scene scene;
    add_camera_and_key_light(scene); // Camera at +Z looking down -Z.

    // A cube placed behind the camera (positive Z, further from the origin than
    // the eye) cannot be visible. It must be culled, not drawn.
    Node* behind = scene.create_node("Behind");
    auto* behind_renderer = behind->add_component<MeshRenderer3D>();
    behind_renderer->mesh = Mesh::make_cube();
    behind->transform().position = Vec3{0.0f, 0.0f, 6.0f};

    context.device->begin_frame(64, 64, rhi::ClearColor{0.0f, 0.0f, 0.0f, 1.0f});
    renderer3d::Renderer3D renderer;
    REQUIRE_FALSE(renderer.initialize(*context.device));
    auto result = renderer.render(*context.device, scene, 64, 64);
    REQUIRE(result);
    CHECK(result.value() == 0);
    CHECK(renderer.last_draw_calls() == 0);
    CHECK(renderer.last_culled() == 1);
    context.device->end_frame();
}

TEST_CASE("renderer3d does not cull a mesh in front of the camera") {
    GraphicsContext3D context;
    if (!context.available) {
        MESSAGE("skipping: no graphics context");
        return;
    }

    Scene scene;
    add_camera_and_key_light(scene);

    Node* front = scene.create_node("Front");
    auto* front_renderer = front->add_component<MeshRenderer3D>();
    front_renderer->mesh = Mesh::make_cube();
    front->transform().position = Vec3{0.0f, 0.0f, 0.0f};

    context.device->begin_frame(64, 64, rhi::ClearColor{0.0f, 0.0f, 0.0f, 1.0f});
    renderer3d::Renderer3D renderer;
    REQUIRE_FALSE(renderer.initialize(*context.device));
    auto result = renderer.render(*context.device, scene, 64, 64);
    REQUIRE(result);
    CHECK(result.value() == 1);
    CHECK(renderer.last_culled() == 0);
    context.device->end_frame();
}

TEST_CASE("renderer3d keeps a culled mesh's buffers cached") {
    GraphicsContext3D context;
    if (!context.available) {
        MESSAGE("skipping: no graphics context");
        return;
    }

    Scene scene;
    add_camera_and_key_light(scene);

    auto cube = Mesh::make_cube();
    Node* node = scene.create_node("Mover");
    auto* mesh_renderer = node->add_component<MeshRenderer3D>();
    mesh_renderer->mesh = cube;

    renderer3d::Renderer3D renderer;
    REQUIRE_FALSE(renderer.initialize(*context.device));

    // Frame 1: in front of the camera, drawn and uploaded.
    context.device->begin_frame(64, 64, rhi::ClearColor{0.0f, 0.0f, 0.0f, 1.0f});
    REQUIRE(renderer.render(*context.device, scene, 64, 64));
    CHECK(renderer.last_draw_calls() == 1);
    CHECK(renderer.cached_mesh_count() == 1);
    context.device->end_frame();

    // Frame 2: moved behind the camera, culled. The buffers must survive so the
    // mesh does not re-upload when it comes back into view.
    node->transform().position = Vec3{0.0f, 0.0f, 6.0f};
    context.device->begin_frame(64, 64, rhi::ClearColor{0.0f, 0.0f, 0.0f, 1.0f});
    REQUIRE(renderer.render(*context.device, scene, 64, 64));
    CHECK(renderer.last_draw_calls() == 0);
    CHECK(renderer.last_culled() == 1);
    CHECK(renderer.cached_mesh_count() == 1);
    context.device->end_frame();
}

TEST_CASE("renderer3d depth-tests so a nearer mesh occludes a farther one") {
    GraphicsContext3D context;
    if (!context.available) {
        MESSAGE("skipping: no graphics context");
        return;
    }

    Scene scene;
    add_camera_and_key_light(scene);

    // A far green cube and a near red cube. Both fill the view; the near one
    // must win if depth testing is working.
    Node* far_node = scene.create_node("Far");
    auto* far_renderer = far_node->add_component<MeshRenderer3D>();
    far_renderer->mesh = Mesh::make_cube();
    far_renderer->base_color = Vec4{0.0f, 1.0f, 0.0f, 1.0f};
    far_node->transform().position = Vec3{0.0f, 0.0f, 0.0f};

    Node* near_node = scene.create_node("Near");
    auto* near_renderer = near_node->add_component<MeshRenderer3D>();
    near_renderer->mesh = Mesh::make_cube();
    near_renderer->base_color = Vec4{1.0f, 0.0f, 0.0f, 1.0f};
    near_node->transform().position = Vec3{0.0f, 0.0f, 1.0f};

    const Pixel centre = render_and_sample_centre(context, scene, 64, 64);
    CHECK(centre.r > 200);
    CHECK(centre.g < 60);
}
