// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/systems/SpriteAnimationSystem.h"

#include "fe/scene/Node.h"
#include "fe/scene/Scene.h"
#include "fe/scene/Sprite2D.h"

#include <cmath>

namespace fe {

void SpriteAnimationSystem::update(const FrameContext& context, Scene& scene) {
    advanced_ = 0;

    // Gather first: walk the graph once and keep only the nodes that carry both
    // an animator and a sprite, with the accumulator each one had last frame.
    // Everything after this works on a flat array.
    //
    // The accumulator is keyed on the animator pointer so a node that was here
    // last frame resumes its fractional progress instead of snapping to a cell
    // boundary. Entries for nodes that left the scene simply drop out.
    std::vector<Entry> previous = std::move(entries_);
    entries_.clear();
    entries_.reserve(previous.size());

    scene.for_each_node([&](Node& node) {
        auto* animator = node.get_component<SpriteAnimator>();
        if (animator == nullptr) {
            return;
        }
        auto* sprite = node.get_component<Sprite2D>();
        if (sprite == nullptr) {
            return;
        }

        // Adopt the elapsed time this animator carried last frame, if any.
        f32 elapsed = 0.0f;
        for (const Entry& prior : previous) {
            if (prior.animator == animator) {
                elapsed = prior.elapsed;
                break;
            }
        }
        entries_.push_back(Entry{animator, sprite, elapsed});
    });

    // Advance in a tight loop over the flat list.
    for (Entry& entry : entries_) {
        SpriteAnimator& animator = *entry.animator;
        const u32 total = animator.count();

        // Normalise a hand-edited or stale frame into range before doing
        // anything, so the displayed cell and the stored value never disagree.
        if (total > 0 && animator.current_frame >= total) {
            animator.current_frame = animator.loop ? animator.current_frame % total : total - 1;
        }

        // A degenerate layout, a paused animator, or a non-positive rate
        // contributes no frame advance, but we still publish the UVs below so
        // the sprite reflects the component's current_frame even when stopped.
        if (total > 1 && animator.playing && animator.fps > 0.0f) {
            entry.elapsed += context.delta_time * animator.fps;

            // Advance whole cells; keep the remainder for the next frame so slow
            // frame rates and short frames both stay accurate.
            const f32 step = std::floor(entry.elapsed);
            if (step >= 1.0f) {
                entry.elapsed -= step;
                const u32 before = animator.current_frame;
                u32 frame = animator.current_frame + static_cast<u32>(step);
                if (animator.loop) {
                    frame %= total;
                } else if (frame >= total) {
                    frame = total - 1;
                }
                animator.current_frame = frame;
                if (frame != before) {
                    ++advanced_;
                }
            }
        }

        // Hand the result to the renderer's own representation. The animator
        // owns the state; the sprite owns what is drawn. Keeping the two in sync
        // here is why a caller only has to add the component.
        entry.sprite->uv = animator.uv();
    }
}

} // namespace fe
