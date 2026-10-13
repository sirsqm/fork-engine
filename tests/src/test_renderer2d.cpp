// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
//
// End-to-end renderer tests. These exercise the real path: window + OpenGL
// context + RHI + Renderer2D + scene. They assert on pixels read back from the
// framebuffer, which is the only way to prove that geometry was actually drawn
// rather than merely submitted.
//
// If no graphics context can be created (no display and no software GL), the
// tests report that and pass with a warning instead of failing, so `ctest`
// stays usable on machines without a GPU. The engine's own code is still
// compiled and linked by this target regardless.

#include <doctest/doctest.h>

#include "fe/platform/Window.h"
#include "fe/renderer2d/Renderer2D.h"
#include "fe/resources/Texture2D.h"
#include "fe/rhi/Device.h"
#include "fe/scene/Camera2D.h"
#include "fe/scene/Node.h"
#include "fe/scene/Scene.h"
#include "fe/scene/Sprite2D.h"

#include <memory>
#include <vector>

using namespace fe;

namespace {

// Owns a window and a device for a single test case. Created hidden so tests
// never steal focus.
struct GraphicsContext {
    std::unique_ptr<Window> window;
    std::unique_ptr<rhi::Device> device;
    bool available = false;

    GraphicsContext() {
        WindowDesc desc;
        desc.title = "fork engine test";
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

    ~GraphicsContext() {
        device.reset();
        window.reset();
    }
};

// Renders one frame of `scene` and reads the centre pixel back.
struct Pixel {
    u8 r = 0;
    u8 g = 0;
    u8 b = 0;
    u8 a = 0;
};

[[nodiscard]] Pixel render_and_sample_centre(GraphicsContext& context, const Scene& scene, u32 width, u32 height) {
    // Force the window surface to be realized before reading, so the default
    // framebuffer is guaranteed to hold this frame's contents on drivers that
    // lazily allocate it (Mesa/llvmpipe under Xvfb).
    context.window->poll_events();
    context.window->swap_buffers();

    // Mirror the runtime: clear to the active camera's colour.
    rhi::ClearColor clear{};
    if (const Camera2D* camera = renderer2d::Renderer2D::find_active_camera(scene); camera != nullptr) {
        clear = rhi::ClearColor{camera->clear_r, camera->clear_g, camera->clear_b, camera->clear_a};
    }

    context.device->begin_frame(width, height, clear);

    renderer2d::Renderer2D renderer;
    REQUIRE_FALSE(renderer.initialize(*context.device));
    auto result = renderer.render(*context.device, scene, width, height);
    REQUIRE(result);

    std::vector<u8> pixels(static_cast<usize>(width) * height * 4U, 0);
    context.device->read_pixels(width / 2, height / 2, 1, 1, std::span<u8>(pixels.data(), pixels.size()));

    context.device->end_frame();
    return Pixel{pixels[0], pixels[1], pixels[2], pixels[3]};
}

// A solid-colour 1x1 texture is enough to prove the sprite pipeline; the
// colour is supplied by the sprite's tint.
[[nodiscard]] std::shared_ptr<Texture2D> make_solid_texture(rhi::Device& device, u8 r, u8 g, u8 b, u8 a) {
    const u8 texel[4] = {r, g, b, a};
    rhi::TextureDesc desc;
    desc.name = "solid";
    desc.width = 1;
    desc.height = 1;
    desc.pixels = texel;
    auto texture = device.create_texture(desc);
    REQUIRE(texture);
    return std::make_shared<Texture2D>("solid", texture.value());
}

} // namespace

TEST_CASE("renderer draws a sprite at the centre of the screen") {
    GraphicsContext context;
    if (!context.available) {
        MESSAGE("skipping: no graphics context");
        return;
    }

    Scene scene;
    Node* camera_node = scene.create_node("Camera");
    camera_node->add_component<Camera2D>();

    Node* sprite_node = scene.create_node("Sprite");
    auto* sprite = sprite_node->add_component<Sprite2D>();
    sprite->texture = make_solid_texture(*context.device, 255, 255, 255, 255);
    sprite->size = Vec2{32.0f, 32.0f};
    sprite->color = Vec4{1.0f, 0.0f, 0.0f, 1.0f}; // Red.

    const Pixel centre = render_and_sample_centre(context, scene, 64, 64);
    // The sprite covers the centre, so the read-back pixel must be red.
    CHECK(centre.r > 200);
    CHECK(centre.g < 60);
    CHECK(centre.b < 60);
}

TEST_CASE("an empty scene clears to the camera colour") {
    GraphicsContext context;
    if (!context.available) {
        MESSAGE("skipping: no graphics context");
        return;
    }

    Scene scene;
    Node* camera_node = scene.create_node("Camera");
    auto* camera = camera_node->add_component<Camera2D>();
    camera->clear_r = 0.0f;
    camera->clear_g = 0.0f;
    camera->clear_b = 1.0f; // Blue.
    camera->clear_a = 1.0f;

    const Pixel centre = render_and_sample_centre(context, scene, 64, 64);
    CHECK(centre.b > 200);
    CHECK(centre.r < 60);
}

TEST_CASE("sprites sharing a texture collapse into one draw call") {
    GraphicsContext context;
    if (!context.available) {
        MESSAGE("skipping: no graphics context");
        return;
    }

    Scene scene;
    scene.create_node("Camera")->add_component<Camera2D>();

    auto texture = make_solid_texture(*context.device, 255, 255, 255, 255);
    for (int i = 0; i < 8; ++i) {
        Node* node = scene.create_node("Sprite");
        auto* sprite = node->add_component<Sprite2D>();
        sprite->texture = texture;
        sprite->size = Vec2{8.0f, 8.0f};
        node->transform().position = Vec3{static_cast<f32>(i) * 4.0f - 16.0f, 0.0f, 0.0f};
    }

    context.device->begin_frame(64, 64, rhi::ClearColor{0.0f, 0.0f, 0.0f, 1.0f});
    renderer2d::Renderer2D renderer;
    REQUIRE_FALSE(renderer.initialize(*context.device));
    auto result = renderer.render(*context.device, scene, 64, 64);
    REQUIRE(result);
    CHECK(result.value() == 8);
    CHECK(renderer.last_draw_calls() == 1);
    context.device->end_frame();
}

TEST_CASE("sprites on different layers sort back-to-front") {
    GraphicsContext context;
    if (!context.available) {
        MESSAGE("skipping: no graphics context");
        return;
    }

    Scene scene;
    scene.create_node("Camera")->add_component<Camera2D>();

    auto white = make_solid_texture(*context.device, 255, 255, 255, 255);

    // Two overlapping full-screen quads. The higher layer must win.
    Node* bottom = scene.create_node("Bottom");
    auto* bottom_sprite = bottom->add_component<Sprite2D>();
    bottom_sprite->texture = white;
    bottom_sprite->size = Vec2{64.0f, 64.0f};
    bottom_sprite->color = Vec4{1.0f, 0.0f, 0.0f, 1.0f}; // Red.
    bottom_sprite->layer = 0;

    Node* top = scene.create_node("Top");
    auto* top_sprite = top->add_component<Sprite2D>();
    top_sprite->texture = white;
    top_sprite->size = Vec2{64.0f, 64.0f};
    top_sprite->color = Vec4{0.0f, 1.0f, 0.0f, 1.0f}; // Green.
    top_sprite->layer = 10;

    const Pixel centre = render_and_sample_centre(context, scene, 64, 64);
    CHECK(centre.g > 200);
    CHECK(centre.r < 60);
}

TEST_CASE("a render target accepts drawing and its texture can be read back") {
    GraphicsContext context;
    if (!context.available) {
        MESSAGE("skipping: no graphics context");
        return;
    }

    rhi::RenderTargetDesc desc;
    desc.name = "viewport";
    desc.width = 32;
    desc.height = 32;
    auto target = context.device->create_render_target(desc);
    REQUIRE(target);
    CHECK(target.value()->width() == 32);
    CHECK(target.value()->color_texture()->width() == 32);

    // Draw a red sprite into the target, not the window.
    Scene scene;
    scene.create_node("Camera")->add_component<Camera2D>();
    Node* sprite_node = scene.create_node("Sprite");
    auto* sprite = sprite_node->add_component<Sprite2D>();
    sprite->texture = make_solid_texture(*context.device, 255, 255, 255, 255);
    sprite->size = Vec2{32.0f, 32.0f};
    sprite->color = Vec4{1.0f, 0.0f, 0.0f, 1.0f};

    context.device->begin_frame(64, 64, rhi::ClearColor{0.0f, 0.0f, 0.0f, 1.0f});
    context.device->bind_render_target(target.value().get());
    CHECK(context.device->bound_render_target() == target.value().get());

    renderer2d::Renderer2D renderer;
    REQUIRE_FALSE(renderer.initialize(*context.device));
    REQUIRE(renderer.render(*context.device, scene, 32, 32));

    std::vector<u8> pixels(32U * 32U * 4U, 0);
    context.device->read_pixels(16, 16, 1, 1, std::span<u8>(pixels.data(), pixels.size()));
    CHECK(pixels[0] > 200); // red
    CHECK(pixels[1] < 60);
    CHECK(pixels[2] < 60);

    // The window still holds its own (black) contents, proving the sprite did
    // not leak onto the default framebuffer.
    context.device->bind_render_target(nullptr);
    CHECK(context.device->bound_render_target() == nullptr);
    std::vector<u8> window_pixel(4, 0);
    context.device->read_pixels(32, 32, 1, 1, std::span<u8>(window_pixel.data(), window_pixel.size()));
    CHECK(window_pixel[0] < 60);
    context.device->end_frame();
}

TEST_CASE("the render target colour attachment can be sampled as a texture") {
    GraphicsContext context;
    if (!context.available) {
        MESSAGE("skipping: no graphics context");
        return;
    }

    rhi::RenderTargetDesc desc;
    desc.name = "sampled";
    desc.width = 16;
    desc.height = 16;
    auto target = context.device->create_render_target(desc);
    REQUIRE(target);

    // Clear the target to a known colour and then sample its texture onto the
    // window with an ordinary sprite, closing the loop the editor viewport
    // relies on: render off-screen, then blit the result.
    context.device->begin_frame(64, 64, rhi::ClearColor{0.0f, 0.0f, 0.0f, 1.0f});
    context.device->bind_render_target(target.value().get());
    target.value()->clear(rhi::ClearColor{0.0f, 0.0f, 1.0f, 1.0f}); // Blue.
    context.device->bind_render_target(nullptr);

    Scene scene;
    scene.create_node("Camera")->add_component<Camera2D>();
    Node* sprite_node = scene.create_node("Sprite");
    auto* sprite = sprite_node->add_component<Sprite2D>();
    sprite->texture = std::make_shared<Texture2D>("sampled", target.value()->color_texture());
    sprite->size = Vec2{64.0f, 64.0f};

    renderer2d::Renderer2D renderer;
    REQUIRE_FALSE(renderer.initialize(*context.device));
    REQUIRE(renderer.render(*context.device, scene, 64, 64));

    std::vector<u8> pixels(64U * 64U * 4U, 0);
    context.device->read_pixels(32, 32, 1, 1, std::span<u8>(pixels.data(), pixels.size()));
    CHECK(pixels[2] > 200); // blue from the target
    context.device->end_frame();
}

TEST_CASE("creating a render target with zero size fails cleanly") {
    GraphicsContext context;
    if (!context.available) {
        MESSAGE("skipping: no graphics context");
        return;
    }

    rhi::RenderTargetDesc desc;
    desc.name = "invalid";
    desc.width = 0;
    desc.height = 16;
    CHECK_FALSE(context.device->create_render_target(desc));
}
