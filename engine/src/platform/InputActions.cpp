// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/platform/InputActions.h"

#include <algorithm>

namespace fe {
namespace {

// Whether a normalised point lies in a normalised rectangle. Corners are not
// required to be ordered, so a binding written bottom-right-to-top-left still
// works: the comparison normalises rather than trusting the caller.
[[nodiscard]] bool inside_region(f32 x, f32 y, f32 x0, f32 y0, f32 x1, f32 y1) noexcept {
    const f32 min_x = std::min(x0, x1);
    const f32 max_x = std::max(x0, x1);
    const f32 min_y = std::min(y0, y1);
    const f32 max_y = std::max(y0, y1);
    return x >= min_x && x <= max_x && y >= min_y && y <= max_y;
}

} // namespace

ActionMap::Id ActionMap::add(std::string_view name) {
    names_.emplace_back(name);
    // Ids are 1-based positions, so the handle is stable as long as actions are
    // only ever added. Gameplay holds handles for the life of the process, which
    // is how a game declares its actions once at startup.
    return static_cast<Id>(names_.size());
}

ActionMap::Id ActionMap::find(std::string_view name) const noexcept {
    for (usize i = 0; i < names_.size(); ++i) {
        if (names_[i] == name) {
            return static_cast<Id>(i + 1);
        }
    }
    return kNone;
}

std::string_view ActionMap::action_name(Id action) const noexcept {
    if (!valid(action)) {
        return {};
    }
    return names_[static_cast<usize>(action) - 1];
}

void ActionMap::bind_key(Id action, Key key) {
    if (valid(action) && key != Key::Unknown && key != Key::Count) {
        keys_.push_back(KeyBinding{action, key});
    }
}

void ActionMap::bind_mouse_button(Id action, MouseButton button) {
    if (valid(action) && button != MouseButton::Count) {
        buttons_.push_back(ButtonBinding{action, button});
    }
}

void ActionMap::bind_touch_region(Id action, f32 x0, f32 y0, f32 x1, f32 y1) {
    if (valid(action)) {
        touch_regions_.push_back(TouchRegion{action, x0, y0, x1, y1});
    }
}

void ActionMap::clear_key_bindings(Id action) {
    std::erase_if(keys_, [action](const KeyBinding& binding) { return binding.action == action; });
}

void ActionMap::clear_mouse_bindings(Id action) {
    std::erase_if(buttons_, [action](const ButtonBinding& binding) { return binding.action == action; });
}

void ActionMap::clear_touch_regions(Id action) {
    std::erase_if(touch_regions_, [action](const TouchRegion& region) { return region.action == action; });
}

void ActionMap::update(const InputState& input, Vec2 viewport) {
    const usize count = names_.size();
    // Preserve the previous frame's `down` so the edges can be derived; the
    // edge arrays are recomputed from scratch each frame, which is what makes
    // them one-frame facts exactly as in InputState.
    const std::vector<bool> previous = down_;
    down_.assign(count, false);
    pressed_.assign(count, false);
    released_.assign(count, false);

    for (const KeyBinding& binding : keys_) {
        if (input.is_key_down(binding.key)) {
            down_[binding.action - 1] = true;
        }
    }
    for (const ButtonBinding& binding : buttons_) {
        if (input.is_mouse_down(binding.button)) {
            down_[binding.action - 1] = true;
        }
    }

    // A touch triggers every region it lies inside, so one finger can hold two
    // actions (a thumb on "left" and a finger on "jump" are different contacts;
    // the same contact overlapping two regions is the game's choice to make).
    // Positions are normalised against the viewport, with a non-positive
    // dimension treated as unknown so a zero-sized canvas cannot divide by zero.
    if (viewport.x > 0.0f && viewport.y > 0.0f) {
        const usize contacts = input.touch_count();
        for (usize i = 0; i < contacts; ++i) {
            const u32 id = input.touch_id_at(i);
            if (id == InputState::kInvalidTouchId || !input.is_touch_down(id)) {
                continue;
            }
            const Vec2 position = input.touch_position(id);
            const f32 nx = position.x / viewport.x;
            const f32 ny = position.y / viewport.y;
            for (const TouchRegion& region : touch_regions_) {
                if (inside_region(nx, ny, region.x0, region.y0, region.x1, region.y1)) {
                    down_[region.action - 1] = true;
                }
            }
        }
    }

    for (usize i = 0; i < count; ++i) {
        const bool was_down = i < previous.size() && previous[i];
        pressed_[i] = down_[i] && !was_down;
        released_[i] = !down_[i] && was_down;
    }
}

bool ActionMap::is_down(Id action) const noexcept {
    return action != kNone && static_cast<usize>(action) <= down_.size() && down_[action - 1];
}

bool ActionMap::was_pressed(Id action) const noexcept {
    return action != kNone && static_cast<usize>(action) <= pressed_.size() && pressed_[action - 1];
}

bool ActionMap::was_released(Id action) const noexcept {
    return action != kNone && static_cast<usize>(action) <= released_.size() && released_[action - 1];
}

} // namespace fe
