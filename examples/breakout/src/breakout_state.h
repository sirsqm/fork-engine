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

} // namespace config

// The score, the lives, and the win/lose transition.
//
// A tiny state machine with an explicit result rather than a bool pair, so
// "playing", "won", and "lost" cannot be represented inconsistently. Every
// transition is guarded: a brick hit after the game is over, or a second miss on
// the last life, cannot corrupt the score or flip a finished result. Those
// guards are what the unit tests pin down.
class BreakoutState {
public:
    enum class Result {
        Playing,
        Won,
        Lost,
    };

    explicit BreakoutState(int brick_count = config::kBrickCount, int lives = config::kStartingLives) noexcept;

    // --- Queries -----------------------------------------------------------
    [[nodiscard]] int score() const noexcept { return score_; }
    [[nodiscard]] int lives() const noexcept { return lives_; }
    [[nodiscard]] int bricks_remaining() const noexcept { return bricks_remaining_; }
    [[nodiscard]] int bricks_total() const noexcept { return bricks_total_; }
    [[nodiscard]] int paddle_hits() const noexcept { return paddle_hits_; }
    [[nodiscard]] Result result() const noexcept { return result_; }
    [[nodiscard]] bool is_over() const noexcept { return result_ != Result::Playing; }
    [[nodiscard]] bool won() const noexcept { return result_ == Result::Won; }

    // A short, stable label for the result, used in the machine-checkable log
    // line ("result=win"). Stable so a CI grep can rely on it.
    [[nodiscard]] const char* result_name() const noexcept;

    // --- Transitions -------------------------------------------------------
    // Records that one brick broke: one fewer brick, `kPointsPerBrick` more
    // score, and a win when that was the last one. A no-op once the game is over.
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
    Result result_ = Result::Playing;
    bool restart_requested_ = false;
};

} // namespace breakout
