// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "breakout_state.h"

#include <algorithm>

namespace breakout {

BreakoutState::BreakoutState(int brick_count, int lives) noexcept
    : bricks_total_(std::max(brick_count, 0)), bricks_remaining_(std::max(brick_count, 0)), lives_(std::max(lives, 0)),
      starting_lives_(std::max(lives, 0)) {}

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

void BreakoutState::brick_hit() noexcept {
    if (is_over() || bricks_remaining_ <= 0) {
        return;
    }
    --bricks_remaining_;
    score_ += config::kPointsPerBrick;
    if (bricks_remaining_ == 0) {
        set_over(Result::Won);
    }
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
    result_ = Result::Playing;
    restart_requested_ = false;
}

bool BreakoutState::consume_restart() noexcept {
    const bool requested = restart_requested_;
    restart_requested_ = false;
    return requested;
}

} // namespace breakout
