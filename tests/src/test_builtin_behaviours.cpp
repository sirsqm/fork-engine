// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
//
// Tests for the engine-shipped gameplay behaviours (fe/player/, ADR-0078).
//
// These are pure logic over a Scene, a ScriptHost and a FrameContext, so no
// window, no scene file, and no browser is involved: the same reason the
// action-layer tests call InputState's on_*() methods directly. What is being
// proved is that the behaviours a web build ships — the only gameplay a browser
// can run, since it has no dynamic loader — actually respond to the standard
// actions, and that the standard bindings drive them from both a keyboard and a
// touch.
//
// Every behaviour is attached through ScriptHost, not by calling Script::attach
// directly. The host is what binds a script to its node; a direct call would
// leave node() null and the test would silently assert on a behaviour that never
// ran. Using the host is also the production path (ScriptSystem drives it in the
// frame loop), so these tests exercise the wiring a game actually gets.

#include <doctest/doctest.h>

#include "fe/physics3d/Collider3D.h"
#include "fe/physics3d/Physics3DSystem.h"
#include "fe/physics3d/RigidBody3D.h"
#include "fe/platform/InputActions.h"
#include "fe/platform/InputState.h"
#include "fe/player/BuiltinBehaviours.h"
#include "fe/scene/Node.h"
#include "fe/scene/Scene.h"
#include "fe/script/ScriptHost.h"
#include "fe/script/ScriptRegistry.h"
#include "fe/systems/FrameContext.h"

#include <cmath>
#include <memory>

// doctest stringifies through std::ostream; MSVC's <string_view> does not pull
// it in (the trap in AGENTS.md).
#include <ostream>

using namespace fe;

namespace {

constexpr f32 kTolerance = 1e-4f;
constexpr Vec2 kViewport{1280.0f, 720.0f};

[[nodiscard]] bool nearly(f32 a, f32 b, f32 tolerance = kTolerance) {
    return std::fabs(a - b) <= tolerance;
}

// Binds a behaviour to `node` through the host, the way a scene load does, and
// returns the concrete instance so a test can read its state.
template <typename T>
[[nodiscard]] T* attach_behaviour(ScriptHost& host, Node& node) {
    auto script = std::make_unique<T>();
    T* raw = script.get();
    REQUIRE(host.attach_script(node, std::move(script)).ok());
    return raw;
}

// A frame's input, its action map evaluated from that input, and the context a
// behaviour is handed. Everything a behaviour reads lives here, so a test only
// has to press a key or place a finger.
struct Fixture {
    InputState input;
    ActionMap actions;
    FrameContext context;

    explicit Fixture(f32 delta = 1.0f / 60.0f) {
        player::declare_builtin_actions(actions);
        actions.update(input, kViewport);
        context.delta_time = delta;
        context.input = &input;
        context.actions = &actions;
    }

    // Presses `key` and re-evaluates the actions, which is exactly what the
    // runtime's frame loop does before the systems run.
    void press(Key key) {
        input.on_key(key, true);
        actions.update(input, kViewport);
    }

    void release(Key key) {
        input.on_key(key, false);
        actions.update(input, kViewport);
    }

    // Advances one frame with the input held as it is: the runtime's
    // begin_frame() moves current to previous, so a key held down stays down while
    // its press edge is a fact for exactly one frame. A test that steps a
    // behaviour across frames must call this, or a single press would look like a
    // key that is re-pressed every frame.
    void tick() {
        input.begin_frame();
        actions.update(input, kViewport);
    }

    // Puts a finger down in the middle of a normalised region, which is how the
    // touch bindings are exercised without a browser.
    void touch(u32 id, f32 nx, f32 ny) {
        input.on_touch(TouchPhase::Began, id, nx * kViewport.x, ny * kViewport.y);
        actions.update(input, kViewport);
    }
};

} // namespace

TEST_CASE("the built-in actions are declared once and findable by name") {
    ActionMap actions;
    player::declare_builtin_actions(actions);

    CHECK(actions.action_count() == 12);
    CHECK(actions.find(player::actions::kMoveForward) != ActionMap::kNone);
    CHECK(actions.find(player::actions::kRun) != ActionMap::kNone);
    CHECK(actions.find(player::actions::kJump) != ActionMap::kNone);
    CHECK(actions.find("NotAnAction") == ActionMap::kNone);
}

TEST_CASE("a key moves a MoveNode3D and the same touch region moves it too") {
    // The acceptance criterion in miniature: the identical behaviour responds to
    // a key and to a finger, because it reads actions rather than keys.
    Scene scene;
    Node& node = *scene.create_node("Player");
    ScriptHost host;

    SUBCASE("a keyboard press moves it forward") {
        Fixture fixture;
        fixture.press(Key::W);

        auto* move = attach_behaviour<player::MoveNode3D>(host, node);
        CHECK(move->update(fixture.context).ok());

        // Forward is the node's own -Z (the engine's facing convention).
        CHECK(nearly(node.transform().position.z, -3.0f / 60.0f));
        CHECK(nearly(node.transform().position.x, 0.0f));
    }

    SUBCASE("a touch in the forward region moves it the same way") {
        Fixture fixture;
        fixture.touch(1, 0.125f, 0.625f); // inside the lower-left forward region

        auto* move = attach_behaviour<player::MoveNode3D>(host, node);
        CHECK(move->update(fixture.context).ok());

        CHECK(nearly(node.transform().position.z, -3.0f / 60.0f));
    }
}

TEST_CASE("MoveNode3D holds its Y unless vertical movement is unlocked") {
    Scene scene;
    Node& node = *scene.create_node("Walker");
    ScriptHost host;
    auto* move = attach_behaviour<player::MoveNode3D>(host, node);

    Fixture fixture;
    fixture.press(Key::Space); // MoveUp
    CHECK(move->update(fixture.context).ok());
    // The default locks vertical motion, so a player pressing Space while
    // walking does not leave the floor.
    CHECK(nearly(node.transform().position.y, 0.0f));

    move->lock_vertical = false;
    CHECK(move->update(fixture.context).ok());
    CHECK(nearly(node.transform().position.y, 3.0f / 60.0f));
}

TEST_CASE("MoveNode3D moves in the node's own frame") {
    Scene scene;
    Node& node = *scene.create_node("Rotated");
    // Facing +X (a quarter turn from -Z).
    node.transform().rotation = Vec3{0.0f, -kPi * 0.5f, 0.0f};
    ScriptHost host;
    auto* move = attach_behaviour<player::MoveNode3D>(host, node);

    Fixture fixture;
    fixture.press(Key::W);
    CHECK(move->update(fixture.context).ok());

    // Forward is now +X, so the motion follows the node's facing rather than the
    // world axes.
    CHECK(nearly(node.transform().position.x, 3.0f / 60.0f));
    CHECK(nearly(node.transform().position.z, 0.0f));
}

TEST_CASE("the run action scales movement by the multiplier") {
    Scene scene;
    Node& node = *scene.create_node("Runner");
    ScriptHost host;
    auto* move = attach_behaviour<player::MoveNode3D>(host, node);

    Fixture fixture;
    fixture.press(Key::W);
    fixture.press(Key::LeftControl); // Run
    CHECK(move->update(fixture.context).ok());

    CHECK(nearly(node.transform().position.z, -(3.0f * 2.5f) / 60.0f));
}

TEST_CASE("FlyCamera3D moves along the direction it is looking") {
    Scene scene;
    Node& camera = *scene.create_node("Camera");
    ScriptHost host;
    auto* fly = attach_behaviour<player::FlyCamera3D>(host, camera);

    Fixture fixture;
    fixture.press(Key::W);
    CHECK(fly->update(fixture.context).ok());

    // With no rotation the camera looks down -Z, so forward motion is -Z.
    CHECK(nearly(camera.transform().position.z, -fly->move_speed / 60.0f));
    CHECK(nearly(camera.transform().position.x, 0.0f));

    // Turning right by a quarter turn and moving forward must now go +X. This is
    // the check that the movement basis and the camera's own view direction
    // agree — the bug the hand-derived basis had.
    camera.transform().position = Vec3{};
    fly->yaw = -kPi * 0.5f;
    CHECK(fly->update(fixture.context).ok());
    CHECK(nearly(camera.transform().position.x, fly->move_speed / 60.0f));
    CHECK(nearly(camera.transform().position.z, 0.0f));
}

TEST_CASE("FlyCamera3D looks with a key and with a touch strip") {
    Scene scene;
    Node& camera = *scene.create_node("Camera");
    ScriptHost host;
    auto* fly = attach_behaviour<player::FlyCamera3D>(host, camera);

    SUBCASE("the look-right key yaws") {
        Fixture fixture;
        fixture.press(Key::L);
        CHECK(fly->update(fixture.context).ok());
        CHECK(fly->yaw < 0.0f); // yaw decreases to look right in this convention
    }

    SUBCASE("a touch in the look strip yaws the same way") {
        Fixture fixture;
        fixture.touch(7, 0.875f, 0.75f); // the look-right region
        CHECK(fly->update(fixture.context).ok());
        CHECK(fly->yaw < 0.0f);
    }
}

TEST_CASE("FlyCamera3D keeps pitch inside the pole") {
    Scene scene;
    Node& camera = *scene.create_node("Camera");
    ScriptHost host;
    auto* fly = attach_behaviour<player::FlyCamera3D>(host, camera);

    Fixture fixture;
    fixture.press(Key::I); // LookUp
    for (int i = 0; i < 600; ++i) {
        CHECK(fly->update(fixture.context).ok());
    }

    // Never at or past a right angle, whatever the frame count, so the view can
    // never flip over the pole. Held long enough to be pinned against the clamp.
    CHECK(fly->pitch <= player::FlyCamera3D::kMaxPitchRadians);
    CHECK(nearly(fly->pitch, player::FlyCamera3D::kMaxPitchRadians, 1e-3f));
}

TEST_CASE("FlyCamera3D takes the node's authored orientation as its start") {
    Scene scene;
    Node& camera = *scene.create_node("Camera");
    camera.transform().rotation = Vec3{0.25f, 0.5f, 0.0f};
    ScriptHost host;
    auto* fly = attach_behaviour<player::FlyCamera3D>(host, camera);

    CHECK(nearly(fly->pitch, 0.25f));
    CHECK(nearly(fly->yaw, 0.5f));
}

TEST_CASE("SpinNode3D turns its node at a rate independent of the frame rate") {
    Scene scene;
    Node& node = *scene.create_node("Spinner");
    ScriptHost host;
    auto* spin = attach_behaviour<player::SpinNode3D>(host, node);

    // Two frames of half the duration must land in the same place as one frame
    // of the full duration: the spin is rate-based, not step-based.
    Fixture half(0.5f);
    CHECK(spin->update(half.context).ok());
    CHECK(spin->update(half.context).ok());
    const Vec3 two_half_frames = node.transform().rotation;

    Scene scene_other;
    Node& other = *scene_other.create_node("Spinner2");
    ScriptHost host_other;
    auto* spin_other = attach_behaviour<player::SpinNode3D>(host_other, other);
    Fixture whole(1.0f);
    CHECK(spin_other->update(whole.context).ok());

    CHECK(nearly(two_half_frames.y, other.transform().rotation.y, 1e-3f));
    CHECK(nearly(two_half_frames.y, radians(90.0f), 1e-3f));
}

TEST_CASE("SpinNode3D does nothing for a degenerate axis") {
    Scene scene;
    Node& node = *scene.create_node("Still");
    ScriptHost host;
    auto* spin = attach_behaviour<player::SpinNode3D>(host, node);
    spin->axis_x = 0.0f;
    spin->axis_y = 0.0f;
    spin->axis_z = 0.0f;

    Fixture fixture;
    CHECK(spin->update(fixture.context).ok());
    CHECK(nearly(node.transform().rotation.y, 0.0f));
}

TEST_CASE("a behaviour with no action map is inert rather than a crash") {
    // A headless tool or a test runs the schedule with no input and no actions;
    // the behaviour must simply not move (the null-check the FrameContext
    // contract requires).
    Scene scene;
    Node& moved_node = *scene.create_node("Headless");
    Node& flown_node = *scene.create_node("HeadlessCamera");
    ScriptHost host;
    // One script per node: attaching both to one node would replace the first,
    // so each behaviour gets its own node.
    auto* move = attach_behaviour<player::MoveNode3D>(host, moved_node);
    auto* fly = attach_behaviour<player::FlyCamera3D>(host, flown_node);

    FrameContext bare;
    bare.delta_time = 1.0f / 60.0f;
    CHECK(move->update(bare).ok());
    CHECK(fly->update(bare).ok());
    CHECK(nearly(moved_node.transform().position.x, 0.0f));
    CHECK(nearly(moved_node.transform().position.y, 0.0f));
    CHECK(nearly(moved_node.transform().position.z, 0.0f));
    CHECK(nearly(flown_node.transform().position.x, 0.0f));
    CHECK(nearly(flown_node.transform().position.y, 0.0f));
    CHECK(nearly(flown_node.transform().position.z, 0.0f));
}

TEST_CASE("the built-in behaviours register under the names a scene binds") {
    ScriptRegistry registry;
    player::register_builtin_behaviours(registry);

    for (const char* name : {"FlyCamera3D", "SpinNode3D", "MoveNode3D", "CharacterController3D", "FollowCamera3D"}) {
        std::unique_ptr<Script> script = registry.create(name);
        REQUIRE(script != nullptr);
        CHECK(script->name() == name);
    }
    CHECK(registry.create("NotABehaviour") == nullptr);
}

TEST_CASE("a registered behaviour carries its reflected tuning") {
    ScriptRegistry registry;
    player::register_builtin_behaviours(registry);

    const ScriptDesc* desc = registry.find("FlyCamera3D");
    REQUIRE(desc != nullptr);
    // The tuning a designer edits must be reflected, or it would neither persist
    // in the scene nor appear in the inspector (ADR-0063).
    CHECK(desc->properties.size() == 5);
    CHECK(desc->properties[0].name == "move_speed");
}

TEST_CASE("a script host drives the built-in behaviours through the ordinary path") {
    // The end-to-end shape a scene load produces: a behaviour attached to a node
    // and updated by the host, exactly as ScriptSystem drives it in the frame
    // loop. Nothing here is web-specific, which is the point — the same host runs
    // in a browser.
    Scene scene;
    Node& node = *scene.create_node("Spinner");
    ScriptRegistry registry;
    player::register_builtin_behaviours(registry);

    ScriptHost host;
    std::unique_ptr<Script> script = registry.create("SpinNode3D");
    REQUIRE(script != nullptr);
    CHECK(host.attach_script(node, std::move(script)).ok());
    CHECK(host.script_count() == 1);

    FrameContext context;
    context.delta_time = 0.5f;
    host.update(context);
    CHECK(nearly(node.transform().rotation.y, radians(45.0f), 1e-3f));
}

// --- CharacterController3D -------------------------------------------------
//
// A controller test needs a floor to stand on, so these build a small world: a
// static floor node (a Collider3D and no body) and the character (a Collider3D
// and a body). That is the same composition a scene file describes, so the tests
// exercise the queries against real colliders rather than a stub.

namespace {

// The character's collider: a 1x1x1 box, so its half-height is 0.5 and its centre
// sits 0.5 above the floor when grounded.
void add_body(Node& node) {
    auto* collider = node.add_component<physics3d::Collider3D>();
    collider->shape = physics3d::ShapeType3D::Box;
    collider->half_extent = Vec3{0.5f, 0.5f, 0.5f};
    node.add_component<physics3d::RigidBody3D>();
}

// A wide, thin static floor centred at the origin, its top surface at y = 0.
Node& add_floor(Scene& scene) {
    Node& floor = *scene.create_node("Floor");
    floor.transform().position = Vec3{0.0f, -0.5f, 0.0f};
    auto* collider = floor.add_component<physics3d::Collider3D>();
    collider->shape = physics3d::ShapeType3D::Box;
    collider->half_extent = Vec3{20.0f, 0.5f, 20.0f};
    return floor;
}

} // namespace

TEST_CASE("the character falls until it finds the ground and then stays there") {
    Scene scene;
    add_floor(scene);
    Node& character = *scene.create_node("Player");
    character.transform().position = Vec3{0.0f, 3.0f, 0.0f};
    add_body(character);

    ScriptHost host;
    auto* controller = attach_behaviour<player::CharacterController3D>(host, character);
    Fixture fixture;

    // It starts in the air and is not grounded, so the first step asks the body
    // to fall: with a rigid body the controller writes a velocity and leaves the
    // transform to the physics step.
    CHECK_FALSE(controller->grounded());
    CHECK(controller->update(fixture.context).ok());
    auto* body = character.get_component<physics3d::RigidBody3D>();
    REQUIRE(body != nullptr);
    CHECK(body->velocity.y < 0.0f);

    // It lands: after enough steps the body is resting on the floor and reports
    // itself grounded. The body is moved by the physics step, so the test runs
    // the real solver rather than integrating the fall by hand. The loop runs a
    // fixed count rather than stopping at the first grounded frame, so the
    // assertion below is on the settled height, not on the frame the probe first
    // reached the floor.
    physics3d::Physics3DSystem physics;
    FrameContext step = fixture.context;
    for (int i = 0; i < 240; ++i) {
        CHECK(controller->update(step).ok());
        physics.update(step, scene);
    }
    CHECK(controller->grounded());
    // The centre rests one half-height above the floor's surface, to within the
    // solver's penetration slop.
    INFO("resting height: ", character.transform().position.y);
    CHECK(nearly(character.transform().position.y, 0.5f, 0.02f));
}

TEST_CASE("a jump lifts a grounded character but is ignored in mid-air") {
    Scene scene;
    add_floor(scene);
    Node& character = *scene.create_node("Player");
    character.transform().position = Vec3{0.0f, 0.5f, 0.0f};
    add_body(character);

    ScriptHost host;
    auto* controller = attach_behaviour<player::CharacterController3D>(host, character);
    Fixture fixture;
    physics3d::Physics3DSystem physics;

    // Settle on the floor first, so the jump starts from a grounded state.
    for (int i = 0; i < 30; ++i) {
        CHECK(controller->update(fixture.context).ok());
        physics.update(fixture.context, scene);
        fixture.tick();
    }
    REQUIRE(controller->grounded());

    // A press on the ground is a jump: the character leaves the floor.
    fixture.press(Key::Space);
    CHECK(controller->update(fixture.context).ok());
    physics.update(fixture.context, scene);
    fixture.tick();
    CHECK_FALSE(controller->grounded());
    const f32 airborne_height = character.transform().position.y;
    CHECK(airborne_height > 0.5f);

    // A second press while airborne is ignored: the character is already off the
    // ground, so there is nothing to push away from. Without this the jump key
    // would be continuous thrust and the character could fly.
    fixture.press(Key::Space);
    CHECK(controller->update(fixture.context).ok());
    physics.update(fixture.context, scene);
    fixture.tick();
    // Gravity is still winning, so the height has not been re-boosted upward by
    // the second press.
    CHECK(character.transform().position.y < airborne_height + 0.5f);
}

TEST_CASE("a jump is a press, not a held key") {
    // The edge semantics that make a jump a jump: holding Space must not be a
    // continuous thrust. Space is pressed once and held down for a whole arc, and
    // the character must come back down and land.
    Scene scene;
    add_floor(scene);
    Node& character = *scene.create_node("Player");
    character.transform().position = Vec3{0.0f, 0.5f, 0.0f};
    add_body(character);

    ScriptHost host;
    auto* controller = attach_behaviour<player::CharacterController3D>(host, character);
    physics3d::Physics3DSystem physics;
    Fixture fixture;

    for (int i = 0; i < 30; ++i) {
        CHECK(controller->update(fixture.context).ok());
        physics.update(fixture.context, scene);
        fixture.tick();
    }
    REQUIRE(controller->grounded());

    fixture.press(Key::Space);
    f32 peak = character.transform().position.y;
    bool landed_again = false;
    for (int i = 0; i < 240; ++i) {
        CHECK(controller->update(fixture.context).ok());
        physics.update(fixture.context, scene);
        // The key is still held, but begin_frame() makes its press edge a fact of
        // one frame only, so this is a held key and not a re-press.
        fixture.tick();
        peak = std::max(peak, character.transform().position.y);
        if (i > 10 && controller->grounded()) {
            landed_again = true;
            break;
        }
    }
    INFO("peak height: ", peak);
    CHECK(peak > 0.5f + 0.2f);
    CHECK(landed_again);
}

TEST_CASE("the turn actions yaw the character and the move follows the facing") {
    Scene scene;
    // No rigid body here: the controller then moves the transform directly, so
    // the facing maths is observable without running a physics step.
    Node& character = *scene.create_node("Player");
    ScriptHost host;
    auto* controller = attach_behaviour<player::CharacterController3D>(host, character);

    // Facing -Z at yaw 0, forward is -Z.
    {
        Fixture fixture;
        fixture.press(Key::W);
        CHECK(controller->update(fixture.context).ok());
        CHECK(nearly(controller->yaw(), 0.0f));
        CHECK(character.transform().position.z < 0.0f);
        CHECK(nearly(character.transform().position.x, 0.0f));
    }

    // A right turn of 90 degrees faces -X, and forward now moves along -X.
    Node& turned = *scene.create_node("Turned");
    auto* turning = attach_behaviour<player::CharacterController3D>(host, turned);
    Fixture turn_fixture;
    turn_fixture.press(Key::D);
    // Enough frames at the fixture's step to pass a quarter turn at the default
    // rate, so the facing is a clean -X. The turn rate is degrees per second, so
    // the frames needed are (90 / turn_speed) / delta.
    const int frames =
        static_cast<int>(radians(90.0f) / radians(turning->turn_speed) / turn_fixture.context.delta_time);
    for (int i = 0; i < frames; ++i) {
        CHECK(turning->update(turn_fixture.context).ok());
    }
    CHECK(nearly(std::fabs(turning->yaw()), radians(90.0f), 0.05f));

    // Walking forward now moves along the new facing. A right turn swings the
    // facing from -Z towards +X, so forward is now +X: the movement direction
    // followed the turn rather than staying in the world.
    Fixture walk_fixture;
    walk_fixture.press(Key::W);
    const Vec3 before = turned.transform().position;
    for (int i = 0; i < 30; ++i) {
        CHECK(turning->update(walk_fixture.context).ok());
        walk_fixture.tick();
    }
    const Vec3 moved = turned.transform().position - before;
    INFO("moved: ", moved.x, ", ", moved.z);
    CHECK(moved.x > 0.01f);
    CHECK(std::fabs(moved.z) < std::fabs(moved.x));
}

TEST_CASE("the character stays upright and only its yaw is written back") {
    Scene scene;
    Node& character = *scene.create_node("Player");
    add_body(character);
    // An authored tilt must not survive: a character is always upright, which is
    // also what keeps the axis-aligned collider honest.
    character.transform().rotation = Vec3{0.3f, 0.0f, 0.4f};

    ScriptHost host;
    auto* controller = attach_behaviour<player::CharacterController3D>(host, character);
    Fixture fixture;
    CHECK(controller->update(fixture.context).ok());
    CHECK(nearly(character.transform().rotation.x, 0.0f));
    CHECK(nearly(character.transform().rotation.z, 0.0f));
}

TEST_CASE("attaching the controller turns the body's own gravity off") {
    // The script owns the vertical axis; a body left under the solver's gravity
    // would fight it for Y. The controller asserts this on attach rather than
    // trusting the scene author to remember.
    Scene scene;
    Node& character = *scene.create_node("Player");
    add_body(character);
    auto* body = character.get_component<physics3d::RigidBody3D>();
    REQUIRE(body != nullptr);
    CHECK(body->affected_by_gravity);

    ScriptHost host;
    auto* controller = attach_behaviour<player::CharacterController3D>(host, character);
    CHECK(controller != nullptr);
    CHECK_FALSE(body->affected_by_gravity);
}

TEST_CASE("the controller runs with no action map and with no scene") {
    // The FrameContext contract: a headless tool runs the schedule with no input
    // and a bare scene. The controller must be inert, not a crash.
    Scene scene;
    Node& character = *scene.create_node("Player");
    add_body(character);

    ScriptHost host;
    auto* controller = attach_behaviour<player::CharacterController3D>(host, character);
    FrameContext bare;
    bare.delta_time = 1.0f / 60.0f;
    CHECK(controller->update(bare).ok());
    CHECK_FALSE(controller->grounded());
    CHECK(nearly(character.transform().position.x, 0.0f));
    CHECK(nearly(character.transform().position.z, 0.0f));
}

TEST_CASE("the controller asks a body for its motion rather than moving it directly") {
    // The seam that lets the solver do the collision: with a rigid body present
    // the controller writes a velocity and leaves the transform alone, so the
    // physics step is what moves (and stops) the character.
    Scene scene;
    Node& character = *scene.create_node("Player");
    add_body(character);
    auto* body = character.get_component<physics3d::RigidBody3D>();
    REQUIRE(body != nullptr);

    ScriptHost host;
    auto* controller = attach_behaviour<player::CharacterController3D>(host, character);
    Fixture fixture;
    fixture.press(Key::W);
    CHECK(controller->update(fixture.context).ok());

    // A velocity was handed over and the transform was not touched: the fall is
    // the body's to apply.
    CHECK(body->velocity.z < 0.0f);
    CHECK(nearly(character.transform().position.x, 0.0f));
    CHECK(nearly(character.transform().position.z, 0.0f));
}

// --- FollowCamera3D --------------------------------------------------------

TEST_CASE("the follow camera trails its named target from behind and above") {
    Scene scene;
    Node& target = *scene.create_node("Player");
    target.transform().position = Vec3{0.0f, 0.0f, 0.0f};
    Node& camera = *scene.create_node("Camera");
    // Start the camera where it will end up, so smoothing does not hide the
    // steady-state offset this test asserts.
    camera.transform().position = Vec3{0.0f, 3.2f, 7.0f};

    ScriptHost host;
    auto* follow = attach_behaviour<player::FollowCamera3D>(host, camera);
    follow->smoothing = 0.0f;
    follow->target = "Player";
    Fixture fixture;

    CHECK(follow->update(fixture.context).ok());
    // At yaw 0 the target faces -Z, so "behind" is +Z.
    CHECK(nearly(camera.transform().position.z, 7.0f));
    CHECK(nearly(camera.transform().position.x, 0.0f));
    CHECK(nearly(camera.transform().position.y, 3.2f));
}

TEST_CASE("the follow camera swings round behind the target as it turns") {
    Scene scene;
    Node& target = *scene.create_node("Player");
    Node& camera = *scene.create_node("Camera");

    ScriptHost host;
    auto* follow = attach_behaviour<player::FollowCamera3D>(host, camera);
    follow->smoothing = 0.0f;
    Fixture fixture;
    REQUIRE(follow->update(fixture.context).ok());

    // Turn the target a quarter turn; the camera must move to the other side,
    // which is what keeps it behind the character rather than fixed in the world.
    target.transform().rotation = Vec3{0.0f, radians(90.0f), 0.0f};
    REQUIRE(follow->update(fixture.context).ok());
    // Facing -X now, so behind is +X.
    CHECK(camera.transform().position.x > 5.0f);
    CHECK(nearly(camera.transform().position.z, 0.0f, 0.1f));
}

TEST_CASE("the follow camera is inert with no target name or no such node") {
    Scene scene;
    Node& camera = *scene.create_node("Camera");
    ScriptHost host;
    auto* follow = attach_behaviour<player::FollowCamera3D>(host, camera);
    Fixture fixture;

    follow->target.clear();
    CHECK(follow->update(fixture.context).ok());
    CHECK(nearly(camera.transform().position.x, 0.0f));

    follow->target = "NoSuchNode";
    CHECK(follow->update(fixture.context).ok());
    CHECK(nearly(camera.transform().position.x, 0.0f));
}

TEST_CASE("the follow camera reconnects when its target is replaced") {
    // A scene reload gives the target a new id under the same name; the camera
    // must follow the new node rather than hold the old one.
    Scene scene;
    Node& first = *scene.create_node("Player");
    Node& camera = *scene.create_node("Camera");
    ScriptHost host;
    auto* follow = attach_behaviour<player::FollowCamera3D>(host, camera);
    follow->smoothing = 0.0f;
    Fixture fixture;
    REQUIRE(follow->update(fixture.context).ok());

    std::unique_ptr<Node> removed = scene.remove_root(&first);
    REQUIRE(removed != nullptr);
    removed.reset();
    Node& second = *scene.create_node("Player");
    second.transform().position = Vec3{10.0f, 0.0f, 0.0f};
    REQUIRE(follow->update(fixture.context).ok());
    CHECK(camera.transform().position.x > 9.0f);
}
