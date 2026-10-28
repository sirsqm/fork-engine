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
#include "fe/renderer3d/PostProcess.h"
#include "fe/renderer3d/Renderer3D.h"
#include "fe/rhi/Device.h"
#include "fe/scene/Node.h"
#include "fe/scene/Scene.h"
#include "fe/scene3d/Camera3D.h"
#include "fe/scene3d/DirectionalLight3D.h"
#include "fe/scene3d/MeshRenderer3D.h"
#include "fe/scene3d/PointLight3D.h"
#include "fe/scene3d/SpotLight3D.h"

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

// Renders `scene` and reads a pixel at (x, y) with the origin at the top-left of
// the framebuffer, which is the coordinate a test reasons about when it wants a
// specific screen region rather than the centre.
[[nodiscard]] Pixel render_and_sample_at(GraphicsContext3D& context, const Scene& scene, u32 width, u32 height, u32 x,
                                         u32 y) {
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
    context.device->read_pixels(x, y, 1, 1, std::span<u8>(pixels.data(), pixels.size()));

    context.device->end_frame();
    return Pixel{pixels[0], pixels[1], pixels[2], pixels[3]};
}

// Renders `scene` through a Renderer3D with the given post-processing settings
// and reads a pixel at (x, y), origin top-left. Returns the pixel and leaves the
// renderer's counters in `renderer` for the caller to assert on.
[[nodiscard]] Pixel render_with_settings(GraphicsContext3D& context, renderer3d::Renderer3D& renderer,
                                         const Scene& scene, u32 width, u32 height, u32 x, u32 y,
                                         const renderer3d::PostProcessSettings& settings) {
    context.window->poll_events();
    context.window->swap_buffers();

    rhi::ClearColor clear{};
    if (const Camera3D* camera = renderer3d::Renderer3D::find_active_camera(scene); camera != nullptr) {
        clear = rhi::ClearColor{camera->clear_r, camera->clear_g, camera->clear_b, camera->clear_a};
    }

    context.device->begin_frame(width, height, clear);
    REQUIRE_FALSE(renderer.initialize(*context.device));
    auto result = renderer.render(*context.device, scene, width, height, settings);
    REQUIRE(result);

    std::vector<u8> pixels(static_cast<usize>(width) * height * 4U, 0);
    context.device->read_pixels(x, y, 1, 1, std::span<u8>(pixels.data(), pixels.size()));

    context.device->end_frame();
    return Pixel{pixels[0], pixels[1], pixels[2], pixels[3]};
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

TEST_CASE("a render through the OpenGL device leaves no GL error behind") {
    GraphicsContext3D context;
    if (!context.available) {
        MESSAGE("skipping: no graphics context");
        return;
    }

    Scene scene;
    add_camera_and_key_light(scene);
    Node* cube_node = scene.create_node("Cube");
    auto* mesh_renderer = cube_node->add_component<MeshRenderer3D>();
    mesh_renderer->mesh = Mesh::make_cube();

    context.device->begin_frame(64, 64, rhi::ClearColor{0.0f, 0.0f, 0.0f, 1.0f});
    renderer3d::Renderer3D renderer;
    REQUIRE_FALSE(renderer.initialize(*context.device));
    auto result = renderer.render(*context.device, scene, 64, 64);
    REQUIRE(result);
    context.device->end_frame();

    // "It drew something" and "it drew legally" are different claims. The web
    // acceptance check drains this same queue after a headless run, because a
    // WebGL2 context rejects calls a desktop GL 3.3 context tolerates and can
    // still leave a non-blank frame behind. This test pins the desktop half: a
    // correct render leaves the queue clean, so a future change that makes the
    // drain report a false positive fails here, on a desktop context, rather
    // than as a confusing red web job.
    CHECK(context.device->drain_errors().empty());
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
    // Three instances of one mesh, one material: one instanced draw call, not
    // three. The count is what instancing exists to reduce, so it is asserted
    // here rather than left to the benchmark.
    CHECK(renderer.last_draw_calls() == 1);
    // The shared mesh is uploaded once, not per instance.
    CHECK(renderer.cached_mesh_count() == 1);
    context.device->end_frame();
}

TEST_CASE("renderer3d draws distinct meshes and materials in separate batches") {
    GraphicsContext3D context;
    if (!context.available) {
        MESSAGE("skipping: no graphics context");
        return;
    }

    Scene scene;
    add_camera_and_key_light(scene);

    auto cube = Mesh::make_cube();
    auto plane = Mesh::make_plane(0.5f, 0.5f);

    // Two instances of the cube, one of the plane: two meshes, so two draws.
    for (int i = 0; i < 2; ++i) {
        Node* node = scene.create_node("Cube");
        auto* renderer = node->add_component<MeshRenderer3D>();
        renderer->mesh = cube;
        node->transform().position = Vec3{static_cast<f32>(i) * 0.4f - 0.2f, 0.0f, 0.0f};
    }
    Node* plane_node = scene.create_node("Plane");
    auto* plane_renderer = plane_node->add_component<MeshRenderer3D>();
    plane_renderer->mesh = plane;
    plane_node->transform().position = Vec3{0.0f, 0.0f, 0.0f};

    context.device->begin_frame(64, 64, rhi::ClearColor{0.0f, 0.0f, 0.0f, 1.0f});
    renderer3d::Renderer3D renderer;
    REQUIRE_FALSE(renderer.initialize(*context.device));
    auto result = renderer.render(*context.device, scene, 64, 64);
    REQUIRE(result);
    CHECK(result.value() == 3);
    CHECK(renderer.last_draw_calls() == 2);
    CHECK(renderer.cached_mesh_count() == 2);
    context.device->end_frame();
}

TEST_CASE("renderer3d splits one mesh into a batch per material") {
    GraphicsContext3D context;
    if (!context.available) {
        MESSAGE("skipping: no graphics context");
        return;
    }

    Scene scene;
    add_camera_and_key_light(scene);

    auto cube = Mesh::make_cube();

    // One mesh, but two instances whose materials differ: the shading state is
    // per-batch, so they cannot share a draw call. The renderer must split them
    // rather than shade one with the other's colour.
    for (int i = 0; i < 2; ++i) {
        Node* node = scene.create_node("Cube");
        auto* renderer = node->add_component<MeshRenderer3D>();
        renderer->mesh = cube;
        renderer->override_material = true;
        renderer->material.base_color = i == 0 ? Vec4{1.0f, 0.0f, 0.0f, 1.0f} : Vec4{0.0f, 1.0f, 0.0f, 1.0f};
        node->transform().position = Vec3{static_cast<f32>(i) * 0.4f - 0.2f, 0.0f, 0.0f};
    }

    context.device->begin_frame(64, 64, rhi::ClearColor{0.0f, 0.0f, 0.0f, 1.0f});
    renderer3d::Renderer3D renderer;
    REQUIRE_FALSE(renderer.initialize(*context.device));
    auto result = renderer.render(*context.device, scene, 64, 64);
    REQUIRE(result);
    CHECK(result.value() == 2);
    CHECK(renderer.last_draw_calls() == 2);
    context.device->end_frame();
}

TEST_CASE("renderer3d gives each instanced draw its own transform") {
    GraphicsContext3D context;
    if (!context.available) {
        MESSAGE("skipping: no graphics context");
        return;
    }

    Scene scene;
    add_camera_and_key_light(scene); // Camera at +Z, looking down -Z.

    // Two instances of one mesh at opposite sides of the frame, so they share a
    // draw call but must land in different places. The failure this catches is
    // the one instancing invites: the per-instance transform not being read, so
    // every instance is drawn with one matrix and the second copy vanishes or
    // overlaps the first. A draw-call count alone would not catch that.
    auto cube = Mesh::make_cube();
    for (int i = 0; i < 2; ++i) {
        Node* node = scene.create_node("Cube");
        auto* renderer = node->add_component<MeshRenderer3D>();
        renderer->mesh = cube;
        node->transform().position = Vec3{i == 0 ? -1.0f : 1.0f, 0.0f, 0.0f};
    }

    context.window->poll_events();
    context.window->swap_buffers();
    context.device->begin_frame(64, 64, rhi::ClearColor{0.0f, 0.0f, 0.0f, 1.0f});
    renderer3d::Renderer3D renderer;
    REQUIRE_FALSE(renderer.initialize(*context.device));
    REQUIRE(renderer.render(*context.device, scene, 64, 64));
    CHECK(renderer.last_draw_calls() == 1);

    // Sample the centre of each cube's screen footprint: world x = -1 and +1 at
    // z = 0 project to roughly pixel 13 and 50 across a 64-wide frame.
    std::vector<u8> left(4, 0);
    std::vector<u8> right(4, 0);
    context.device->read_pixels(13, 32, 1, 1, std::span<u8>(left.data(), left.size()));
    context.device->read_pixels(50, 32, 1, 1, std::span<u8>(right.data(), right.size()));
    context.device->end_frame();

    CHECK(left[0] > 30);  // The left instance was drawn where it belongs.
    CHECK(right[0] > 30); // And so was the right one, with its own transform.
}

TEST_CASE("renderer3d draws a camera's gradient background") {
    GraphicsContext3D context;
    if (!context.available) {
        MESSAGE("skipping: no graphics context");
        return;
    }

    Scene scene;
    Node* camera_node = scene.create_node("Camera");
    camera_node->transform().position = Vec3{0.0f, 0.0f, 3.0f};
    auto* camera = camera_node->add_component<Camera3D>();
    camera->background_enabled = true;
    camera->background_top = Vec3{0.0f, 0.0f, 1.0f};    // Blue at the top.
    camera->background_bottom = Vec3{1.0f, 0.0f, 0.0f}; // Red at the bottom.

    const u32 width = 64;
    const u32 height = 64;
    context.device->begin_frame(width, height, rhi::ClearColor{0.0f, 0.0f, 0.0f, 1.0f});
    renderer3d::Renderer3D renderer;
    REQUIRE_FALSE(renderer.initialize(*context.device));
    auto result = renderer.render(*context.device, scene, width, height);
    REQUIRE(result);

    std::vector<u8> pixels(static_cast<usize>(width) * height * 4U, 0);
    context.device->read_pixels(0, 0, width, height, std::span<u8>(pixels.data(), pixels.size()));
    const auto at = [&](u32 x, u32 y) {
        const usize i = (static_cast<usize>(y) * width + x) * 4U;
        return Pixel{pixels[i], pixels[i + 1], pixels[i + 2], pixels[i + 3]};
    };

    // read_pixels is bottom-up, so row 0 is the bottom of the frame.
    const Pixel bottom = at(width / 2, 1);
    const Pixel top = at(width / 2, height - 2);

    CHECK(bottom.r > 200);
    CHECK(bottom.b < 60);
    CHECK(top.b > 200);
    CHECK(top.r < 60);
    CHECK(renderer.background_draws() == 1);

    context.device->end_frame();
}

TEST_CASE("the post-processing chain grades the frame when enabled") {
    GraphicsContext3D context;
    if (!context.available) {
        MESSAGE("skipping: no graphics context");
        return;
    }

    Scene scene;
    Node* camera_node = scene.create_node("Camera");
    camera_node->transform().position = Vec3{0.0f, 0.0f, 3.0f};
    auto* camera = camera_node->add_component<Camera3D>();
    camera->background_enabled = true;
    // A flat mid-grey background makes the grading arithmetic deterministic.
    camera->background_top = Vec3{0.5f, 0.5f, 0.5f};
    camera->background_bottom = Vec3{0.5f, 0.5f, 0.5f};

    renderer3d::Renderer3D renderer;

    renderer3d::PostProcessSettings off;
    off.enabled = false;
    const Pixel plain = render_with_settings(context, renderer, scene, 64, 64, 32, 32, off);
    CHECK(renderer.post_process_frames() == 0);
    CHECK(renderer.post_process().pass_count() == 0);

    renderer3d::PostProcessSettings on;
    on.enabled = true;
    on.exposure = 0.5f;
    on.tonemap = false;
    on.vignette = 0.0f;
    const Pixel graded = render_with_settings(context, renderer, scene, 64, 64, 32, 32, on);

    CHECK(renderer.post_process_frames() == 1);
    CHECK(renderer.post_process().pass_count() == 1);
    // Half exposure halves the grey; the tonemap is off so the relationship is
    // linear and the centre is unaffected by the (disabled) vignette.
    CHECK(graded.r < plain.r);
    const int expected = static_cast<int>(plain.r) / 2;
    const int diff = static_cast<int>(graded.r) - expected;
    CHECK((diff < 6 && diff > -6));
}

TEST_CASE("a vignette darkens the corners but not the centre") {
    GraphicsContext3D context;
    if (!context.available) {
        MESSAGE("skipping: no graphics context");
        return;
    }

    Scene scene;
    Node* camera_node = scene.create_node("Camera");
    camera_node->transform().position = Vec3{0.0f, 0.0f, 3.0f};
    auto* camera = camera_node->add_component<Camera3D>();
    camera->background_enabled = true;
    camera->background_top = Vec3{0.5f, 0.5f, 0.5f};
    camera->background_bottom = Vec3{0.5f, 0.5f, 0.5f};

    renderer3d::Renderer3D renderer;

    renderer3d::PostProcessSettings on;
    on.enabled = true;
    on.exposure = 1.0f;
    on.tonemap = false;
    on.vignette = 0.9f;

    // Sample the bottom-left corner and the centre in two renders, keeping the
    // renderer (and therefore the chain) alive across both.
    const Pixel corner = render_with_settings(context, renderer, scene, 64, 64, 2, 2, on);
    const Pixel centre = render_with_settings(context, renderer, scene, 64, 64, 32, 32, on);

    CHECK(corner.r < centre.r);
    CHECK(centre.r > 100);
    CHECK(corner.r < 60);
    CHECK(renderer.post_process_frames() == 2);
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

TEST_CASE("renderer3d lights a mesh with a point light") {
    GraphicsContext3D context;
    if (!context.available) {
        MESSAGE("skipping: no graphics context");
        return;
    }

    Scene scene;
    // A camera and no directional light: the point light is the only emitter, so
    // a lit centre proves the point-light path, not the ambient term.
    Node* camera_node = scene.create_node("Camera");
    camera_node->transform().position = Vec3{0.0f, 0.0f, 3.0f};
    camera_node->add_component<Camera3D>();

    Node* cube_node = scene.create_node("Cube");
    auto* renderer = cube_node->add_component<MeshRenderer3D>();
    renderer->mesh = Mesh::make_cube();
    renderer->base_color = Vec4{1.0f, 1.0f, 1.0f, 1.0f};

    // A white point light between the camera and the cube, in range.
    Node* light_node = scene.create_node("Lamp");
    auto* light = light_node->add_component<PointLight3D>();
    light->color = Vec3{1.0f, 1.0f, 1.0f};
    light->intensity = 1.0f;
    light->range = 10.0f;
    light_node->transform().position = Vec3{0.0f, 0.0f, 2.0f};

    context.device->begin_frame(64, 64, rhi::ClearColor{0.0f, 0.0f, 0.0f, 1.0f});
    renderer3d::Renderer3D r;
    REQUIRE_FALSE(r.initialize(*context.device));
    auto result = r.render(*context.device, scene, 64, 64);
    REQUIRE(result);
    CHECK(r.last_point_lights() == 1);
    context.device->end_frame();

    const Pixel centre = render_and_sample_centre(context, scene, 64, 64);
    CHECK(centre.r > 100);
    CHECK(centre.g > 100);
    CHECK(centre.b > 100);
}

TEST_CASE("renderer3d leaves a mesh dark when a point light is out of range") {
    GraphicsContext3D context;
    if (!context.available) {
        MESSAGE("skipping: no graphics context");
        return;
    }

    Scene scene;
    Node* camera_node = scene.create_node("Camera");
    camera_node->transform().position = Vec3{0.0f, 0.0f, 3.0f};
    camera_node->add_component<Camera3D>();

    Node* cube_node = scene.create_node("Cube");
    auto* renderer = cube_node->add_component<MeshRenderer3D>();
    renderer->mesh = Mesh::make_cube();
    renderer->base_color = Vec4{1.0f, 1.0f, 1.0f, 1.0f};

    // A directional light contributes nothing (intensity 0, ambient 0), so the
    // out-of-range point light below is the only emitter and the centre must be
    // black. Without this the renderer's no-light fallback ambient would light
    // the mesh and the test would not isolate the range cutoff.
    Node* sun = scene.create_node("Sun");
    auto* sun_light = sun->add_component<DirectionalLight3D>();
    sun_light->intensity = 0.0f;
    sun_light->ambient = 0.0f;

    // A point light whose range is far smaller than its distance to the cube.
    Node* light_node = scene.create_node("Lamp");
    auto* light = light_node->add_component<PointLight3D>();
    light->range = 0.5f;
    light_node->transform().position = Vec3{0.0f, 0.0f, 2.0f};

    const Pixel centre = render_and_sample_centre(context, scene, 64, 64);
    // No ambient and no other light: an out-of-range point light leaves black.
    CHECK(centre.r < 20);
    CHECK(centre.g < 20);
    CHECK(centre.b < 20);
}

TEST_CASE("renderer3d caps the number of point lights it forwards") {
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

    for (u32 i = 0; i < renderer3d::Renderer3D::kMaxPointLights + 3; ++i) {
        Node* light_node = scene.create_node("Lamp");
        light_node->add_component<PointLight3D>();
        light_node->transform().position = Vec3{static_cast<f32>(i) * 0.1f, 0.0f, 2.0f};
    }

    context.device->begin_frame(64, 64, rhi::ClearColor{0.0f, 0.0f, 0.0f, 1.0f});
    renderer3d::Renderer3D r;
    REQUIRE_FALSE(r.initialize(*context.device));
    auto result = r.render(*context.device, scene, 64, 64);
    REQUIRE(result);
    CHECK(r.last_point_lights() == renderer3d::Renderer3D::kMaxPointLights);
    context.device->end_frame();
}

TEST_CASE("renderer3d lights a mesh inside a spot light's cone") {
    GraphicsContext3D context;
    if (!context.available) {
        MESSAGE("skipping: no graphics context");
        return;
    }

    Scene scene;
    Node* camera_node = scene.create_node("Camera");
    camera_node->transform().position = Vec3{0.0f, 0.0f, 3.0f};
    camera_node->add_component<Camera3D>();

    Node* cube_node = scene.create_node("Cube");
    auto* renderer = cube_node->add_component<MeshRenderer3D>();
    renderer->mesh = Mesh::make_cube();
    renderer->base_color = Vec4{1.0f, 1.0f, 1.0f, 1.0f};

    // A spot light at +Z pointing down -Z (the default node orientation) at the
    // cube, with a wide enough cone to cover it.
    Node* light_node = scene.create_node("Spot");
    auto* light = light_node->add_component<SpotLight3D>();
    light->color = Vec3{1.0f, 1.0f, 1.0f};
    light->intensity = 1.0f;
    light->range = 10.0f;
    light->inner_cone_degrees = 30.0f;
    light->outer_cone_degrees = 45.0f;
    light_node->transform().position = Vec3{0.0f, 0.0f, 2.0f};

    context.device->begin_frame(64, 64, rhi::ClearColor{0.0f, 0.0f, 0.0f, 1.0f});
    renderer3d::Renderer3D r;
    REQUIRE_FALSE(r.initialize(*context.device));
    auto result = r.render(*context.device, scene, 64, 64);
    REQUIRE(result);
    CHECK(r.last_spot_lights() == 1);
    context.device->end_frame();

    const Pixel centre = render_and_sample_centre(context, scene, 64, 64);
    CHECK(centre.r > 100);
    CHECK(centre.g > 100);
    CHECK(centre.b > 100);
}

// --- Shadow mapping ---------------------------------------------------------
//
// These tests drive the directional shadow map through real GL. The setup that
// makes the shadow observable is a lit floor with a large occluder floating
// above it: the floor is uniformly lit without an occluder, and a patch of it
// goes dark underneath when one is present. Sampling the floor near the screen
// edge, rather than the centre, keeps the read away from the occluder's own
// pixels.

namespace {

// A wide lit floor in the XZ plane and a camera above it looking down, so the
// floor fills most of the view and a downward light casts shadows onto it.
void add_floor_and_overhead_light(Scene& scene) {
    Node* camera_node = scene.create_node("Camera");
    camera_node->transform().position = Vec3{0.0f, 4.0f, 4.0f};
    // Tilt the camera down to look at the floor. Rotating about X by -45 degrees
    // aims the local -Z at the origin from this position.
    camera_node->transform().rotation = Vec3{-0.78539816339f, 0.0f, 0.0f};
    camera_node->add_component<Camera3D>();

    Node* light_node = scene.create_node("Sun");
    auto* light = light_node->add_component<DirectionalLight3D>();
    light->direction = Vec3{0.0f, -1.0f, 0.0f}; // Straight down.
    light->color = Vec3{1.0f, 1.0f, 1.0f};
    light->intensity = 1.0f;
    light->ambient = 0.0f;

    Node* floor_node = scene.create_node("Floor");
    auto* floor_renderer = floor_node->add_component<MeshRenderer3D>();
    floor_renderer->mesh = Mesh::make_plane(8.0f, 8.0f);
    floor_renderer->base_color = Vec4{1.0f, 1.0f, 1.0f, 1.0f}; // White.
    floor_renderer->specular = 0.0f;                           // Diffuse only, for a predictable value.
}

} // namespace

TEST_CASE("renderer3d renders a depth pass when a directional light is present") {
    GraphicsContext3D context;
    if (!context.available) {
        MESSAGE("skipping: no graphics context");
        return;
    }

    Scene scene;
    add_floor_and_overhead_light(scene);

    context.device->begin_frame(64, 64, rhi::ClearColor{0.0f, 0.0f, 0.0f, 1.0f});
    renderer3d::Renderer3D renderer;
    REQUIRE_FALSE(renderer.initialize(*context.device));
    auto result = renderer.render(*context.device, scene, 64, 64);
    REQUIRE(result);
    // The floor is the one caster, so the depth pass drew it exactly once.
    CHECK(renderer.last_shadow_casters() == 1);
    context.device->end_frame();
}

TEST_CASE("renderer3d runs no depth pass without a directional light") {
    GraphicsContext3D context;
    if (!context.available) {
        MESSAGE("skipping: no graphics context");
        return;
    }

    Scene scene;
    Node* camera_node = scene.create_node("Camera");
    camera_node->transform().position = Vec3{0.0f, 0.0f, 3.0f};
    camera_node->add_component<Camera3D>();

    Node* cube_node = scene.create_node("Cube");
    auto* renderer = cube_node->add_component<MeshRenderer3D>();
    renderer->mesh = Mesh::make_cube();

    context.device->begin_frame(64, 64, rhi::ClearColor{0.0f, 0.0f, 0.0f, 1.0f});
    renderer3d::Renderer3D r;
    REQUIRE_FALSE(r.initialize(*context.device));
    auto result = r.render(*context.device, scene, 64, 64);
    REQUIRE(result);
    CHECK(r.last_shadow_casters() == 0);
    context.device->end_frame();
}

TEST_CASE("an occluder darkens the floor beneath it") {
    GraphicsContext3D context;
    if (!context.available) {
        MESSAGE("skipping: no graphics context");
        return;
    }

    // Baseline: the lit floor with no occluder. The centre of the view is floor
    // at the world origin, which is exactly where the occluder's shadow falls.
    Scene unshadowed;
    add_floor_and_overhead_light(unshadowed);
    const Pixel lit = render_and_sample_at(context, unshadowed, 64, 64, 32, 32);

    // Same scene plus a slab floating above the floor. The camera looks at the
    // origin from a 45-degree angle, so the slab (higher up) appears on screen
    // above the floor point it shadows. That separation is what lets the test
    // read the shadow itself rather than the slab's own pixels: the centre of
    // the view is floor in both renders, and only the shadowed render darkens it.
    Scene shadowed;
    add_floor_and_overhead_light(shadowed);
    Node* occluder = shadowed.create_node("Occluder");
    auto* occluder_renderer = occluder->add_component<MeshRenderer3D>();
    occluder_renderer->mesh = Mesh::make_plane(1.5f, 1.5f);
    occluder->transform().position = Vec3{0.0f, 3.0f, 0.0f};

    const Pixel dark = render_and_sample_at(context, shadowed, 64, 64, 32, 32);

    // Move the occluder sideways. Its shadow must follow it off the centre of the
    // floor, which proves the darkening tracks the occluder's light-space depth
    // rather than being the occluder's own pixels covering the sample point.
    Scene moved;
    add_floor_and_overhead_light(moved);
    Node* moved_occluder = moved.create_node("Occluder");
    auto* moved_renderer = moved_occluder->add_component<MeshRenderer3D>();
    moved_renderer->mesh = Mesh::make_plane(1.5f, 1.5f);
    moved_occluder->transform().position = Vec3{2.0f, 3.0f, 0.0f};

    const Pixel relit = render_and_sample_at(context, moved, 64, 64, 32, 32);

    // The floor is fully lit with no occluder; the occluder overhead casts a
    // shadow on the sample point, and moving the occluder away lifts it again.
    // The last check is what makes this a shadow test rather than an accidental
    // read of the occluder's own pixels: the sample point is floor in all three
    // renders, and its brightness follows the occluder's position.
    CHECK(lit.r > 200);
    CHECK(dark.r < lit.r);
    CHECK(static_cast<int>(lit.r) - static_cast<int>(dark.r) > 40);
    CHECK(relit.r > 200);
    CHECK(static_cast<int>(relit.r) - static_cast<int>(dark.r) > 40);
}

TEST_CASE("renderer3d uses a mesh's imported material when the component has none") {
    GraphicsContext3D context;
    if (!context.available) {
        MESSAGE("skipping: no graphics context");
        return;
    }

    Scene scene;
    add_camera_and_key_light(scene);

    Node* cube_node = scene.create_node("Cube");
    auto* renderer = cube_node->add_component<MeshRenderer3D>();
    auto mesh = Mesh::make_cube();
    // The mesh carries a red material, as an import would. The component authors
    // nothing, so this is the surface the draw must use.
    Material imported;
    imported.base_color = Vec4{1.0f, 0.0f, 0.0f, 1.0f};
    imported.roughness = 1.0f;
    mesh->set_material(imported);
    renderer->mesh = mesh;

    const Pixel centre = render_and_sample_centre(context, scene, 64, 64);
    CHECK(centre.r > 100);
    CHECK(centre.g < 40);
    CHECK(centre.b < 40);
}

TEST_CASE("renderer3d prefers an authored material over the mesh's imported one") {
    GraphicsContext3D context;
    if (!context.available) {
        MESSAGE("skipping: no graphics context");
        return;
    }

    Scene scene;
    add_camera_and_key_light(scene);

    Node* cube_node = scene.create_node("Cube");
    auto* renderer = cube_node->add_component<MeshRenderer3D>();
    auto mesh = Mesh::make_cube();
    Material imported;
    imported.base_color = Vec4{1.0f, 0.0f, 0.0f, 1.0f}; // red
    imported.roughness = 1.0f;
    mesh->set_material(imported);
    renderer->mesh = mesh;

    // The designer overrides it with green, which must win.
    renderer->override_material = true;
    renderer->material.base_color = Vec4{0.0f, 1.0f, 0.0f, 1.0f};
    renderer->material.roughness = 1.0f;

    const Pixel centre = render_and_sample_centre(context, scene, 64, 64);
    CHECK(centre.g > 100);
    CHECK(centre.r < 40);
}

TEST_CASE("renderer3d adds a material's emissive colour in the dark") {
    GraphicsContext3D context;
    if (!context.available) {
        MESSAGE("skipping: no graphics context");
        return;
    }

    Scene scene;
    Node* camera_node = scene.create_node("Camera");
    camera_node->transform().position = Vec3{0.0f, 0.0f, 3.0f};
    camera_node->add_component<Camera3D>();

    // No light at all: the directional light contributes nothing and ambient is
    // zero, so any brightness at the centre can only be the emissive term.
    Node* sun = scene.create_node("Sun");
    auto* sun_light = sun->add_component<DirectionalLight3D>();
    sun_light->intensity = 0.0f;
    sun_light->ambient = 0.0f;

    Node* cube_node = scene.create_node("Cube");
    auto* renderer = cube_node->add_component<MeshRenderer3D>();
    renderer->mesh = Mesh::make_cube();
    renderer->override_material = true;
    renderer->material.base_color = Vec4{0.0f, 0.0f, 0.0f, 1.0f};
    renderer->material.emissive = Vec3{0.0f, 0.0f, 1.0f};

    const Pixel centre = render_and_sample_centre(context, scene, 64, 64);
    CHECK(centre.b > 100);
    CHECK(centre.r < 40);
    CHECK(centre.g < 40);
}
