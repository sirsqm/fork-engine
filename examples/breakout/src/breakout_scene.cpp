// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "breakout_scene.h"

#include "breakout_systems.h"

#include "fe/audio/AudioSource.h"
#include "fe/physics2d/Collider2D.h"
#include "fe/physics2d/Physics2DSystem.h"
#include "fe/physics2d/RigidBody2D.h"
#include "fe/scene/Camera2D.h"
#include "fe/scene/Node.h"
#include "fe/scene/Scene.h"
#include "fe/scene/Sprite2D.h"
#include "fe/systems/SceneSchedule.h"

#include <string>

namespace breakout {
namespace {

namespace cfg = config;

void add_sprite(fe::Node* node, const std::shared_ptr<fe::Resource>& texture, const fe::Vec2& size,
                const fe::Vec4& color, fe::i32 layer) {
    auto* sprite = node->add_component<fe::Sprite2D>();
    sprite->texture = texture;
    sprite->size = size;
    sprite->color = color;
    sprite->layer = layer;
}

// A wall: a static box collider (no rigid body) with a sprite. Tinted at draw
// time, so one texture serves all three.
void add_wall(fe::Scene& scene, const char* name, const fe::Vec3& position, const fe::Vec2& half_extent,
              const std::shared_ptr<fe::Resource>& texture) {
    fe::Node* node = scene.create_node(name);
    auto* collider = node->add_component<fe::physics2d::Collider2D>();
    collider->shape = fe::physics2d::ShapeType::Box;
    collider->half_extent = half_extent;
    collider->restitution = 1.0f;
    node->transform().position = position;
    add_sprite(node, texture, fe::Vec2{half_extent.x * 2.0f, half_extent.y * 2.0f}, fe::Vec4{0.5f, 0.55f, 0.7f, 1.0f},
               0);
}

} // namespace

void build_breakout_scene(fe::Scene& scene, const SceneAssets& assets) {
    scene.set_name("Breakout");

    fe::Node* camera_node = scene.create_node("Camera");
    auto* camera = camera_node->add_component<fe::Camera2D>();
    camera->zoom = 1.0f;
    camera->clear_r = 0.06f;
    camera->clear_g = 0.07f;
    camera->clear_b = 0.12f;

    // Walls: left, right and top; the bottom is open so a missed ball can fall
    // out. They are wide enough to overlap at the corners.
    const fe::f32 wall_x = cfg::kFieldHalfWidth + cfg::kWallThickness;
    const fe::f32 wall_y = cfg::kFieldHalfHeight + cfg::kWallThickness;
    add_wall(scene, "WallLeft", fe::Vec3{-wall_x, 0.0f, 0.0f},
             fe::Vec2{cfg::kWallThickness, wall_y + cfg::kWallThickness}, assets.wall);
    add_wall(scene, "WallRight", fe::Vec3{wall_x, 0.0f, 0.0f},
             fe::Vec2{cfg::kWallThickness, wall_y + cfg::kWallThickness}, assets.wall);
    add_wall(scene, "WallTop", fe::Vec3{0.0f, wall_y, 0.0f},
             fe::Vec2{wall_x + cfg::kWallThickness, cfg::kWallThickness}, assets.wall);

    // Paddle: a collider plus a kinematic rigid body. Kinematic because the
    // control system moves it directly, while the solver still lets the ball
    // bounce off its (effectively infinite) mass.
    fe::Node* paddle = scene.create_node(std::string(names::kPaddle));
    auto* paddle_collider = paddle->add_component<fe::physics2d::Collider2D>();
    paddle_collider->shape = fe::physics2d::ShapeType::Box;
    paddle_collider->half_extent = fe::Vec2{cfg::kPaddleHalfWidth, cfg::kPaddleHalfHeight};
    paddle_collider->restitution = 1.0f;
    auto* paddle_body = paddle->add_component<fe::physics2d::RigidBody2D>();
    paddle_body->kinematic = true;
    paddle_body->affected_by_gravity = false;
    paddle->transform().position = fe::Vec3{0.0f, cfg::kPaddleY, 0.0f};
    add_sprite(paddle, assets.paddle, fe::Vec2{cfg::kPaddleHalfWidth * 2.0f, cfg::kPaddleHalfHeight * 2.0f},
               fe::Vec4{1.0f, 1.0f, 1.0f, 1.0f}, 1);

    // Ball: a dynamic circle. fixed_rotation so a drawn ball never spins off its
    // texture; can_sleep false and no damping so it keeps its energy and never
    // settles; restitution 1 for an elastic bounce. It starts at rest, so the
    // launcher owns when it begins moving.
    fe::Node* ball = scene.create_node(std::string(names::kBall));
    auto* ball_collider = ball->add_component<fe::physics2d::Collider2D>();
    ball_collider->shape = fe::physics2d::ShapeType::Circle;
    ball_collider->radius = cfg::kBallRadius;
    ball_collider->restitution = 1.0f;
    ball_collider->friction = 0.0f;
    auto* ball_body = ball->add_component<fe::physics2d::RigidBody2D>();
    ball_body->mass = 1.0f;
    ball_body->fixed_rotation = true;
    ball_body->can_sleep = false;
    ball_body->affected_by_gravity = false;
    ball_body->linear_damping = 0.0f;
    ball->transform().position = fe::Vec3{cfg::kBallStartX, cfg::kBallStartY, 0.0f};
    add_sprite(ball, assets.ball, fe::Vec2{cfg::kBallRadius * 2.0f, cfg::kBallRadius * 2.0f},
               fe::Vec4{1.0f, 1.0f, 1.0f, 1.0f}, 2);

    // Bricks: static box colliders, each its own node, all sharing the name
    // "Brick" so the scoring system recognises them by name. They are children of
    // one parent so the scene tree stays tidy.
    fe::Node* brick_root = scene.create_node("Bricks");
    for (int row = 0; row < cfg::kBrickRows; ++row) {
        for (int col = 0; col < cfg::kBrickCols; ++col) {
            const fe::f32 x = cfg::kBrickFirstX + static_cast<fe::f32>(col) * cfg::kBrickPitchX;
            const fe::f32 y = cfg::kBrickFirstY - static_cast<fe::f32>(row) * cfg::kBrickPitchY;
            fe::Node* brick = brick_root->create_child<fe::Node>(std::string(names::kBrick));
            auto* collider = brick->add_component<fe::physics2d::Collider2D>();
            collider->shape = fe::physics2d::ShapeType::Box;
            collider->half_extent = fe::Vec2{cfg::kBrickHalfWidth, cfg::kBrickHalfHeight};
            collider->restitution = 1.0f;
            brick->transform().position = fe::Vec3{x, y, 0.0f};
            add_sprite(brick, assets.brick, fe::Vec2{cfg::kBrickHalfWidth * 2.0f, cfg::kBrickHalfHeight * 2.0f},
                       fe::Vec4{1.0f, 1.0f, 1.0f, 1.0f}, 0);
        }
    }

    // The brick-break cue: a one-shot that starts silent (play_on_start false)
    // and is re-triggered by BrickBreakerSystem through play(). It lives in the
    // scene so the sound is authored, not hardcoded; the application only
    // attaches the decoded clip.
    fe::Node* audio = scene.create_node(std::string(names::kAudio));
    auto* source = audio->add_component<fe::audio::AudioSource>();
    source->gain = 0.7f;
    source->loop = false;
    source->play_on_start = false;
}

fe::physics2d::Physics2DSystem* install_breakout_systems(fe::SceneSchedule& schedule, BreakoutState& state,
                                                         bool autoplay) {
    auto* paddle_system = schedule.add_system<PaddleControlSystem>();
    paddle_system->set_autoplay(autoplay);

    auto* launch_system = schedule.add_system<BallLaunchSystem>();
    launch_system->set_autoplay(autoplay);
    launch_system->set_state(&state);

    auto* physics = schedule.add_system<fe::physics2d::Physics2DSystem>();
    physics->gravity = fe::Vec2{0.0f, 0.0f}; // breakout is gravity-free

    auto* breaker = schedule.add_system<BallContactSystem>();
    breaker->set_physics(physics);
    breaker->set_state(&state);

    auto* game_state = schedule.add_system<GameStateSystem>();
    game_state->set_state(&state);

    return physics;
}

} // namespace breakout
