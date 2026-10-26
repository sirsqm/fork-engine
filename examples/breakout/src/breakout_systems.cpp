// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "breakout_systems.h"

#include "fe/audio/AudioSource.h"
#include "fe/core/Log.h"
#include "fe/physics2d/Collider2D.h"
#include "fe/physics2d/Physics2DSystem.h"
#include "fe/physics2d/RigidBody2D.h"
#include "fe/platform/InputState.h"
#include "fe/scene/Node.h"
#include "fe/scene/Scene.h"
#include "fe/scene/TextLabel.h"
#include "fe/systems/FrameContext.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace breakout {
namespace {

// A ball below this speed is treated as at rest (waiting to launch).
constexpr fe::f32 kAtRestSpeedSquared = 0.25f * 0.25f;

} // namespace

void PaddleControlSystem::update(const fe::FrameContext& context, fe::Scene& scene) {
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
    if (state_ != nullptr && state_->is_over()) {
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

    body->velocity = fe::Vec2{std::sin(config::kLaunchAngle) * config::kLaunchSpeed,
                              std::cos(config::kLaunchAngle) * config::kLaunchSpeed};
    body->angular_velocity = 0.0f;
    body->wake();
}

void BallContactSystem::update(const fe::FrameContext& /*context*/, fe::Scene& scene) {
    if (physics_ == nullptr || state_ == nullptr || state_->is_over()) {
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

    // Aimed paddle bounce: where the ball struck the paddle decides its new
    // direction, and its speed is restored to the launch speed so a long rally
    // neither stalls nor accelerates out of control.
    if (paddle_hit && ball_body != nullptr && paddle != nullptr) {
        state_->paddle_hit();

        const fe::f32 offset =
            (ball->transform().position.x - paddle->transform().position.x) / config::kPaddleHalfWidth;
        const fe::f32 clamped = std::clamp(offset, -1.0f, 1.0f);
        const fe::f32 angle = clamped * config::kMaxBounceAngle;
        ball_body->velocity = fe::Vec2{std::sin(angle) * config::kLaunchSpeed, std::cos(angle) * config::kLaunchSpeed};
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
            state_->request_restart();
        }
    }

    fe::Node* ball = scene.find_node_by_name(names::kBall);
    if (ball == nullptr) {
        return;
    }
    auto* body = ball->get_component<fe::physics2d::RigidBody2D>();

    if (!state_->is_over() && ball->transform().position.y < config::kFieldBottomY) {
        state_->ball_missed();
        if (state_->is_over()) {
            FE_LOG_INFO("breakout: no lives left (result=%s)", state_->result_name());
        } else {
            FE_LOG_INFO("breakout: ball missed, %d lives left", state_->lives());
            respawn_ball(scene);
        }
    }

    if (state_->is_over() && body != nullptr) {
        // Freeze the ball so a finished game is visually still rather than
        // continuing to fall forever behind the end state.
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

    if (fe::Node* node = scene.find_node_by_name(names::kScoreLabel); node != nullptr) {
        if (auto* label = node->get_component<fe::TextLabel>(); label != nullptr) {
            label->text = "SCORE " + std::to_string(state_->score());
        }
    }

    if (fe::Node* node = scene.find_node_by_name(names::kLivesLabel); node != nullptr) {
        if (auto* label = node->get_component<fe::TextLabel>(); label != nullptr) {
            // The result replaces the lives once the game ends, so the end state
            // is legible in the browser without reading the console. It is
            // centred over the field so it reads as a message, not a counter.
            if (state_->is_over()) {
                label->text = state_->won() ? "YOU WIN" : "GAME OVER";
                label->align = fe::TextLabel::Align::Center;
                node->transform().position.x = 0.0f;
                node->transform().position.y = 0.0f;
            } else {
                label->text = "LIVES " + std::to_string(state_->lives());
            }
        }
    }
}

} // namespace breakout
