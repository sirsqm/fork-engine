// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
//
// Tests for the audio listener system. The listener is what turns a camera into
// an ear: the mixer attenuates a positional source by its distance to the
// listener, so if nothing moves the listener the whole scene is heard from the
// world origin. These tests pin that the system reads the *active* camera's
// world position -- including a camera parented to a moving rig -- and that it
// is a no-op with no camera or no audio system, which is what lets a caller add
// it unconditionally.

#include <doctest/doctest.h>

#include "fe/audio/AudioSystem.h"
#include "fe/core/Log.h"
#include "fe/platform/Window.h"
#include "fe/runtime/AudioListenerSystem.h"
#include "fe/runtime/Engine.h"
#include "fe/scene/Camera2D.h"
#include "fe/scene/Node.h"
#include "fe/scene/Scene.h"
#include "fe/scene3d/Camera3D.h"
#include "fe/systems/FrameContext.h"
#include "fe/systems/SceneSchedule.h"

#include <memory>

using namespace fe;

namespace {

FrameContext step(f32 delta) {
    FrameContext context;
    context.delta_time = delta;
    return context;
}

// A headless engine, so the integration test below can adopt a real audio system
// from a real Engine without a window or a sound card.
Engine::Config null_config() {
    Engine::Config config;
    config.window.backend = WindowBackend::Null;
    config.window.width = 320;
    config.window.height = 200;
    return config;
}

} // namespace

TEST_CASE("the listener follows a 3D camera's world position") {
    audio::AudioSystem audio(nullptr);
    AudioListenerSystem listener(&audio);

    Scene scene;
    Node* camera_node = scene.create_node("Camera");
    camera_node->add_component<Camera3D>();
    camera_node->transform().position = Vec3{4.0f, 5.0f, 6.0f};

    listener.update(step(0.016f), scene);
    CHECK(audio.listener_position().x == doctest::Approx(4.0f));
    CHECK(audio.listener_position().y == doctest::Approx(5.0f));
    CHECK(audio.listener_position().z == doctest::Approx(6.0f));

    // Moving the camera moves the listener, because the position is read fresh
    // each frame rather than cached at construction.
    camera_node->transform().position = Vec3{-1.0f, 0.0f, 2.0f};
    listener.update(step(0.016f), scene);
    CHECK(audio.listener_position().x == doctest::Approx(-1.0f));
    CHECK(audio.listener_position().z == doctest::Approx(2.0f));
}

TEST_CASE("the listener follows a parented camera, not just a root one") {
    // The point of reading the world matrix: a camera hung off a rig must drag
    // the listener with the rig's transform. A local-transform read would leave
    // the listener at the rig's child-local position and hear the world wrong.
    audio::AudioSystem audio(nullptr);
    AudioListenerSystem listener(&audio);

    Scene scene;
    Node* rig = scene.create_node("Rig");
    rig->transform().position = Vec3{10.0f, 0.0f, 0.0f};
    Node* camera_node = rig->create_child<Node>("Camera");
    camera_node->add_component<Camera3D>();
    camera_node->transform().position = Vec3{0.0f, 3.0f, 0.0f};

    listener.update(step(0.016f), scene);
    CHECK(audio.listener_position().x == doctest::Approx(10.0f));
    CHECK(audio.listener_position().y == doctest::Approx(3.0f));
}

TEST_CASE("the listener falls back to a 2D camera when the scene has no 3D one") {
    audio::AudioSystem audio(nullptr);
    AudioListenerSystem listener(&audio);

    Scene scene;
    Node* camera_node = scene.create_node("Camera2D");
    camera_node->add_component<Camera2D>();
    camera_node->transform().position = Vec3{7.0f, -2.0f, 0.0f};

    listener.update(step(0.016f), scene);
    CHECK(audio.listener_position().x == doctest::Approx(7.0f));
    CHECK(audio.listener_position().y == doctest::Approx(-2.0f));
}

TEST_CASE("a 3D camera wins when a scene has both kinds") {
    // A scene with both cameras is drawn through the 3D pass, so the player
    // hears from the 3D camera; the 2D one is not the ear.
    audio::AudioSystem audio(nullptr);
    AudioListenerSystem listener(&audio);

    Scene scene;
    Node* camera2d = scene.create_node("Camera2D");
    camera2d->add_component<Camera2D>();
    camera2d->transform().position = Vec3{100.0f, 0.0f, 0.0f};

    Node* camera3d = scene.create_node("Camera3D");
    camera3d->add_component<Camera3D>();
    camera3d->transform().position = Vec3{1.0f, 1.0f, 1.0f};

    listener.update(step(0.016f), scene);
    CHECK(audio.listener_position().x == doctest::Approx(1.0f));
}

TEST_CASE("the listener is a no-op with no audio system") {
    // The null contract: a caller may add the system without proving an audio
    // system exists, and it must not crash. A scene with a camera is used so the
    // camera lookup runs and only the null check stops it.
    AudioListenerSystem listener(nullptr);

    Scene scene;
    Node* camera_node = scene.create_node("Camera");
    camera_node->add_component<Camera3D>();
    camera_node->transform().position = Vec3{4.0f, 5.0f, 6.0f};

    listener.update(step(0.016f), scene);
    CHECK(listener.audio_system() == nullptr);
}

TEST_CASE("the listener leaves the position untouched with no camera") {
    // A scene with no camera has no ear; the listener keeps its last value
    // rather than snapping to the origin, which would make a momentarily
    // camera-less scene suddenly hear every sound from the world origin.
    audio::AudioSystem audio(nullptr);
    AudioListenerSystem listener(&audio);

    Scene scene;
    static_cast<void>(scene.create_node("Empty"));

    audio.set_listener_position(Vec3{9.0f, 9.0f, 9.0f});
    listener.update(step(0.016f), scene);
    CHECK(audio.listener_position().x == doctest::Approx(9.0f));
    CHECK(audio.listener_position().y == doctest::Approx(9.0f));
    CHECK(audio.listener_position().z == doctest::Approx(9.0f));
}

TEST_CASE("the system reports a stable name and can be re-pointed at an audio system") {
    AudioListenerSystem listener(nullptr);
    CHECK(listener.name() == "AudioListenerSystem");

    audio::AudioSystem audio(nullptr);
    listener.set_audio_system(&audio);
    CHECK(listener.audio_system() == &audio);

    Scene scene;
    Node* camera_node = scene.create_node("Camera");
    camera_node->add_component<Camera3D>();
    camera_node->transform().position = Vec3{2.0f, 3.0f, 4.0f};
    listener.update(step(0.016f), scene);
    CHECK(audio.listener_position().x == doctest::Approx(2.0f));
}

TEST_CASE("a schedule drives the listener from a real engine's audio system") {
    // The integration seam the player and the editor both use: adopt the
    // engine's audio system, add the listener before it, and let the schedule
    // run a frame. This is the whole path end to end -- transfer ownership,
    // order the systems, and read the camera -- with the real Engine rather than
    // a hand-built AudioSystem.
    Engine engine;
    // Engine::initialize() sets the process-wide log level from its config, so
    // save and restore it: a later test (test_log) asserts on captured output and
    // must not inherit this test's level.
    const LogLevel previous_level = log::min_level();
    REQUIRE(engine.initialize(null_config()).ok());
    REQUIRE(engine.audio_system() != nullptr);

    auto system = engine.take_audio_system();
    REQUIRE(system != nullptr);
    CHECK(engine.audio_system() == nullptr); // ownership transferred

    SceneSchedule schedule;
    schedule.add_system<AudioListenerSystem>(system.get());
    schedule.add_system(std::move(system));
    CHECK(schedule.system_count() == 2);

    Scene scene;
    Node* camera_node = scene.create_node("Camera");
    camera_node->add_component<Camera3D>();
    camera_node->transform().position = Vec3{3.0f, 1.0f, -2.0f};

    schedule.update(step(0.016f), scene);
    const auto* audio = static_cast<const audio::AudioSystem*>(schedule.find("AudioSystem"));
    REQUIRE(audio != nullptr);
    CHECK(audio->listener_position().x == doctest::Approx(3.0f));
    CHECK(audio->listener_position().y == doctest::Approx(1.0f));
    CHECK(audio->listener_position().z == doctest::Approx(-2.0f));

    engine.shutdown();
    log::set_min_level(previous_level);
}
