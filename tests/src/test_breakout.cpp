// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include <doctest/doctest.h>

#include "breakout_scene.h"
#include "breakout_state.h"
#include "breakout_systems.h"

#include "fe/audio/AudioSource.h"
#include "fe/audio/AudioTypes.h"
#include "fe/physics2d/Collider2D.h"
#include "fe/physics2d/Physics2DSystem.h"
#include "fe/physics2d/PhysicsTypes.h"
#include "fe/physics2d/RigidBody2D.h"
#include "fe/platform/InputActions.h"
#include "fe/platform/InputState.h"
#include "fe/reflect/ComponentTypes.h"
#include "fe/scene/Camera2D.h"
#include "fe/scene/Node.h"
#include "fe/scene/Scene.h"
#include "fe/serialization/SceneSerializer.h"
#include "fe/systems/FrameContext.h"
#include "fe/systems/SceneSchedule.h"

#include <string>
#include <utility>

// The breakout vertical slice's tests.
//
// Two layers, matching the two halves of the game:
//
//   * BreakoutState — the rules. Pure, no engine, driven directly. These are the
//     numbers the game must get right.
//   * The systems and the field builder — compiled from the example's own
//     sources (see tests/CMakeLists.txt), so this is the *shipped* code, not a
//     copy. A scene is built in memory and the schedule is stepped, with no
//     window, GPU, or sound card, which is what lets this run in CI.
//
// A missing asset never fails a test: the builder treats textures as optional,
// so the field and its colliders are identical with or without them.

using namespace fe;

namespace {

FrameContext step(f32 delta) {
    FrameContext context;
    context.delta_time = delta;
    context.elapsed_time = static_cast<f64>(delta);
    return context;
}

int count_bricks(const Scene& scene) {
    int bricks = 0;
    scene.for_each_node([&](const Node& node) {
        if (node.name() == breakout::names::kBrick) {
            ++bricks;
        }
    });
    return bricks;
}

} // namespace

// ---------------------------------------------------------------------------
// Rules: BreakoutState on its own.
// ---------------------------------------------------------------------------

TEST_CASE("a fresh game is playing with the full field") {
    breakout::BreakoutState state; // 11x5 bricks, 3 lives
    CHECK(state.result() == breakout::BreakoutState::Result::Playing);
    CHECK(state.bricks_total() == breakout::config::kBrickCount);
    CHECK(state.bricks_remaining() == breakout::config::kBrickCount);
    CHECK(state.score() == 0);
    CHECK(state.lives() == breakout::config::kStartingLives);
    CHECK_FALSE(state.is_over());
    CHECK(std::string(state.result_name()) == "playing");
}

TEST_CASE("breaking a brick scores and decrements the count") {
    breakout::BreakoutState state(3, 3);
    state.brick_hit();
    CHECK(state.bricks_remaining() == 2);
    CHECK(state.score() == breakout::config::kPointsPerBrick);
    CHECK(state.result() == breakout::BreakoutState::Result::Playing);
}

TEST_CASE("breaking the last brick wins") {
    breakout::BreakoutState state(2, 3);
    state.brick_hit();
    CHECK_FALSE(state.is_over());
    state.brick_hit();
    CHECK(state.bricks_remaining() == 0);
    CHECK(state.score() == 2 * breakout::config::kPointsPerBrick);
    CHECK(state.won());
    CHECK(std::string(state.result_name()) == "win");
}

TEST_CASE("a brick broken after the win does not score") {
    breakout::BreakoutState state(1, 3);
    state.brick_hit(); // wins
    state.brick_hit(); // ignored
    CHECK(state.bricks_remaining() == 0);
    CHECK(state.score() == breakout::config::kPointsPerBrick);
}

TEST_CASE("missing the ball costs a life and the last one ends the game") {
    breakout::BreakoutState state(5, 3);
    state.ball_missed();
    CHECK(state.lives() == 2);
    CHECK_FALSE(state.is_over());
    state.ball_missed();
    state.ball_missed();
    CHECK(state.lives() == 0);
    CHECK(std::string(state.result_name()) == "lose");
    state.ball_missed(); // ignored; lives do not go negative
    CHECK(state.lives() == 0);
}

TEST_CASE("a paddle hit is counted but does not score") {
    breakout::BreakoutState state(5, 3);
    state.paddle_hit();
    state.paddle_hit();
    CHECK(state.paddle_hits() == 2);
    CHECK(state.score() == 0);
}

TEST_CASE("the result is sticky once the game is over") {
    breakout::BreakoutState state(1, 1);
    state.ball_missed(); // lost
    state.brick_hit();   // ignored
    CHECK(state.lives() == 0);
    CHECK(state.bricks_remaining() == 1);
    CHECK(state.score() == 0);
    CHECK(std::string(state.result_name()) == "lose");
}

TEST_CASE("reset restores the field and clears the restart request") {
    breakout::BreakoutState state(4, 2);
    state.brick_hit();
    state.ball_missed();
    state.request_restart();
    state.reset();
    CHECK(state.score() == 0);
    CHECK(state.lives() == 2);
    CHECK(state.bricks_remaining() == 4);
    CHECK(state.paddle_hits() == 0);
    CHECK(state.result() == breakout::BreakoutState::Result::Playing);
    CHECK_FALSE(state.restart_requested());
}

TEST_CASE("a restart request is consumed exactly once") {
    breakout::BreakoutState state;
    state.request_restart();
    CHECK(state.consume_restart());
    CHECK_FALSE(state.consume_restart());
}

// ---------------------------------------------------------------------------
// The field builder and the system wiring.
// ---------------------------------------------------------------------------

TEST_CASE("the field builder makes a complete field") {
    Scene scene;
    breakout::build_breakout_scene(scene);

    CHECK(scene.find_node_by_name(breakout::names::kPaddle) != nullptr);
    CHECK(scene.find_node_by_name(breakout::names::kBall) != nullptr);
    CHECK(scene.find_node_by_name(breakout::names::kAudio) != nullptr);
    CHECK(scene.find_node_by_name("WallLeft") != nullptr);
    CHECK(scene.find_node_by_name("WallRight") != nullptr);
    CHECK(scene.find_node_by_name("WallTop") != nullptr);
    CHECK(count_bricks(scene) == breakout::config::kBrickCount);

    // The ball is a dynamic circle and the paddle is kinematic: the two facts the
    // gameplay depends on (the ball moves on its own; the paddle is driven).
    auto* ball = scene.find_node_by_name(breakout::names::kBall)->get_component<physics2d::RigidBody2D>();
    REQUIRE(ball != nullptr);
    CHECK(ball->is_dynamic());

    auto* paddle = scene.find_node_by_name(breakout::names::kPaddle)->get_component<physics2d::RigidBody2D>();
    REQUIRE(paddle != nullptr);
    CHECK(paddle->kinematic);
}

TEST_CASE("the whole field is visible at the window sizes the game runs at") {
    // The paddle sits at y = -380, below the field's own half-height of 480, and
    // the top wall at y = 495 above it. A camera that showed viewport_size world
    // units put both outside a window shorter than 960 units, so the paddle was
    // driven off-screen and the game looked unresponsive. The reference box makes
    // the camera fit the field instead; this is the regression guard.
    Scene scene;
    breakout::build_breakout_scene(scene);

    auto* camera = scene.find_node_by_name("Camera")->get_component<Camera2D>();
    REQUIRE(camera != nullptr);
    CHECK(camera->reference_width > 0.0f);
    CHECK(camera->reference_height > 0.0f);

    // The extreme world points the player must be able to see: the paddle at both
    // ends of its travel, and the top wall.
    const f32 paddle_limit =
        breakout::config::kFieldHalfWidth - breakout::config::kPaddleHalfWidth - breakout::config::kWallThickness;
    const Vec2 extremes[] = {
        Vec2{0.0f, breakout::config::kPaddleY},
        Vec2{paddle_limit, breakout::config::kPaddleY},
        Vec2{-paddle_limit, breakout::config::kPaddleY},
        Vec2{0.0f, breakout::config::kFieldHalfHeight + breakout::config::kWallThickness},
    };

    // Every size the game is actually played at, including the short viewport a
    // browser canvas produces.
    const std::pair<u32, u32> viewports[] = {{1280, 720}, {1280, 577}, {960, 540}, {1920, 1080}, {640, 360}};
    for (const auto& [width, height] : viewports) {
        const Mat4 view_projection = camera->view_projection(width, height);
        for (const Vec2& point : extremes) {
            const Vec3 clip = transform_point(view_projection, Vec3{point.x, point.y, 0.0f});
            // Inside the clip cube on both axes means the point is on screen.
            CHECK(clip.x >= -1.0f);
            CHECK(clip.x <= 1.0f);
            CHECK(clip.y >= -1.0f);
            CHECK(clip.y <= 1.0f);
        }
    }
}

TEST_CASE("the game systems are installed in order") {
    Scene scene;
    breakout::build_breakout_scene(scene);
    SceneSchedule schedule;
    breakout::BreakoutState state;

    auto* physics = breakout::install_breakout_systems(schedule, state, /*autoplay=*/false);
    CHECK(physics != nullptr);
    REQUIRE(schedule.system_count() == 5);
    CHECK(std::string(schedule.system_at(0)->name()) == "PaddleControlSystem");
    CHECK(std::string(schedule.system_at(1)->name()) == "BallLaunchSystem");
    CHECK(std::string(schedule.system_at(2)->name()) == "Physics2DSystem");
    CHECK(std::string(schedule.system_at(3)->name()) == "BallContactSystem");
    CHECK(std::string(schedule.system_at(4)->name()) == "GameStateSystem");
}

TEST_CASE("the field is gravity-free") {
    Scene scene;
    breakout::build_breakout_scene(scene);
    SceneSchedule schedule;
    breakout::BreakoutState state;
    auto* physics = breakout::install_breakout_systems(schedule, state, /*autoplay=*/false);

    CHECK(physics->gravity.x == doctest::Approx(0.0f));
    CHECK(physics->gravity.y == doctest::Approx(0.0f));

    // With no gravity and no launch, the resting ball stays put.
    const f32 before = scene.find_node_by_name(breakout::names::kBall)->transform().position.y;
    for (int i = 0; i < 60; ++i) {
        schedule.update(step(1.0f / 60.0f), scene);
    }
    CHECK(scene.find_node_by_name(breakout::names::kBall)->transform().position.y == doctest::Approx(before));
}

// ---------------------------------------------------------------------------
// Integration: the systems actually play the game.
// ---------------------------------------------------------------------------

TEST_CASE("autoplay launches the ball and it breaks bricks") {
    Scene scene;
    breakout::build_breakout_scene(scene);
    SceneSchedule schedule;
    breakout::BreakoutState state;
    static_cast<void>(breakout::install_breakout_systems(schedule, state, /*autoplay=*/true));

    // Step until the ball has both broken a brick and been returned once, so the
    // test proves a full rally, not just the first launch.
    for (int i = 0; i < 3600 && (state.score() == 0 || state.paddle_hits() == 0); ++i) {
        schedule.update(step(1.0f / 60.0f), scene);
    }

    CHECK(state.score() >= breakout::config::kPointsPerBrick);
    CHECK(count_bricks(scene) < breakout::config::kBrickCount);
    // The paddle tracks the ball, so it should be returning it: the aimed bounce
    // is what lets the ball reach bricks across the field rather than looping.
    CHECK(state.paddle_hits() >= 1);
}

TEST_CASE("a paddle hit off-centre aims the ball away from the paddle's centre") {
    Scene scene;
    breakout::build_breakout_scene(scene);
    SceneSchedule schedule;
    breakout::BreakoutState state;
    auto* physics = breakout::install_breakout_systems(schedule, state, /*autoplay=*/false);

    Node* ball = scene.find_node_by_name(breakout::names::kBall);
    Node* paddle = scene.find_node_by_name(breakout::names::kPaddle);
    auto* ball_body = ball->get_component<physics2d::RigidBody2D>();
    REQUIRE(ball_body != nullptr);

    // Park the paddle so the ball, placed just above its right edge, is touching
    // it, and give the ball a straight-down velocity. One step should register a
    // paddle contact and re-aim the ball to the right and upward.
    paddle->transform().position.x = 0.0f;
    ball->transform().position = Vec3{
        breakout::config::kPaddleHalfWidth * 0.9f,
        breakout::config::kPaddleY + breakout::config::kPaddleHalfHeight + breakout::config::kBallRadius - 1.0f, 0.0f};
    ball_body->velocity = Vec2{0.0f, -400.0f};
    ball_body->wake();

    schedule.update(step(1.0f / 60.0f), scene);

    CHECK(state.paddle_hits() == 1);
    CHECK(ball_body->velocity.x > 0.0f);
    CHECK(ball_body->velocity.y > 0.0f);
    // The speed is restored to the launch speed by the bounce.
    CHECK(ball_body->velocity.length() == doctest::Approx(breakout::config::kLaunchSpeed).epsilon(0.01));
    // Contact was read from the system that ran earlier this frame.
    CHECK_FALSE(physics->contact_events().empty());
}

TEST_CASE("a centered paddle hit sends the ball straight up") {
    Scene scene;
    breakout::build_breakout_scene(scene);
    SceneSchedule schedule;
    breakout::BreakoutState state;
    static_cast<void>(breakout::install_breakout_systems(schedule, state, /*autoplay=*/false));

    Node* ball = scene.find_node_by_name(breakout::names::kBall);
    Node* paddle = scene.find_node_by_name(breakout::names::kPaddle);
    auto* ball_body = ball->get_component<physics2d::RigidBody2D>();

    paddle->transform().position.x = 0.0f;
    ball->transform().position = Vec3{
        0.0f, breakout::config::kPaddleY + breakout::config::kPaddleHalfHeight + breakout::config::kBallRadius - 1.0f,
        0.0f};
    ball_body->velocity = Vec2{0.0f, -400.0f};
    ball_body->wake();

    schedule.update(step(1.0f / 60.0f), scene);

    CHECK(state.paddle_hits() == 1);
    CHECK(ball_body->velocity.x == doctest::Approx(0.0f).epsilon(0.02));
    CHECK(ball_body->velocity.y == doctest::Approx(breakout::config::kLaunchSpeed).epsilon(0.01));
}

TEST_CASE("a ball that falls past the field loses a life") {
    Scene scene;
    breakout::build_breakout_scene(scene);
    SceneSchedule schedule;
    breakout::BreakoutState state(breakout::config::kBrickCount, 1);
    static_cast<void>(breakout::install_breakout_systems(schedule, state, /*autoplay=*/false));

    // Drop the (unlaunched) ball below the field and step once: the game-state
    // system should see it and take the last life.
    scene.find_node_by_name(breakout::names::kBall)->transform().position =
        Vec3{0.0f, breakout::config::kFieldBottomY - 100.0f, 0.0f};
    schedule.update(step(1.0f / 60.0f), scene);

    CHECK(state.lives() == 0);
    CHECK(std::string(state.result_name()) == "lose");
}

TEST_CASE("a missed ball with lives left respawns the ball") {
    Scene scene;
    breakout::build_breakout_scene(scene);
    SceneSchedule schedule;
    breakout::BreakoutState state(breakout::config::kBrickCount, 3);
    static_cast<void>(breakout::install_breakout_systems(schedule, state, /*autoplay=*/false));

    scene.find_node_by_name(breakout::names::kBall)->transform().position =
        Vec3{0.0f, breakout::config::kFieldBottomY - 100.0f, 0.0f};
    schedule.update(step(1.0f / 60.0f), scene);

    CHECK(state.lives() == 2);
    CHECK_FALSE(state.is_over());
    const Vec3 position = scene.find_node_by_name(breakout::names::kBall)->transform().position;
    CHECK(position.x == doctest::Approx(breakout::config::kBallStartX));
    CHECK(position.y == doctest::Approx(breakout::config::kBallStartY));
}

// ---------------------------------------------------------------------------
// Serialization: the field persists, including the audio node.
// ---------------------------------------------------------------------------

TEST_CASE("the built field round-trips through the scene serializer") {
    reflect::register_builtin_types();
    reflect::register_physics_types();
    reflect::register_audio_types();

    Scene scene;
    breakout::build_breakout_scene(scene);

    auto* source = scene.find_node_by_name(breakout::names::kAudio)->get_component<audio::AudioSource>();
    REQUIRE(source != nullptr);
    source->gain = 0.4f;
    source->pitch = 1.5f;
    source->loop = true;
    source->play_on_start = true;

    auto json = serialization::scene::to_json(scene);
    REQUIRE(json);

    Scene loaded;
    REQUIRE_FALSE(serialization::scene::from_json(json.value(), loaded));

    CHECK(count_bricks(loaded) == breakout::config::kBrickCount);
    CHECK(loaded.find_node_by_name(breakout::names::kPaddle) != nullptr);
    CHECK(loaded.find_node_by_name(breakout::names::kBall) != nullptr);

    auto* loaded_source = loaded.find_node_by_name(breakout::names::kAudio)->get_component<audio::AudioSource>();
    REQUIRE(loaded_source != nullptr);
    CHECK(loaded_source->gain == doctest::Approx(0.4f));
    CHECK(loaded_source->pitch == doctest::Approx(1.5f));
    CHECK(loaded_source->loop == true);
    CHECK(loaded_source->play_on_start == true);
}

// ---------------------------------------------------------------------------
// Actions: the same gameplay code, driven by a key and by a touch.
//
// This is the end-to-end acceptance test for the `touch-input-actions` queue
// item. It runs the *shipped* paddle system through a real SceneSchedule and
// proves that a finger on the left third of the screen moves the paddle the way
// the Left key does — which is the whole point of the action layer, and the
// thing a unit test of the ActionMap alone cannot show.
// ---------------------------------------------------------------------------

TEST_CASE("a touch steers the paddle through the shipped system") {
    Scene scene;
    breakout::build_breakout_scene(scene);
    SceneSchedule schedule;
    breakout::BreakoutState state;

    ActionMap actions;
    const breakout::BreakoutActions bindings = breakout::declare_breakout_actions(actions);
    static_cast<void>(breakout::install_breakout_systems(schedule, state, /*autoplay=*/false, bindings));

    InputState input;
    const Vec2 viewport{1280.0f, 720.0f};
    Node* paddle = scene.find_node_by_name(breakout::names::kPaddle);
    REQUIRE(paddle != nullptr);
    const f32 start_x = paddle->transform().position.x;

    // A finger in the left third, held across frames, must drive the paddle left
    // exactly as holding the Left key would.
    input.on_touch(TouchPhase::Began, 1, 100.0f, 400.0f);
    for (int i = 0; i < 10; ++i) {
        input.begin_frame();
        actions.update(input, viewport);
        FrameContext context = step(1.0f / 60.0f);
        context.input = &input;
        context.actions = &actions;
        schedule.update(context, scene);
    }
    CHECK(paddle->transform().position.x < start_x);

    // Lifting the finger stops the paddle, which is what proves the region is
    // read every frame rather than latching.
    const f32 moved_x = paddle->transform().position.x;
    input.on_touch(TouchPhase::Ended, 1, 100.0f, 400.0f);
    for (int i = 0; i < 5; ++i) {
        input.begin_frame();
        actions.update(input, viewport);
        FrameContext context = step(1.0f / 60.0f);
        context.input = &input;
        context.actions = &actions;
        schedule.update(context, scene);
    }
    CHECK(paddle->transform().position.x == doctest::Approx(moved_x));
}

TEST_CASE("a touch and a key move the paddle the same way") {
    // The equivalence the action layer exists to provide: the two devices are
    // interchangeable, so a phone and a desktop play the same game.
    const auto run = [](bool use_touch) {
        Scene scene;
        breakout::build_breakout_scene(scene);
        SceneSchedule schedule;
        breakout::BreakoutState state;
        ActionMap actions;
        const breakout::BreakoutActions bindings = breakout::declare_breakout_actions(actions);
        static_cast<void>(breakout::install_breakout_systems(schedule, state, /*autoplay=*/false, bindings));

        InputState input;
        if (use_touch) {
            input.on_touch(TouchPhase::Began, 1, 100.0f, 400.0f);
        } else {
            input.on_key(Key::Left, true);
        }
        for (int i = 0; i < 10; ++i) {
            input.begin_frame();
            actions.update(input, Vec2{1280.0f, 720.0f});
            FrameContext context = step(1.0f / 60.0f);
            context.input = &input;
            context.actions = &actions;
            schedule.update(context, scene);
        }
        return scene.find_node_by_name(breakout::names::kPaddle)->transform().position.x;
    };

    CHECK(run(true) == doctest::Approx(run(false)));
}

TEST_CASE("a tap in the middle launches the ball through the shipped system") {
    Scene scene;
    breakout::build_breakout_scene(scene);
    SceneSchedule schedule;
    breakout::BreakoutState state;

    ActionMap actions;
    const breakout::BreakoutActions bindings = breakout::declare_breakout_actions(actions);
    static_cast<void>(breakout::install_breakout_systems(schedule, state, /*autoplay=*/false, bindings));

    auto* ball = scene.find_node_by_name(breakout::names::kBall)->get_component<physics2d::RigidBody2D>();
    REQUIRE(ball != nullptr);
    REQUIRE(ball->velocity.length_squared() == doctest::Approx(0.0f));

    InputState input;
    input.on_touch(TouchPhase::Began, 1, 640.0f, 400.0f); // the middle third
    input.begin_frame();
    actions.update(input, Vec2{1280.0f, 720.0f});
    FrameContext context = step(1.0f / 60.0f);
    context.input = &input;
    context.actions = &actions;
    schedule.update(context, scene);

    CHECK(ball->velocity.length_squared() > 0.0f);
}

TEST_CASE("the three-argument install still works with no action map") {
    // The no-actions path a headless tool and an older caller use must keep
    // playing: with no action map, the systems fall back to the keys.
    Scene scene;
    breakout::build_breakout_scene(scene);
    SceneSchedule schedule;
    breakout::BreakoutState state;
    static_cast<void>(breakout::install_breakout_systems(schedule, state, /*autoplay=*/false));

    InputState input;
    input.on_key(Key::Left, true);
    Node* paddle = scene.find_node_by_name(breakout::names::kPaddle);
    const f32 start_x = paddle->transform().position.x;
    for (int i = 0; i < 10; ++i) {
        input.begin_frame();
        FrameContext context = step(1.0f / 60.0f);
        context.input = &input;
        // Deliberately no context.actions: the fallback must carry it.
        schedule.update(context, scene);
    }
    CHECK(paddle->transform().position.x < start_x);
}
