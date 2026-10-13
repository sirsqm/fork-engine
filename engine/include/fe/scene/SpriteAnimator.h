// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Math.h"
#include "fe/core/Types.h"
#include "fe/scene/Component.h"

namespace fe {

// Sprite-sheet animation state for a node's Sprite2D.
//
// The component holds *state* (which cell of the sheet is showing, and whether
// time is advancing); the SpriteAnimationSystem holds the *behaviour* that
// advances it. Splitting them this way is the point of the systems layer: the
// state is authored, serialized, and inspected like any other component, while
// the update loop is a system that can be reasoned about and tested on its own.
//
// The sheet is laid out as a grid of cells read left-to-right, top-to-bottom,
// which is the convention every sprite-sheet tool exports by default. The
// frames fields define the grid; uv() turns the current cell into the UV
// sub-rectangle a Sprite2D draws.
class SpriteAnimator final : public Component {
public:
    static constexpr const char* kTypeName = "SpriteAnimator";

    [[nodiscard]] const char* type_name() const noexcept override { return kTypeName; }

    // Grid size of the sheet. A zero in either axis means "no animation";
    // count() is then zero and the system leaves the sprite's UVs alone.
    u32 frames_x = 1;
    u32 frames_y = 1;

    // Playback rate. Frames advance at `fps`; a non-positive rate pauses.
    f32 fps = 8.0f;

    // When false the system does not advance time. The component keeps its
    // current frame, so pausing and resuming is exact.
    bool playing = true;

    // When false the animation stops on the last frame instead of wrapping.
    bool loop = true;

    // The cell currently shown. Always kept in [0, count()); the system wraps
    // it and clamps hand-edited values here.
    u32 current_frame = 0;

    // Total cells in the sheet. Zero means the layout is invalid and animation
    // is disabled, which is a well-defined state rather than a special case
    // every caller has to guard against.
    [[nodiscard]] u32 count() const noexcept { return frames_x * frames_y; }

    // The UV sub-rectangle (u0, v0, u1, v1) of the current cell, matching the
    // layout Sprite2D::uv expects. Returns the full texture when the layout is
    // invalid, so a misconfigured animator draws the whole image rather than a
    // degenerate one.
    [[nodiscard]] Vec4 uv() const noexcept {
        const u32 total = count();
        if (total == 0) {
            return Vec4{0.0f, 0.0f, 1.0f, 1.0f};
        }
        const u32 frame = current_frame % total;
        const f32 cell_w = 1.0f / static_cast<f32>(frames_x);
        const f32 cell_h = 1.0f / static_cast<f32>(frames_y);
        const u32 col = frame % frames_x;
        const u32 row = frame / frames_x;
        const f32 u0 = static_cast<f32>(col) * cell_w;
        const f32 v0 = static_cast<f32>(row) * cell_h;
        return Vec4{u0, v0, u0 + cell_w, v0 + cell_h};
    }
};

} // namespace fe
