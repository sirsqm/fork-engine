// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Types.h"

// The breakout game's rules and geometry.
//
// Everything in this header is pure: no scene, no physics, no GPU, no
// resources. That is deliberate. The rules a game must get right (score a brick,
// lose a life, win when the field is clear, lose when the lives run out) are the
// part that is worth testing directly, so they live in a class the tests can
// construct and drive with no engine running at all. The systems in
// breakout_systems.h are the only things that touch the scene; they read and
// write this state.

namespace breakout {

// The playfield geometry, shared by the scene builder (main.cpp) and the
// gameplay systems so the two never disagree about where the field ends.
//
// The authored scene (scenes/breakout.fescene) uses these same numbers. They are
// the single source of truth for the rules; the scene file is a serialized copy
// of the same layout, which is what makes the file load path and the code
// fallback build an identical field.
namespace config {

inline constexpr fe::f32 kFieldHalfWidth = 768.0f;
inline constexpr fe::f32 kFieldHalfHeight = 480.0f;
inline constexpr fe::f32 kWallThickness = 15.0f;

// The box the camera fits into the viewport: the field plus the walls that
// outline it. The camera scales this box to fit whatever viewport it is given,
// so the whole field -- paddle included, which sits below the field's own
// half-height -- is visible at any window size. Fitting the field alone would
// clip the paddle off the bottom on a short viewport.
inline constexpr fe::f32 kViewHalfWidth = kFieldHalfWidth + kWallThickness * 2.0f;
inline constexpr fe::f32 kViewHalfHeight = kFieldHalfHeight + kWallThickness * 2.0f;

inline constexpr fe::f32 kPaddleHalfWidth = 90.0f;
inline constexpr fe::f32 kPaddleHalfHeight = 14.0f;
inline constexpr fe::f32 kPaddleY = -380.0f;

inline constexpr fe::f32 kBallRadius = 13.0f;
inline constexpr fe::f32 kBallStartX = 0.0f;
inline constexpr fe::f32 kBallStartY = -280.0f;

inline constexpr fe::f32 kBrickHalfWidth = 44.0f;
inline constexpr fe::f32 kBrickHalfHeight = 16.0f;
inline constexpr fe::f32 kBrickPitchX = 98.0f;
inline constexpr fe::f32 kBrickPitchY = 42.0f;
inline constexpr int kBrickCols = 11;
inline constexpr int kBrickRows = 5;
inline constexpr fe::f32 kBrickFirstX = -490.0f; // centre of column 0
inline constexpr fe::f32 kBrickFirstY = 300.0f;  // centre of row 0

// Launch: up and a little to the side, so the ball never starts perfectly
// vertical (a vertical ball against a flat paddle would loop forever).
inline constexpr fe::f32 kLaunchSpeed = 700.0f;
inline constexpr fe::f32 kLaunchAngle = 0.38f; // radians off vertical
inline constexpr fe::f32 kMinBallSpeed = 0.6f * kLaunchSpeed;
inline constexpr fe::f32 kMaxBallSpeed = 1.4f * kLaunchSpeed;

// Levels (`slice-scoring-and-progression`). The board is cleared once per level
// and the difficulty rises with the level, so there is a reason to keep playing
// after the first clear. The lever is the ball's speed: a faster ball leaves the
// player less time to react, which is the difficulty a breakout player feels,
// and it needs no new scene data — the layout is the same board at a higher
// pace.
//
// The gain is a multiplier per level and the result is clamped to
// `kMaxBallSpeed`, so the ball can never outrun the solver (a ball that moves
// more than its own radius per step would tunnel through a brick). With the
// shipped values the top level lands under the clamp.
inline constexpr int kLevelCount = 3;
inline constexpr fe::f32 kLevelSpeedGain = 0.15f; // +15% ball speed per level
inline constexpr int kLevelClearBonus = 100;      // score for clearing a board

// How far off vertical the ball can be sent by the paddle. A hit on the paddle's
// centre sends the ball straight up; a hit on an edge sends it at this angle.
// Without this the ball only ever mirrors its incoming angle off a flat paddle,
// which makes a vertical shot loop forever - classic breakout needs the paddle
// to aim, and so does this one.
inline constexpr fe::f32 kMaxBounceAngle = 1.05f; // ~60 degrees

inline constexpr int kStartingLives = 3;
inline constexpr int kPointsPerBrick = 10;
inline constexpr int kBrickCount = kBrickCols * kBrickRows;

// The bottom of the field is open: the ball is missed once it falls past it.
inline constexpr fe::f32 kFieldBottomY = -kFieldHalfHeight;

// The heads-up display. The score is pinned to the top-left corner and the lives
// to the top-right, both inside the walls so the field's own sprites never draw
// over them. The text is drawn in engine units, so it scales with the camera fit
// like everything else in the field.
inline constexpr fe::f32 kHudTextSize = 44.0f;
inline constexpr fe::f32 kHudMargin = 24.0f;
inline constexpr fe::f32 kHudTopY = kFieldHalfHeight - kHudMargin;
inline constexpr fe::f32 kHudLeftX = -kFieldHalfWidth + kHudMargin;
inline constexpr fe::f32 kHudRightX = kFieldHalfWidth - kHudMargin;

// The menu text (title, pause, game over). Larger than the HUD counters, because
// a menu line is the only thing on its screen and should read at a glance.
inline constexpr fe::f32 kMenuTextSize = 72.0f;

// The menu strings, authored in one place next to the layout they belong to.
inline constexpr const char* kTitleText = "BREAKOUT";
inline constexpr const char* kPauseText = "PAUSED";
inline constexpr const char* kGameOverText = "GAME OVER";

} // namespace config

// The score, the lives, and the win/lose transition.
//
// A tiny state machine with an explicit result rather than a bool pair, so
// "playing", "won", and "lost" cannot be represented inconsistently. Every
// transition is guarded: a brick hit after the game is over, or a second miss on
// the last life, cannot corrupt the score or flip a finished result. Those
// guards are what the unit tests pin down.
//
// It also owns the *screen* the game is on (docs/PLAN.md,
// `slice-title-and-menus`), which is the phase the presentation is in rather
// than the outcome: a game opens on a title, runs while playing, can be paused,
// and ends on a game-over screen that leads back to the title. Keeping the
// screen here rather than in the systems means the flow title -> play -> game
// over -> title is one testable object with no scene and no engine, exactly as
// the rules are.
//
// A freshly constructed state is *playing*: the rules object has no opinion
// about presentation, and a headless autoplay run (and every unit test) drives a
// live game with no menu. The application is what puts a fresh game on the title
// with show_title(), which is the one place the flow is a choice.
class BreakoutState {
public:
    enum class Result {
        Playing,
        Won,
        Lost,
    };

    // Which screen the game is showing. This is the presentation phase, distinct
    // from Result: the game is over (Result != Playing) exactly when the screen
    // is GameOver, but a game that is on the Title or Paused is still "playing"
    // in the rules sense until it is won or lost.
    enum class Screen {
        Title,    // the game has not started; the field sits idle behind the title
        Playing,  // the ball is live and the paddle responds
        Paused,   // the player stopped the clock; the field is frozen
        GameOver, // the game ended; the result is shown and a press returns to title
    };

    explicit BreakoutState(int brick_count = config::kBrickCount, int lives = config::kStartingLives,
                           int level_count = config::kLevelCount) noexcept;

    // --- Queries -----------------------------------------------------------
    [[nodiscard]] int score() const noexcept { return score_; }
    [[nodiscard]] int lives() const noexcept { return lives_; }
    [[nodiscard]] int bricks_remaining() const noexcept { return bricks_remaining_; }
    [[nodiscard]] int bricks_total() const noexcept { return bricks_total_; }
    [[nodiscard]] int paddle_hits() const noexcept { return paddle_hits_; }
    [[nodiscard]] Result result() const noexcept { return result_; }
    [[nodiscard]] bool is_over() const noexcept { return result_ != Result::Playing; }
    [[nodiscard]] bool won() const noexcept { return result_ == Result::Won; }

    // The level the player is on, 1-based, and the last one. Clearing the board on
    // the last level is a win; clearing it earlier advances.
    [[nodiscard]] int level() const noexcept { return level_; }
    [[nodiscard]] int level_count() const noexcept { return level_count_; }

    // The ball's launch speed at the current level. This is the difficulty: the
    // speed rises with the level and is clamped, so every system that launches or
    // re-aims the ball asks the rules for one number rather than reading the
    // constant and inventing its own progression.
    [[nodiscard]] fe::f32 launch_speed() const noexcept;

    // The screen the game is on, and the questions a system asks about it. A
    // gameplay system runs only while `is_playing()`; the menu system drives the
    // transitions between the rest.
    [[nodiscard]] Screen screen() const noexcept { return screen_; }
    [[nodiscard]] bool is_title() const noexcept { return screen_ == Screen::Title; }
    [[nodiscard]] bool is_playing() const noexcept { return screen_ == Screen::Playing; }
    [[nodiscard]] bool is_paused() const noexcept { return screen_ == Screen::Paused; }

    // A short, stable label for the result, used in the machine-checkable log
    // line ("result=win"). Stable so a CI grep can rely on it.
    [[nodiscard]] const char* result_name() const noexcept;

    // A short, stable label for the screen ("title", "playing", "paused",
    // "gameover"), for the same machine-checkable log line.
    [[nodiscard]] const char* screen_name() const noexcept;

    // Starts a fresh game from the title (or the game-over screen): the field is
    // reset and the screen becomes Playing. This is the "press to play" edge.
    void start_game() noexcept;

    // Returns to the title from any screen, resetting the field so the title
    // shows a fresh board. The flow's last step: game over -> title.
    void return_to_title() noexcept;

    // Puts the game on the title *without* requesting a rebuild, for the one
    // caller that already has a fresh board: the application before its first
    // frame. A menu transition uses return_to_title() instead, which rebuilds.
    void show_title() noexcept;

    // Pauses and resumes a live game. Both are no-ops unless the game is on the
    // screen they act on, so a stray press cannot pause the title or resume a
    // finished game.
    void pause() noexcept;
    void resume() noexcept;

    // Reconciles the screen with the rules after a transition that ends the game
    // (a brick hit that wins, a missed ball that loses). A system calls it after
    // touching the rules so the presentation follows them; the screen becomes
    // GameOver exactly when the result stops being Playing.
    void sync_screen() noexcept;

    // --- Transitions -------------------------------------------------------
    // Records that one brick broke: one fewer brick and `kPointsPerBrick` more
    // score. Clearing the board adds `kLevelClearBonus`; on an earlier level it
    // advances to the next one (a fresh board, a faster ball) and on the last it
    // wins. A no-op once the game is over.
    void brick_hit() noexcept;

    // Records a paddle bounce. Counted for the log line only; it does not affect
    // the score.
    void paddle_hit() noexcept;

    // Records that the ball was missed: one life fewer, and a loss when none
    // remain. A no-op once the game is over.
    void ball_missed() noexcept;

    // Starts a fresh game with the same brick count and lives the first one had.
    void reset() noexcept;

    // --- Restart handshake -------------------------------------------------
    // A system sets the request (the player pressed R); whoever owns the scene
    // consumes it and rebuilds. Keeping the flag here rather than in the system
    // means the restart survives the frame it was requested on, even though the
    // systems themselves are stateless.
    void request_restart() noexcept { restart_requested_ = true; }
    [[nodiscard]] bool restart_requested() const noexcept { return restart_requested_; }
    bool consume_restart() noexcept;

    // --- Level handshake ---------------------------------------------------
    // The same handshake as the restart, for the same reason: the rules advance
    // the level (a fresh board's worth of bricks and a faster ball), but only the
    // owner of the scene can rebuild the bricks the last level broke. The rules
    // set the request when a board is cleared short of the last level; the
    // consumer rebuilds and clears it.
    [[nodiscard]] bool level_change_requested() const noexcept { return level_change_requested_; }
    bool consume_level_change() noexcept;

private:
    void set_over(Result result) noexcept {
        if (result_ == Result::Playing) {
            result_ = result;
        }
    }

    int bricks_total_ = 0;
    int bricks_remaining_ = 0;
    int score_ = 0;
    int lives_ = 0;
    int starting_lives_ = 0;
    int paddle_hits_ = 0;
    int level_ = 1;
    int level_count_ = config::kLevelCount;
    Result result_ = Result::Playing;
    Screen screen_ = Screen::Playing;
    bool restart_requested_ = false;
    bool level_change_requested_ = false;
};

} // namespace breakout
