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
#include "fe/ui/UiElement.h"
#include "fe/ui/UiInputSystem.h"

#include <string>
#include <string_view>

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

// A HUD text node, laid out by the UI system in screen space (ADR-0084). The
// node carries a `UiElement` under the canvas rather than a world-space label, so
// the score stays pinned to the window's corner at any window size and the
// end-of-game message can be centred on the screen rather than on the field.
//
// `anchor` names the canvas corner the element is placed against and `offset`
// nudges it inward, which is why the score and the lives use opposite-signed x.
fe::Node* add_hud_label(fe::Node* canvas, std::string_view name, fe::ui::UiAnchor anchor, const fe::Vec2& offset,
                        fe::ui::UiAlign align) {
    fe::Node* node = canvas->create_child<fe::Node>(std::string(name));
    auto* element = node->add_component<fe::ui::UiElement>();
    element->width = fe::ui::UiSize::content();
    element->height = fe::ui::UiSize::content();
    element->anchor = anchor;
    element->offset = offset;
    element->text_align = align;
    // The end-of-game message is centred on the screen, so it uses the middle
    // anchor and centre alignment; the counters use a corner and the matching
    // edge alignment. One component serves both, which is the point.
    element->text_vertical = fe::ui::UiAlign::Center;
    return node;
}

// A full-screen menu canvas with a centred label, hidden until its screen is
// current. One per screen (title, pause, game over); MenuSystem toggles the
// canvas's `visible` flag to show exactly one at a time, so a screen change is
// one bool, not a rebuild.
//
// The label is the whole menu for now: a line of text a click anywhere on (the
// label is hit-tested, and MenuSystem treats a click on it as "go"). A richer
// menu is a flow container of buttons inside this same canvas, which the
// authoring model already supports.
fe::Node* add_menu_canvas(fe::Scene& scene, std::string_view canvas_name, std::string_view label_name,
                          const std::shared_ptr<fe::Resource>& font, const char* text) {
    fe::Node* canvas_node = scene.create_node(std::string(canvas_name));
    auto* canvas = canvas_node->add_component<fe::ui::UiCanvas>();
    canvas->design_width = cfg::kViewHalfWidth * 2.0f;
    canvas->design_height = cfg::kViewHalfHeight * 2.0f;
    canvas->font = font;
    canvas->text_size = cfg::kMenuTextSize;
    // Above the HUD (layer 10), so a menu covers the field and the counters.
    canvas->layer = 20;
    // Hidden until MenuSystem shows it; the field is drawn behind either way.
    canvas->visible = false;

    fe::Node* label = canvas_node->create_child<fe::Node>(std::string(label_name));
    auto* element = label->add_component<fe::ui::UiElement>();
    element->width = fe::ui::UiSize::content();
    element->height = fe::ui::UiSize::content();
    element->anchor = fe::ui::UiAnchor::MiddleCenter;
    element->text_align = fe::ui::UiAlign::Center;
    element->text_vertical = fe::ui::UiAlign::Center;
    element->text = text;
    return canvas_node;
}

} // namespace

void build_breakout_scene(fe::Scene& scene, const SceneAssets& assets) {
    scene.set_name("Breakout");

    fe::Node* camera_node = scene.create_node("Camera");
    auto* camera = camera_node->add_component<fe::Camera2D>();
    camera->zoom = 1.0f;
    // Fit the whole field, not just viewport_size world units, so the paddle and
    // the top wall are on screen in any window. Without this the field is shown
    // one world unit per pixel and a window shorter than 960 units crops the
    // paddle off the bottom.
    camera->reference_width = cfg::kViewHalfWidth * 2.0f;
    camera->reference_height = cfg::kViewHalfHeight * 2.0f;
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

    // The HUD: a screen-space UI canvas (ADR-0084) whose score is pinned to the
    // top-left and lives to the top-right. Unlike the world-space labels this
    // replaces, the corners are the *window's* corners, so the counters stay put
    // in a browser window of any shape. The text is filled in each frame by
    // HudSystem, so the canvas is authored with empty elements and the game's
    // state is what makes them readable. An empty element draws nothing.
    fe::Node* hud = scene.create_node(std::string(names::kHudCanvas));
    auto* canvas = hud->add_component<fe::ui::UiCanvas>();
    // The design resolution the HUD is authored against. The camera fits the same
    // box, so the counters line up with the field's corners at the reference size
    // and stay inside the walls as the window grows.
    canvas->design_width = cfg::kViewHalfWidth * 2.0f;
    canvas->design_height = cfg::kViewHalfHeight * 2.0f;
    canvas->font = assets.font;
    canvas->text_size = cfg::kHudTextSize;
    // Above the field's sprites (which sit at layer 0) and below any future
    // overlay, so the HUD reads over the walls and the ball.
    canvas->layer = 10;

    add_hud_label(hud, names::kScoreLabel, fe::ui::UiAnchor::TopLeft, fe::Vec2{cfg::kHudMargin, cfg::kHudMargin},
                  fe::ui::UiAlign::Start);
    add_hud_label(hud, names::kLivesLabel, fe::ui::UiAnchor::TopRight, fe::Vec2{-cfg::kHudMargin, cfg::kHudMargin},
                  fe::ui::UiAlign::End);
    // The level sits between them, so the three readouts never overlap and a
    // cleared board is visible the moment it happens.
    add_hud_label(hud, names::kLevelLabel, fe::ui::UiAnchor::TopCenter, fe::Vec2{0.0f, cfg::kHudMargin},
                  fe::ui::UiAlign::Center);

    // The menu screens (docs/PLAN.md, `slice-title-and-menus`): a title,
    // a pause, and a game-over canvas, each hidden until MenuSystem shows it.
    // They are built here so the authored scene file and the code-built field
    // stay identical, exactly as the field's own nodes are. The game-over canvas
    // reads "GAME OVER" for a loss; the won case is overwritten by HudSystem,
    // which is why the label text is authored rather than left empty.
    add_menu_canvas(scene, names::kTitleCanvas, names::kTitleLabel, assets.font, cfg::kTitleText);
    add_menu_canvas(scene, names::kPauseCanvas, names::kPauseLabel, assets.font, cfg::kPauseText);
    add_menu_canvas(scene, names::kGameOverCanvas, names::kGameOverLabel, assets.font, cfg::kGameOverText);
}

fe::physics2d::Physics2DSystem* install_breakout_systems(fe::SceneSchedule& schedule, BreakoutState& state,
                                                         bool autoplay) {
    return install_breakout_systems(schedule, state, autoplay, BreakoutActions{});
}

BreakoutActions declare_breakout_actions(fe::ActionMap& actions) {
    BreakoutActions bindings;
    bindings.move_left = actions.add("MoveLeft");
    bindings.move_right = actions.add("MoveRight");
    bindings.launch = actions.add("Launch");
    bindings.pause = actions.add("Pause");

    // Keyboard: the arrows and WASD for steering, Space and Enter to serve. Both
    // spellings are bound so a player who reaches for either is not surprised.
    actions.bind_key(bindings.move_left, fe::Key::Left);
    actions.bind_key(bindings.move_left, fe::Key::A);
    actions.bind_key(bindings.move_right, fe::Key::Right);
    actions.bind_key(bindings.move_right, fe::Key::D);
    actions.bind_key(bindings.launch, fe::Key::Space);
    actions.bind_key(bindings.launch, fe::Key::Enter);
    // P pauses and resumes. It is a menu action, not a gameplay one, so it has no
    // touch region: the pause canvas is advanced by the same confirm press.
    actions.bind_key(bindings.pause, fe::Key::P);

    // Touch: the outer thirds steer and the middle third serves, so a phone can
    // play the same game with two thumbs. These are the bindings that make the
    // keyboard-only game of yesterday playable on a touchscreen; the regions are
    // normalised, so they hold in portrait and landscape alike.
    actions.bind_touch_region(bindings.move_left, 0.0f, 0.0f, 0.34f, 1.0f);
    actions.bind_touch_region(bindings.move_right, 0.66f, 0.0f, 1.0f, 1.0f);
    // The launch strip is the middle third, the whole height: a tap anywhere in
    // the middle serves, which is the most forgiving target for a thumb. The same
    // strip starts a game and returns from the game-over screen, because the
    // menu's confirm action is this same action.
    actions.bind_touch_region(bindings.launch, 0.34f, 0.0f, 0.66f, 1.0f);

    return bindings;
}

fe::physics2d::Physics2DSystem* install_breakout_systems(fe::SceneSchedule& schedule, BreakoutState& state,
                                                         bool autoplay, const BreakoutActions& bindings) {
    // The UI input system runs first, so the interaction state a menu reads this
    // frame is already written before any gameplay system looks at it. The HUD
    // itself is not interactive yet, but registering the system is what makes a
    // pause menu or a title screen a matter of reading `UiInteraction::clicked`
    // rather than new plumbing.
    static_cast<void>(schedule.add_system<fe::ui::UiInputSystem>());

    // The menu system runs before the gameplay systems: it is what decides the
    // screen, and the gameplay systems read that decision this same frame (they
    // freeze themselves when the game is not playing). Running it first means a
    // press that starts a game lets the very next system see a live game, rather
    // than a frame of lag.
    auto* menu = schedule.add_system<MenuSystem>();
    menu->set_state(&state);
    menu->set_confirm_action(bindings.launch);
    menu->set_pause_action(bindings.pause);

    auto* paddle_system = schedule.add_system<PaddleControlSystem>();
    paddle_system->set_autoplay(autoplay);
    paddle_system->set_state(&state);
    paddle_system->set_move_actions(bindings.move_left, bindings.move_right);

    auto* launch_system = schedule.add_system<BallLaunchSystem>();
    launch_system->set_autoplay(autoplay);
    launch_system->set_state(&state);
    launch_system->set_launch_action(bindings.launch);

    auto* physics = schedule.add_system<fe::physics2d::Physics2DSystem>();
    physics->gravity = fe::Vec2{0.0f, 0.0f}; // breakout is gravity-free

    auto* breaker = schedule.add_system<BallContactSystem>();
    breaker->set_physics(physics);
    breaker->set_state(&state);

    auto* game_state = schedule.add_system<GameStateSystem>();
    game_state->set_state(&state);

    // Last, so it reads the state the systems above it produced this frame. It
    // owns nothing, so a restart that rebuilds the field needs no hand-off.
    auto* hud = schedule.add_system<HudSystem>();
    hud->set_state(&state);

    return physics;
}

} // namespace breakout
