// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Error.h"
#include "fe/core/Math.h"
#include "fe/core/Types.h"
#include "fe/rhi/Resources.h"

#include <vector>

namespace fe {

class Scene;

namespace rhi {
class Device;
} // namespace rhi

namespace debugdraw {

// One line of a debug draw list: two world-space endpoints and a colour.
//
// A flat value type, so a caller can build a list by pushing `Line`s and a
// renderer can upload the whole list as one vertex buffer with no per-line
// state changes.
struct Line {
    Vec3 a{};
    Vec3 b{};
    Vec4 color{1.0f, 1.0f, 1.0f, 1.0f};
};

// A CPU-side list of world-space lines, the "immediate mode" half of debug
// drawing.
//
// A gameplay or editor system appends to a list each frame; the list is cleared
// and rebuilt rather than persisted, so there is no lifetime to manage. It is
// deliberately just a `std::vector<Line>` with helpers: the interesting part is
// what fills it (collider wireframes) and what draws it, not the container.
class DebugDrawList {
public:
    void clear() noexcept { lines_.clear(); }

    [[nodiscard]] bool empty() const noexcept { return lines_.empty(); }
    [[nodiscard]] usize size() const noexcept { return lines_.size(); }

    [[nodiscard]] const std::vector<Line>& lines() const noexcept { return lines_; }

    void add_line(const Vec3& a, const Vec3& b, const Vec4& color = Vec4{1.0f, 1.0f, 1.0f, 1.0f}) {
        lines_.push_back(Line{a, b, color});
    }

    // The 12 edges of an axis-aligned box centred at `center` with `half_extent`.
    void add_box(const Vec3& center, const Vec3& half_extent, const Vec4& color = Vec4{1.0f, 1.0f, 1.0f, 1.0f});

    // A circle of `segments` chords on the plane whose normal is `axis`, centred
    // at `center` with `radius`. Three calls (one per axis) draw a wire sphere.
    void add_circle(const Vec3& center, f32 radius, u32 axis, u32 segments = 16,
                    const Vec4& color = Vec4{1.0f, 1.0f, 1.0f, 1.0f});

    // A wire sphere as three orthogonal circles.
    void add_sphere(const Vec3& center, f32 radius, u32 segments = 16,
                    const Vec4& color = Vec4{1.0f, 1.0f, 1.0f, 1.0f});

    // An axis-aligned cross of `size` at `center`, useful for a joint or a
    // point-of-interest marker.
    void add_cross(const Vec3& center, f32 size, const Vec4& color = Vec4{1.0f, 1.0f, 1.0f, 1.0f});

private:
    std::vector<Line> lines_;
};

// The GPU side: one shader and one vertex buffer, reused across frames.
//
// `render` uploads the list and issues a single line-list draw. It needs a
// view-projection (from the active camera) because debug lines are world-space.
// The pass draws with depth testing on so a wireframe is occluded by the geometry
// it describes, and blending on so a translucent colour reads as an overlay.
class DebugRenderer {
public:
    DebugRenderer() = default;

    DebugRenderer(const DebugRenderer&) = delete;
    DebugRenderer& operator=(const DebugRenderer&) = delete;

    // Builds the shader and grows the vertex buffer to hold at least
    // `initial_capacity_lines` lines. Idempotent: a later call with a larger
    // capacity grows the buffer, a smaller one is ignored.
    [[nodiscard]] Error initialize(rhi::Device& device, usize initial_capacity_lines = 1024);

    // Uploads `list` and draws it. A no-op on an empty list. `view_projection`
    // is the active camera's matrix for the current viewport.
    void render(rhi::Device& device, const DebugDrawList& list, const Mat4& view_projection);

    // The number of lines drawn by the most recent `render`, after the list was
    // uploaded. Exposed for tests and the editor's profiler.
    [[nodiscard]] u32 last_line_count() const noexcept { return last_line_count_; }

    [[nodiscard]] u32 draw_calls() const noexcept { return draw_calls_; }

private:
    rhi::ShaderRef shader_;
    rhi::BufferRef vertex_buffer_;
    rhi::BufferRef index_buffer_;
    usize capacity_lines_ = 0;
    u32 index_buffer_capacity_ = 0;
    u32 last_line_count_ = 0;
    u32 draw_calls_ = 0;
    std::vector<f32> scratch_;
    std::vector<u32> scratch_indices_;
};

// The colours the collider wireframe pass uses, so a caller and a test agree on
// the convention: a static collider is drawn in one colour and a dynamic body in
// another, which is the whole point of the overlay.
[[nodiscard]] constexpr Vec4 static_collider_color() noexcept {
    return Vec4{0.25f, 0.85f, 0.35f, 1.0f};
}
[[nodiscard]] constexpr Vec4 dynamic_collider_color() noexcept {
    return Vec4{0.95f, 0.75f, 0.20f, 1.0f};
}

// Appends a wireframe for every 3D collider in `scene` to `out`: a box for a
// Box shape, a wire sphere for a Sphere. A collider on a node with a dynamic
// RigidBody3D uses the dynamic colour, a static one the static colour. Returns
// the number of colliders drawn.
usize append_collider_wireframes(const Scene& scene, DebugDrawList& out);

// The 2D counterpart: a box or a circle wireframe drawn on the z = 0 plane, so
// the same debug renderer serves a 2D scene. Returns the number of colliders
// drawn.
usize append_collider_wireframes_2d(const Scene& scene, DebugDrawList& out);

} // namespace debugdraw
} // namespace fe
