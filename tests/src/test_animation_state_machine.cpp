// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include <doctest/doctest.h>

#include "fe/anim/AnimationStateMachine.h"
#include "fe/anim/AnimationStateMachineSystem.h"
#include "fe/anim/AnimationTypes.h"
#include "fe/reflect/ComponentTypes.h"
#include "fe/scene/Node.h"
#include "fe/scene/Scene.h"
#include "fe/scene/SpriteAnimator.h"
#include "fe/serialization/SceneSerializer.h"

#include <ostream>
#include <string>
#include <string_view>

using namespace fe;

namespace {

// A looping clip and a one-shot clip, the two shapes every state machine needs.
AnimationState looping(std::string name, u32 first, u32 count, f32 fps = 10.0f) {
    AnimationState state;
    state.name = std::move(name);
    state.first_frame = first;
    state.frame_count = count;
    state.fps = fps;
    state.loop = true;
    return state;
}

AnimationState one_shot(std::string name, u32 first, u32 count, f32 fps = 10.0f) {
    AnimationState state = looping(std::move(name), first, count, fps);
    state.loop = false;
    return state;
}

AnimationTransition transition(std::string from, std::string to, bool require_finished = false,
                               std::vector<std::string> conditions = {}) {
    AnimationTransition edge;
    edge.from = std::move(from);
    edge.to = std::move(to);
    edge.require_finished = require_finished;
    edge.conditions = std::move(conditions);
    return edge;
}

FrameContext frame(f32 delta) {
    FrameContext context;
    context.delta_time = delta;
    context.elapsed_time = static_cast<f64>(delta);
    return context;
}

} // namespace

TEST_CASE("a state machine finds and enters states by name") {
    AnimationStateMachine machine;
    machine.states = {looping("idle", 0, 4), looping("run", 4, 4)};

    CHECK(machine.find_state("idle") == 0);
    CHECK(machine.find_state("run") == 1);
    CHECK(machine.find_state("missing") == -1);
    CHECK(machine.active_state() == -1);
    CHECK(machine.active_state_name().empty());

    CHECK(machine.play("run"));
    CHECK(machine.active_state() == 1);
    CHECK(machine.active_state_name() == "run");
    CHECK(machine.clip_frame() == 0);

    // Playing an unknown state changes nothing and reports the miss.
    CHECK_FALSE(machine.play("missing"));
    CHECK(machine.active_state() == 1);
}

TEST_CASE("advance enters the initial state, then the first state as a fallback") {
    AnimationStateMachine explicit_initial;
    explicit_initial.states = {looping("idle", 0, 4), looping("run", 4, 4)};
    explicit_initial.initial_state = "run";
    explicit_initial.advance(0.1f);
    CHECK(explicit_initial.active_state_name() == "run");

    AnimationStateMachine default_initial;
    default_initial.states = {looping("idle", 0, 4), looping("run", 4, 4)};
    default_initial.advance(0.1f);
    CHECK(default_initial.active_state_name() == "idle");

    // An empty graph is a well-defined no-op rather than a crash.
    AnimationStateMachine empty;
    empty.advance(0.1f);
    CHECK(empty.active_state() == -1);
    CHECK(empty.clip_frame() == 0);
}

TEST_CASE("a looping state advances and wraps without ever finishing") {
    AnimationStateMachine machine;
    machine.states = {looping("idle", 0, 4, 10.0f)};
    machine.initial_state = "idle";

    // The clip advances on the same frame the state is entered, matching how
    // SpriteAnimationSystem advances its own clock: the first 0.1s step at 10fps
    // is one whole frame.
    const u32 expected[] = {1, 2, 3, 0, 1};
    for (u32 frame : expected) {
        machine.advance(0.1f);
        CHECK(machine.clip_frame() == frame);
        CHECK_FALSE(machine.state_finished());
    }
}

TEST_CASE("a one-shot state clamps on its last frame and reports finished") {
    AnimationStateMachine machine;
    machine.states = {one_shot("attack", 0, 3, 10.0f)};
    machine.initial_state = "attack";

    machine.advance(0.1f);
    CHECK(machine.clip_frame() == 1);
    CHECK_FALSE(machine.state_finished());

    machine.advance(0.1f);
    CHECK(machine.clip_frame() == 2);
    CHECK(machine.state_finished());

    // Further frames stay on the last cell; it never wraps.
    machine.advance(0.1f);
    CHECK(machine.clip_frame() == 2);
    CHECK(machine.state_finished());
}

TEST_CASE("a require_finished transition fires only once the source clip ends") {
    AnimationStateMachine machine;
    machine.states = {one_shot("attack", 0, 3, 10.0f), looping("idle", 0, 4)};
    machine.transitions = {transition("attack", "idle", true)};
    machine.initial_state = "attack";

    machine.advance(0.1f);
    CHECK(machine.active_state_name() == "attack");

    // The clip finishes on the second frame, so the edge fires then.
    machine.advance(0.1f);
    CHECK(machine.active_state_name() == "idle");
    CHECK_FALSE(machine.state_finished());
}

TEST_CASE("a trigger condition fires a transition and is consumed") {
    AnimationStateMachine machine;
    machine.states = {looping("idle", 0, 4), looping("run", 4, 4)};
    machine.transitions = {
        transition("", "run", false, {"move"}),
        transition("run", "idle", false, {"stop"}),
    };
    machine.initial_state = "idle";

    // No trigger yet: the machine stays in idle.
    machine.advance(0.1f);
    CHECK(machine.active_state_name() == "idle");

    // Setting the trigger fires the "any state -> run" edge, which consumes it.
    machine.set_trigger("move");
    CHECK(machine.has_trigger("move"));
    machine.advance(0.1f);
    CHECK(machine.active_state_name() == "run");
    CHECK_FALSE(machine.has_trigger("move"));

    // With the trigger consumed, the machine stays in run.
    machine.advance(0.1f);
    CHECK(machine.active_state_name() == "run");

    // The reverse edge needs its own trigger.
    machine.set_trigger("stop");
    machine.advance(0.1f);
    CHECK(machine.active_state_name() == "idle");
}

TEST_CASE("a self-edge with no condition never fires") {
    // Without the guard, an unconditional self-edge would re-enter its own state
    // every frame. The machine must ignore it.
    AnimationStateMachine machine;
    machine.states = {looping("idle", 0, 4)};
    machine.transitions = {transition("idle", "idle")};
    machine.initial_state = "idle";

    machine.advance(0.1f);
    CHECK(machine.active_state_name() == "idle");
    machine.advance(0.1f);
    CHECK(machine.active_state_name() == "idle");
}

TEST_CASE("a paused machine keeps its state and clip position") {
    AnimationStateMachine machine;
    machine.states = {looping("idle", 0, 4, 10.0f)};
    machine.initial_state = "idle";

    machine.advance(0.1f);
    const u32 frozen = machine.clip_frame();

    machine.playing = false;
    machine.advance(0.1f);
    machine.advance(0.1f);
    CHECK(machine.clip_frame() == frozen);
    CHECK(machine.active_state_name() == "idle");
}

TEST_CASE("triggers are a set: setting twice and clearing are well defined") {
    AnimationStateMachine machine;
    machine.set_trigger("a");
    machine.set_trigger("a");
    CHECK(machine.has_trigger("a"));

    machine.clear_trigger("a");
    CHECK_FALSE(machine.has_trigger("a"));

    // Clearing an unset trigger is harmless.
    machine.clear_trigger("missing");
    CHECK_FALSE(machine.has_trigger("missing"));
}

TEST_CASE("the graph round-trips through to_json and from_json") {
    AnimationStateMachine machine;
    machine.states = {looping("idle", 0, 4), one_shot("attack", 8, 3, 12.0f)};
    machine.transitions = {transition("idle", "attack", false, {"hit"}), transition("attack", "idle", true)};
    machine.initial_state = "idle";
    machine.playing = false;
    machine.drive_animator = false;

    const std::string text = machine.to_json();

    AnimationStateMachine restored;
    REQUIRE(restored.from_json(text));
    CHECK(restored.states.size() == 2);
    CHECK(restored.states[0].name == "idle");
    CHECK(restored.states[0].frame_count == 4);
    CHECK(restored.states[0].loop == true);
    CHECK(restored.states[1].name == "attack");
    CHECK(restored.states[1].first_frame == 8);
    CHECK(restored.states[1].fps == doctest::Approx(12.0f));
    CHECK(restored.states[1].loop == false);
    CHECK(restored.transitions.size() == 2);
    CHECK(restored.transitions[0].conditions == std::vector<std::string>{"hit"});
    CHECK(restored.transitions[1].require_finished == true);
    CHECK(restored.initial_state == "idle");
    CHECK(restored.playing == false);
    CHECK(restored.drive_animator == false);
}

TEST_CASE("a malformed graph document is rejected without clobbering the machine") {
    AnimationStateMachine machine;
    machine.states = {looping("idle", 0, 4)};

    CHECK_FALSE(machine.from_json("this is not json"));
    CHECK_FALSE(machine.from_json("[1, 2, 3]"));
    // The original graph survives a rejected parse.
    REQUIRE(machine.states.size() == 1);
    CHECK(machine.states[0].name == "idle");
}

TEST_CASE("the driving system writes the active state's clip onto the animator") {
    Scene scene;
    Node* node = scene.create_node("Actor");
    auto* machine = node->add_component<AnimationStateMachine>();
    auto* animator = node->add_component<SpriteAnimator>();
    animator->frames_x = 8;
    animator->frames_y = 1;
    machine->states = {looping("run", 2, 4, 10.0f)};
    machine->initial_state = "run";

    AnimationStateMachineSystem system;
    system.update(frame(0.1f), scene);
    CHECK(system.last_advanced_count() == 1);
    // One frame into the clip, over a first_frame of 2, is cell 3.
    CHECK(animator->current_frame == 3);

    system.update(frame(0.1f), scene);
    CHECK(animator->current_frame == 4);
}

TEST_CASE("the driving system leaves the animator alone when drive_animator is false") {
    Scene scene;
    Node* node = scene.create_node("Actor");
    auto* machine = node->add_component<AnimationStateMachine>();
    auto* animator = node->add_component<SpriteAnimator>();
    animator->frames_x = 8;
    animator->frames_y = 1;
    animator->current_frame = 7;
    machine->states = {looping("run", 2, 4, 10.0f)};
    machine->initial_state = "run";
    machine->drive_animator = false;

    AnimationStateMachineSystem system;
    system.update(frame(0.1f), scene);

    // The machine advanced, but it wrote nothing to the animator.
    CHECK(machine->active_state_name() == "run");
    CHECK(animator->current_frame == 7);
}

TEST_CASE("the driving system ignores nodes with no state machine") {
    Scene scene;
    Node* plain = scene.create_node("Plain");
    plain->add_component<SpriteAnimator>();

    AnimationStateMachineSystem system;
    system.update(frame(0.1f), scene);
    CHECK(system.last_advanced_count() == 0);
}

TEST_CASE("a state machine round-trips through the scene serializer") {
    // The scene loader registers only the built-in types, so register the anim
    // types explicitly here, exactly as an Engine or a tool would.
    reflect::register_builtin_types();
    reflect::register_anim_types();

    Scene scene;
    Node* node = scene.create_node("Actor");
    auto* machine = node->add_component<AnimationStateMachine>();
    machine->states = {looping("idle", 0, 4), one_shot("attack", 4, 3)};
    machine->transitions = {transition("idle", "attack", false, {"hit"})};
    machine->initial_state = "idle";

    auto json = serialization::scene::to_json(scene);
    REQUIRE(json);

    Scene loaded;
    REQUIRE_FALSE(serialization::scene::from_json(json.value(), loaded));

    Node* loaded_node = loaded.find_node_by_name("Actor");
    REQUIRE(loaded_node != nullptr);
    const auto* loaded_machine = loaded_node->get_component<AnimationStateMachine>();
    REQUIRE(loaded_machine != nullptr);
    REQUIRE(loaded_machine->states.size() == 2);
    CHECK(loaded_machine->states[0].name == "idle");
    CHECK(loaded_machine->states[1].name == "attack");
    REQUIRE(loaded_machine->transitions.size() == 1);
    CHECK(loaded_machine->transitions[0].conditions == std::vector<std::string>{"hit"});
    CHECK(loaded_machine->initial_state == "idle");
}
