// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Types.h"
#include "fe/physics2d/Geometry.h"

#include <span>
#include <vector>

namespace fe::physics2d {

// A candidate overlapping pair, as indices into the bounds array the broad phase
// was built from. `first < second` always, so a pair has one canonical form and
// the set is comparable regardless of the order bodies were inserted.
struct BroadPhasePair {
    usize first = 0;
    usize second = 0;

    [[nodiscard]] bool operator==(const BroadPhasePair& other) const noexcept {
        return first == other.first && second == other.second;
    }
    [[nodiscard]] bool operator<(const BroadPhasePair& other) const noexcept {
        if (first != other.first) {
            return first < other.first;
        }
        return second < other.second;
    }
};

// A uniform-grid (spatial hash) broad phase over a set of axis-aligned bounds.
//
// The solver used to test every pair of bodies, and it rebuilt that pair set on
// every sequential-impulse iteration, so a step did O(n^2 * iterations) work.
// The grid replaces the pair enumeration with "insert each bound into the cells
// it overlaps, then a pair is a candidate if the two share a cell". Sharing a
// cell is necessary but not sufficient: the caller still runs the existing
// `bounds_overlap` prefilter and then the narrow phase, so the grid is free to
// be conservative and return extra candidates.
//
// The result is a *superset* of the pairs whose bounds truly overlap, and that
// is exact rather than probabilistic: if two boxes overlap there is a point in
// both, that point lies in some cell, and both boxes were inserted into it. So
// the grid never drops a pair, for any positive cell size.
//
// This type is pure: it knows only bounds and indices, no scene, no components,
// no allocation once its scratch has grown. It is the part of the broad phase
// worth testing exhaustively, so it is kept free of anything that would make
// that harder -- the same reason `Geometry.h` and `JointSolver.h` are separate.
class BroadPhase {
public:
    // The grid cell is this multiple of the mean body extent. Two is the usual
    // choice: small enough that a cell holds few bodies (keeping the per-cell
    // pair count low) and large enough that a body rarely spans many cells.
    static constexpr f32 kCellSizeScale = 2.0f;

    // The floor on the cell size, in world units. A scene of point-sized bodies
    // would otherwise pick a zero cell size and divide by zero.
    static constexpr f32 kMinCellSize = 0.1f;

    // Rebuilds the candidate set for `bounds`. Call once per step, before the
    // solver's iteration loop, not once per iteration.
    //
    // Deterministic: the output order depends only on the input order and the
    // geometry, never on hash iteration or address. Scratch buffers are reused,
    // so a steady-state rebuild allocates nothing.
    void build(std::span<const Aabb2D> bounds);

    // The candidate pairs, sorted by (first, second) index and de-duplicated.
    [[nodiscard]] std::span<const BroadPhasePair> pairs() const noexcept { return pairs_; }
    [[nodiscard]] usize candidate_count() const noexcept { return pairs_.size(); }

    // The cell size chosen for the most recent build, exposed for tests and the
    // editor's debug overlay.
    [[nodiscard]] f32 cell_size() const noexcept { return cell_size_; }

private:
    // One (cell, body) insertion. A body that spans several cells contributes
    // one entry per cell.
    struct CellEntry {
        u64 cell = 0;
        usize body = 0;
    };

    [[nodiscard]] static u64 cell_key(i32 x, i32 y) noexcept;

    std::vector<CellEntry> entries_;
    std::vector<BroadPhasePair> pairs_;
    f32 cell_size_ = kMinCellSize;
};

} // namespace fe::physics2d
