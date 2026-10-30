// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
//
// End-to-end tests for the skinned draw path (ADR-0104): a `SkinnedMesh` drawn
// through the skinned vertex shader with a palette uniform, and a posed character
// moving on screen. These create a hidden window and a real OpenGL context, then
// assert on pixels read back from the framebuffer, which is the only way to prove
// the skinned shader compiled and the palette reached it. Skipped (with a
// warning) when no context is available.

#include <doctest/doctest.h>

#include "fe/anim/Animator.h"
#include "fe/anim/Skeleton.h"
#include "fe/anim/SkinnedMesh.h"
#include "fe/anim/SkinnedMeshRenderer3D.h"
#include "fe/platform/Window.h"
#include "fe/renderer3d/Renderer3D.h"
#include "fe/rhi/Device.h"
#include "fe/scene/Node.h"
#include "fe/scene/Scene.h"
#include "fe/scene3d/Camera3D.h"
#include "fe/scene3d/DirectionalLight3D.h"

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
        desc.title = "fork engine skinned test";
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

// A one-joint skeleton whose single joint is the identity in the bind pose, so
// the bind-pose palette is the identity and an unposed draw is the mesh's static
// shape.
[[nodiscard]] std::shared_ptr<anim::Skeleton> single_joint_skeleton() {
    std::vector<anim::Skeleton::Joint> joints(1);
    joints[0].name = "root";
    auto skeleton = std::make_shared<anim::Skeleton>("test.skeleton", std::move(joints));
    skeleton->compute_inverse_bind_matrices();
    return skeleton;
}

// A quad on the XY plane, facing +Z, with every vertex fully weighted to joint 0.
// Two triangles, so it is a solid red square covering the centre of the view.
[[nodiscard]] std::shared_ptr<anim::SkinnedMesh> quad_mesh(const std::shared_ptr<anim::Skeleton>& skeleton) {
    const auto vertex = [](f32 x, f32 y) {
        SkinnedVertex3D v{};
        v.position = Vec3{x, y, 0.0f};
        v.normal = Vec3{0.0f, 0.0f, 1.0f};
        v.uv = Vec2{(x + 1.0f) * 0.5f, (y + 1.0f) * 0.5f};
        v.joints = {0.0f, 0.0f, 0.0f, 0.0f};
        v.weights = {1.0f, 0.0f, 0.0f, 0.0f};
        return v;
    };
    std::vector<SkinnedVertex3D> vertices = {
        vertex(-1.0f, -1.0f),
        vertex(1.0f, -1.0f),
        vertex(1.0f, 1.0f),
        vertex(-1.0f, 1.0f),
    };
    std::vector<u32> indices = {0, 1, 2, 0, 2, 3};
    return std::make_shared<anim::SkinnedMesh>("test.quad", std::move(vertices), std::move(indices), skeleton);
}

// A camera at +Z looking down -Z, and a light aimed straight at the viewer so a
// face pointing at the camera receives full diffuse lighting.
void add_camera_and_key_light(Scene& scene) {
    Node* camera_node = scene.create_node("Camera");
    camera_node->transform().position = Vec3{0.0f, 0.0f, 3.0f};
    camera_node->add_component<Camera3D>();

    Node* light_node = scene.create_node("Sun");
    auto* light = light_node->add_component<DirectionalLight3D>();
    light->direction = Vec3{0.0f, 0.0f, -1.0f};
    light->color = Vec3{1.0f, 1.0f, 1.0f};
    light->intensity = 1.0f;
    light->ambient = 0.0f;
}

// Renders `scene` with a Renderer3D and reads a pixel at (x, y), origin top-left.
[[nodiscard]] Pixel render_and_sample(GraphicsContext3D& context, const Scene& scene, u32 width, u32 height, u32 x,
                                      u32 y, renderer3d::Renderer3D& renderer) {
    context.window->poll_events();
    context.window->swap_buffers();

    rhi::ClearColor clear{0.0f, 0.0f, 0.0f, 1.0f};
    if (const Camera3D* camera = renderer3d::Renderer3D::find_active_camera(scene); camera != nullptr) {
        clear = rhi::ClearColor{camera->clear_r, camera->clear_g, camera->clear_b, camera->clear_a};
    }

    context.device->begin_frame(width, height, clear);
    for (const std::string& e : context.device->drain_errors()) {
        MESSAGE("GL error after begin_frame: " << e);
    }
    auto result = renderer.render(*context.device, scene, width, height);
    REQUIRE(result);
    for (const std::string& e : context.device->drain_errors()) {
        MESSAGE("GL error after render: " << e);
    }

    std::vector<u8> pixels(static_cast<usize>(width) * height * 4U, 0);
    context.device->read_pixels(x, y, 1, 1, std::span<u8>(pixels.data(), pixels.size()));
    context.device->end_frame();
    return Pixel{pixels[0], pixels[1], pixels[2], pixels[3]};
}

// A node with a skinned quad, its component's surface set to a flat red so the
// draw's colour is unambiguous.
Node* add_skinned_quad(Scene& scene, const std::shared_ptr<anim::SkinnedMesh>& mesh) {
    Node* node = scene.create_node("Character");
    auto* renderer = node->add_component<anim::SkinnedMeshRenderer3D>();
    renderer->mesh = mesh;
    renderer->surface.base_color = Vec4{1.0f, 0.0f, 0.0f, 1.0f};
    return node;
}

} // namespace

TEST_CASE("renderer3d draws a skinned mesh in its bind pose") {
    GraphicsContext3D context;
    if (!context.available) {
        MESSAGE("skipping: no graphics context");
        return;
    }

    Scene scene;
    add_camera_and_key_light(scene);
    add_skinned_quad(scene, quad_mesh(single_joint_skeleton()));

    renderer3d::Renderer3D renderer;
    REQUIRE_FALSE(renderer.initialize(*context.device));

    const Pixel centre = render_and_sample(context, scene, 64, 64, 32, 32, renderer);
    CHECK(centre.r > 200);
    CHECK(centre.g < 60);
    CHECK(centre.b < 60);

    // The skinned pass ran: one draw for the one instance, and nothing culled.
    CHECK(renderer.last_skinned_draw_calls() == 1);
    CHECK(renderer.last_skinned_culled() == 0);
}

TEST_CASE("a skinned draw leaves no GL error behind") {
    GraphicsContext3D context;
    if (!context.available) {
        MESSAGE("skipping: no graphics context");
        return;
    }

    Scene scene;
    add_camera_and_key_light(scene);
    add_skinned_quad(scene, quad_mesh(single_joint_skeleton()));

    renderer3d::Renderer3D renderer;
    REQUIRE_FALSE(renderer.initialize(*context.device));

    (void)render_and_sample(context, scene, 64, 64, 32, 32, renderer);

    // The error queue is the only way to catch a shader that compiled but
    // mis-declared a uniform: the draw silently does nothing. A clean queue after
    // the frame is what a passing skinned shader looks like.
    const auto errors = context.device->drain_errors();
    for (const std::string& e : errors) {
        MESSAGE("GL error: " << e);
    }
    CHECK(errors.empty());
}

TEST_CASE("a posed skinned mesh moves, proving the palette reaches the shader") {
    GraphicsContext3D context;
    if (!context.available) {
        MESSAGE("skipping: no graphics context");
        return;
    }

    Scene scene;
    add_camera_and_key_light(scene);

    auto skeleton = single_joint_skeleton();
    auto mesh = quad_mesh(skeleton);
    Node* node = add_skinned_quad(scene, mesh);

    // The animator drives the palette. Its single joint is translated far to the
    // right, so the quad leaves the centre of the view and the centre pixel goes
    // black while an off-centre pixel lights up. A shader that ignored the
    // palette would draw the quad at the centre in both frames.
    auto* animator = node->add_component<anim::Animator>();
    animator->skeleton = skeleton;
    animator->mutable_pose().resize(1);
    animator->mutable_pose().at(0).translation = Vec3{1.4f, 0.0f, 0.0f};
    animator->mutable_palette().assign(1, Mat4::identity());
    animator->mutable_palette()[0] = Mat4::translation(Vec3{1.4f, 0.0f, 0.0f});
    animator->mark_palette_valid();

    renderer3d::Renderer3D renderer;
    REQUIRE_FALSE(renderer.initialize(*context.device));

    // The quad has moved right, so the centre is background and a pixel to the
    // right is lit red.
    const Pixel centre = render_and_sample(context, scene, 64, 64, 32, 32, renderer);
    CHECK(centre.r < 40);

    const Pixel right = render_and_sample(context, scene, 64, 64, 50, 32, renderer);
    CHECK(right.r > 150);
    CHECK(renderer.last_skinned_draw_calls() == 1);
}
