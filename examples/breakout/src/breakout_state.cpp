// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "breakout_state.h"

#include <algorithm>

namespace breakout {

BreakoutState::BreakoutState(int brick_count, int lives, int level_count) noexcept
    : bricks_total_(std::max(brick_count, 0)), bricks_remaining_(std::max(brick_count, 0)), lives_(std::max(lives, 0)),
      starting_lives_(std::max(lives, 0)), level_count_(std::max(level_count, 1)) {}

const char* BreakoutState::result_name() const noexcept {
    switch (result_) {
    case Result::Won:
        return "win";
    case Result::Lost:
        return "lose";
    case Result::Playing:
        break;
    }
    return "playing";
}

fe::f32 BreakoutState::launch_speed() const noexcept {
    // Level 1 is the authored speed and each level after it is faster. Clamped to
    // the ceiling so a long progression can never make the ball step past a brick
    // in one solver step, which would tunnel through it.
    const fe::f32 scale = 1.0f + config::kLevelSpeedGain * static_cast<fe::f32>(level_ - 1);
    return std::min(config::kLaunchSpeed * scale, config::kMaxBallSpeed);
}

const char* BreakoutState::screen_name() const noexcept {
    switch (screen_) {
    case Screen::Title:
        return "title";
    case Screen::Playing:
        return "playing";
    case Screen::Paused:
        return "paused";
    case Screen::GameOver:
        return "gameover";
    }
    return "playing";
}

void BreakoutState::start_game() noexcept {
    // A fresh board and the clock running. No scene rebuild is requested: the
    // title always shows a board that is already fresh (return_to_title() reset
    // and rebuilt it), so starting from it has nothing to rebuild. The flow's
    // "title -> play" step.
    reset();
    screen_ = Screen::Playing;
}

void BreakoutState::return_to_title() noexcept {
    // Back to the title with a fresh board behind it, so the title never shows a
    // half-cleared field from the game that just ended. The rebuild is requested
    // because the board the last game left is not fresh; the owner of the scene
    // consumes it. The flow's "game over -> title" step.
    reset();
    screen_ = Screen::Title;
    request_restart();
}

void BreakoutState::show_title() noexcept {
    // Puts the game on the title without rebuilding: used once by the
    // application before the first frame, when the board is already fresh. A
    // menu transition uses return_to_title() instead, which rebuilds.
    reset();
    screen_ = Screen::Title;
}

void BreakoutState::pause() noexcept {
    if (screen_ == Screen::Playing) {
        screen_ = Screen::Paused;
    }
}

void BreakoutState::resume() noexcept {
    if (screen_ == Screen::Paused) {
        screen_ = Screen::Playing;
    }
}

void BreakoutState::sync_screen() noexcept {
    // The presentation follows the rules: a game that stopped being played is on
    // its game-over screen. A game still in progress is left on whatever screen
    // it is on (title, playing, or paused), so this never un-pauses or reopens a
    // title.
    if (is_over()) {
        screen_ = Screen::GameOver;
    }
}

void BreakoutState::brick_hit() noexcept {
    // A pending level change also stops scoring: two bricks can break in one
    // solver step, and the second must not decrement the fresh board the first
    // one already installed.
    if (is_over() || bricks_remaining_ <= 0 || level_change_requested_) {
        return;
    }
    --bricks_remaining_;
    score_ += config::kPointsPerBrick;
    if (bricks_remaining_ > 0) {
        return;
    }

    // The board is clear. The clear itself is worth a bonus, and where it leads
    // depends on the level: another board at a higher speed, or the win.
    score_ += config::kLevelClearBonus;
    if (level_ >= level_count_) {
        set_over(Result::Won);
        return;
    }

    // A fresh board's worth of bricks and a faster ball, both owned by the rules;
    // the scene rebuild is the owner's job, so it is requested rather than done.
    ++level_;
    bricks_remaining_ = bricks_total_;
    level_change_requested_ = true;
}

void BreakoutState::paddle_hit() noexcept {
    if (is_over()) {
        return;
    }
    ++paddle_hits_;
}

void BreakoutState::ball_missed() noexcept {
    if (is_over()) {
        return;
    }
    --lives_;
    if (lives_ <= 0) {
        lives_ = 0;
        set_over(Result::Lost);
    }
}

void BreakoutState::reset() noexcept {
    bricks_remaining_ = bricks_total_;
    score_ = 0;
    lives_ = starting_lives_;
    paddle_hits_ = 0;
    level_ = 1;
    result_ = Result::Playing;
    restart_requested_ = false;
    level_change_requested_ = false;
}

bool BreakoutState::consume_restart() noexcept {
    const bool requested = restart_requested_;
    restart_requested_ = false;
    return requested;
}

bool BreakoutState::consume_level_change() noexcept {
    const bool requested = level_change_requested_;
    level_change_requested_ = false;
    return requested;
}

} // namespace breakout
