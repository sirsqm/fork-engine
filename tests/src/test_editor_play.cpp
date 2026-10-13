// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
//
// Play-in-editor, end to end. Unlike the headless editor tests, this one embeds
// a real Engine (hidden window + OpenGL context), starts a PlaySession against
// an authored scene, drives frames through the engine, and reads the framebuffer
// back to prove the *play copy* actually rendered - the property that makes
// play-in-editor more than a scene clone.
//
// Like the renderer tests it skips cleanly when no graphics context can be
// created, so `ctest` stays green on machines without a GPU.

#include <doctest/doctest.h>

#include "fe/editor/PlaySession.h"
#include "fe/renderer2d/Renderer2D.h"
#include "fe/resources/Texture2D.h"
#include "fe/scene/Camera2D.h"
#include "fe/scene/Node.h"
#include "fe/scene/Scene.h"
#include "fe/scene/Sprite2D.h"

#include <memory>
#include <span>

using namespace fe;
using namespace fe::editor;

namespace {

struct PlayContext {
    Engine engine;
    bool available = false;

    PlayContext() {
        Engine::Config config;
        config.window.title = "fork engine editor play test";
        config.window.width = 64;
        config.window.height = 64;
        config.window.visible = false;
        config.window.vsync = false;
        if (Error error = engine.initialize(config); error) {
            MESSAGE("graphics context unavailable: " << error.message());
            return;
        }
        available = true;
    }

    ~PlayContext() { engine.shutdown(); }
};

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

void register_defaults(PlaySession& session) {
    session.register_component_factory(Camera2D::kTypeName, [](const Node& source, Node& destination) {
        const auto* component = source.get_component<Camera2D>();
        if (component == nullptr) {
            return;
        }
        auto* copy = destination.add_component<Camera2D>();
        copy->zoom = component->zoom;
        copy->primary = component->primary;
        copy->clear_r = component->clear_r;
        copy->clear_g = component->clear_g;
        copy->clear_b = component->clear_b;
        copy->clear_a = component->clear_a;
    });
    session.register_component_factory(Sprite2D::kTypeName, [](const Node& source, Node& destination) {
        const auto* component = source.get_component<Sprite2D>();
        if (component == nullptr) {
            return;
        }
        auto* copy = destination.add_component<Sprite2D>();
        copy->size = component->size;
        copy->color = component->color;
        copy->uv = component->uv;
        copy->layer = component->layer;
        copy->texture = component->texture;
    });
}

struct Pixel {
    u8 r = 0;
    u8 g = 0;
    u8 b = 0;
    u8 a = 0;
};

} // namespace

TEST_CASE("play-in-editor renders the play copy through the embedded engine") {
    PlayContext context;
    if (!context.available) {
        MESSAGE("skipping: no graphics context");
        return;
    }

    // Author a scene whose centre is a solid red sprite.
    Scene authored;
    authored.create_node("Camera")->add_component<Camera2D>();
    Node* sprite_node = authored.create_node("Sprite");
    auto* sprite = sprite_node->add_component<Sprite2D>();
    sprite->texture = make_solid_texture(context.engine.device(), 255, 255, 255, 255);
    sprite->size = Vec2{32.0f, 32.0f};
    sprite->color = Vec4{1.0f, 0.0f, 0.0f, 1.0f}; // Red.

    PlaySession session;
    register_defaults(session);
    REQUIRE_FALSE(session.start(authored));
    CHECK(session.is_playing());

    // Model the editor host's frame: tick the play session, read back, present.
    auto drawn = session.tick(context.engine);
    REQUIRE(drawn);
    CHECK(drawn.value() == 1); // One sprite in the play copy.

    std::vector<u8> pixels(64U * 64U * 4U, 0);
    context.engine.device().read_pixels(32, 32, 1, 1, std::span<u8>(pixels.data(), pixels.size()));
    context.engine.device().end_frame();

    const Pixel centre{pixels[0], pixels[1], pixels[2], pixels[3]};
    CHECK(centre.r > 200);
    CHECK(centre.g < 60);
    CHECK(centre.b < 60);

    // After stopping, the authored scene is intact and no longer being rendered.
    session.stop();
    CHECK_FALSE(session.is_playing());
    CHECK(authored.find_node_by_name("Sprite") != nullptr);
}

TEST_CASE("ticking a stopped play session is reported as an error") {
    PlayContext context;
    if (!context.available) {
        MESSAGE("skipping: no graphics context");
        return;
    }

    PlaySession session;
    // Not started: ticking is a programming error and must be reported, not
    // silently ignored.
    auto result = session.tick(context.engine);
    CHECK_FALSE(result);
}
