// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "breakout_state.h"

#include "fe/core/Math.h"
#include "fe/platform/InputActions.h"
#include "fe/systems/System.h"

#include <string_view>

namespace fe {
class Node;
class Scene;
struct FrameContext;
namespace physics2d {
class Physics2DSystem;
}
namespace audio {
class AudioSource;
}
} // namespace fe

// The breakout gameplay systems.
//
// Each is a fe::System: a unit of per-frame behaviour the SceneSchedule runs, in
// the order the game adds them (ADR-0008). None of them is a node method; the
// scene is only data, and these read and write it. That split is the whole point
// of the milestone — gameplay is a system you add, not a class a node inherits.
//
// Every system resolves the nodes it cares about *by name* each frame rather
// than caching a Node*, because a scene reload or a rebuild (restart) replaces
// every node. The lookup is a handful of string compares over a small scene, so
// it is not worth caching and caching would be a stale-pointer bug waiting to
// happen.
//
// The rules themselves live in BreakoutState (see breakout_state.h); these
// systems only translate scene events into calls on it, and its result back into
// the scene. That is what keeps the rules unit-testable with no engine running.

namespace breakout {

// The node names the systems look for. One place, so the scene builder and the
// systems cannot drift apart.
namespace names {
inline constexpr std::string_view kPaddle = "Paddle";
inline constexpr std::string_view kBall = "Ball";
inline constexpr std::string_view kBrick = "Brick";
inline constexpr std::string_view kAudio = "BrickAudio";
inline constexpr std::string_view kHudCanvas = "HudCanvas";
inline constexpr std::string_view kScoreLabel = "ScoreLabel";
inline constexpr std::string_view kLivesLabel = "LivesLabel";
// The menu canvases. Each is a screen-space canvas (ADR-0086) hidden by default
// and shown by MenuSystem while its screen is current, so the whole flow is one
// flag per canvas rather than a rebuild.
inline constexpr std::string_view kTitleCanvas = "TitleCanvas";
inline constexpr std::string_view kTitleLabel = "TitleLabel";
inline constexpr std::string_view kPauseCanvas = "PauseCanvas";
inline constexpr std::string_view kPauseLabel = "PauseLabel";
inline constexpr std::string_view kGameOverCanvas = "GameOverCanvas";
inline constexpr std::string_view kGameOverLabel = "GameOverLabel";
} // namespace names

// Shared tuning for the systems that drive the ball and paddle.
struct Controls {
    fe::f32 paddle_speed = 780.0f; // world units per second
};

// Moves the paddle left and right.
//
// It reads gameplay actions rather than keys, so the same code serves a keyboard
// (Left/Right or A/D) and a touchscreen (a finger in the left or right third of
// the screen). The bindings are declared once by the application; this system
// only asks whether the player means to move. When no action map is available
// (a headless tool, an older test) it falls back to the keys, so a schedule
// built without actions still plays.
//
// It moves the paddle only while the game is on the Playing screen: on the title
// and the game-over screen the field is idle, and a paused game is frozen. That
// gate is why it holds the state.
//
// In autoplay mode (used by a headless run) it instead steers the paddle towards
// the ball, so a machine with no player still reaches an outcome. The choice is
// a flag, not a different class, because the paddle's job — track a target x —
// is identical either way.
class PaddleControlSystem final : public fe::System {
public:
    explicit PaddleControlSystem(Controls controls = {}) : controls_(controls) {}

    [[nodiscard]] std::string_view name() const noexcept override { return "PaddleControlSystem"; }

    void update(const fe::FrameContext& context, fe::Scene& scene) override;

    void set_autoplay(bool autoplay) noexcept { autoplay_ = autoplay; }
    [[nodiscard]] bool autoplay() const noexcept { return autoplay_; }

    // The game state, so the paddle is frozen when the game is not playing.
    // Borrowed; the application owns it. Null means "always play", which keeps a
    // schedule built without state behaving as it did before screens existed.
    void set_state(BreakoutState* state) noexcept { state_ = state; }

    // The action handles to read. Zero (the default) means the action is unbound
    // and the keyboard fallback is used instead, so a caller that declares no
    // actions still gets a playable paddle.
    void set_move_actions(fe::ActionMap::Id move_left, fe::ActionMap::Id move_right) noexcept {
        move_left_ = move_left;
        move_right_ = move_right;
    }

private:
    Controls controls_;
    bool autoplay_ = false;
    BreakoutState* state_ = nullptr;
    fe::ActionMap::Id move_left_ = 0;
    fe::ActionMap::Id move_right_ = 0;
};

// Launches the ball from its resting position on Space (or automatically in
// autoplay). Until it launches the ball stays still: the game's physics has no
// gravity, so a zero-velocity dynamic body simply waits. Re-arming is inferred
// from the ball being at rest, which is what a life loss and a restart leave
// behind, so no launch flag has to be threaded through the state.
class BallLaunchSystem final : public fe::System {
public:
    [[nodiscard]] std::string_view name() const noexcept override { return "BallLaunchSystem"; }

    void update(const fe::FrameContext& context, fe::Scene& scene) override;

    void set_autoplay(bool autoplay) noexcept { autoplay_ = autoplay; }
    [[nodiscard]] bool autoplay() const noexcept { return autoplay_; }

    // In autoplay the ball launches after this many seconds, so a headless run
    // starts promptly and deterministically.
    void set_autoplay_delay(fe::f32 seconds) noexcept { autoplay_delay_ = seconds; }

    // The game state, so the launcher does not restart the ball after the game
    // has ended. Borrowed; the application owns it.
    void set_state(BreakoutState* state) noexcept { state_ = state; }

    // The launch action. Zero means unbound, so the Space/Enter fallback applies.
    void set_launch_action(fe::ActionMap::Id launch) noexcept { launch_ = launch; }

private:
    bool autoplay_ = false;
    fe::f32 autoplay_delay_ = 0.25f;
    fe::f32 autoplay_elapsed_ = -1.0f;
    BreakoutState* state_ = nullptr;
    fe::ActionMap::Id launch_ = 0;
};

// Turns a ball-vs-brick contact into a score change, a broken brick, and a
// sound. This is the scoring logic, and it is the one place contact events are
// read.
//
// It holds a borrowed pointer to the Physics2DSystem that must run earlier in
// the schedule; the events it reads describe exactly the step that just
// finished, so the reaction is in the same frame the collision was resolved.
//
// It also gives the ball its classic *aimed* bounce: a paddle hit off-centre
// sends the ball away at an angle proportional to how far from the centre it
// landed, and the speed is renormalised to a constant. That is what turns a
// physics demo into a game - without it, a vertical shot loops forever and can
// never clear the field.
class BallContactSystem final : public fe::System {
public:
    [[nodiscard]] std::string_view name() const noexcept override { return "BallContactSystem"; }

    void update(const fe::FrameContext& context, fe::Scene& scene) override;

    // The system is borrowed, not owned: the schedule owns it. Must be set
    // before the first update for scoring to work.
    void set_physics(fe::physics2d::Physics2DSystem* physics) noexcept { physics_ = physics; }
    void set_state(BreakoutState* state) noexcept { state_ = state; }

private:
    // True when `node` is a brick, by name. Bricks share one name; the specific
    // node comes from the contact event, so the shared name is unambiguous.
    [[nodiscard]] static bool is_brick(const fe::Node& node) noexcept;

    // Starts the brick-break sound on the audio node, if one is present.
    void play_break_sound(fe::Scene& scene);

    fe::physics2d::Physics2DSystem* physics_ = nullptr;
    BreakoutState* state_ = nullptr;
};

// Watches the ball, applies a missed-ball life loss, freezes the field when the
// game ends, and honours the restart key. It is the bridge between the rules in
// BreakoutState and the scene.
class GameStateSystem final : public fe::System {
public:
    [[nodiscard]] std::string_view name() const noexcept override { return "GameStateSystem"; }

    void update(const fe::FrameContext& context, fe::Scene& scene) override;

    void set_state(BreakoutState* state) noexcept { state_ = state; }

private:
    // Puts the ball back at its launch position, motionless, ready to launch.
    static void respawn_ball(fe::Scene& scene);

    BreakoutState* state_ = nullptr;
};

// Writes the score and the lives into the scene's text labels every frame, and
// the win/lose result into the game-over screen's label, so the player can read
// the game's state on screen instead of in the log. It is the last system in the
// schedule: it reads the state the systems above it produced, and nothing reads
// its output.
//
// It owns no scene data of its own. The labels are created by the scene builder
// (or loaded from the authored document) and this system only fills in their
// text, which is why it is safe to rebuild the field without touching it. The
// canvas each label lives under is shown and hidden by MenuSystem; this system
// only writes text.
class HudSystem final : public fe::System {
public:
    [[nodiscard]] std::string_view name() const noexcept override { return "HudSystem"; }

    void update(const fe::FrameContext& context, fe::Scene& scene) override;

    void set_state(BreakoutState* state) noexcept { state_ = state; }

private:
    BreakoutState* state_ = nullptr;
};

// Drives the screen flow: title -> play -> game over -> title, with a pause in
// between. This is the menu half of `slice-title-and-menus`.
//
// It is the one system that *decides* rather than reacts, because a menu is the
// one place a press means "change what the game is doing" rather than "do
// something in the game". It reads the same gameplay actions the paddle and the
// launcher read — so a tap, a click and a key all advance a menu — plus the UI
// click state `UiInputSystem` wrote earlier this frame, so a button is a real
// hit-tested target rather than a full-screen "any press" zone.
//
// It owns no scene data: the menu canvases are built once by the scene builder
// and this system only toggles their `visible` flag to match the screen, so
// switching screens is one bool per canvas, not a rebuild. The gameplay systems
// read the state's screen and freeze themselves accordingly, which is what makes
// the title and the pause actually stop the game rather than draw over it.
class MenuSystem final : public fe::System {
public:
    [[nodiscard]] std::string_view name() const noexcept override { return "MenuSystem"; }

    void update(const fe::FrameContext& context, fe::Scene& scene) override;

    void set_state(BreakoutState* state) noexcept { state_ = state; }

    // The action that advances a menu (start the game, return to the title). It
    // is breakout's existing launch action: the same "go" press that serves the
    // ball starts a game, so a player learns one button. Zero means unbound and
    // the Space/Enter fallback applies.
    void set_confirm_action(fe::ActionMap::Id confirm) noexcept { confirm_ = confirm; }

    // The action that pauses and resumes a live game. Zero means unbound and the
    // P key is the fallback.
    void set_pause_action(fe::ActionMap::Id pause) noexcept { pause_ = pause; }

private:
    // Shows the canvas for the current screen and hides the others. Idempotent,
    // so it is safe to call every frame; the flag write is cheap and keeps the
    // scene honest even if something else cleared it.
    static void apply_visibility(fe::Scene& scene, BreakoutState::Screen screen);

    // Saves and restores the ball's motion around a pause, so a paused rally
    // resumes exactly where it stopped. A no-op when there is no ball.
    void save_ball_velocity(fe::Scene& scene) noexcept;
    void restore_ball_velocity(fe::Scene& scene) const noexcept;

    BreakoutState* state_ = nullptr;
    fe::ActionMap::Id confirm_ = 0;
    fe::ActionMap::Id pause_ = 0;

    // The ball's motion, saved when the game is paused and restored when it
    // resumes. Physics has no pause switch, so a paused game freezes the ball by
    // stopping it; keeping the velocity it had means resuming continues the rally
    // instead of leaving the ball dead at the paddle.
    fe::Vec2 paused_ball_velocity_{};
    fe::f32 paused_ball_angular_velocity_ = 0.0f;
};

} // namespace breakout
