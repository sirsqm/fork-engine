// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/physics3d/BroadPhase3D.h"

#include <algorithm>
#include <cmath>

namespace fe::physics3d {

u64 BroadPhase3D::cell_key(i32 x, i32 y, i32 z) noexcept {
    // Mix the three cell coordinates into one integer so a cell is a single
    // sortable value. The mixing is a cheap 3D hash; the ordering is only used
    // for grouping, and equal cells still produce equal keys. The multipliers are
    // large odd primes so that nearby cells on different axes rarely collide.
    const u64 ux = static_cast<u32>(x);
    const u64 uy = static_cast<u32>(y);
    const u64 uz = static_cast<u32>(z);
    return ux * 73856093ULL ^ uy * 19349663ULL ^ uz * 83492791ULL;
}

void BroadPhase3D::build(std::span<const Aabb3D> bounds) {
    pairs_.clear();
    entries_.clear();
    if (bounds.empty()) {
        return;
    }

    // Cell size from the bodies' own scale: the mean of the largest extent of
    // each body's bounds, scaled. A scene of tiny bodies gets a tiny cell and one
    // of large bodies a large cell, so the number of cells a body spans stays
    // roughly constant without the caller tuning anything.
    f64 extent_sum = 0.0;
    for (const Aabb3D& box : bounds) {
        const f32 extent = std::max({2.0f * box.half_extent.x, 2.0f * box.half_extent.y, 2.0f * box.half_extent.z});
        extent_sum += extent > 0.0f ? static_cast<f64>(extent) : 0.0;
    }
    const f32 mean_extent = static_cast<f32>(extent_sum / static_cast<f64>(bounds.size()));
    cell_size_ = std::max(mean_extent * kCellSizeScale, kMinCellSize);

    const f32 inv_cell = 1.0f / cell_size_;

    // Insert. A body is added to every cell its bounds overlaps, so a pair that
    // straddles a cell boundary still shares at least one cell and is found --
    // the classic spatial-hash bug is to key a body by its centre cell alone,
    // which misses exactly those pairs.
    for (usize body = 0; body < bounds.size(); ++body) {
        const Aabb3D& box = bounds[body];
        if (box.half_extent.x < 0.0f || box.half_extent.y < 0.0f || box.half_extent.z < 0.0f) {
            continue;
        }
        const i32 min_cx = static_cast<i32>(std::floor(box.min_x() * inv_cell));
        const i32 max_cx = static_cast<i32>(std::floor(box.max_x() * inv_cell));
        const i32 min_cy = static_cast<i32>(std::floor(box.min_y() * inv_cell));
        const i32 max_cy = static_cast<i32>(std::floor(box.max_y() * inv_cell));
        const i32 min_cz = static_cast<i32>(std::floor(box.min_z() * inv_cell));
        const i32 max_cz = static_cast<i32>(std::floor(box.max_z() * inv_cell));
        for (i32 cz = min_cz; cz <= max_cz; ++cz) {
            for (i32 cy = min_cy; cy <= max_cy; ++cy) {
                for (i32 cx = min_cx; cx <= max_cx; ++cx) {
                    entries_.push_back(CellEntry{cell_key(cx, cy, cz), body});
                }
            }
        }
    }

    // Group by cell. Sorting the (cell, body) entries puts every body in a cell
    // next to the others, with no hash table and no iteration-order dependence.
    std::sort(entries_.begin(), entries_.end(), [](const CellEntry& a, const CellEntry& b) {
        if (a.cell != b.cell) {
            return a.cell < b.cell;
        }
        return a.body < b.body;
    });

    // Emit a pair for every two bodies that share a cell. Identical pairs are
    // deduplicated later; a duplicate is produced when two bodies span the same
    // two cells (a large body next to another large body, for example).
    usize cell_begin = 0;
    while (cell_begin < entries_.size()) {
        usize cell_end = cell_begin;
        while (cell_end < entries_.size() && entries_[cell_end].cell == entries_[cell_begin].cell) {
            ++cell_end;
        }
        for (usize i = cell_begin; i < cell_end; ++i) {
            for (usize j = i + 1; j < cell_end; ++j) {
                usize a = entries_[i].body;
                usize b = entries_[j].body;
                if (a == b) {
                    continue;
                }
                if (a > b) {
                    std::swap(a, b);
                }
                pairs_.push_back(BroadPhasePair3D{a, b});
            }
        }
        cell_begin = cell_end;
    }

    // Canonical, deterministic order: sort then collapse the adjacent
    // duplicates. The caller (and its tests) can then diff the set against a
    // brute-force reference directly.
    std::sort(pairs_.begin(), pairs_.end());
    pairs_.erase(std::unique(pairs_.begin(), pairs_.end()), pairs_.end());
}

} // namespace fe::physics3d
