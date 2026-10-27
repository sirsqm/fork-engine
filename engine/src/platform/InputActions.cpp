// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/platform/InputActions.h"

#include <algorithm>
#include <utility>

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

void ActionMap::set_safe_area_insets(f32 top, f32 right, f32 bottom, f32 left) {
    // Each inset is clamped to [0, 1]. A pair that would reserve the whole axis
    // is then treated as no inset at all: a report that leaves nothing usable is
    // far more likely a broken measurement than a real 100%-reserved screen, and
    // the two failure modes are not equal - ignoring it keeps the game playable,
    // while honouring it makes the whole axis dead. Falling back to zero also
    // keeps the usable area strictly positive, so the normalised division in
    // update() can never be by zero.
    const auto resolve_axis = [](f32 a, f32 b) -> std::pair<f32, f32> {
        const f32 first = std::clamp(a, 0.0f, 1.0f);
        const f32 second = std::clamp(b, 0.0f, 1.0f);
        if (first + second >= 1.0f) {
            return {0.0f, 0.0f};
        }
        return {first, second};
    };
    const auto [clamped_top, clamped_bottom] = resolve_axis(top, bottom);
    const auto [clamped_left, clamped_right] = resolve_axis(left, right);
    safe_top_ = clamped_top;
    safe_bottom_ = clamped_bottom;
    safe_left_ = clamped_left;
    safe_right_ = clamped_right;
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
    //
    // The point is then remapped out of the OS-reserved bands and into the
    // usable area, which is the space regions are written in. The mapping is
    // affine and per-axis: a point in the left inset maps to the usable area's
    // left edge, and a point outside the usable area (in the inset itself) maps
    // outside 0..1, so it matches no region - which is the point of an inset.
    const f32 usable_width = 1.0f - safe_left_ - safe_right_;
    const f32 usable_height = 1.0f - safe_top_ - safe_bottom_;
    if (viewport.x > 0.0f && viewport.y > 0.0f && usable_width > 0.0f && usable_height > 0.0f) {
        const usize contacts = input.touch_count();
        for (usize i = 0; i < contacts; ++i) {
            const u32 id = input.touch_id_at(i);
            if (id == InputState::kInvalidTouchId || !input.is_touch_down(id)) {
                continue;
            }
            const Vec2 position = input.touch_position(id);
            const f32 nx = (position.x / viewport.x - safe_left_) / usable_width;
            const f32 ny = (position.y / viewport.y - safe_top_) / usable_height;
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
