// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "breakout_state.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>

namespace breakout {

namespace {

fe::f32 clampf(fe::f32 value, fe::f32 low, fe::f32 high) noexcept {
    return std::min(std::max(value, low), high);
}

// Parses one "key=value" line's value as a float. A value that is not a finite
// number is rejected rather than clamped, because clamping a NaN would be
// meaningless and clamping an infinity would silently pick an end.
bool parse_float(std::string_view text, fe::f32& out) noexcept {
    if (text.empty()) {
        return false;
    }
    fe::f32 value = 0.0f;
    const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{} || ptr != text.data() + text.size() || !std::isfinite(value)) {
        return false;
    }
    out = value;
    return true;
}

} // namespace

void Settings::set_volume(fe::f32 value) noexcept {
    volume = clampf(value, config::kMinVolume, config::kMaxVolume);
}

void Settings::set_paddle_speed(fe::f32 value) noexcept {
    paddle_speed = clampf(value, config::kMinPaddleSpeed, config::kMaxPaddleSpeed);
}

void Settings::adjust_volume(int steps) noexcept {
    set_volume(volume + config::kVolumeStep * static_cast<fe::f32>(steps));
}

void Settings::adjust_paddle_speed(int steps) noexcept {
    set_paddle_speed(paddle_speed + config::kPaddleSpeedStep * static_cast<fe::f32>(steps));
}

std::string serialize_settings(const Settings& settings) {
    // One "key=value" line per field, in a fixed order, with a fixed precision so
    // a save round-trips through text exactly. Written by hand rather than through
    // a generic writer because the format is two numbers and a reader that trusts
    // a schema would be more code than the schema.
    char buffer[64];
    std::string text;
    std::snprintf(buffer, sizeof(buffer), "volume=%.3f\n", static_cast<double>(settings.volume));
    text += buffer;
    std::snprintf(buffer, sizeof(buffer), "paddle_speed=%.3f\n", static_cast<double>(settings.paddle_speed));
    text += buffer;
    return text;
}

bool parse_settings(std::string_view text, Settings& out) noexcept {
    bool read_any = false;
    std::size_t pos = 0;
    while (pos < text.size()) {
        const std::size_t newline = text.find('\n', pos);
        const std::size_t end = newline == std::string_view::npos ? text.size() : newline;
        const std::string_view line = text.substr(pos, end - pos);
        pos = end + 1;

        const std::size_t eq = line.find('=');
        if (eq == std::string_view::npos) {
            continue; // not a key=value line: skip it rather than fail
        }
        const std::string_view key = line.substr(0, eq);
        const std::string_view value_text = line.substr(eq + 1);

        fe::f32 value = 0.0f;
        if (!parse_float(value_text, value)) {
            continue;
        }
        if (key == "volume") {
            out.set_volume(value);
            read_any = true;
        } else if (key == "paddle_speed") {
            out.set_paddle_speed(value);
            read_any = true;
        }
        // An unknown key is ignored, not fatal: a save from a newer build that
        // added a setting must still load here.
    }
    return read_any;
}

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
    case Screen::Settings:
        return "settings";
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

void BreakoutState::open_settings() noexcept {
    // Allowed from the title and a pause -- the two screens that carry a settings
    // button -- and nowhere else, so a setting cannot be changed mid-rally or from
    // the game-over screen (whose result would immediately re-assert itself over
    // the settings screen). A screen that is already the settings screen keeps the
    // return it recorded, so re-opening is idempotent rather than a second push.
    if (screen_ == Screen::Title || screen_ == Screen::Paused) {
        settings_return_ = screen_;
        screen_ = Screen::Settings;
    }
}

void BreakoutState::close_settings() noexcept {
    if (screen_ != Screen::Settings) {
        return;
    }
    // Back to where the screen was opened from. A game that was paused stays
    // paused; a title stays a title. The alternative -- always the title -- would
    // silently abandon a paused game.
    screen_ = settings_return_;
}

void BreakoutState::set_best_score(int value) noexcept {
    // A loaded save may be lower than a record this process already set (a save
    // from an older run, or a corrupt file); the record only ever rises, so a
    // lower value is ignored rather than trusted.
    if (!has_best_score_ || value > best_score_) {
        best_score_ = value;
        has_best_score_ = true;
    }
}

bool BreakoutState::record_best_score() noexcept {
    if (score_ > best_score_) {
        best_score_ = score_;
        has_best_score_ = true;
        return true;
    }
    // A first game that ends on zero still has a best score of zero, which is
    // worth knowing (the HUD stops leaving the line blank) even though the number
    // did not move.
    if (!has_best_score_) {
        best_score_ = score_;
        has_best_score_ = true;
        return true;
    }
    return false;
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
