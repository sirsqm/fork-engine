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
#include "fe/scene/SpriteAnimator.h"

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
    session.register_component_factory(SpriteAnimator::kTypeName, [](const Node& source, Node& destination) {
        const auto* component = source.get_component<SpriteAnimator>();
        if (component == nullptr) {
            return;
        }
        auto* copy = destination.add_component<SpriteAnimator>();
        copy->frames_x = component->frames_x;
        copy->frames_y = component->frames_y;
        copy->fps = component->fps;
        copy->playing = component->playing;
        copy->loop = component->loop;
        copy->current_frame = component->current_frame;
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

TEST_CASE("play-in-editor runs the schedule, so an animator advances while playing") {
    PlayContext context;
    if (!context.available) {
        MESSAGE("skipping: no graphics context");
        return;
    }

    // Author a scene with an animated sprite: SpriteAnimator state plus the
    // Sprite2D it drives.
    Scene authored;
    authored.create_node("Camera")->add_component<Camera2D>();
    Node* node = authored.create_node("Animated");
    auto* sprite = node->add_component<Sprite2D>();
    sprite->texture = make_solid_texture(context.engine.device(), 255, 255, 255, 255);
    sprite->size = Vec2{32.0f, 32.0f};
    auto* animator = node->add_component<SpriteAnimator>();
    animator->frames_x = 4;
    animator->frames_y = 1;
    animator->fps = 20.0f; // One cell every 50ms; fast enough that a few hundred
                           // real frames reach the next cell on any machine.

    PlaySession session;
    register_defaults(session);
    // The session registers SpriteAnimationSystem by default, so this is here
    // only to document the expectation: one default system, the animation one.
    CHECK(session.schedule().system_count() == 0); // empty until start()

    REQUIRE_FALSE(session.start(authored));
    CHECK(session.schedule().find("SpriteAnimationSystem") != nullptr);

    // The play copy owns its own animator, independent of the authored one.
    const auto& play_scene = session.play_scene();
    const Node* play_node = play_scene.find_node_by_name("Animated");
    REQUIRE(play_node != nullptr);
    const auto* play_animator = play_node->get_component<SpriteAnimator>();
    const auto* play_sprite = play_node->get_component<Sprite2D>();
    REQUIRE(play_animator != nullptr);
    REQUIRE(play_sprite != nullptr);
    // Before the first tick the system has not published UVs yet, so the copy's
    // sprite still carries the authored default (the full texture).
    CHECK(play_sprite->uv == Vec4{0.0f, 0.0f, 1.0f, 1.0f});

    // Drive many frames. The engine's delta is real wall-clock time, so rather
    // than guess the frame count we loop until the animator has advanced past
    // its first cell, with a generous bound so a slow machine still finishes.
    const u32 initial_cell = play_animator->current_frame;
    bool advanced = false;
    for (int frame = 0; frame < 600 && !advanced; ++frame) {
        REQUIRE(session.tick(context.engine));
        context.engine.device().end_frame();
        if (play_animator->current_frame != initial_cell) {
            advanced = true;
        }
    }
    CHECK(advanced);
    // The system wrote the new cell's UVs onto the play sprite, which is what
    // the renderer draws: proof the schedule feeds the render path.
    CHECK(play_sprite->uv.x == doctest::Approx(static_cast<f32>(play_animator->current_frame) * 0.25f));
    CHECK(play_sprite->uv.z == doctest::Approx(static_cast<f32>(play_animator->current_frame + 1) * 0.25f));

    // The authored animator was never touched: play-in-editor copies.
    CHECK(animator->current_frame == 0);
}
