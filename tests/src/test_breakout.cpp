// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include <doctest/doctest.h>

#include "breakout_scene.h"
#include "breakout_state.h"
#include "breakout_systems.h"

#include "fe/audio/AudioSource.h"
#include "fe/audio/AudioSystem.h"
#include "fe/audio/AudioTypes.h"
#include "fe/physics2d/Collider2D.h"
#include "fe/physics2d/Physics2DSystem.h"
#include "fe/physics2d/PhysicsTypes.h"
#include "fe/physics2d/RigidBody2D.h"
#include "fe/platform/InputActions.h"
#include "fe/platform/InputState.h"
#include "fe/platform/UserData.h"
#include "fe/reflect/ComponentTypes.h"
#include "fe/resources/Font.h"
#include "fe/resources/Texture2D.h"
#include "fe/rhi/Device.h"
#include "fe/rhi/Types.h"
#include "fe/scene/Camera2D.h"
#include "fe/scene/Node.h"
#include "fe/scene/Scene.h"
#include "fe/serialization/SceneSerializer.h"
#include "fe/systems/FrameContext.h"
#include "fe/systems/SceneSchedule.h"
#include "fe/ui/UiElement.h"
#include "fe/ui/UiLayout.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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

TEST_CASE("breaking the last brick on the last level wins") {
    // A one-level game: clearing the only board is the win, so the level
    // progression does not hide the end state behind another board.
    breakout::BreakoutState state(2, 3, /*level_count=*/1);
    state.brick_hit();
    CHECK_FALSE(state.is_over());
    state.brick_hit();
    CHECK(state.bricks_remaining() == 0);
    CHECK(state.score() == 2 * breakout::config::kPointsPerBrick + breakout::config::kLevelClearBonus);
    CHECK(state.won());
    CHECK(std::string(state.result_name()) == "win");
}

TEST_CASE("a brick broken after the win does not score") {
    breakout::BreakoutState state(1, 3, /*level_count=*/1);
    state.brick_hit(); // wins
    state.brick_hit(); // ignored
    CHECK(state.bricks_remaining() == 0);
    CHECK(state.score() == breakout::config::kPointsPerBrick + breakout::config::kLevelClearBonus);
}

// ---------------------------------------------------------------------------
// Levels: clearing a board advances and makes the ball faster
// (docs/PLAN.md, `slice-scoring-and-progression`).
// ---------------------------------------------------------------------------

TEST_CASE("clearing a board advances the level and speeds the ball up") {
    // The acceptance test for `slice-scoring-and-progression`: a clear must both
    // advance the level and raise the difficulty, and both must be observable
    // through the rules with no scene and no engine.
    breakout::BreakoutState state(2, 3);
    const fe::f32 level_one_speed = state.launch_speed();
    CHECK(state.level() == 1);

    state.brick_hit(); // one brick left
    CHECK(state.level() == 1);
    CHECK_FALSE(state.level_change_requested());

    state.brick_hit(); // the board is clear: advance, do not win
    CHECK_FALSE(state.is_over());
    CHECK(state.level() == 2);
    CHECK(state.bricks_remaining() == state.bricks_total());
    CHECK(state.level_change_requested());
    CHECK(state.consume_level_change());
    CHECK_FALSE(state.level_change_requested());

    // The difficulty rose: the ball is launched faster on the new level.
    CHECK(state.launch_speed() > level_one_speed);
}

TEST_CASE("clearing the last board wins instead of advancing") {
    breakout::BreakoutState state(1, 3, /*level_count=*/1);
    state.brick_hit();
    CHECK(state.is_over());
    CHECK(state.won());
    CHECK(state.level() == 1); // no level past the last one
    CHECK_FALSE(state.level_change_requested());
}

TEST_CASE("the difficulty rises with the level and stays under the ceiling") {
    // The lever is the ball's launch speed. It must rise with the level and never
    // exceed the clamp, or a fast ball could step past a brick.
    breakout::BreakoutState state(breakout::config::kBrickCount, 3, /*level_count=*/8);
    fe::f32 previous = state.launch_speed();
    CHECK(previous == breakout::config::kLaunchSpeed); // level 1 is the authored speed
    for (int level = 2; level <= state.level_count(); ++level) {
        for (int i = 0; i < state.bricks_total(); ++i) {
            state.brick_hit();
        }
        state.consume_level_change();
        CHECK(state.level() == level);
        const fe::f32 speed = state.launch_speed();
        CHECK(speed >= previous);
        CHECK(speed <= breakout::config::kMaxBallSpeed);
        previous = speed;
    }
    // Eight levels of the gain would pass the ceiling; the clamp holds it.
    CHECK(state.launch_speed() == breakout::config::kMaxBallSpeed);
}

TEST_CASE("a restart clears the level back to the first") {
    breakout::BreakoutState state(2, 3);
    state.brick_hit();
    state.brick_hit(); // clears the board, advances to level 2
    state.consume_level_change();
    REQUIRE(state.level() == 2);

    state.start_game(); // R / a fresh game
    CHECK(state.level() == 1);
    CHECK(state.bricks_remaining() == state.bricks_total());
}

TEST_CASE("the HUD shows the level") {
    Scene scene;
    breakout::build_breakout_scene(scene);
    SceneSchedule schedule;
    breakout::BreakoutState state(2, 3);
    static_cast<void>(breakout::install_breakout_systems(schedule, state, /*autoplay=*/false));

    Node* level_node = scene.find_node_by_name(breakout::names::kLevelLabel);
    REQUIRE(level_node != nullptr);
    auto* level_element = level_node->get_component<ui::UiElement>();
    REQUIRE(level_element != nullptr);

    schedule.update(step(1.0f / 60.0f), scene);
    CHECK(level_element->text == "LEVEL 1/3");

    state.brick_hit();
    state.brick_hit(); // clears the board, advances
    state.consume_level_change();
    schedule.update(step(1.0f / 60.0f), scene);
    CHECK(level_element->text == "LEVEL 2/3");
}

TEST_CASE("the rebuild handshake brings the bricks back for the next level") {
    // The rules advance the level and request a rebuild; the application owns the
    // scene and does it. This drives both halves in the order main.cpp does, so
    // the handshake that ships is the one under test: clear the board, run a
    // frame, and the field is back with the score and the lives kept.
    Scene scene;
    breakout::build_breakout_scene(scene);
    SceneSchedule schedule;
    breakout::BreakoutState state; // the real board: kBrickCount bricks
    static_cast<void>(breakout::install_breakout_systems(schedule, state, /*autoplay=*/false));

    for (int i = 0; i < breakout::config::kBrickCount; ++i) {
        state.brick_hit(); // clears the board
    }
    schedule.update(step(1.0f / 60.0f), scene); // systems see the clear
    REQUIRE(state.level_change_requested());
    REQUIRE(state.level() == 2);
    const int score_after_clear = state.score();

    // What the application does when it consumes the request.
    if (state.consume_level_change()) {
        scene.clear();
        breakout::build_breakout_scene(scene);
    }

    CHECK(count_bricks(scene) == breakout::config::kBrickCount);
    CHECK(state.bricks_remaining() == breakout::config::kBrickCount);
    CHECK(state.score() == score_after_clear); // the score carries over
    CHECK(state.lives() == 3);
    CHECK(state.launch_speed() > breakout::config::kLaunchSpeed); // and the ball is faster
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
    REQUIRE(schedule.system_count() == 10);
    // The UI input system runs first: it writes each element's interaction state
    // from this frame's pointer before any gameplay system reads it, so the menu
    // system sees the same frame's clicks.
    CHECK(std::string(schedule.system_at(0)->name()) == "ui/input");
    // The menu runs next: it decides the screen, and the gameplay systems below
    // read that decision this same frame.
    CHECK(std::string(schedule.system_at(1)->name()) == "MenuSystem");
    // Settings runs right after the menu, so a click that changed a setting this
    // frame is applied and saved this frame.
    CHECK(std::string(schedule.system_at(2)->name()) == "SettingsSystem");
    CHECK(std::string(schedule.system_at(3)->name()) == "PaddleControlSystem");
    CHECK(std::string(schedule.system_at(4)->name()) == "BallLaunchSystem");
    CHECK(std::string(schedule.system_at(5)->name()) == "Physics2DSystem");
    CHECK(std::string(schedule.system_at(6)->name()) == "BallContactSystem");
    CHECK(std::string(schedule.system_at(7)->name()) == "GameStateSystem");
    // Persistence runs after the rules have settled the screen, so the game-over
    // edge it saves on is the real one, and before the HUD, so a best score set
    // this frame is drawn this frame.
    CHECK(std::string(schedule.system_at(8)->name()) == "PersistSystem");
    // The HUD runs last so it reads the state every system above it produced.
    CHECK(std::string(schedule.system_at(9)->name()) == "HudSystem");
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

// ---------------------------------------------------------------------------
// The HUD: the score and lives the player reads on screen.
// ---------------------------------------------------------------------------

TEST_CASE("the HUD writes the score and lives into the scene's UI elements") {
    Scene scene;
    breakout::build_breakout_scene(scene);
    SceneSchedule schedule;
    breakout::BreakoutState state;
    static_cast<void>(breakout::install_breakout_systems(schedule, state, /*autoplay=*/false));

    Node* score_node = scene.find_node_by_name(breakout::names::kScoreLabel);
    Node* lives_node = scene.find_node_by_name(breakout::names::kLivesLabel);
    REQUIRE(score_node != nullptr);
    REQUIRE(lives_node != nullptr);
    auto* score_element = score_node->get_component<ui::UiElement>();
    auto* lives_element = lives_node->get_component<ui::UiElement>();
    REQUIRE(score_element != nullptr);
    REQUIRE(lives_element != nullptr);

    // Authored empty: the game's state is what makes the text readable.
    CHECK(score_element->text.empty());
    CHECK(lives_element->text.empty());

    schedule.update(step(1.0f / 60.0f), scene);
    CHECK(score_element->text == "SCORE 0");
    CHECK(lives_element->text == "LIVES 3");

    // Break a brick and miss a ball through the rules, then step: the elements
    // must follow the state, not a private copy of it.
    state.brick_hit();
    state.ball_missed();
    schedule.update(step(1.0f / 60.0f), scene);
    CHECK(score_element->text == "SCORE 10");
    CHECK(lives_element->text == "LIVES 2");

    // The HUD is a screen-space canvas above the field's sprites, so it is never
    // drawn over.
    Node* canvas_node = scene.find_node_by_name(breakout::names::kHudCanvas);
    REQUIRE(canvas_node != nullptr);
    auto* canvas = canvas_node->get_component<ui::UiCanvas>();
    REQUIRE(canvas != nullptr);
    CHECK(canvas->layer > 0);

    // The score is pinned to one corner and the lives to the other, which is the
    // whole reason for anchoring: the two counters stay apart at any window size.
    CHECK(score_element->anchor == ui::UiAnchor::TopLeft);
    CHECK(lives_element->anchor == ui::UiAnchor::TopRight);
}

TEST_CASE("the HUD replaces the lives with the result once the game ends") {
    Scene scene;
    breakout::build_breakout_scene(scene);
    SceneSchedule schedule;
    breakout::BreakoutState state;
    static_cast<void>(breakout::install_breakout_systems(schedule, state, /*autoplay=*/false));

    Node* game_over_node = scene.find_node_by_name(breakout::names::kGameOverLabel);
    REQUIRE(game_over_node != nullptr);
    auto* game_over_element = game_over_node->get_component<ui::UiElement>();
    REQUIRE(game_over_element != nullptr);

    // Lose every life. The end-of-game message lives on the game-over screen's
    // label -- centred on the *screen*, not the field -- so it reads as a message
    // over the whole window, which is what a UI canvas is for. It is authored
    // "GAME OVER" and reads "YOU WIN" on a win, so a loss and a win differ.
    for (int i = 0; i < breakout::config::kStartingLives; ++i) {
        state.ball_missed();
    }
    REQUIRE(state.is_over());
    schedule.update(step(1.0f / 60.0f), scene);

    CHECK(game_over_element->text == "GAME OVER");
    CHECK(game_over_element->anchor == ui::UiAnchor::MiddleCenter);
    CHECK(game_over_element->text_align == ui::UiAlign::Center);

    // The lives counter is hidden with the HUD on the game-over screen, but its
    // text stays current so it reads correctly the instant play resumes.
    Node* lives_node = scene.find_node_by_name(breakout::names::kLivesLabel);
    REQUIRE(lives_node != nullptr);
    auto* lives_element = lives_node->get_component<ui::UiElement>();
    REQUIRE(lives_element != nullptr);
    CHECK(lives_element->text == "LIVES 0");
}

TEST_CASE("the game-over screen says YOU WIN after a win") {
    Scene scene;
    breakout::build_breakout_scene(scene);
    SceneSchedule schedule;
    breakout::BreakoutState state(1, 3, /*level_count=*/1);
    static_cast<void>(breakout::install_breakout_systems(schedule, state, /*autoplay=*/false));

    Node* game_over_node = scene.find_node_by_name(breakout::names::kGameOverLabel);
    REQUIRE(game_over_node != nullptr);
    auto* game_over_element = game_over_node->get_component<ui::UiElement>();
    REQUIRE(game_over_element != nullptr);

    state.brick_hit(); // the last brick: a win
    REQUIRE(state.won());
    schedule.update(step(1.0f / 60.0f), scene);

    CHECK(game_over_element->text == "YOU WIN");
}

// ---------------------------------------------------------------------------
// Screens: the title -> play -> game over -> title flow
// (docs/PLAN.md, `slice-title-and-menus`).
//
// The rules half is BreakoutState on its own, with no engine; the shipped half
// drives the real scene and the real schedule, which is what proves the flow the
// game actually runs rather than a re-implementation of it.
// ---------------------------------------------------------------------------

namespace {

// One frame of the shipped schedule with the given input snapshot, in the order
// Application::step_frame() drives it: advance the input edges, evaluate the
// actions, then run the systems. The caller reads the scene and the state.
void run_screen_frame(SceneSchedule& schedule, Scene& scene, InputState& input, ActionMap& actions) {
    input.begin_frame();
    actions.update(input, Vec2{1280.0f, 720.0f});
    FrameContext context = step(1.0f / 60.0f);
    context.input = &input;
    context.actions = &actions;
    context.viewport = Vec2{1280.0f, 720.0f};
    schedule.update(context, scene);
}

[[nodiscard]] bool canvas_visible(const Scene& scene, std::string_view name) {
    const Node* node = scene.find_node_by_name(name);
    if (node == nullptr) {
        return false;
    }
    const auto* canvas = node->get_component<ui::UiCanvas>();
    return canvas != nullptr && canvas->visible;
}

} // namespace

TEST_CASE("a fresh game is on the playing screen") {
    // The rules object has no opinion about presentation: a headless autoplay
    // run and every unit test drive a live game with no menu. The application is
    // what opens on the title, with show_title().
    breakout::BreakoutState state;
    CHECK(state.screen() == breakout::BreakoutState::Screen::Playing);
    CHECK(state.is_playing());
    CHECK(std::string(state.screen_name()) == "playing");
}

TEST_CASE("the title -> play -> game over -> title flow") {
    // The acceptance test for `slice-title-and-menus`, at the rules level: the
    // whole flow is one object with no scene and no engine.
    breakout::BreakoutState state(2, 1);

    // Title: a fresh board, no game running.
    state.show_title();
    CHECK(state.is_title());
    CHECK(std::string(state.screen_name()) == "title");
    CHECK_FALSE(state.is_over());

    // Play: the title's "press to play" edge resets the board and starts the
    // clock.
    state.start_game();
    CHECK(state.is_playing());
    CHECK(state.lives() == 1);
    CHECK(state.bricks_remaining() == 2);

    // Game over: the last life lost ends the game, and the screen follows the
    // rules once sync_screen() runs.
    state.ball_missed();
    REQUIRE(state.is_over());
    state.sync_screen();
    CHECK(state.screen() == breakout::BreakoutState::Screen::GameOver);
    CHECK(std::string(state.screen_name()) == "gameover");

    // Back to the title: the game-over screen's "press to continue" edge resets
    // the board and rebuilds it (the request is how the owner of the scene knows
    // the board it left is not fresh).
    state.return_to_title();
    CHECK(state.is_title());
    CHECK(state.restart_requested());
    CHECK(state.lives() == 1);
    CHECK(state.bricks_remaining() == 2);
}

TEST_CASE("pause freezes a live game and resume returns to play") {
    breakout::BreakoutState state;
    REQUIRE(state.is_playing());

    state.pause();
    CHECK(state.is_paused());
    CHECK(std::string(state.screen_name()) == "paused");
    CHECK_FALSE(state.is_over());

    state.resume();
    CHECK(state.is_playing());
}

TEST_CASE("pause and resume are no-ops on the wrong screen") {
    breakout::BreakoutState state(breakout::config::kBrickCount, 1);

    // Pausing the title does nothing: there is no clock to stop.
    state.show_title();
    state.pause();
    CHECK(state.is_title());

    // Resuming a game that is already playing does nothing.
    state.start_game();
    state.resume();
    CHECK(state.is_playing());

    // Pausing a finished game does nothing: the game-over screen is not a pause.
    state.ball_missed();
    state.sync_screen();
    REQUIRE(state.is_over());
    state.pause();
    CHECK(state.screen() == breakout::BreakoutState::Screen::GameOver);
}

TEST_CASE("sync_screen never reopens a title or a pause") {
    // sync_screen() reconciles the presentation with the rules; while the game is
    // still in progress it must leave the screen exactly where it is, so a title
    // or a pause is not silently un-done.
    breakout::BreakoutState state;
    state.show_title();
    state.sync_screen();
    CHECK(state.is_title());

    state.start_game();
    state.pause();
    state.sync_screen();
    CHECK(state.is_paused());
}

TEST_CASE("the field builder makes the three menu canvases, hidden") {
    Scene scene;
    breakout::build_breakout_scene(scene);

    // Each menu canvas exists with its label, and is hidden until MenuSystem
    // shows it, so a fresh scene draws only the field.
    const std::pair<std::string_view, std::string_view> menus[] = {
        {breakout::names::kTitleCanvas, breakout::names::kTitleLabel},
        {breakout::names::kPauseCanvas, breakout::names::kPauseLabel},
        {breakout::names::kGameOverCanvas, breakout::names::kGameOverLabel},
    };
    for (const auto& [canvas_name, label_name] : menus) {
        Node* canvas_node = scene.find_node_by_name(canvas_name);
        REQUIRE(canvas_node != nullptr);
        auto* canvas = canvas_node->get_component<ui::UiCanvas>();
        REQUIRE(canvas != nullptr);
        CHECK_FALSE(canvas->visible);
        // Above the HUD, so a menu covers the field and the counters.
        CHECK(canvas->layer > 10);

        Node* label_node = scene.find_node_by_name(label_name);
        REQUIRE(label_node != nullptr);
        auto* label = label_node->get_component<ui::UiElement>();
        REQUIRE(label != nullptr);
        CHECK_FALSE(label->text.empty());
        CHECK(label->anchor == ui::UiAnchor::MiddleCenter);
    }
}

TEST_CASE("the menu drives the shipped schedule through the whole flow") {
    // The integration acceptance test: the real scene and the real schedule run
    // title -> play -> game over -> title, and the canvases shown track the
    // screen. Nothing here re-implements the flow; it drives the shipped systems.
    Scene scene;
    breakout::build_breakout_scene(scene);
    SceneSchedule schedule;
    breakout::BreakoutState state;

    ActionMap actions;
    const breakout::BreakoutActions bindings = breakout::declare_breakout_actions(actions);
    static_cast<void>(breakout::install_breakout_systems(schedule, state, /*autoplay=*/false, bindings));

    InputState input;

    // The application opens on the title. One frame with no input settles the
    // canvases: the title is up, the HUD is down.
    state.show_title();
    run_screen_frame(schedule, scene, input, actions);
    CHECK(state.is_title());
    CHECK(canvas_visible(scene, breakout::names::kTitleCanvas));
    CHECK_FALSE(canvas_visible(scene, breakout::names::kHudCanvas));
    CHECK_FALSE(canvas_visible(scene, breakout::names::kGameOverCanvas));

    // Press "go" (Space, the confirm action): the game starts and the title gives
    // way to the HUD.
    input.on_key(Key::Space, true);
    run_screen_frame(schedule, scene, input, actions);
    CHECK(state.is_playing());
    CHECK_FALSE(canvas_visible(scene, breakout::names::kTitleCanvas));
    CHECK(canvas_visible(scene, breakout::names::kHudCanvas));

    // Lose every life: the screen becomes the game-over screen and its canvas is
    // shown instead of the HUD.
    input.on_key(Key::Space, false);
    run_screen_frame(schedule, scene, input, actions); // release the key
    for (int i = 0; i < breakout::config::kStartingLives; ++i) {
        state.ball_missed();
    }
    REQUIRE(state.is_over());
    run_screen_frame(schedule, scene, input, actions);
    CHECK(state.screen() == breakout::BreakoutState::Screen::GameOver);
    CHECK(canvas_visible(scene, breakout::names::kGameOverCanvas));
    CHECK_FALSE(canvas_visible(scene, breakout::names::kHudCanvas));

    // Press "go" again: back to the title, with a fresh board.
    input.on_key(Key::Space, true);
    run_screen_frame(schedule, scene, input, actions);
    CHECK(state.is_title());
    CHECK(canvas_visible(scene, breakout::names::kTitleCanvas));
    CHECK_FALSE(canvas_visible(scene, breakout::names::kGameOverCanvas));
    CHECK(state.lives() == breakout::config::kStartingLives);
}

TEST_CASE("the pause key freezes and unfreezes a live game") {
    Scene scene;
    breakout::build_breakout_scene(scene);
    SceneSchedule schedule;
    breakout::BreakoutState state;

    ActionMap actions;
    const breakout::BreakoutActions bindings = breakout::declare_breakout_actions(actions);
    static_cast<void>(breakout::install_breakout_systems(schedule, state, /*autoplay=*/false, bindings));

    InputState input;
    state.start_game();

    // P pauses: the pause canvas is shown, the HUD hidden, and the field freezes.
    input.on_key(Key::P, true);
    run_screen_frame(schedule, scene, input, actions);
    CHECK(state.is_paused());
    CHECK(canvas_visible(scene, breakout::names::kPauseCanvas));
    CHECK_FALSE(canvas_visible(scene, breakout::names::kHudCanvas));

    // The ball is frozen: the node does not move while the game is paused.
    Node* ball = scene.find_node_by_name(breakout::names::kBall);
    REQUIRE(ball != nullptr);
    const Vec3 paused_position = ball->transform().position;
    input.on_key(Key::P, false);
    for (int i = 0; i < 5; ++i) {
        run_screen_frame(schedule, scene, input, actions);
    }
    CHECK(state.is_paused());
    CHECK(ball->transform().position.x == doctest::Approx(paused_position.x));
    CHECK(ball->transform().position.y == doctest::Approx(paused_position.y));

    // P again resumes: back to play, the HUD returns.
    input.on_key(Key::P, true);
    run_screen_frame(schedule, scene, input, actions);
    CHECK(state.is_playing());
    CHECK_FALSE(canvas_visible(scene, breakout::names::kPauseCanvas));
    CHECK(canvas_visible(scene, breakout::names::kHudCanvas));
}

TEST_CASE("the paddle and the ball are idle on the title screen") {
    // The title is not just a drawing over a live game: the gameplay systems are
    // frozen, so a press cannot steer a board that is not being played.
    Scene scene;
    breakout::build_breakout_scene(scene);
    SceneSchedule schedule;
    breakout::BreakoutState state;

    ActionMap actions;
    const breakout::BreakoutActions bindings = breakout::declare_breakout_actions(actions);
    static_cast<void>(breakout::install_breakout_systems(schedule, state, /*autoplay=*/false, bindings));

    InputState input;
    state.show_title();

    Node* paddle = scene.find_node_by_name(breakout::names::kPaddle);
    Node* ball = scene.find_node_by_name(breakout::names::kBall);
    REQUIRE(paddle != nullptr);
    REQUIRE(ball != nullptr);
    const f32 paddle_x = paddle->transform().position.x;
    const f32 ball_y = ball->transform().position.y;

    // Hold "move left" across several frames: the paddle does not respond on the
    // title, and the game stays on the title. (The launch/confirm key is not
    // pressed here -- it is what *leaves* the title -- so the field is genuinely
    // idle rather than a frame away from starting.)
    input.on_key(Key::Left, true);
    for (int i = 0; i < 10; ++i) {
        run_screen_frame(schedule, scene, input, actions);
    }
    CHECK(state.is_title());
    CHECK(paddle->transform().position.x == doctest::Approx(paddle_x));
    CHECK(ball->transform().position.y == doctest::Approx(ball_y));
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

// ---------------------------------------------------------------------------
// The ball must stay in the field and keep its bounce, at every frame rate the
// game can be run at. Both of these regressions shipped: a ball that tunnelled
// through a wall was gone for good, and a ball that lost its vertical speed to
// wall friction ended up travelling dead horizontal forever, which looks like
// the game has hung.
// ---------------------------------------------------------------------------

namespace {

// The ball's centre may reach the field's inner face plus its radius; anything
// past the outer face of the wall is out of the field.
constexpr f32 kOuterX = breakout::config::kFieldHalfWidth + breakout::config::kWallThickness;
constexpr f32 kOuterY = breakout::config::kFieldHalfHeight + breakout::config::kWallThickness;

// Launches the ball from the field centre at `angle` with no paddle and no
// bricks, so the only thing that can affect it is the walls, and returns the
// furthest the ball reached and how much vertical motion it kept.
//
// The bottom of the field is open, so the ball eventually falls out and the
// game-state system respawns it at rest. The loop re-launches it whenever that
// happens, which keeps the ball bouncing off the walls for the whole run instead
// of stopping after the first miss.
struct WallRun {
    f32 max_x = 0.0f;
    f32 max_y = 0.0f;
    f32 min_vertical_ratio = 1.0f;
    int wall_hits = 0;
};

WallRun run_against_walls(f32 angle, f32 delta, int steps) {
    Scene scene;
    breakout::build_breakout_scene(scene);
    SceneSchedule schedule;
    breakout::BreakoutState state(breakout::config::kBrickCount, 99);
    static_cast<void>(breakout::install_breakout_systems(schedule, state, /*autoplay=*/false));

    Node* ball = scene.find_node_by_name(breakout::names::kBall);
    auto* body = ball->get_component<physics2d::RigidBody2D>();

    // Remove the paddle and every brick: this test is about the walls.
    if (Node* paddle = scene.find_node_by_name(breakout::names::kPaddle); paddle != nullptr) {
        if (Node* parent = paddle->parent(); parent != nullptr) {
            static_cast<void>(parent->remove_child(paddle));
        }
    }
    std::vector<Node*> bricks;
    scene.for_each_node([&](Node& node) {
        if (node.name() == breakout::names::kBrick) {
            bricks.push_back(&node);
        }
    });
    for (Node* brick : bricks) {
        if (Node* parent = brick->parent(); parent != nullptr) {
            static_cast<void>(parent->remove_child(brick));
        }
    }

    const Vec2 launch{std::sin(angle), std::cos(angle)};
    ball->transform().position = Vec3{0.0f, 0.0f, 0.0f};
    body->velocity = launch * breakout::config::kLaunchSpeed;
    body->wake();

    WallRun run;
    f32 last_vx = body->velocity.x;
    for (int i = 0; i < steps; ++i) {
        schedule.update(step(delta), scene);
        const Vec3 p = ball->transform().position;
        const Vec2 v = body->velocity;
        const f32 speed = v.length();

        // Respawned at rest (the ball fell out): relaunch it so the run keeps
        // exercising the walls.
        if (speed < 1.0f) {
            ball->transform().position = Vec3{0.0f, 0.0f, 0.0f};
            body->velocity = launch * breakout::config::kLaunchSpeed;
            body->wake();
            last_vx = body->velocity.x;
            continue;
        }

        run.max_x = std::max(run.max_x, std::abs(p.x));
        run.max_y = std::max(run.max_y, p.y);
        if (v.x * last_vx < 0.0f) {
            ++run.wall_hits;
            last_vx = v.x;
        }
        run.min_vertical_ratio = std::min(run.min_vertical_ratio, std::abs(v.y) / speed);
    }
    return run;
}

} // namespace

TEST_CASE("the ball never leaves the field through a wall at any frame rate") {
    // A wall is thin (15 units) and the ball moves up to ~12 units a step at
    // 60 Hz, so a fast ball can be inside a wall on the frame the contact is
    // detected. The narrow phase must then push it back out the way it came; if
    // the inside-case normal is inverted it is pushed out the far side instead
    // and the ball is gone.
    for (f32 delta : {1.0f / 60.0f, 1.0f / 30.0f}) {
        for (int d = 0; d < 16; ++d) {
            const f32 angle = static_cast<f32>(d) * 6.2831853f / 16.0f;
            const WallRun run = run_against_walls(angle, delta, 1500);
            CHECK(run.max_x <= kOuterX + 0.5f);
            CHECK(run.max_y <= kOuterY + 0.5f);
        }
    }
}

TEST_CASE("a ball with a vertical component keeps bouncing rather than going flat") {
    // The wall material must not steal the ball's tangential (vertical) speed.
    // With friction combining as a maximum, the ball's authored friction 0 was
    // overridden by the wall's default and the vertical component decayed to
    // zero over a handful of hits, leaving the ball running side to side
    // forever.
    for (f32 delta : {1.0f / 60.0f, 1.0f / 30.0f}) {
        // Start well off horizontal so there is vertical motion to lose.
        const WallRun run = run_against_walls(0.6f, delta, 1500);
        REQUIRE(run.wall_hits > 4);
        CHECK(run.min_vertical_ratio > 0.05f);
    }
}

// ---------------------------------------------------------------------------
// Persistence: the best score through the storage seam
// (docs/PLAN.md, `slice-persistence`).
//
// The seam itself is tested in test_user_data.cpp; here it is the *game* that
// is under test: that a played game records a best score, saves it on the
// game-over edge, and that a fresh game on the same store reads it back -- the
// reload the queue item is about. The store is a real desktop store pointed at a
// temporary directory, so this exercises the shipped backend and not a mock.
// ---------------------------------------------------------------------------

namespace {

// A unique temporary directory per case, removed on destruction. The same shape
// test_user_data.cpp uses, so the two suites clean up identically.
class ScoreDir {
public:
    explicit ScoreDir(std::string name) {
        path_ = std::filesystem::temp_directory_path() / ("fe_breakout_score_" + name);
        std::filesystem::remove_all(path_);
    }
    ~ScoreDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    [[nodiscard]] std::string str() const { return path_.string(); }

private:
    std::filesystem::path path_;
};

// A desktop store in `dir`, so the game saves to a real file in a temp directory
// rather than the developer's home directory.
[[nodiscard]] std::unique_ptr<UserData> score_store(const ScoreDir& dir) {
    UserData::Desc desc;
    desc.app_id = breakout::config::kAppId;
    desc.root_override = dir.str();
    return UserData::create(desc);
}

// One frame of the shipped schedule with the store attached, exactly as
// Application does: the context carries the store so the persistence system
// finds it there and never reaches for the engine.
void run_persist_frame(SceneSchedule& schedule, Scene& scene, UserData& store) {
    FrameContext context = step(1.0f / 60.0f);
    context.user_data = &store;
    schedule.update(context, scene);
}

} // namespace

TEST_CASE("the best score is a record that survives reset") {
    // The rules half, with no engine: a fresh game has no best score, playing
    // raises it, and a reset (a new game) keeps it.
    breakout::BreakoutState state(1, 3, /*level_count=*/1);

    CHECK_FALSE(state.has_best_score());
    CHECK(state.best_score() == 0);

    state.brick_hit();
    CHECK(state.record_best_score());
    CHECK(state.has_best_score());
    CHECK(state.best_score() == breakout::config::kPointsPerBrick + breakout::config::kLevelClearBonus);

    // A fresh game does not lower the record, and a stale save cannot either.
    state.reset();
    CHECK(state.best_score() == breakout::config::kPointsPerBrick + breakout::config::kLevelClearBonus);
    state.set_best_score(5);
    CHECK(state.best_score() == breakout::config::kPointsPerBrick + breakout::config::kLevelClearBonus);
}

TEST_CASE("the title does not record a best score") {
    // A fresh game on the title has a score of zero that is not a result, so the
    // HUD must stay blank rather than claim a best of zero.
    breakout::BreakoutState state;
    state.show_title();
    CHECK_FALSE(state.has_best_score());
}

TEST_CASE("a played game saves its best score and a fresh game reads it back") {
    // The acceptance test for `slice-persistence` on desktop: the shipped
    // schedule plays a game to the game-over screen through a real store, and a
    // *fresh* state on the same store reads the score back. That second read is
    // the reload the queue item is about -- it is a different BreakoutState, so
    // nothing in memory carries the value across.
    ScoreDir dir("roundtrip");
    auto store = score_store(dir);
    REQUIRE(store != nullptr);
    int earned = 0;

    {
        Scene scene;
        breakout::build_breakout_scene(scene);
        SceneSchedule schedule;
        breakout::BreakoutState state(2, 1, /*level_count=*/1);
        static_cast<void>(breakout::install_breakout_systems(schedule, state, /*autoplay=*/false));

        state.start_game();
        state.brick_hit(); // score some points
        earned = state.score();
        REQUIRE(earned > 0);

        // Lose the last life: the game ends, and the next frame is the game-over
        // edge the persistence system writes on.
        state.ball_missed();
        REQUIRE(state.is_over());
        run_persist_frame(schedule, scene, *store);
        CHECK(state.has_best_score());
        CHECK(state.best_score() == earned);
    }

    // A fresh state and schedule on the same store -- a reload.
    Scene scene;
    breakout::build_breakout_scene(scene);
    SceneSchedule schedule;
    breakout::BreakoutState state(2, 1, /*level_count=*/1);
    static_cast<void>(breakout::install_breakout_systems(schedule, state, /*autoplay=*/false));
    CHECK_FALSE(state.has_best_score()); // nothing in memory yet
    run_persist_frame(schedule, scene, *store);
    CHECK(state.has_best_score());
    // The reload sees exactly what the first game saved, not a value this test
    // recomputed -- the two would agree by construction and prove nothing.
    CHECK(state.best_score() == earned);
}

TEST_CASE("the HUD shows the best score once there is one") {
    ScoreDir dir("hud");
    auto store = score_store(dir);
    REQUIRE(store != nullptr);
    // Pre-seed a save so a fresh game starts with a record to show.
    REQUIRE_FALSE(store->write(breakout::config::kBestScoreKey, std::string_view("1230")));

    Scene scene;
    breakout::build_breakout_scene(scene);
    SceneSchedule schedule;
    breakout::BreakoutState state(2, 3, /*level_count=*/1);
    static_cast<void>(breakout::install_breakout_systems(schedule, state, /*autoplay=*/false));

    Node* best_node = scene.find_node_by_name(breakout::names::kBestScoreLabel);
    REQUIRE(best_node != nullptr);
    auto* best_element = best_node->get_component<ui::UiElement>();
    REQUIRE(best_element != nullptr);

    // Before the first frame the label is blank: a first run shows no best score.
    CHECK(best_element->text.empty());

    state.show_title();
    run_persist_frame(schedule, scene, *store);
    // The title loads the save and shows it, even though no game is running.
    CHECK(state.best_score() == 1230);
    CHECK(best_element->text == "BEST 1230");
}

TEST_CASE("a run with no store tracks the best score in memory and persists nothing") {
    // The context's store is nullable. A run handed none must still play: the
    // best score is tracked in memory (so the HUD is right), but nothing reaches
    // a disk. That is what lets a headless autoplay run avoid touching a real
    // save directory.
    ScoreDir dir("nostore");

    Scene scene;
    breakout::build_breakout_scene(scene);
    SceneSchedule schedule;
    breakout::BreakoutState state(1, 1, /*level_count=*/1);
    static_cast<void>(breakout::install_breakout_systems(schedule, state, /*autoplay=*/false));

    state.start_game();
    state.brick_hit();
    const int earned = state.score();
    state.ball_missed();
    REQUIRE(state.is_over());
    // No store on the context.
    schedule.update(step(1.0f / 60.0f), scene);

    // The record is known in memory...
    CHECK(state.has_best_score());
    CHECK(state.best_score() == earned);

    // ...and a store in the directory the game would have used is empty: nothing
    // was written, which is the point of a run with no application id.
    auto store = score_store(dir);
    REQUIRE(store != nullptr);
    CHECK_FALSE(store->contains(breakout::config::kBestScoreKey));
}

// ---------------------------------------------------------------------------
// Settings: volume and controls, persisted
// (docs/PLAN.md, `slice-settings`).
//
// Two layers again. The rules half is Settings on its own -- the clamping and the
// text round-trip, with no engine. The shipped half drives the real settings
// screen and the real schedule, and proves a change survives a *reload*: a fresh
// state on the same store reads the changed value back.
// ---------------------------------------------------------------------------

namespace {

// A store whose root is a temp directory, for the settings tests. The same shape
// score_store() uses; a settings save and a best-score save are independent keys,
// so one store can hold both.
[[nodiscard]] std::unique_ptr<UserData> settings_store(const ScoreDir& dir) {
    return score_store(dir);
}

// The null RHI device, kept for the process's lifetime. The atlas below holds a
// `TextureRef` the device owns, so the device must outlive it -- a device created
// inside the font's initializer would be destroyed with the texture still holding
// its handle.
[[nodiscard]] rhi::Device& null_device() {
    static std::unique_ptr<rhi::Device> device = [] {
        auto created = rhi::create_null_device();
        REQUIRE(created.ok());
        return std::move(created.value());
    }();
    return *device;
}

// A `Font` with a fixed advance for every printable byte, so a menu button's text
// has a non-zero extent and therefore a real hit-testable box. The shipped font
// loader is exercised by the text tests; here the point is a click landing where
// the button draws, which needs only a measurable width. Built on the same null
// RHI device the UI tests use.
[[nodiscard]] std::shared_ptr<Font> test_font() {
    static std::shared_ptr<Font> font = [] {
        rhi::TextureDesc desc;
        desc.name = "breakout-test-atlas";
        desc.width = 8;
        desc.height = 8;
        std::vector<u8> pixels(8 * 8 * 4, 255);
        desc.pixels = pixels.data();
        auto texture = null_device().create_texture(desc);
        REQUIRE(texture.ok());
        auto atlas = std::make_shared<Texture2D>("tex:breakout-test.png", texture.value());

        std::vector<Font::Glyph> glyphs(128);
        for (Font::Glyph& glyph : glyphs) {
            glyph.uv = Vec4{0.0f, 0.0f, 0.5f, 0.5f};
            glyph.advance = 0.5f;
            glyph.size = Vec2{0.5f, 1.0f};
        }
        return std::make_shared<Font>("breakout-test.fefont", atlas, 1.25f, std::move(glyphs));
    }();
    return font;
}

// The assets a test needs for the menus to be clickable: just the font. The
// textures stay absent, which the builder treats as optional.
[[nodiscard]] breakout::SceneAssets test_assets() {
    breakout::SceneAssets assets;
    assets.font = test_font();
    return assets;
}

// Runs one frame of the shipped schedule with the given context fields, *without*
// touching the input snapshot. The caller has already advanced the input edges
// (begin_frame + the on_* calls) and evaluated the actions, so a press or release
// it just injected is still live for this frame -- which is exactly what
// UiInputSystem reads.
void run_settings_frame_after_input(SceneSchedule& schedule, Scene& scene, InputState& input, ActionMap& actions,
                                    UserData& store) {
    actions.update(input, Vec2{1280.0f, 720.0f});
    FrameContext context = step(1.0f / 60.0f);
    context.input = &input;
    context.actions = &actions;
    context.viewport = Vec2{1280.0f, 720.0f};
    context.user_data = &store;
    schedule.update(context, scene);
}

// One idle frame: advance the input edges (no new events) and run the schedule.
void run_settings_frame(SceneSchedule& schedule, Scene& scene, InputState& input, ActionMap& actions, UserData& store) {
    input.begin_frame();
    run_settings_frame_after_input(schedule, scene, input, actions, store);
}

// The viewport-space centre of a named UI element, from the shipped layout, so a
// test clicks where the button actually draws. Returns false when the element is
// not laid out this frame (e.g. its canvas is hidden).
[[nodiscard]] bool element_centre(const Scene& scene, std::string_view name, Vec2& out) {
    ui::UiLayoutResult layout;
    ui::layout(scene, 1280, 720, layout);
    for (const ui::UiResolved& resolved : layout) {
        if (resolved.node != nullptr && resolved.node->name() == name) {
            out = resolved.rect.center();
            return true;
        }
    }
    return false;
}

// A full press-and-release on a named element: move the pointer to its centre,
// press on one frame, release on the next, running the schedule each time so the
// menu sees the click. The two-frame shape is what UiInputSystem needs to call a
// press and a release on the same element a click.
void click_element(SceneSchedule& schedule, Scene& scene, InputState& input, ActionMap& actions, UserData& store,
                   std::string_view name) {
    Vec2 centre{};
    REQUIRE(element_centre(scene, name, centre));

    // Frame 1: the pointer arrives and goes down.
    input.begin_frame();
    input.on_mouse_move(centre.x, centre.y);
    input.on_mouse_button(MouseButton::Left, true);
    run_settings_frame_after_input(schedule, scene, input, actions, store);

    // Frame 2: the pointer comes up over the element: a click.
    input.begin_frame();
    input.on_mouse_button(MouseButton::Left, false);
    run_settings_frame_after_input(schedule, scene, input, actions, store);
}

} // namespace

TEST_CASE("a setting clamps to its range") {
    breakout::Settings settings;
    CHECK(settings.volume == breakout::config::kDefaultVolume);
    CHECK(settings.paddle_speed == breakout::config::kDefaultPaddleSpeed);

    settings.set_volume(5.0f);
    CHECK(settings.volume == breakout::config::kMaxVolume);
    settings.set_volume(-3.0f);
    CHECK(settings.volume == breakout::config::kMinVolume);

    settings.set_paddle_speed(100000.0f);
    CHECK(settings.paddle_speed == breakout::config::kMaxPaddleSpeed);
    settings.set_paddle_speed(0.0f);
    CHECK(settings.paddle_speed == breakout::config::kMinPaddleSpeed);
}

TEST_CASE("a setting steps and sticks to its end") {
    breakout::Settings settings;
    settings.adjust_volume(-1);
    CHECK(settings.volume == doctest::Approx(breakout::config::kDefaultVolume - breakout::config::kVolumeStep));
    // Far past the bottom: it sticks to the minimum rather than going negative.
    for (int i = 0; i < 100; ++i) {
        settings.adjust_volume(-1);
    }
    CHECK(settings.volume == breakout::config::kMinVolume);
}

TEST_CASE("settings round-trip through their text form") {
    breakout::Settings original;
    original.set_volume(0.4f);
    original.set_paddle_speed(1020.0f);

    breakout::Settings parsed;
    REQUIRE(breakout::parse_settings(breakout::serialize_settings(original), parsed));
    CHECK(parsed.volume == doctest::Approx(0.4f));
    CHECK(parsed.paddle_speed == doctest::Approx(1020.0f));
}

TEST_CASE("an unknown or malformed settings line is skipped, not fatal") {
    // A save from a newer build, or a corrupt one, must not stop the game: a
    // known key still loads and everything else is ignored.
    breakout::Settings parsed;
    parsed.set_volume(0.5f);
    const std::string text = "volume=0.25\nfuture_setting=42\ngarbage\npaddle_speed=abc\n";
    REQUIRE(breakout::parse_settings(text, parsed));
    CHECK(parsed.volume == doctest::Approx(0.25f));
    // The unreadable paddle_speed line left the current value alone.
    CHECK(parsed.paddle_speed == breakout::config::kDefaultPaddleSpeed);

    // A text with nothing recognisable reports "nothing read".
    breakout::Settings untouched;
    CHECK_FALSE(breakout::parse_settings("nonsense=here\n", untouched));
}

TEST_CASE("the settings screen opens from the title and returns to it") {
    breakout::BreakoutState state;
    state.show_title();
    REQUIRE(state.is_title());
    state.open_settings();
    CHECK(state.is_settings());
    CHECK(std::string(state.screen_name()) == "settings");
    state.close_settings();
    CHECK(state.is_title());
}

TEST_CASE("the settings screen returns to the pause it was opened from") {
    // Opening from a pause and closing must not silently abandon the paused game.
    breakout::BreakoutState state;
    state.start_game();
    state.pause();
    REQUIRE(state.is_paused());
    state.open_settings();
    CHECK(state.is_settings());
    state.close_settings();
    CHECK(state.is_paused());
}

TEST_CASE("the settings screen cannot be opened mid-rally or from game over") {
    breakout::BreakoutState state;
    state.start_game();
    state.open_settings();
    CHECK(state.is_playing()); // no-op while playing

    state.ball_missed();
    state.ball_missed();
    state.ball_missed();
    state.sync_screen();
    REQUIRE(state.screen() == breakout::BreakoutState::Screen::GameOver);
    state.open_settings();
    CHECK(state.screen() == breakout::BreakoutState::Screen::GameOver); // no-op from game over
}

TEST_CASE("the field builder makes the settings canvas and its controls, hidden") {
    Scene scene;
    breakout::build_breakout_scene(scene, test_assets());

    Node* canvas_node = scene.find_node_by_name(breakout::names::kSettingsCanvas);
    REQUIRE(canvas_node != nullptr);
    auto* canvas = canvas_node->get_component<ui::UiCanvas>();
    REQUIRE(canvas != nullptr);
    CHECK_FALSE(canvas->visible);
    CHECK(canvas->layer > 10);

    // Every control exists and is a hit-testable UiElement, so a tap and a click
    // both reach it -- the touch-friendly menu the queue item asks for.
    const std::string_view controls[] = {
        breakout::names::kSettingsTitleLabel,  breakout::names::kSettingsVolumeDown, breakout::names::kSettingsVolumeUp,
        breakout::names::kSettingsVolumeValue, breakout::names::kSettingsSpeedDown,  breakout::names::kSettingsSpeedUp,
        breakout::names::kSettingsSpeedValue,  breakout::names::kSettingsBack,
    };
    for (std::string_view name : controls) {
        Node* node = scene.find_node_by_name(name);
        REQUIRE(node != nullptr);
        auto* element = node->get_component<ui::UiElement>();
        REQUIRE(element != nullptr);
        CHECK(element->interactable);
    }

    // The settings button on the title and the pause screens, so the screen is
    // reachable by a tap rather than only by a key.
    CHECK(scene.find_node_by_name(breakout::names::kSettingsButton) != nullptr);
    CHECK(scene.find_node_by_name(breakout::names::kPauseSettingsButton) != nullptr);
}

TEST_CASE("a setting changed on the menu is saved and a fresh game reads it back") {
    // The acceptance test for `slice-settings`: change a setting through the
    // shipped menu, then a *fresh* state on the same store reads it back. The
    // second state is the reload the queue item is about -- nothing in memory
    // carries the value across.
    ScoreDir dir("settings_roundtrip");
    auto store = settings_store(dir);
    REQUIRE(store != nullptr);

    {
        Scene scene;
        breakout::build_breakout_scene(scene, test_assets());
        SceneSchedule schedule;
        breakout::BreakoutState state;
        ActionMap actions;
        const breakout::BreakoutActions bindings = breakout::declare_breakout_actions(actions);
        static_cast<void>(breakout::install_breakout_systems(schedule, state, /*autoplay=*/false, bindings));

        InputState input;
        state.show_title();

        // One priming frame: the settings system reads the (empty) store and
        // establishes its baseline, exactly as the application's first frame does.
        run_settings_frame(schedule, scene, input, actions, *store);
        const f32 start_volume = state.settings().volume;

        // Open the settings screen by clicking the title's SETTINGS button.
        click_element(schedule, scene, input, actions, *store, breakout::names::kSettingsButton);
        REQUIRE(state.is_settings());

        // Turn the volume down one step and let the settings system save it.
        click_element(schedule, scene, input, actions, *store, breakout::names::kSettingsVolumeDown);
        run_settings_frame(schedule, scene, input, actions, *store);
        CHECK(state.settings().volume < start_volume);
    }

    // A fresh state and schedule on the same store -- a reload.
    Scene scene;
    breakout::build_breakout_scene(scene, test_assets());
    SceneSchedule schedule;
    breakout::BreakoutState state;
    ActionMap actions;
    const breakout::BreakoutActions bindings = breakout::declare_breakout_actions(actions);
    static_cast<void>(breakout::install_breakout_systems(schedule, state, /*autoplay=*/false, bindings));
    InputState input;
    CHECK(state.settings().volume == breakout::config::kDefaultVolume); // nothing in memory yet
    run_settings_frame(schedule, scene, input, actions, *store);
    CHECK(state.settings().volume == doctest::Approx(breakout::config::kDefaultVolume - breakout::config::kVolumeStep));
}

TEST_CASE("the settings screen shows the values and reflects a click") {
    ScoreDir dir("settings_labels");
    auto store = settings_store(dir);
    REQUIRE(store != nullptr);

    Scene scene;
    breakout::build_breakout_scene(scene, test_assets());
    SceneSchedule schedule;
    breakout::BreakoutState state;
    ActionMap actions;
    const breakout::BreakoutActions bindings = breakout::declare_breakout_actions(actions);
    static_cast<void>(breakout::install_breakout_systems(schedule, state, /*autoplay=*/false, bindings));

    InputState input;
    state.show_title();
    // One priming frame so MenuSystem shows the title canvas -- a click can only
    // land on a laid-out element, and nothing is laid out until a frame has run.
    run_settings_frame(schedule, scene, input, actions, *store);
    click_element(schedule, scene, input, actions, *store, breakout::names::kSettingsButton);
    REQUIRE(state.is_settings());

    // The readouts match the settings in force.
    Node* volume_value = scene.find_node_by_name(breakout::names::kSettingsVolumeValue);
    REQUIRE(volume_value != nullptr);
    auto* volume_element = volume_value->get_component<ui::UiElement>();
    REQUIRE(volume_element != nullptr);
    CHECK(volume_element->text == "VOLUME 100%");

    // A click on the "+"/"-" button changes the value and the readout together.
    click_element(schedule, scene, input, actions, *store, breakout::names::kSettingsVolumeDown);
    CHECK(volume_element->text == "VOLUME 90%");

    // "BACK" returns to the title and hides the settings canvas.
    click_element(schedule, scene, input, actions, *store, breakout::names::kSettingsBack);
    CHECK(state.is_title());
    CHECK_FALSE(canvas_visible(scene, breakout::names::kSettingsCanvas));
    CHECK(canvas_visible(scene, breakout::names::kTitleCanvas));
}

TEST_CASE("the volume setting drives the audio master gain") {
    // The setting is what the player hears: the settings system mirrors it onto
    // the mixer's master gain, so a change is audible on the same frame.
    ScoreDir dir("settings_audio");
    auto store = settings_store(dir);
    REQUIRE(store != nullptr);

    Scene scene;
    breakout::build_breakout_scene(scene, test_assets());
    SceneSchedule schedule;
    breakout::BreakoutState state;
    static_cast<void>(breakout::install_breakout_systems(schedule, state, /*autoplay=*/false));

    audio::AudioSystem audio(nullptr); // no device: the gain is still set
    auto* settings = static_cast<breakout::SettingsSystem*>(schedule.find("SettingsSystem"));
    REQUIRE(settings != nullptr);
    settings->set_audio(&audio);

    state.settings().set_volume(0.5f);
    FrameContext context = step(1.0f / 60.0f);
    context.user_data = store.get();
    schedule.update(context, scene);
    CHECK(audio.master_gain() == doctest::Approx(0.5f));
}

TEST_CASE("the paddle speed setting drives the paddle") {
    // The control setting is not merely stored: the paddle moves at the speed the
    // player chose, so a slower setting moves it less in the same number of frames.
    const auto distance_over_frames = [](f32 speed) {
        Scene scene;
        breakout::build_breakout_scene(scene, test_assets());
        SceneSchedule schedule;
        breakout::BreakoutState state;
        ActionMap actions;
        const breakout::BreakoutActions bindings = breakout::declare_breakout_actions(actions);
        static_cast<void>(breakout::install_breakout_systems(schedule, state, /*autoplay=*/false, bindings));
        state.start_game();
        state.settings().set_paddle_speed(speed);

        InputState input;
        input.on_key(Key::Left, true);
        Node* paddle = scene.find_node_by_name(breakout::names::kPaddle);
        const f32 start_x = paddle->transform().position.x;
        for (int i = 0; i < 5; ++i) {
            input.begin_frame();
            actions.update(input, Vec2{1280.0f, 720.0f});
            FrameContext context = step(1.0f / 60.0f);
            context.input = &input;
            context.actions = &actions;
            context.viewport = Vec2{1280.0f, 720.0f};
            schedule.update(context, scene);
        }
        return start_x - paddle->transform().position.x;
    };

    const f32 slow = distance_over_frames(breakout::config::kMinPaddleSpeed);
    const f32 fast = distance_over_frames(breakout::config::kMaxPaddleSpeed);
    CHECK(slow > 0.0f);
    CHECK(fast > slow);
}

TEST_CASE("a run with no store keeps settings in memory and persists nothing") {
    // The context's store is nullable. A run handed none must still apply a
    // setting (the game is playable) but write nothing to disk, which is what lets
    // a headless autoplay run avoid touching a real save directory.
    ScoreDir dir("settings_nostore");

    Scene scene;
    breakout::build_breakout_scene(scene, test_assets());
    SceneSchedule schedule;
    breakout::BreakoutState state;
    static_cast<void>(breakout::install_breakout_systems(schedule, state, /*autoplay=*/false));

    state.settings().set_volume(0.3f);
    // No store on the context.
    schedule.update(step(1.0f / 60.0f), scene);

    // The change is in force...
    CHECK(state.settings().volume == doctest::Approx(0.3f));

    // ...and nothing was written to the directory the game would have used.
    auto store = settings_store(dir);
    REQUIRE(store != nullptr);
    CHECK_FALSE(store->contains(breakout::config::kSettingsKey));
}
