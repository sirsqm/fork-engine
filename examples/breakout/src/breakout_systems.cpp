// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "breakout_systems.h"

#include "fe/audio/AudioSource.h"
#include "fe/core/Log.h"
#include "fe/physics2d/Collider2D.h"
#include "fe/physics2d/Physics2DSystem.h"
#include "fe/physics2d/RigidBody2D.h"
#include "fe/platform/InputState.h"
#include "fe/platform/UserData.h"
#include "fe/scene/Node.h"
#include "fe/scene/Scene.h"
#include "fe/systems/FrameContext.h"
#include "fe/ui/UiElement.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <string>
#include <vector>

namespace breakout {
namespace {

// A ball below this speed is treated as at rest (waiting to launch).
constexpr fe::f32 kAtRestSpeedSquared = 0.25f * 0.25f;

} // namespace

void PaddleControlSystem::update(const fe::FrameContext& context, fe::Scene& scene) {
    // Only a live game moves the paddle. On the title and the game-over screen
    // the field is idle, and a paused game is frozen, so a press cannot steer a
    // board that is not being played.
    if (state_ != nullptr && !state_->is_playing()) {
        return;
    }

    fe::Node* paddle = scene.find_node_by_name(names::kPaddle);
    if (paddle == nullptr) {
        return;
    }

    const fe::f32 limit = config::kFieldHalfWidth - config::kPaddleHalfWidth - config::kWallThickness;
    fe::f32 x = paddle->transform().position.x;

    if (autoplay_) {
        // Steer straight for the ball. This is not a good player - it ignores
        // where the ball will be - but it always returns the ball, so a headless
        // run makes progress without a human.
        if (fe::Node* ball = scene.find_node_by_name(names::kBall); ball != nullptr) {
            const fe::f32 dx = ball->transform().position.x - x;
            const fe::f32 step = controls_.paddle_speed * context.delta_time;
            x += std::clamp(dx, -step, step);
        }
    } else if (const fe::InputState* input = context.input; input != nullptr) {
        fe::f32 direction = 0.0f;
        // Actions first: they are the device-independent path, so a touch region
        // moves the paddle exactly as a key does. The key reads remain as the
        // fallback for a schedule built with no action map.
        if (context.actions != nullptr && move_left_ != fe::ActionMap::kNone) {
            if (context.actions->is_down(move_left_)) {
                direction -= 1.0f;
            }
            if (context.actions->is_down(move_right_)) {
                direction += 1.0f;
            }
        } else {
            if (input->is_key_down(fe::Key::Left) || input->is_key_down(fe::Key::A)) {
                direction -= 1.0f;
            }
            if (input->is_key_down(fe::Key::Right) || input->is_key_down(fe::Key::D)) {
                direction += 1.0f;
            }
        }
        x += direction * controls_.paddle_speed * context.delta_time;
    }

    paddle->transform().position.x = std::clamp(x, -limit, limit);
}

void BallLaunchSystem::update(const fe::FrameContext& context, fe::Scene& scene) {
    // The ball is only launched in a live game: on the title, the game-over
    // screen, and while paused it stays at rest where the last miss or the last
    // rebuild left it.
    if (state_ != nullptr && !state_->is_playing()) {
        return;
    }

    fe::Node* ball = scene.find_node_by_name(names::kBall);
    if (ball == nullptr) {
        return;
    }
    auto* body = ball->get_component<fe::physics2d::RigidBody2D>();
    if (body == nullptr) {
        return;
    }

    // Already flying: nothing to do. "At rest" is the whole launch condition, so
    // no launch flag has to survive a respawn or a restart.
    if (body->velocity.length_squared() > kAtRestSpeedSquared) {
        return;
    }

    bool launch = false;
    if (autoplay_) {
        if (autoplay_elapsed_ < 0.0f) {
            autoplay_elapsed_ = 0.0f;
        } else {
            autoplay_elapsed_ += context.delta_time;
        }
        launch = autoplay_elapsed_ >= autoplay_delay_;
    } else if (const fe::InputState* input = context.input; input != nullptr) {
        // Actions first, for the same reason the paddle reads them: a tap is how
        // a phone launches the ball, and the key reads are the no-action-map
        // fallback.
        if (context.actions != nullptr && launch_ != fe::ActionMap::kNone) {
            launch = context.actions->was_pressed(launch_);
        } else {
            launch = input->was_key_pressed(fe::Key::Space) || input->was_key_pressed(fe::Key::Enter);
        }
    }

    if (!launch) {
        return;
    }

    // The level's speed, so a launch after a clear serves a faster ball. The rules
    // own the number; the fallback covers a schedule built with no state, which
    // keeps playing at the authored speed.
    const fe::f32 launch_speed = state_ != nullptr ? state_->launch_speed() : config::kLaunchSpeed;
    body->velocity =
        fe::Vec2{std::sin(config::kLaunchAngle) * launch_speed, std::cos(config::kLaunchAngle) * launch_speed};
    body->angular_velocity = 0.0f;
    body->wake();
}

void BallContactSystem::update(const fe::FrameContext& /*context*/, fe::Scene& scene) {
    if (physics_ == nullptr || state_ == nullptr || !state_->is_playing()) {
        return;
    }

    fe::Node* ball = scene.find_node_by_name(names::kBall);
    fe::Node* paddle = scene.find_node_by_name(names::kPaddle);
    auto* ball_body = ball != nullptr ? ball->get_component<fe::physics2d::RigidBody2D>() : nullptr;

    // Collect the bricks the ball began touching this step, then act. Acting
    // after the scan keeps us from touching the event list while we destroy
    // nodes it still points at.
    std::vector<fe::Node*> broken;
    bool paddle_hit = false;
    for (const fe::physics2d::ContactEvent& event : physics_->contact_events()) {
        if (event.phase != fe::physics2d::ContactPhase::Begin) {
            continue;
        }
        fe::Node* a = event.a;
        fe::Node* b = event.b;
        if (a == nullptr || b == nullptr) {
            continue;
        }

        // A paddle contact is remembered; the aim is applied once, below, after
        // the whole scan (so a brick hit in the same step cannot be undone by it).
        if ((a->name() == names::kPaddle && b->name() == names::kBall) ||
            (b->name() == names::kPaddle && a->name() == names::kBall)) {
            paddle_hit = true;
            continue;
        }

        fe::Node* brick = nullptr;
        fe::Node* other = nullptr;
        if (is_brick(*a)) {
            brick = a;
            other = b;
        } else if (is_brick(*b)) {
            brick = b;
            other = a;
        } else {
            continue;
        }
        // Only the ball breaks a brick. This also rejects a brick-vs-wall
        // overlap, which cannot happen with this layout but is cheap to rule out.
        if (other->name() != names::kBall) {
            continue;
        }
        if (std::find(broken.begin(), broken.end(), brick) == broken.end()) {
            broken.push_back(brick);
        }
    }

    for (fe::Node* brick : broken) {
        state_->brick_hit();
        play_break_sound(scene);

        // remove_child()/remove_root() hand back ownership; the node dies here,
        // which is exactly the intent.
        fe::Node* parent = brick->parent();
        if (parent != nullptr) {
            static_cast<void>(parent->remove_child(brick));
        } else {
            static_cast<void>(scene.remove_root(brick));
        }
    }
    // Breaking the last brick wins. The presentation follows the rules here, in
    // the same frame, so the game-over screen is up before the next frame draws.
    if (!broken.empty()) {
        state_->sync_screen();
    }

    // Aimed paddle bounce: where the ball struck the paddle decides its new
    // direction, and its speed is restored to the launch speed so a long rally
    // neither stalls nor accelerates out of control.
    if (paddle_hit && ball_body != nullptr && paddle != nullptr) {
        state_->paddle_hit();

        const fe::f32 offset =
            (ball->transform().position.x - paddle->transform().position.x) / config::kPaddleHalfWidth;
        const fe::f32 clamped = std::clamp(offset, -1.0f, 1.0f);
        const fe::f32 angle = clamped * config::kMaxBounceAngle;
        // Renormalised to the level's speed, not the authored one, so the aim
        // cannot quietly undo the difficulty the level raised.
        const fe::f32 speed = state_->launch_speed();
        ball_body->velocity = fe::Vec2{std::sin(angle) * speed, std::cos(angle) * speed};
        ball_body->wake();
    }
}

bool BallContactSystem::is_brick(const fe::Node& node) noexcept {
    return node.name() == names::kBrick;
}

void BallContactSystem::play_break_sound(fe::Scene& scene) {
    fe::Node* node = scene.find_node_by_name(names::kAudio);
    if (node == nullptr) {
        return;
    }
    if (auto* source = node->get_component<fe::audio::AudioSource>(); source != nullptr) {
        source->play();
    }
}

void GameStateSystem::update(const fe::FrameContext& context, fe::Scene& scene) {
    if (state_ == nullptr) {
        return;
    }

    if (const fe::InputState* input = context.input; input != nullptr) {
        if (input->was_key_pressed(fe::Key::R)) {
            // R restarts the game wherever the player is: the rules reset and the
            // screen returns to play, and the field rebuild is requested so the
            // bricks the last game broke come back. start_game() clears the
            // request flag as part of its reset, so the request goes after it.
            state_->start_game();
            state_->request_restart();
        }
    }

    fe::Node* ball = scene.find_node_by_name(names::kBall);
    if (ball == nullptr) {
        return;
    }
    auto* body = ball->get_component<fe::physics2d::RigidBody2D>();

    // A missed ball is only a miss while the game is being played: on the title,
    // the game-over screen, and while paused the ball is not live, so it cannot
    // cost a life.
    if (state_->is_playing() && ball->transform().position.y < config::kFieldBottomY) {
        state_->ball_missed();
        // The presentation follows the rules: losing the last life ends the game,
        // so the screen becomes the game-over screen now rather than a frame
        // later.
        state_->sync_screen();
        if (state_->is_over()) {
            FE_LOG_INFO("breakout: no lives left (result=%s)", state_->result_name());
        } else {
            FE_LOG_INFO("breakout: ball missed, %d lives left", state_->lives());
            respawn_ball(scene);
        }
    }

    if (!state_->is_playing() && body != nullptr) {
        // Freeze the ball whenever the game is not live, so a paused or finished
        // game is visually still rather than continuing to fall behind the menu.
        body->velocity = fe::Vec2{};
        body->angular_velocity = 0.0f;
    }
}

void GameStateSystem::respawn_ball(fe::Scene& scene) {
    fe::Node* ball = scene.find_node_by_name(names::kBall);
    if (ball == nullptr) {
        return;
    }
    ball->transform().position = fe::Vec3{config::kBallStartX, config::kBallStartY, 0.0f};
    if (auto* body = ball->get_component<fe::physics2d::RigidBody2D>(); body != nullptr) {
        body->velocity = fe::Vec2{};
        body->angular_velocity = 0.0f;
        body->wake();
    }
}

void HudSystem::update(const fe::FrameContext& context, fe::Scene& scene) {
    (void)context;
    if (state_ == nullptr) {
        return;
    }

    // The counters. They are only drawn while playing (MenuSystem hides the HUD
    // canvas on the other screens), but their text is kept current every frame so
    // they read correctly the instant play resumes.
    if (fe::Node* node = scene.find_node_by_name(names::kScoreLabel); node != nullptr) {
        if (auto* element = node->get_component<fe::ui::UiElement>(); element != nullptr) {
            element->text = "SCORE " + std::to_string(state_->score());
        }
    }
    if (fe::Node* node = scene.find_node_by_name(names::kLivesLabel); node != nullptr) {
        if (auto* element = node->get_component<fe::ui::UiElement>(); element != nullptr) {
            element->text = "LIVES " + std::to_string(state_->lives());
        }
    }
    if (fe::Node* node = scene.find_node_by_name(names::kLevelLabel); node != nullptr) {
        if (auto* element = node->get_component<fe::ui::UiElement>(); element != nullptr) {
            element->text = "LEVEL " + std::to_string(state_->level()) + "/" + std::to_string(state_->level_count());
        }
    }

    // The game-over screen's message. It is the one place the result is shown, so
    // the end state is legible in the browser without reading the console; the
    // label is authored "GAME OVER" and this system corrects it to "YOU WIN" on a
    // win, so a win reads differently from a loss.
    if (fe::Node* node = scene.find_node_by_name(names::kGameOverLabel); node != nullptr) {
        if (auto* element = node->get_component<fe::ui::UiElement>(); element != nullptr) {
            element->text = state_->won() ? "YOU WIN" : config::kGameOverText;
        }
    }

    // The best score, blank until there is one. It is written here rather than by
    // PersistSystem so every label is filled in one place; PersistSystem owns the
    // *value*, this owns the drawing.
    if (fe::Node* node = scene.find_node_by_name(names::kBestScoreLabel); node != nullptr) {
        if (auto* element = node->get_component<fe::ui::UiElement>(); element != nullptr) {
            element->text = state_->has_best_score() ? "BEST " + std::to_string(state_->best_score()) : std::string{};
        }
    }
}

void PersistSystem::load_once(fe::UserData& store) {
    loaded_ = true;
    const fe::Result<std::string> saved = store.read_text(key_);
    if (!saved) {
        // No save yet: a first run. Leaving has_best_score() false is what makes
        // the HUD blank rather than showing a best of zero.
        return;
    }

    int value = 0;
    const char* begin = saved.value().data();
    const char* end = begin + saved.value().size();
    const auto [ptr, ec] = std::from_chars(begin, end, value);
    if (ec != std::errc{} || ptr != end || value < 0) {
        // A save that is not a plain non-negative integer is treated as absent
        // rather than fatal: a corrupt file must not stop the game from starting.
        FE_LOG_WARN("breakout: ignoring unreadable best score '%s'", saved.value().c_str());
        return;
    }
    state_->set_best_score(value);
    FE_LOG_INFO("breakout: loaded best score %d", value);
}

void PersistSystem::update(const fe::FrameContext& context, fe::Scene& scene) {
    (void)scene;
    if (state_ == nullptr) {
        return;
    }

    fe::UserData* store = context.user_data;

    // Load once, on the first frame the store is available. Doing it here rather
    // than in a constructor is what lets the store come from the context; a run
    // whose context has no store never loads and never saves.
    if (!loaded_ && store != nullptr) {
        load_once(*store);
    }

    const BreakoutState::Screen screen = state_->screen();

    // The title shows a fresh, unplayed game, so its score is not a result and
    // must not be recorded as a best; only a game that has actually been played
    // (playing, paused, or ended) counts. That is what keeps a first run's HUD
    // blank until the player has played.
    if (screen != BreakoutState::Screen::Title) {
        // The best score rises with the live score, so the HUD shows a new record
        // the moment it is set rather than only after the game ends.
        if (state_->record_best_score()) {
            FE_LOG_INFO("breakout: new best score %d", state_->best_score());
        }
    }

    // Persist on the edge *into* the game-over screen: the score is final there,
    // and it is one write per game instead of one per frame. last_screen_ is
    // updated every frame so the edge is detected exactly once.
    const bool just_ended =
        screen == BreakoutState::Screen::GameOver && last_screen_ != BreakoutState::Screen::GameOver;
    last_screen_ = screen;

    if (!just_ended || store == nullptr) {
        return;
    }

    const std::string text = std::to_string(state_->best_score());
    if (fe::Error error = store->write(key_, std::string_view(text)); error) {
        FE_LOG_WARN("breakout: could not save the best score: %s", error.message().c_str());
        return;
    }
    FE_LOG_INFO("breakout: saved best score %d", state_->best_score());
}

void MenuSystem::apply_visibility(fe::Scene& scene, BreakoutState::Screen screen) {
    // One canvas per screen, each shown exactly on its own screen. The HUD
    // counters stay visible only while playing, so the title and the end message
    // are not competing with a score from a game that is not running.
    const auto set_canvas = [&scene](std::string_view name, bool visible) {
        if (fe::Node* node = scene.find_node_by_name(name); node != nullptr) {
            if (auto* canvas = node->get_component<fe::ui::UiCanvas>(); canvas != nullptr) {
                canvas->visible = visible;
            }
        }
    };
    set_canvas(names::kTitleCanvas, screen == BreakoutState::Screen::Title);
    set_canvas(names::kPauseCanvas, screen == BreakoutState::Screen::Paused);
    set_canvas(names::kGameOverCanvas, screen == BreakoutState::Screen::GameOver);
    set_canvas(names::kHudCanvas, screen == BreakoutState::Screen::Playing);
}

void MenuSystem::save_ball_velocity(fe::Scene& scene) noexcept {
    fe::Node* ball = scene.find_node_by_name(names::kBall);
    if (ball == nullptr) {
        return;
    }
    if (const auto* body = ball->get_component<fe::physics2d::RigidBody2D>(); body != nullptr) {
        paused_ball_velocity_ = body->velocity;
        paused_ball_angular_velocity_ = body->angular_velocity;
    }
}

void MenuSystem::restore_ball_velocity(fe::Scene& scene) const noexcept {
    fe::Node* ball = scene.find_node_by_name(names::kBall);
    if (ball == nullptr) {
        return;
    }
    if (auto* body = ball->get_component<fe::physics2d::RigidBody2D>(); body != nullptr) {
        body->velocity = paused_ball_velocity_;
        body->angular_velocity = paused_ball_angular_velocity_;
        body->wake();
    }
}

void MenuSystem::update(const fe::FrameContext& context, fe::Scene& scene) {
    if (state_ == nullptr) {
        return;
    }

    // The presentation follows the rules: if something ended the game since the
    // last frame -- the ball was missed, the last brick was broken, a test drove
    // the rules directly -- the screen is the game-over screen now. Reconciling
    // here, before deciding anything, means the menu always acts on the screen
    // the rules imply rather than a stale one.
    state_->sync_screen();

    // Whether the player asked to "go" this frame: the confirm action, a UI
    // button click, or -- with no action map -- the Space/Enter fallback. One
    // flag, so a click, a tap and a key all advance a menu identically.
    bool confirm = false;
    if (context.actions != nullptr && confirm_ != fe::ActionMap::kNone) {
        confirm = context.actions->was_pressed(confirm_);
    } else if (const fe::InputState* input = context.input; input != nullptr) {
        confirm = input->was_key_pressed(fe::Key::Space) || input->was_key_pressed(fe::Key::Enter);
    }

    // A menu button is a hit-tested target: the title's "press to play" and the
    // game-over screen's "press to continue" carry a UiElement, and a click on
    // one is the same as the confirm press. Read from the interaction state
    // UiInputSystem wrote earlier this frame, so the click lands where the button
    // draws.
    const auto label_clicked = [&scene](std::string_view name) {
        const fe::Node* node = scene.find_node_by_name(name);
        if (node == nullptr) {
            return false;
        }
        const auto* element = node->get_component<fe::ui::UiElement>();
        return element != nullptr && element->interaction.clicked;
    };
    confirm = confirm || label_clicked(names::kTitleLabel) || label_clicked(names::kGameOverLabel) ||
              label_clicked(names::kPauseLabel);

    bool pause_toggle = false;
    if (context.actions != nullptr && pause_ != fe::ActionMap::kNone) {
        pause_toggle = context.actions->was_pressed(pause_);
    } else if (const fe::InputState* input = context.input; input != nullptr) {
        pause_toggle = input->was_key_pressed(fe::Key::P);
    }

    switch (state_->screen()) {
    case BreakoutState::Screen::Title:
        if (confirm) {
            state_->start_game();
        }
        break;
    case BreakoutState::Screen::Playing:
        if (pause_toggle) {
            // Save the ball's motion before the game freezes it, so resuming
            // continues the rally rather than leaving the ball dead.
            save_ball_velocity(scene);
            state_->pause();
        }
        break;
    case BreakoutState::Screen::Paused:
        if (pause_toggle || confirm) {
            state_->resume();
            restore_ball_velocity(scene);
        }
        break;
    case BreakoutState::Screen::GameOver:
        if (confirm) {
            state_->return_to_title();
        }
        break;
    }

    // Show the canvas for whatever screen we ended on. Done after the transition
    // so the frame that changes the screen also draws the right menu.
    apply_visibility(scene, state_->screen());
}

} // namespace breakout
