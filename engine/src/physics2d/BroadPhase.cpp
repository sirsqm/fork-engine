// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/physics2d/BroadPhase.h"

#include "fe/jobs/JobSystem.h"

#include <algorithm>
#include <cmath>

namespace fe::physics2d {
namespace {

// The cell range a body's bounds overlaps. Returns false when the body is
// skipped (a negative extent is a malformed bound). Shared by the serial insert
// and both parallel passes, so the count and the write can never disagree on
// which cells a body touches.
[[nodiscard]] bool body_cell_range(const Aabb2D& box, f32 inv_cell, i32& min_cx, i32& max_cx, i32& min_cy,
                                   i32& max_cy) noexcept {
    if (box.half_extent.x < 0.0f || box.half_extent.y < 0.0f) {
        return false;
    }
    min_cx = static_cast<i32>(std::floor(box.min_x() * inv_cell));
    max_cx = static_cast<i32>(std::floor(box.max_x() * inv_cell));
    min_cy = static_cast<i32>(std::floor(box.min_y() * inv_cell));
    max_cy = static_cast<i32>(std::floor(box.max_y() * inv_cell));
    return true;
}

// The number of (cell, body) entries one body contributes.
[[nodiscard]] usize body_entry_count(const Aabb2D& box, f32 inv_cell) noexcept {
    i32 min_cx = 0;
    i32 max_cx = 0;
    i32 min_cy = 0;
    i32 max_cy = 0;
    if (!body_cell_range(box, inv_cell, min_cx, max_cx, min_cy, max_cy)) {
        return 0;
    }
    return static_cast<usize>(max_cy - min_cy + 1) * static_cast<usize>(max_cx - min_cx + 1);
}

} // namespace

u64 BroadPhase::cell_key(i32 x, i32 y) noexcept {
    // Pack the two cell coordinates into one integer so a cell is a single
    // sortable value. Reinterpreting through u32 is deliberate: the ordering is
    // only used for grouping, and a wrap at the coordinate extremes still groups
    // equal cells together.
    return (static_cast<u64>(static_cast<u32>(x)) << 32) | static_cast<u32>(y);
}

void BroadPhase::build(std::span<const Aabb2D> bounds, JobSystem* jobs) {
    pairs_.clear();
    entries_.clear();
    if (bounds.empty()) {
        return;
    }

    // Cell size from the bodies' own scale: the mean of the larger extent of
    // each body's bounds, scaled. A scene of tiny bodies gets a tiny cell and
    // one of large bodies a large cell, so the number of cells a body spans
    // stays roughly constant without the caller tuning anything.
    f64 extent_sum = 0.0;
    for (const Aabb2D& box : bounds) {
        const f32 extent = std::max(2.0f * box.half_extent.x, 2.0f * box.half_extent.y);
        extent_sum += extent > 0.0f ? static_cast<f64>(extent) : 0.0;
    }
    const f32 mean_extent = static_cast<f32>(extent_sum / static_cast<f64>(bounds.size()));
    cell_size_ = std::max(mean_extent * kCellSizeScale, kMinCellSize);

    const f32 inv_cell = 1.0f / cell_size_;

    // Insert. A body is added to every cell its bounds overlaps, so a pair that
    // straddles a cell boundary still shares at least one cell and is found --
    // the classic spatial-hash bug is to key a body by its centre cell alone,
    // which misses exactly those pairs.
    //
    // This is the O(n * cells-per-body) part of the build, so it is the part
    // worth fanning out. The parallel path writes the same entries in the same
    // order as the serial one, so everything downstream -- the sort, the pair
    // emission, the final dedup -- sees an identical array and produces an
    // identical pair set.
    const bool use_jobs = jobs != nullptr && jobs->is_threaded() && bounds.size() >= kParallelBuildThreshold;
    if (!use_jobs) {
        for (usize body = 0; body < bounds.size(); ++body) {
            i32 min_cx = 0;
            i32 max_cx = 0;
            i32 min_cy = 0;
            i32 max_cy = 0;
            if (!body_cell_range(bounds[body], inv_cell, min_cx, max_cx, min_cy, max_cy)) {
                continue;
            }
            for (i32 cy = min_cy; cy <= max_cy; ++cy) {
                for (i32 cx = min_cx; cx <= max_cx; ++cx) {
                    entries_.push_back(CellEntry{cell_key(cx, cy), body});
                }
            }
        }
    } else {
        const usize n = bounds.size();
        const u32 chunks = static_cast<u32>(std::min<usize>(n, static_cast<usize>(jobs->worker_count()) * 4));

        // Count each chunk's entries, then turn the counts into offsets. Each
        // chunk then writes straight into its own slice, so the concatenated
        // result is exactly the serial entry array -- same values, same order --
        // with no per-chunk vector to allocate or join.
        std::vector<usize> counts(chunks, 0);
        jobs->parallel_for(0, chunks, [&](usize c) {
            const usize lo = n * c / chunks;
            const usize hi = n * (c + 1) / chunks;
            usize count = 0;
            for (usize i = lo; i < hi; ++i) {
                count += body_entry_count(bounds[i], inv_cell);
            }
            counts[c] = count;
        });

        usize total = 0;
        for (u32 c = 0; c < chunks; ++c) {
            const usize count = counts[c];
            counts[c] = total;
            total += count;
        }

        entries_.resize(total);
        jobs->parallel_for(0, chunks, [&](usize c) {
            const usize lo = n * c / chunks;
            const usize hi = n * (c + 1) / chunks;
            usize write = counts[c];
            for (usize body = lo; body < hi; ++body) {
                i32 min_cx = 0;
                i32 max_cx = 0;
                i32 min_cy = 0;
                i32 max_cy = 0;
                if (!body_cell_range(bounds[body], inv_cell, min_cx, max_cx, min_cy, max_cy)) {
                    continue;
                }
                for (i32 cy = min_cy; cy <= max_cy; ++cy) {
                    for (i32 cx = min_cx; cx <= max_cx; ++cx) {
                        entries_[write++] = CellEntry{cell_key(cx, cy), body};
                    }
                }
            }
        });
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
                pairs_.push_back(BroadPhasePair{a, b});
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

} // namespace fe::physics2d
