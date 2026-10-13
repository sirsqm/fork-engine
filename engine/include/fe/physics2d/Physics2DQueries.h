// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Math.h"
#include "fe/core/Types.h"

#include <vector>

namespace fe {

class Node;
class Scene;

namespace physics2d {

// Ray and overlap queries against a scene's 2D colliders.
//
// These are read-only: they never move a body and never touch the solver. They
// walk the scene's colliders directly (the same shapes the broad phase sees) so
// they work whether or not a Physics2DSystem has ever run, which is what makes
// them usable from gameplay and from the editor's picking tools alike.
//
// Allocation: a raycast returns a single value and never allocates. The overlap
// queries append to a caller-owned vector so a hot loop can reuse one buffer;
// once the vector has capacity for the result it does not allocate again. See
// each function for the exact contract.
//
// Shapes are axis-aligned and centred on the node's world position, matching
// Collider2D (ADR-0009): a rotated node is drawn rotated but queried as its
// unrotated AABB, which is the same limit the solver has.

// The result of a raycast. `hit` is the primary answer; the remaining fields
// are only meaningful when it is true.
struct RaycastHit {
    bool hit = false;

    // The node whose collider was struck.
    Node* node = nullptr;

    // The world-space point of intersection.
    Vec2 point{};

    // The surface normal at the hit, pointing back towards the ray origin (the
    // direction that would push a projectile away from the surface).
    Vec2 normal{};

    // Distance along the ray from the origin to `point`, in world units. Zero
    // when the origin starts inside the struck shape.
    f32 distance = 0.0f;
};

// One collider found by an overlap query.
struct OverlapHit {
    // The node whose collider overlaps the query shape.
    Node* node = nullptr;

    // The separation normal from the query shape towards the collider, and the
    // overlap depth along it. For a point query the depth is zero and the
    // normal is unspecified.
    Vec2 normal{};
    f32 penetration = 0.0f;
};

// Casts a ray from `origin` in `direction` and returns the nearest collider it
// strikes.
//
// `direction` need not be unit length; it is normalised here. A zero-length
// direction is a miss (a guard against a degenerate call, not an error).
// `max_distance` limits the search when it is > 0; a value <= 0 means unbounded.
// Returns a miss (`hit == false`) when nothing is struck, including when the
// scene holds no colliders at all.
[[nodiscard]] RaycastHit raycast(const Scene& scene, const Vec2& origin, const Vec2& direction,
                                 f32 max_distance = 0.0f) noexcept;

// Appends every collider overlapping a circle at `center` with `radius` to
// `out`, returning how many were appended. `out` is not cleared, so a caller
// that reuses one buffer should clear it between calls; the vector keeps its
// capacity and does not reallocate once it is large enough.
usize overlap_circle(const Scene& scene, const Vec2& center, f32 radius, std::vector<OverlapHit>& out);

// The box equivalent of overlap_circle: a box of the given half-extent centred
// at `center`. Same append contract.
usize overlap_box(const Scene& scene, const Vec2& center, const Vec2& half_extent, std::vector<OverlapHit>& out);

// True when any collider contains `point`. When `out` is not null it is filled
// with the first such collider. Never allocates.
[[nodiscard]] bool overlap_point(const Scene& scene, const Vec2& point, OverlapHit* out = nullptr) noexcept;

} // namespace physics2d
} // namespace fe
