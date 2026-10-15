// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/core/SpatialBvh.h"

#include <algorithm>
#include <array>
#include <limits>

namespace fe {
namespace {

// Half the surface area of a box: the area of three faces. The constant factor
// of two cancels out of every SAH comparison, so it is dropped to save work.
[[nodiscard]] f32 half_area(const Aabb& box) noexcept {
    if (box.is_empty()) {
        return 0.0f;
    }
    const Vec3 e = box.extent();
    return e.x * e.y + e.y * e.z + e.z * e.x;
}

[[nodiscard]] f32 axis_value(const Vec3& v, u32 axis) noexcept {
    return axis == 0 ? v.x : (axis == 1 ? v.y : v.z);
}

// Which of the three axes has the widest centroid spread: the axis the SAH bins
// along, because that is where a split can separate the primitives.
[[nodiscard]] u32 widest_axis(const Vec3& extent) noexcept {
    if (extent.x >= extent.y && extent.x >= extent.z) {
        return 0;
    }
    return extent.y >= extent.z ? 1u : 2u;
}

} // namespace

void SpatialBvh::build(std::span<const Aabb> bounds) {
    nodes_.clear();
    scratch_.clear();
    primitives_.clear();
    bounds_.assign(bounds.begin(), bounds.end());

    if (bounds.empty()) {
        root_bounds_ = Aabb{};
        return;
    }

    scratch_.resize(bounds.size());
    for (usize i = 0; i < bounds.size(); ++i) {
        scratch_[i] = BuildPrimitive{bounds[i], bounds[i].center(), static_cast<u32>(i)};
    }

    // A binary tree over at most `n` leaves has at most `2n - 1` nodes. Reserve
    // that up front so a build does not reallocate mid-recursion.
    nodes_.reserve(2 * bounds.size() - 1);
    (void)build_range(0, bounds.size(), 0);

    // Copy the leaf order out so a leaf's `[first, first + count)` range indexes
    // a stable array. Each range is sorted by primitive index, so the stored
    // order -- and therefore the tree -- does not depend on how the partition
    // shuffled equal-bin primitives.
    primitives_.resize(bounds.size());
    for (const Node& node : nodes_) {
        if (!node.is_leaf()) {
            continue;
        }
        const auto first = static_cast<std::ptrdiff_t>(node.first);
        for (u32 i = 0; i < node.count; ++i) {
            primitives_[node.first + i] = scratch_[node.first + i].index;
        }
        std::sort(primitives_.begin() + first, primitives_.begin() + first + static_cast<std::ptrdiff_t>(node.count));
    }

    root_bounds_ = nodes_.empty() ? Aabb{} : nodes_[0].bounds;
}

Aabb SpatialBvh::union_bounds(std::span<const Aabb> bounds, std::span<const BuildPrimitive> prims, usize begin,
                              usize end) noexcept {
    Aabb result = Aabb::empty();
    for (usize i = begin; i < end; ++i) {
        result = result.expanded(bounds[prims[i].index]);
    }
    return result;
}

u32 SpatialBvh::largest_centroid_axis(usize begin, usize end) const {
    Vec3 min_center{std::numeric_limits<f32>::infinity(), std::numeric_limits<f32>::infinity(),
                    std::numeric_limits<f32>::infinity()};
    Vec3 max_center{-std::numeric_limits<f32>::infinity(), -std::numeric_limits<f32>::infinity(),
                    -std::numeric_limits<f32>::infinity()};
    for (usize i = begin; i < end; ++i) {
        const Vec3& c = scratch_[i].center;
        min_center = Vec3{std::min(min_center.x, c.x), std::min(min_center.y, c.y), std::min(min_center.z, c.z)};
        max_center = Vec3{std::max(max_center.x, c.x), std::max(max_center.y, c.y), std::max(max_center.z, c.z)};
    }
    return widest_axis(max_center - min_center);
}

SpatialBvh::SplitChoice SpatialBvh::choose_split(usize begin, usize end) {
    const usize count = end - begin;
    SplitChoice choice;
    choice.position = begin + count / 2;
    choice.gain = false;
    if (count < 2) {
        return choice;
    }

    const u32 axis = largest_centroid_axis(begin, end);
    f32 axis_min = std::numeric_limits<f32>::infinity();
    f32 axis_max = -std::numeric_limits<f32>::infinity();
    for (usize i = begin; i < end; ++i) {
        const f32 value = axis_value(scratch_[i].center, axis);
        axis_min = std::min(axis_min, value);
        axis_max = std::max(axis_max, value);
    }

    // All centroids coincide on the widest axis (so, since it is the widest, on
    // every axis): no spatial split exists. The caller falls back to the median,
    // which still halves the range.
    const f32 axis_extent = axis_max - axis_min;
    if (!(axis_extent > 0.0f)) {
        return choice;
    }

    struct Bin {
        Aabb bounds = Aabb::empty();
        usize count = 0;
    };
    std::array<Bin, kBinCount> bins{};
    const f32 scale = static_cast<f32>(kBinCount) / axis_extent;

    const auto bin_of = [&](const BuildPrimitive& prim) {
        const usize bin = static_cast<usize>((axis_value(prim.center, axis) - axis_min) * scale);
        return bin < kBinCount ? bin : kBinCount - 1;
    };

    for (usize i = begin; i < end; ++i) {
        const usize bin = bin_of(scratch_[i]);
        bins[bin].bounds = bins[bin].bounds.expanded(scratch_[i].bounds);
        ++bins[bin].count;
    }

    // Suffix pass: the bounds and count of everything from bin `i` rightward.
    std::array<Aabb, kBinCount> suffix_bounds{};
    std::array<usize, kBinCount> suffix_counts{};
    Aabb running = Aabb::empty();
    usize running_count = 0;
    for (usize i = kBinCount; i-- > 0;) {
        running = running.expanded(bins[i].bounds);
        running_count += bins[i].count;
        suffix_bounds[i] = running;
        suffix_counts[i] = running_count;
    }

    // Sweep left to right. A split between bin `b` and bin `b + 1` puts bins
    // `[0, b]` on the left; its SAH cost is the surface area of each side times
    // its primitive count. The parent cost is what "leave them together" costs,
    // so a split is only taken when it is cheaper -- the gain test that lets the
    // tree stop early.
    Aabb prefix = Aabb::empty();
    usize prefix_count = 0;
    const f32 parent_cost = half_area(union_bounds(bounds_, scratch_, begin, end));
    f32 best_cost = parent_cost;
    usize best_boundary = 0;
    for (usize b = 0; b + 1 < kBinCount; ++b) {
        prefix = prefix.expanded(bins[b].bounds);
        prefix_count += bins[b].count;
        const usize right_count = suffix_counts[b + 1];
        if (prefix_count == 0 || right_count == 0) {
            continue;
        }
        const f32 cost = half_area(prefix) * static_cast<f32>(prefix_count) +
                         half_area(suffix_bounds[b + 1]) * static_cast<f32>(right_count);
        if (cost < best_cost) {
            best_cost = cost;
            best_boundary = b + 1;
            choice.gain = true;
        }
    }

    if (!choice.gain) {
        choice.position = begin + count / 2;
        return choice;
    }

    // Partition in place so everything in a bin below the boundary comes first.
    // The order within each side is not stable, but the leaf ranges are re-sorted
    // by primitive index after the build, so the tree does not depend on it.
    auto* first = scratch_.data() + begin;
    auto* last = scratch_.data() + end;
    auto* middle =
        std::partition(first, last, [&](const BuildPrimitive& prim) { return bin_of(prim) < best_boundary; });
    choice.position = static_cast<usize>(middle - scratch_.data());
    return choice;
}

u32 SpatialBvh::build_range(usize begin, usize end, u32 depth) {
    const u32 node_index = static_cast<u32>(nodes_.size());
    nodes_.push_back(Node{});
    nodes_[node_index].bounds = union_bounds(bounds_, scratch_, begin, end);
    nodes_[node_index].first = static_cast<u32>(begin);
    nodes_[node_index].count = static_cast<u32>(end - begin);
    nodes_[node_index].left = kNoChild;
    nodes_[node_index].right = kNoChild;

    const usize count = end - begin;
    if (count <= 1) {
        return node_index;
    }

    // A node splits when it is over the leaf cap, when the depth cap is hit (the
    // cap exists only to bound recursion, so a capped node still splits until it
    // holds one primitive), or when the SAH finds a cheaper split than leaving
    // the primitives together. The leaf cap is a hard rule: a node larger than it
    // must split even when the SAH sees no gain, which is what makes the build
    // terminate on a scene whose centroids all coincide.
    const bool at_depth_cap = depth >= kMaxDepth;
    const bool must_split = count > kMaxLeafSize || at_depth_cap;
    const SplitChoice choice = choose_split(begin, end);
    usize split = choice.gain ? choice.position : (begin + count / 2);

    if (!choice.gain && !must_split) {
        return node_index;
    }
    // The median fallback always halves, so this holds for `count > 1`; the
    // guard is defensive against a degenerate partition position.
    if (split <= begin || split >= end) {
        split = begin + count / 2;
    }
    if (split <= begin || split >= end) {
        return node_index;
    }

    const u32 left = build_range(begin, split, depth + 1);
    const u32 right = build_range(split, end, depth + 1);
    nodes_[node_index].left = left;
    nodes_[node_index].right = right;
    return node_index;
}

void SpatialBvh::collect(const Frustum& frustum, u32 node, std::vector<u32>& out) const {
    const Node& current = nodes_[node];
    if (!frustum.intersects_aabb(current.bounds.min, current.bounds.max)) {
        return;
    }
    if (current.is_leaf()) {
        for (u32 i = 0; i < current.count; ++i) {
            const u32 primitive = primitives_[current.first + i];
            const Aabb& box = bounds_[primitive];
            if (frustum.intersects_aabb(box.min, box.max)) {
                out.push_back(primitive);
            }
        }
        return;
    }
    collect(frustum, current.left, out);
    collect(frustum, current.right, out);
}

void SpatialBvh::collect(const Aabb& region, u32 node, std::vector<u32>& out) const {
    const Node& current = nodes_[node];
    if (!current.bounds.overlaps(region)) {
        return;
    }
    if (current.is_leaf()) {
        for (u32 i = 0; i < current.count; ++i) {
            const u32 primitive = primitives_[current.first + i];
            if (bounds_[primitive].overlaps(region)) {
                out.push_back(primitive);
            }
        }
        return;
    }
    collect(region, current.left, out);
    collect(region, current.right, out);
}

void SpatialBvh::query(const Frustum& frustum, std::vector<u32>& out) const {
    out.clear();
    if (nodes_.empty()) {
        return;
    }
    collect(frustum, 0, out);
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
}

void SpatialBvh::query(const Aabb& region, std::vector<u32>& out) const {
    out.clear();
    if (nodes_.empty()) {
        return;
    }
    collect(region, 0, out);
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
}

} // namespace fe
