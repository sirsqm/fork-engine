// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Types.h"

#include <vector>

namespace fe::physics {

// One link between two dynamic bodies that makes them share an island: a contact
// or a joint. Both endpoints are indices into the island builder's body array.
//
// The link is undirected: `a` and `b` name a pair, not a direction. The builder
// normalises the order internally, so passing (3, 1) or (1, 3) yields the same
// island.
struct IslandLink {
    usize a = 0;
    usize b = 0;
};

// Groups bodies into islands: the connected components of the graph whose
// vertices are dynamic bodies and whose edges are contacts and joints.
//
// Two dynamic bodies that touch, or that a joint connects, are in one island. A
// link to a static body (a floor, a wall, or any immovable collider) is *not* an
// edge: static geometry does not conduct motion between the bodies resting on
// it, so a crate on the left of a floor and a crate on the right are separate
// islands even though both touch the same floor. That is what lets one settle
// and sleep while the other is still moving.
//
// This type is pure: it knows only indices, no scene, no components, and it does
// no physics. It is the part of sleeping worth testing exhaustively -- "which
// bodies are grouped together" is a graph question, and it is answered here
// without a world. Both `Physics2DSystem` and `Physics3DSystem` build the links
// from their contacts and joints and call `build`; a test can call `build` with
// hand-written links. It lives in the shared `physics` module rather than in
// either dimension so the two systems share one implementation and one set of
// graph tests (ADR-0111).
//
// The result is deterministic: bodies are numbered 0..n-1 in the order they were
// given, and the groups are returned sorted by their lowest member. Two identical
// inputs yield an identical output, which is what keeps a sleeping simulation
// reproducible.
class IslandBuilder {
public:
    // Groups `body_count` bodies given the links between them. Clears and
    // rebuilds the internal state, reusing its buffers, so a steady-state step
    // allocates nothing.
    void build(usize body_count, const std::vector<IslandLink>& links);

    // The islands, each a sorted list of body indices, sorted by their first
    // member. Empty until `build` is called.
    [[nodiscard]] const std::vector<std::vector<usize>>& islands() const noexcept { return islands_; }

    // The number of islands found by the most recent `build`.
    [[nodiscard]] usize island_count() const noexcept { return islands_.size(); }

    // Which island each body belongs to, indexed by body, or a value >=
    // island_count() for a body that was not given (defensive; every body
    // 0..body_count-1 gets a group). A body with no links is an island of one.
    [[nodiscard]] const std::vector<usize>& island_of_body() const noexcept { return island_of_body_; }

    // True when the last build put `a` and `b` in the same island. False when
    // either index is out of range, so a caller cannot read past the arrays.
    [[nodiscard]] bool same_island(usize a, usize b) const noexcept {
        if (a >= island_of_body_.size() || b >= island_of_body_.size()) {
            return false;
        }
        return island_of_body_[a] == island_of_body_[b];
    }

private:
    // Union-find parent links, one per body, rebuilt each `build`. A negative
    // parent is a root whose absolute value is the component size.
    std::vector<i32> parent_;

    std::vector<std::vector<usize>> islands_;
    std::vector<usize> island_of_body_;
};

} // namespace fe::physics
