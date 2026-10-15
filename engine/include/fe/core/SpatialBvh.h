// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Frustum.h"
#include "fe/core/Types.h"

#include <algorithm>
#include <limits>
#include <span>
#include <vector>

namespace fe {

// An axis-aligned bounding box in world space, stored as min/max corners.
//
// The physics modules each have their own centre/half-extent box (`Aabb2D`,
// `Aabb3D`) tuned for their solvers. This one is the culling/query box: min/max
// is what the frustum test and the union want directly, and it lives in `core`
// so a spatial index does not have to reach down into physics for a box type.
struct Aabb {
    Vec3 min{};
    Vec3 max{};

    // The box that contains nothing: min at +infinity, max at -infinity. Growing
    // this with `expanded` yields exactly the first box added, which is the
    // standard way to compute a bound from scratch with no "first element" test.
    [[nodiscard]] static constexpr Aabb empty() noexcept {
        constexpr f32 inf = std::numeric_limits<f32>::infinity();
        return Aabb{Vec3{inf, inf, inf}, Vec3{-inf, -inf, -inf}};
    }

    [[nodiscard]] constexpr bool is_empty() const noexcept { return min.x > max.x || min.y > max.y || min.z > max.z; }

    [[nodiscard]] constexpr Vec3 center() const noexcept { return (min + max) * 0.5f; }
    [[nodiscard]] constexpr Vec3 extent() const noexcept { return max - min; }

    [[nodiscard]] constexpr Aabb expanded(const Aabb& other) const noexcept {
        return Aabb{Vec3{std::min(min.x, other.min.x), std::min(min.y, other.min.y), std::min(min.z, other.min.z)},
                    Vec3{std::max(max.x, other.max.x), std::max(max.y, other.max.y), std::max(max.z, other.max.z)}};
    }

    [[nodiscard]] constexpr Aabb expanded(const Vec3& point) const noexcept {
        return Aabb{Vec3{std::min(min.x, point.x), std::min(min.y, point.y), std::min(min.z, point.z)},
                    Vec3{std::max(max.x, point.x), std::max(max.y, point.y), std::max(max.z, point.z)}};
    }

    // True when the two boxes share at least one point (touching counts).
    [[nodiscard]] constexpr bool overlaps(const Aabb& other) const noexcept {
        return min.x <= other.max.x && max.x >= other.min.x && min.y <= other.max.y && max.y >= other.min.y &&
               min.z <= other.max.z && max.z >= other.min.z;
    }
};

// A bounding-volume hierarchy over static axis-aligned boxes, for view culling
// and box queries.
//
// This is the "spatial hierarchy over static geometry" the M4 roadmap defers.
// ADR-0011 and the 3D renderer both describe the current frustum pass as "the
// leaf operation a BVH would call"; this class is that tree, and it reuses
// `Frustum::intersects_aabb` verbatim rather than re-deriving the test.
//
// Shape: a binary BVH built top-down with the surface-area heuristic (SAH),
// using the binned approximation. Leaves hold a few primitives; every internal
// node stores the union of its children's bounds. A query descends the tree and
// skips any subtree whose bounds cannot intersect the query -- the win is that a
// rejected subtree costs one box test instead of one per primitive below it.
//
// Design notes (see docs/adr/0053-spatial-bvh.md):
//   * Pure and headless. It knows boxes and indices only -- no scene, no
//     components, no GPU -- so it is exhaustively testable, the same reason
//     `BroadPhase` and `Frustum` are kept separate.
//   * Deterministic. Node order, split choice, and query output depend only on
//     the input order and the geometry; ties break by primitive index. Two
//     builds of the same input on the same build produce identical trees, and
//     the query output is sorted by primitive index regardless.
//   * Allocation-light. `build` reuses its node and scratch arrays across calls,
//     so a steady-state rebuild does not grow; `query` writes into a caller
//     buffer and never allocates.
//   * Static. There is no insert/remove; a changed scene is rebuilt. That is the
//     static-geometry case the roadmap asks for.
//
// Build cost is O(n log n) on average via binning. Two guards keep a
// pathological input from turning that into O(n^2) or an unbounded recursion: a
// leaf is allowed up to `kMaxLeafSize` primitives when the SAH finds no gain,
// and past `kMaxDepth` the split falls back to the median, which always halves.
class SpatialBvh {
public:
    // Bins per axis for the binned SAH. 12 is the common choice: fine enough
    // that the best split is found within a few percent of the exact SAH, coarse
    // enough that a build stays cheap and the per-node pass is O(bins + n).
    static constexpr usize kBinCount = 12;

    // A leaf built through the no-SAH-gain path holds at most this many
    // primitives, so a build always terminates even when every centroid
    // coincides.
    static constexpr usize kMaxLeafSize = 4;

    // Recursion depth past which the split is forced to the median. Bounds the
    // tree depth (and so the build/query recursion) on adversarial input where
    // the SAH would otherwise peel one primitive off at a time.
    static constexpr u32 kMaxDepth = 64;

    // The child index stored in a leaf node. `left == kNoChild` is what makes a
    // node a leaf.
    static constexpr u32 kNoChild = 0xFFFFFFFFu;

    struct Node {
        Aabb bounds{};
        // First primitive, as a position in `primitive_order()` (leaves only).
        u32 first = 0;
        // Primitives in this subtree.
        u32 count = 0;
        // Children. Both are `kNoChild` in a leaf.
        u32 left = kNoChild;
        u32 right = kNoChild;

        [[nodiscard]] constexpr bool is_leaf() const noexcept { return left == kNoChild; }
    };

    SpatialBvh() = default;

    // Builds the tree over `bounds`. Replaces any previous tree. Empty input
    // yields an empty tree (`empty()` is true).
    void build(std::span<const Aabb> bounds);

    // The node array: node 0 is the root.
    [[nodiscard]] std::span<const Node> nodes() const noexcept { return nodes_; }
    [[nodiscard]] usize node_count() const noexcept { return nodes_.size(); }
    [[nodiscard]] bool empty() const noexcept { return nodes_.empty(); }

    // The primitive index each leaf position refers to. A leaf covers
    // `[first, first + count)` in this array.
    [[nodiscard]] std::span<const u32> primitive_order() const noexcept { return primitives_; }

    // The bounds of each primitive, in the order they were passed to build.
    // Kept so a leaf test can reject a primitive whose box misses the query even
    // though its leaf node's box (the union) intersects.
    [[nodiscard]] std::span<const Aabb> primitive_bounds() const noexcept { return bounds_; }

    // The whole tree's bounds (node 0), or the empty box when the tree is empty.
    [[nodiscard]] const Aabb& root_bounds() const noexcept { return root_bounds_; }

    // Appends the indices of every primitive whose box intersects the frustum.
    // Conservative, matching `Frustum::intersects_aabb`: a box that straddles a
    // plane is kept, so the result is a superset of the truly visible set and
    // the caller can only ever draw one mesh too many, never hide a visible one.
    // `out` is cleared first, then filled sorted by primitive index with no
    // duplicates. Never allocates.
    void query(const Frustum& frustum, std::vector<u32>& out) const;

    // Appends the indices of every primitive whose box overlaps `region`, under
    // the same clear/sort/dedup contract as the frustum query.
    void query(const Aabb& region, std::vector<u32>& out) const;

private:
    struct BuildPrimitive {
        Aabb bounds{};
        Vec3 center{};
        u32 index = 0;
    };

    // The chosen split position in `[begin, end)` and whether the SAH found a
    // positive gain for it. `gain == false` means no candidate split beat
    // leaving the primitives together.
    struct SplitChoice {
        usize position = 0;
        bool gain = false;
    };

    [[nodiscard]] u32 build_range(usize begin, usize end, u32 depth);
    [[nodiscard]] SplitChoice choose_split(usize begin, usize end);
    [[nodiscard]] u32 largest_centroid_axis(usize begin, usize end) const;
    [[nodiscard]] static Aabb union_bounds(std::span<const Aabb> bounds, std::span<const BuildPrimitive> prims,
                                           usize begin, usize end) noexcept;
    void collect(const Frustum& frustum, u32 node, std::vector<u32>& out) const;
    void collect(const Aabb& region, u32 node, std::vector<u32>& out) const;

    std::vector<Node> nodes_;
    std::vector<BuildPrimitive> scratch_;
    std::vector<u32> primitives_;
    std::vector<Aabb> bounds_;
    Aabb root_bounds_{};
};

} // namespace fe
