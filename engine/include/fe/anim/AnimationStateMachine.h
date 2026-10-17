// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Types.h"
#include "fe/scene/Component.h"

#include <string>
#include <string_view>
#include <vector>

namespace fe {

// A small animation state machine for a node's SpriteAnimator.
//
// SpriteAnimator (the M2 prerequisite) plays one sprite-sheet loop. It cannot
// express the thing every game needs next: "idle plays until the player moves,
// then run plays; attack interrupts and returns to idle when it finishes". That
// is a *state machine*, and it belongs to gameplay, not to the frame-stepping
// animator.
//
// This component is that machine. It holds a list of named states, each naming
// a SpriteAnimator clip to play, and a list of transitions between them, each
// gated on a condition. The AnimationStateMachineSystem advances every machine
// once per frame and writes the active state's clip onto the node's animator,
// so the existing SpriteAnimationSystem keeps stepping the sheet unchanged. The
// machine is the authoring representation (states, transitions, and conditions
// are data the editor shows); the system is the behaviour (ADR-0002).
//
// Why not a Script (ADR-0044)? A script is arbitrary game code and carries no
// structure the editor can present. A state machine is a well-understood,
// *structured* behaviour: an editor can list its states and draw its
// transitions, and a designer can author one without writing code. Keeping it a
// component is what makes it serializable and inspectable like every other
// piece of authored data.
//
// This is the frame-playback cut of the M6 "animation state machines" row:
// states, transitions, conditions, entry/exit, and the animation clip a state
// plays. Blend trees, parameters with ranges, and skeletal states build on this
// shape and are deliberately not here yet.
//
// Serialization: the graph is a variable-length list of structs, which the
// reflection model (ADR-0005, ADR-0006) deliberately does not express as a
// property (its Value set is closed and scalar or fixed-group). The states and
// transitions are therefore registered as *serialized properties* only for the
// values a document authors inline; a scene carries them through the JSON
// `components` array the same way any other component does, and the component
// serializes its own graph through to_json/from_json below, which the scene
// serializer round-trips. See ADR-0059.

// One state of an animation state machine.
//
// A state plays the cells [first_frame, first_frame + frame_count) of the
// animator's sheet at `fps`, optionally looping. The frames are absolute indices
// into the sheet grid, so several states can share one sheet as clips, which is
// the usual way a character is authored (one sheet, one clip per row).
struct AnimationState {
    std::string name;
    u32 first_frame = 0;
    u32 frame_count = 1;
    f32 fps = 8.0f;
    bool loop = true;
};

// A directed transition between two states.
//
// `from` and `to` name states. An empty `from` is the "any state" source: the
// transition is considered from every state, which is how an interrupt ("attack
// cancels anything") is authored without repeating the edge on each state.
//
// Conditions are combined with AND. A transition with no conditions fires as
// soon as its source state is active and it is not `require_finished` (or the
// source has finished), which is a plain unconditional edge.
struct AnimationTransition {
    std::string from; // empty means "any state"
    std::string to;
    bool require_finished = false;       // fire only once the source state's clip finished
    std::vector<std::string> conditions; // triggers that must all be set; combined with AND
};

// The state machine. See the file comment for the model.
class AnimationStateMachine final : public Component {
public:
    static constexpr const char* kTypeName = "AnimationStateMachine";

    [[nodiscard]] const char* type_name() const noexcept override { return kTypeName; }

    // The authored graph.
    std::vector<AnimationState> states;
    std::vector<AnimationTransition> transitions;

    // The state playback starts in, by name. Empty means "the first state".
    std::string initial_state;

    // When true the machine does not advance. The active state and its elapsed
    // time are kept, so pausing and resuming is exact.
    bool playing = true;

    // When true the machine writes the active state's clip onto the node's
    // SpriteAnimator each frame, which is how the machine drives playback.
    bool drive_animator = true;

    // --- Authored / queried graph -----------------------------------------

    // The index of the named state, or -1.
    [[nodiscard]] i32 find_state(std::string_view name) const noexcept;

    // The index of the machine's active state, or -1 when the graph is empty.
    [[nodiscard]] i32 active_state() const noexcept { return active_; }

    // The name of the active state, or "" when the graph is empty.
    [[nodiscard]] std::string_view active_state_name() const noexcept;

    // True once the active state's clip has reached its last frame. A looping
    // state never reports finished.
    [[nodiscard]] bool state_finished() const noexcept { return finished_; }

    // The frame within the active state's clip, in [0, frame_count). For a
    // looping state this wraps; for a one-shot state it clamps to the last
    // frame. Zero when the graph is empty. The driving system uses this to
    // compute the animator's absolute cell.
    [[nodiscard]] u32 clip_frame() const noexcept;

    // --- Playback ----------------------------------------------------------

    // Enters the named state, restarting its clip. Returns false (and changes
    // nothing) when no state has that name. A transition is evaluated before
    // this, so the usual caller is the system, not gameplay.
    bool play(std::string_view state_name);

    // Sets a trigger true. Triggers are edge events consumed by the transition
    // that fires on them, so a trigger fires at most one transition per frame.
    void set_trigger(std::string_view name);

    // Clears a trigger without firing anything.
    void clear_trigger(std::string_view name);

    [[nodiscard]] bool has_trigger(std::string_view name) const noexcept;

    // Advances the machine by `delta_time` seconds and applies the first
    // transition whose conditions hold. Idempotent when the graph is empty or
    // the machine is paused. Called by AnimationStateMachineSystem once per
    // frame; exposed for tests.
    void advance(f32 delta_time);

    // --- Persistence -------------------------------------------------------

    // Serializes the graph to the compact string the reflection String property
    // stores, and parses it back. The format is the engine's own and is stable
    // across the component's lifetime; a malformed string leaves the machine
    // empty and returns false rather than throwing.
    [[nodiscard]] std::string to_json() const;
    bool from_json(std::string_view text);

private:
    // Enters state `index` (already validated) and restarts its clip.
    void enter(i32 index);

    // The first transition that fires from the active state, or -1. Consumes
    // any triggers the chosen transition used.
    [[nodiscard]] i32 next_transition() const;

    i32 active_ = -1;
    f32 elapsed_ = 0.0f; // seconds accumulated in the active state
    u32 frame_ = 0;      // whole frames advanced in the active state
    bool finished_ = false;
    std::vector<std::string> triggers_;
};

} // namespace fe
