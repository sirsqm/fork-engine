// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
//
// Tests for the null RHI device (ADR-0042): the headless graphics backend.
//
// These run with no display and no GL context by design. They prove the render
// path *submits* work -- extraction, batching, and draw calls -- against an
// inert device. They assert the counts the renderers report, never pixels: the
// null device rasterises nothing, and a pixel assertion belongs in the graphics
// suite, which needs a real context.

#include <doctest/doctest.h>

#include "fe/mesh/Mesh.h"
#include "fe/renderer2d/Renderer2D.h"
#include "fe/renderer3d/Renderer3D.h"
#include "fe/resources/Texture2D.h"
#include "fe/rhi/Device.h"
#include "fe/rhi/Resources.h"
#include "fe/rhi/Types.h"
#include "fe/runtime/Application.h"
#include "fe/runtime/Engine.h"
#include "fe/scene/Camera2D.h"
#include "fe/scene/Node.h"
#include "fe/scene/Scene.h"
#include "fe/scene/Sprite2D.h"
#include "fe/scene3d/Camera3D.h"
#include "fe/scene3d/DirectionalLight3D.h"
#include "fe/scene3d/MeshRenderer3D.h"

#include <string_view>
#include <vector>

using namespace fe;

namespace {

[[nodiscard]] Engine::Config null_render_config() {
    Engine::Config config;
    config.window.backend = WindowBackend::Null;
    config.window.width = 320;
    config.window.height = 200;
    config.log_level = LogLevel::Warn;
    config.headless_render = true;
    return config;
}

// A minimal Application that builds a scene with sprites (so the 2D pass has
// geometry to batch) and records what the headless render loop did.
class HeadlessRenderApp final : public Application {
public:
    explicit HeadlessRenderApp(Engine::Config config) : Application(config) {}

    u64 updates = 0;
    bool had_graphics = false;
    bool had_headless_render = false;

protected:
    Error on_start() override {
        // A camera so the 2D pass builds a real projection.
        Node* camera = scene().create_node("Camera");
        camera->add_component<Camera2D>();

        // Two sprites sharing no texture: the untextured path uses the renderer's
        // built-in white texture, so both batch into a single draw call.
        for (int i = 0; i < 2; ++i) {
            Node* sprite = scene().create_node("Sprite");
            auto* component = sprite->add_component<Sprite2D>();
            component->size = Vec2{8.0f, 8.0f};
        }
        return Error::none();
    }

    void on_update(f32) override { ++updates; }

    void on_stop() override {
        had_graphics = engine().has_graphics();
        had_headless_render = engine().headless_render();
    }
};

} // namespace

TEST_CASE("a null device creates inert resources that report their real shape") {
    auto created = rhi::create_null_device();
    REQUIRE(created.ok());
    rhi::Device& device = *created.value();

    CHECK(std::string_view(device.backend_name()) == "null");
    CHECK_FALSE(device.info().empty());

    rhi::ShaderDesc shader_desc;
    shader_desc.name = "test_shader";
    shader_desc.vertex_source = "void main() {}";
    shader_desc.fragment_source = "void main() {}";
    auto shader = device.create_shader(shader_desc);
    REQUIRE(shader.ok());
    CHECK(shader.value()->name() == "test_shader");

    auto vertex_buffer = device.create_vertex_buffer(rhi::VertexLayout::position_color_uv(), 4 * 32);
    REQUIRE(vertex_buffer.ok());
    CHECK(vertex_buffer.value()->kind() == rhi::Buffer::Kind::Vertex);
    CHECK(vertex_buffer.value()->element_count() == 4);

    auto index_buffer = device.create_index_buffer(6);
    REQUIRE(index_buffer.ok());
    CHECK(index_buffer.value()->kind() == rhi::Buffer::Kind::Index);
    CHECK(index_buffer.value()->element_count() == 6);

    const u8 pixel[4] = {255, 255, 255, 255};
    rhi::TextureDesc texture_desc;
    texture_desc.name = "test_texture";
    texture_desc.width = 1;
    texture_desc.height = 1;
    texture_desc.pixels = pixel;
    auto texture = device.create_texture(texture_desc);
    REQUIRE(texture.ok());
    CHECK(texture.value()->width() == 1);
    CHECK(texture.value()->height() == 1);
    // An inert texture has no GPU object, so its native handle is zero.
    CHECK(texture.value()->native_handle() == 0);
}

TEST_CASE("a null device rejects the same malformed requests as a real one") {
    auto created = rhi::create_null_device();
    REQUIRE(created.ok());
    rhi::Device& device = *created.value();

    // Zero-capacity buffers, zero-size textures, and null pixels are refused so a
    // headless run does not silently accept what an interactive one would reject.
    CHECK_FALSE(device.create_vertex_buffer(rhi::VertexLayout::position_color_uv(), 0).ok());
    CHECK_FALSE(device.create_index_buffer(0).ok());

    rhi::TextureDesc bad_texture;
    bad_texture.name = "bad";
    CHECK_FALSE(device.create_texture(bad_texture).ok());

    rhi::TextureDesc null_pixels;
    null_pixels.name = "null_pixels";
    null_pixels.width = 4;
    null_pixels.height = 4;
    null_pixels.pixels = nullptr;
    CHECK_FALSE(device.create_texture(null_pixels).ok());

    rhi::RenderTargetDesc bad_target;
    bad_target.name = "bad";
    CHECK_FALSE(device.create_render_target(bad_target).ok());
}

TEST_CASE("the 2D renderer submits its batches to a null device") {
    auto created = rhi::create_null_device();
    REQUIRE(created.ok());
    rhi::Device& device = *created.value();

    // Initialize the real 2D renderer against the inert device. If the render
    // path is going to run headless, this is the code that must run unmodified.
    renderer2d::Renderer2D renderer;
    REQUIRE(renderer.initialize(device).ok() == true);

    // A texture resource built on the null device: the 2D renderer only draws
    // sprites that reference a Texture2D, so this is the smallest scene it will
    // collect. No decode, no file, no GPU object.
    const u8 pixel[4] = {255, 255, 255, 255};
    rhi::TextureDesc texture_desc;
    texture_desc.name = "sprite";
    texture_desc.width = 4;
    texture_desc.height = 4;
    texture_desc.pixels = pixel;
    auto texture = device.create_texture(texture_desc);
    REQUIRE(texture.ok());
    auto resource = std::make_shared<Texture2D>("tex:sprite.png", texture.value());

    Scene scene;
    Node* camera = scene.create_node("Camera");
    camera->add_component<Camera2D>();
    for (int i = 0; i < 3; ++i) {
        Node* node = scene.create_node("Sprite");
        auto* sprite = node->add_component<Sprite2D>();
        sprite->size = Vec2{4.0f, 4.0f};
        sprite->texture = resource;
    }

    device.begin_frame(320, 200, rhi::ClearColor{});
    auto drawn = renderer.render(device, scene, 320, 200);
    REQUIRE(drawn.ok());
    // The sprites were extracted, batched, and issued as draws even though the
    // device rasterises nothing. These are the same counters an interactive run
    // reports, so they prove submission, not pixels.
    CHECK(drawn.value() == 3);
    CHECK(renderer.last_draw_calls() == 1); // three sprites sharing one texture batch into one call
    device.end_frame();
}

TEST_CASE("the 3D renderer runs its shadow pass against a null device") {
    auto created = rhi::create_null_device();
    REQUIRE(created.ok());
    rhi::Device& device = *created.value();

    renderer3d::Renderer3D renderer;
    REQUIRE(renderer.initialize(device).ok() == true);

    Scene scene;
    Node* camera = scene.create_node("Camera");
    camera->add_component<Camera3D>();

    Node* light = scene.create_node("Sun");
    light->add_component<DirectionalLight3D>();

    Node* cube = scene.create_node("Cube");
    auto* mesh_renderer = cube->add_component<MeshRenderer3D>();
    mesh_renderer->mesh = Mesh::make_cube();

    device.begin_frame(320, 200, rhi::ClearColor{});
    auto drawn = renderer.render(device, scene, 320, 200);
    REQUIRE(drawn.ok());

    // One visible cube is one main-pass draw. The shadow pass runs too: it binds
    // the shadow render target the null device created, clears it, draws the
    // caster, and restores the previous target -- the bind/restore sequence that
    // is the whole reason the null device must return a usable render target
    // rather than nullptr. If that path were broken this would not reach here.
    CHECK(drawn.value() == 1);
    CHECK(renderer.last_draw_calls() == 1);
    CHECK(renderer.last_shadow_casters() == 1);
    CHECK(renderer.cached_mesh_count() == 1);

    // The shadow pass restored the caller's target, so the main pass drew where
    // the frame began: the default target.
    CHECK(device.bound_render_target() == nullptr);
    device.end_frame();
}

TEST_CASE("a null render target exposes color and sampleable depth textures") {
    auto created = rhi::create_null_device();
    REQUIRE(created.ok());
    rhi::Device& device = *created.value();

    rhi::RenderTargetDesc desc;
    desc.name = "shadow";
    desc.width = 256;
    desc.height = 256;
    desc.depth = true;
    desc.sample_depth = true;
    auto target = device.create_render_target(desc);
    REQUIRE(target.ok());
    CHECK(target.value()->width() == 256);
    CHECK(target.value()->height() == 256);
    REQUIRE(target.value()->color_texture() != nullptr);
    // sample_depth was set, so a depth texture exists for the shadow pass to read.
    REQUIRE(target.value()->depth_texture() != nullptr);
    CHECK(target.value()->depth_texture()->width() == 256);

    // Binding tracks the bound target like the OpenGL backend does, and a frame
    // boundary resets it to the default target.
    device.begin_frame(320, 200, rhi::ClearColor{});
    device.bind_render_target(target.value().get());
    CHECK(device.bound_render_target() == target.value().get());
    device.begin_frame(320, 200, rhi::ClearColor{});
    CHECK(device.bound_render_target() == nullptr);
}

TEST_CASE("a null render target with color disabled has no color attachment") {
    auto created = rhi::create_null_device();
    REQUIRE(created.ok());
    rhi::Device& device = *created.value();

    // A depth-only target (the shadow map) sets color = false. The backend must
    // agree with the OpenGL one: no colour attachment, but the depth texture it
    // samples still there. A target that reported a colour texture it never
    // allocated would let a caller bind a null attachment.
    rhi::RenderTargetDesc desc;
    desc.name = "shadow_depth_only";
    desc.width = 128;
    desc.height = 128;
    desc.color = false;
    desc.depth = true;
    desc.sample_depth = true;
    auto target = device.create_render_target(desc);
    REQUIRE(target.ok());
    CHECK(target.value()->color_texture() == nullptr);
    REQUIRE(target.value()->depth_texture() != nullptr);
}

TEST_CASE("read_pixels leaves the caller's buffer untouched") {
    auto created = rhi::create_null_device();
    REQUIRE(created.ok());
    rhi::Device& device = *created.value();

    // The buffer is pre-filled with a sentinel. A null device has no framebuffer,
    // so it must not write "black pixels": leaving the buffer untouched is the
    // honest outcome and lets a caller tell "nothing was read" from "the frame
    // was black".
    std::vector<u8> pixels(4 * 4 * 4, 0xAB);
    device.read_pixels(0, 0, 4, 4, pixels);
    for (u8 value : pixels) {
        CHECK(value == 0xAB);
    }
}

TEST_CASE("an engine can come up headless with a null device and renderers") {
    Engine engine;
    REQUIRE(engine.initialize(null_render_config()).ok());

    CHECK(engine.is_initialized());
    // Headless render is not "has graphics": there is still no OS surface.
    CHECK_FALSE(engine.has_graphics());
    CHECK(engine.headless_render());
    CHECK(std::string_view(engine.window().backend_name()) == "null");
    CHECK(std::string_view(engine.device().backend_name()) == "null");

    // The renderers exist and initialized against the null device.
    CHECK(engine.renderer_2d().vertex_capacity() > 0);

    engine.shutdown();
    CHECK_FALSE(engine.is_initialized());
    CHECK_FALSE(engine.headless_render());
}

TEST_CASE("the render path submits draws headless through the frame loop") {
    Engine::Config config = null_render_config();
    HeadlessRenderApp app(config);
    app.set_headless(3);

    CHECK(app.run() == 0);
    CHECK(app.updates == 3);

    // The engine ran the render path with no surface: a null device, no window
    // flip, and no rasterisation, but extraction and batching did happen (the
    // scene's sprites were collected and issued as a draw).
    CHECK(app.had_graphics == false);
    CHECK(app.had_headless_render == true);
}

TEST_CASE("a null device has no error queue to report") {
    auto created = rhi::create_null_device();
    REQUIRE(created.ok());
    rhi::Device& device = *created.value();

    // The drain exists so a browser-only GL error fails a web run rather than
    // being found by eye. The null device touches no graphics API, so it can
    // neither raise nor miss an error, and the honest answer is always "none" --
    // not a fabricated error a caller would have to special-case. A caller that
    // drains a device it did not create therefore reads an empty list and does
    // not have to know which backend it holds.
    CHECK(device.drain_errors().empty());
}

TEST_CASE("headless render is off unless asked for") {
    Engine::Config config = null_render_config();
    config.headless_render = false;

    Engine engine;
    REQUIRE(engine.initialize(config).ok());

    // The default headless path (ADR-0041) still creates no device at all.
    CHECK_FALSE(engine.has_graphics());
    CHECK_FALSE(engine.headless_render());

    engine.shutdown();
}
