// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/anim/AnimationStateMachine.h"

#include <nlohmann/json.hpp>

#include <algorithm>

namespace fe {

i32 AnimationStateMachine::find_state(std::string_view name) const noexcept {
    for (usize i = 0; i < states.size(); ++i) {
        if (states[i].name == name) {
            return static_cast<i32>(i);
        }
    }
    return -1;
}

std::string_view AnimationStateMachine::active_state_name() const noexcept {
    if (active_ < 0 || static_cast<usize>(active_) >= states.size()) {
        return {};
    }
    return states[static_cast<usize>(active_)].name;
}

u32 AnimationStateMachine::clip_frame() const noexcept {
    if (active_ < 0 || static_cast<usize>(active_) >= states.size()) {
        return 0;
    }
    const AnimationState& state = states[static_cast<usize>(active_)];
    if (state.frame_count == 0) {
        return 0;
    }
    if (state.loop) {
        return frame_ % state.frame_count;
    }
    return std::min(frame_, state.frame_count - 1);
}

bool AnimationStateMachine::play(std::string_view state_name) {
    const i32 index = find_state(state_name);
    if (index < 0) {
        return false;
    }
    enter(index);
    return true;
}

void AnimationStateMachine::set_trigger(std::string_view name) {
    for (const std::string& trigger : triggers_) {
        if (trigger == name) {
            return;
        }
    }
    triggers_.emplace_back(name);
}

void AnimationStateMachine::clear_trigger(std::string_view name) {
    triggers_.erase(std::remove(triggers_.begin(), triggers_.end(), name), triggers_.end());
}

bool AnimationStateMachine::has_trigger(std::string_view name) const noexcept {
    return std::find(triggers_.begin(), triggers_.end(), name) != triggers_.end();
}

void AnimationStateMachine::enter(i32 index) {
    active_ = index;
    elapsed_ = 0.0f;
    frame_ = 0;
    finished_ = false;
}

i32 AnimationStateMachine::next_transition() const {
    if (active_ < 0 || static_cast<usize>(active_) >= states.size()) {
        return -1;
    }
    const std::string& current = states[static_cast<usize>(active_)].name;

    for (usize i = 0; i < transitions.size(); ++i) {
        const AnimationTransition& transition = transitions[i];

        // A transition is eligible when its source is the active state, or when
        // it is an "any state" edge (empty `from`).
        const bool source_matches = transition.from.empty() || transition.from == current;
        if (!source_matches) {
            continue;
        }
        // A self-edge with no trigger would fire forever; require a trigger to
        // have been set for a transition back to the same state.
        if (transition.to == current && transition.conditions.empty()) {
            continue;
        }
        if (transition.require_finished && !finished_) {
            continue;
        }
        const bool conditions_hold = std::all_of(transition.conditions.begin(), transition.conditions.end(),
                                                 [this](const std::string& name) { return has_trigger(name); });
        if (conditions_hold) {
            return static_cast<i32>(i);
        }
    }
    return -1;
}

void AnimationStateMachine::advance(f32 delta_time) {
    if (states.empty()) {
        active_ = -1;
        finished_ = false;
        return;
    }

    // Enter the initial state on the first advance (or after a graph edit that
    // left no active state), so a machine works with no explicit play() call.
    if (active_ < 0 || static_cast<usize>(active_) >= states.size()) {
        const i32 initial = find_state(initial_state);
        enter(initial >= 0 ? initial : 0);
    }

    if (!playing) {
        return;
    }

    const AnimationState& state = states[static_cast<usize>(active_)];

    // Advance the clip clock. Whole frames are what a state machine transitions
    // on, so the clock is the frame count, derived from fps exactly as
    // SpriteAnimationSystem derives it from the animator.
    if (state.frame_count > 1 && state.fps > 0.0f) {
        elapsed_ += delta_time * state.fps;
        const f32 step = std::floor(elapsed_);
        if (step >= 1.0f) {
            elapsed_ -= step;
            frame_ += static_cast<u32>(step);
        }
    }

    // Decide whether the clip has finished. A looping state never finishes; a
    // one-shot state finishes on its last frame and stays there.
    if (state.frame_count == 0) {
        finished_ = true;
    } else if (state.loop) {
        finished_ = false;
    } else if (frame_ >= state.frame_count - 1) {
        finished_ = true;
    }

    // Apply the first transition that fires. A transition's conditions are
    // consumed when it fires, so a trigger drives at most one edge.
    const i32 chosen = next_transition();
    if (chosen >= 0) {
        const AnimationTransition& transition = transitions[static_cast<usize>(chosen)];
        for (const std::string& condition : transition.conditions) {
            clear_trigger(condition);
        }
        const i32 target = find_state(transition.to);
        if (target >= 0) {
            enter(target);
        }
    }
}

std::string AnimationStateMachine::to_json() const {
    nlohmann::json document;
    document["initial_state"] = initial_state;
    document["playing"] = playing;
    document["drive_animator"] = drive_animator;

    nlohmann::json states_json = nlohmann::json::array();
    for (const AnimationState& state : states) {
        states_json.push_back(nlohmann::json{
            {"name", state.name},
            {"first_frame", state.first_frame},
            {"frame_count", state.frame_count},
            {"fps", state.fps},
            {"loop", state.loop},
        });
    }
    document["states"] = std::move(states_json);

    nlohmann::json transitions_json = nlohmann::json::array();
    for (const AnimationTransition& transition : transitions) {
        transitions_json.push_back(nlohmann::json{
            {"from", transition.from},
            {"to", transition.to},
            {"require_finished", transition.require_finished},
            {"conditions", transition.conditions},
        });
    }
    document["transitions"] = std::move(transitions_json);

    return document.dump();
}

bool AnimationStateMachine::from_json(std::string_view text) {
    nlohmann::json document;
    try {
        document = nlohmann::json::parse(text);
    } catch (const nlohmann::json::exception&) {
        return false;
    }
    if (!document.is_object()) {
        return false;
    }

    // Parse into temporaries so a malformed document leaves the machine
    // unchanged rather than half-applied.
    std::vector<AnimationState> new_states;
    if (document.contains("states") && document.at("states").is_array()) {
        for (const auto& entry : document.at("states")) {
            if (!entry.is_object()) {
                continue;
            }
            AnimationState state;
            state.name = entry.value("name", std::string{});
            state.first_frame = entry.value("first_frame", 0u);
            state.frame_count = entry.value("frame_count", 1u);
            state.fps = entry.value("fps", 8.0f);
            state.loop = entry.value("loop", true);
            if (!state.name.empty()) {
                new_states.push_back(std::move(state));
            }
        }
    }

    std::vector<AnimationTransition> new_transitions;
    if (document.contains("transitions") && document.at("transitions").is_array()) {
        for (const auto& entry : document.at("transitions")) {
            if (!entry.is_object()) {
                continue;
            }
            AnimationTransition transition;
            transition.from = entry.value("from", std::string{});
            transition.to = entry.value("to", std::string{});
            transition.require_finished = entry.value("require_finished", false);
            if (entry.contains("conditions") && entry.at("conditions").is_array()) {
                transition.conditions = entry.at("conditions").get<std::vector<std::string>>();
            }
            if (!transition.to.empty()) {
                new_transitions.push_back(std::move(transition));
            }
        }
    }

    states = std::move(new_states);
    transitions = std::move(new_transitions);
    initial_state = document.value("initial_state", std::string{});
    playing = document.value("playing", true);
    drive_animator = document.value("drive_animator", true);

    // The graph changed, so any live playback state is stale; re-enter on the
    // next advance.
    active_ = -1;
    elapsed_ = 0.0f;
    frame_ = 0;
    finished_ = false;
    triggers_.clear();
    return true;
}

} // namespace fe
