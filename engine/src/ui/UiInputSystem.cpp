// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/ui/UiInputSystem.h"

#include "fe/platform/InputState.h"
#include "fe/scene/Node.h"
#include "fe/scene/Scene.h"
#include "fe/ui/UiElement.h"

namespace fe::ui {
namespace {

// The pointer position this frame, in viewport pixels, and whether the primary
// pointer is down.
//
// The mouse wins when it has moved or is held, so a desktop run is unaffected by
// the touch path. A touch contact is used otherwise, which is what makes one
// system serve a browser on a phone and a browser on a laptop without the game
// knowing which it is on -- the same reasoning ActionMap uses.
struct PointerState {
    Vec2 position{};
    bool down = false;
    bool pressed = false;
    bool released = false;
    // False when there is no pointer at all (no mouse, no contact), so the system
    // can skip a frame rather than treating (0, 0) as a real position.
    bool valid = false;
};

[[nodiscard]] PointerState read_pointer(const InputState& input) noexcept {
    PointerState pointer;
    pointer.position = input.mouse_position();
    pointer.down = input.is_mouse_down(MouseButton::Left);
    pointer.pressed = input.was_mouse_pressed(MouseButton::Left);
    pointer.released = input.was_mouse_released(MouseButton::Left);
    // A mouse that has ever reported a position is the primary pointer. The
    // fallback below covers a touch-only device, where the mouse never moves.
    pointer.valid = input.mouse_position().x != 0.0f || input.mouse_position().y != 0.0f || pointer.down ||
                    pointer.pressed || pointer.released;

    if (!pointer.valid) {
        for (usize i = 0; i < input.touch_count(); ++i) {
            const u32 id = input.touch_id_at(i);
            if (id == InputState::kInvalidTouchId) {
                continue;
            }
            if (input.is_touch_down(id) || input.was_touch_began(id) || input.was_touch_ended(id)) {
                pointer.position = input.touch_position(id);
                pointer.down = input.is_touch_down(id);
                pointer.pressed = input.was_touch_began(id);
                pointer.released = input.was_touch_ended(id);
                pointer.valid = true;
                break;
            }
        }
    }

    return pointer;
}

// Clears the transient state on every element before this frame's is written, so
// a consumer never reads a stale edge. A walk of the scene rather than of the
// layout result, because an element hidden this frame still holds last frame's
// `clicked` and must not appear to have been clicked.
void clear_interactions(Scene& scene) noexcept {
    scene.for_each_node([](Node& node) {
        if (auto* element = node.get_component<UiElement>(); element != nullptr) {
            element->interaction = UiInteraction{};
        }
    });
}

} // namespace

void UiInputSystem::update(const FrameContext& context, Scene& scene) {
    clear_interactions(scene);
    hovered_ = nullptr;
    viewport_width_ = 0;
    viewport_height_ = 0;
    layout_.clear();

    if (context.input == nullptr) {
        // No window, so no pointer: a headless run or a tool. The elements are
        // cleared and nothing is hit tested, which is the honest answer rather
        // than treating the origin as a click.
        pressed_element_ = nullptr;
        return;
    }

    const InputState& input = *context.input;

    // The viewport the pointer is in. The layout must use the same size the
    // renderer draws with, or the rectangles would not match the pixels; the
    // framebuffer size is what the platform reports positions in, so it is the
    // one answer that is correct for both. A caller with a different surface (the
    // editor's viewport) sets it through the frame context.
    const u32 width = static_cast<u32>(std::max(context.viewport.x, 0.0f));
    const u32 height = static_cast<u32>(std::max(context.viewport.y, 0.0f));
    if (width == 0 || height == 0) {
        pressed_element_ = nullptr;
        return;
    }
    viewport_width_ = width;
    viewport_height_ = height;

    // The one layout call. `render::extract_ui` calls the same function with the
    // same viewport, which is what guarantees the pixels and the hit test agree.
    fe::ui::layout(scene, width, height, layout_);

    const PointerState pointer = read_pointer(input);
    if (!pointer.valid) {
        // A pointer that has never reported a position cannot be over anything,
        // but a press that began last frame must still be able to end, so the
        // pressed element is kept until a release arrives.
        return;
    }

    hovered_ = hit_test(layout_, pointer.position);

    // A press begins on whatever is under the pointer, and the press is
    // remembered by component identity so a release can be matched to it. This is
    // the whole interaction model, in four steps, so the state on an element is
    // always derivable from the last two frames of input.
    if (pointer.pressed && hovered_ != nullptr) {
        pressed_element_ = hovered_->element;
    }

    // A release ends the press wherever it lands. It is a click only when it lands
    // on the element that was pressed, which is what makes dragging off a button
    // cancel it.
    const bool clicked = pointer.released && hovered_ != nullptr && pressed_element_ == hovered_->element;
    if (pointer.released) {
        if (hovered_ != nullptr) {
            auto* element = const_cast<UiElement*>(hovered_->element);
            element->interaction.released = true;
            element->interaction.clicked = clicked;
        }
        pressed_element_ = nullptr;
    }

    if (hovered_ != nullptr) {
        auto* element = const_cast<UiElement*>(hovered_->element);
        element->interaction.hovered = true;
        // `pressed` is the pointer being down on the element it went down on, and
        // it stays true for every frame in between, so a held button reads as held
        // rather than merely as "went down".
        element->interaction.pressed = pointer.down && pressed_element_ == hovered_->element;
        element->interaction.pressed_this_frame = pointer.pressed;
    }
}

} // namespace fe::ui
