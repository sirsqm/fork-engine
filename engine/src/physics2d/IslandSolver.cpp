// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/physics2d/IslandSolver.h"

#include <utility>
#include <vector>

namespace fe::physics2d {
namespace {

// Union-find with path compression and union by size. A negative parent marks a
// root; its magnitude is the component size, which union-by-size reads to
// attach the smaller tree under the larger.
//
// The compression walks to the root, then points every node on the path
// directly at it. It must not use the one-liner `parent[i] = parent[parent[i]]`:
// with this representation a root's parent is its *negative size*, not an index,
// so that form would write a negative value into a non-root and corrupt the
// structure.
usize find_root(std::vector<i32>& parent, usize index) {
    usize root = index;
    while (parent[root] >= 0) {
        root = static_cast<usize>(parent[root]);
    }
    while (parent[index] >= 0) {
        const usize next = static_cast<usize>(parent[index]);
        parent[index] = static_cast<i32>(root);
        index = next;
    }
    return root;
}

} // namespace

void IslandBuilder::build(usize body_count, const std::vector<IslandLink>& links) {
    // Start with every body in its own island.
    parent_.assign(body_count, -1);

    for (const IslandLink& link : links) {
        // A link naming a body that does not exist is a caller bug; skip it
        // rather than reading out of bounds. A self-link is already one island.
        if (link.a >= body_count || link.b >= body_count || link.a == link.b) {
            continue;
        }
        usize root_a = find_root(parent_, link.a);
        usize root_b = find_root(parent_, link.b);
        if (root_a == root_b) {
            continue;
        }
        // Attach the smaller component under the larger, so the tree stays
        // shallow. Ties go to `root_a`, which keeps the merge deterministic.
        if (parent_[root_a] > parent_[root_b]) {
            std::swap(root_a, root_b);
        }
        parent_[root_a] += parent_[root_b];
        parent_[root_b] = static_cast<i32>(root_a);
    }

    // Collect each root's members. Visiting bodies in ascending order and
    // creating an island the first time a root is seen makes the islands sorted
    // by their lowest member and each island's members sorted ascending -- both
    // by construction, not by a later sort.
    islands_.clear();
    island_of_body_.assign(body_count, 0);
    std::vector<usize> root_island(body_count, static_cast<usize>(-1));

    for (usize body = 0; body < body_count; ++body) {
        const usize root = find_root(parent_, body);
        if (root_island[root] == static_cast<usize>(-1)) {
            root_island[root] = islands_.size();
            islands_.emplace_back();
        }
        islands_[root_island[root]].push_back(body);
        island_of_body_[body] = root_island[root];
    }
}

} // namespace fe::physics2d
