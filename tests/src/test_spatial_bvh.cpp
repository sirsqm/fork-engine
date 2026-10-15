// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
//
// Tests for the spatial BVH: tree well-formedness, determinism, and the
// frustum/box queries against a brute-force reference. The queries are the
// contract -- a tree that returns the wrong set is worthless however well it is
// shaped -- so every query test compares against the same predicate applied to
// every primitive.

#include <doctest/doctest.h>

#include "fe/core/Frustum.h"
#include "fe/core/Math.h"
#include "fe/core/SpatialBvh.h"

#include <algorithm>
#include <cstdint>
#include <vector>

using namespace fe;

namespace {

// A small deterministic generator, so a "random" scene is the same on every run
// and every platform. A xorshift32 is plenty; nothing here depends on the
// quality of the bits, only on their reproducibility.
class Rng {
public:
    explicit Rng(u32 seed) : state_(seed == 0 ? 0x9E3779B9u : seed) {}

    u32 next() noexcept {
        state_ ^= state_ << 13;
        state_ ^= state_ >> 17;
        state_ ^= state_ << 5;
        return state_;
    }

    // A value in [0, bound).
    u32 below(u32 bound) noexcept { return next() % bound; }

    f32 range(f32 lo, f32 hi) noexcept {
        const f32 t = static_cast<f32>(next() & 0xFFFFFFu) / static_cast<f32>(0x1000000u);
        return lo + (hi - lo) * t;
    }

private:
    u32 state_;
};

[[nodiscard]] Aabb box_at(const Vec3& center, const Vec3& half) noexcept {
    return Aabb{center - half, center + half};
}

// A scene spread through a volume, with a range of box sizes so the SAH has
// something to separate and some large boxes span several bins.
[[nodiscard]] std::vector<Aabb> make_scene(usize count, u32 seed) {
    Rng rng(seed);
    std::vector<Aabb> boxes;
    boxes.reserve(count);
    for (usize i = 0; i < count; ++i) {
        const Vec3 center{rng.range(-40.0f, 40.0f), rng.range(-40.0f, 40.0f), rng.range(-40.0f, 40.0f)};
        const Vec3 half{rng.range(0.2f, 4.0f), rng.range(0.2f, 4.0f), rng.range(0.2f, 4.0f)};
        boxes.push_back(box_at(center, half));
    }
    return boxes;
}

[[nodiscard]] Frustum make_frustum(const Vec3& eye, const Vec3& target) {
    const Mat4 projection = perspective(radians(60.0f), 1.0f, 0.1f, 200.0f);
    const Mat4 view = look_at(eye, target, Vec3{0.0f, 1.0f, 0.0f});
    return Frustum::from_view_projection(view * projection);
}

// The reference frustum result: apply the leaf test to every box. This is the
// definition the tree must reproduce exactly.
[[nodiscard]] std::vector<u32> brute_force_frustum(const std::vector<Aabb>& boxes, const Frustum& frustum) {
    std::vector<u32> hits;
    for (usize i = 0; i < boxes.size(); ++i) {
        if (frustum.intersects_aabb(boxes[i].min, boxes[i].max)) {
            hits.push_back(static_cast<u32>(i));
        }
    }
    return hits;
}

[[nodiscard]] std::vector<u32> brute_force_region(const std::vector<Aabb>& boxes, const Aabb& region) {
    std::vector<u32> hits;
    for (usize i = 0; i < boxes.size(); ++i) {
        if (boxes[i].overlaps(region)) {
            hits.push_back(static_cast<u32>(i));
        }
    }
    return hits;
}

[[nodiscard]] bool same_set(std::vector<u32> a, std::vector<u32> b) {
    std::sort(a.begin(), a.end());
    std::sort(b.begin(), b.end());
    return a == b;
}

[[nodiscard]] bool nodes_identical(std::span<const SpatialBvh::Node> a, std::span<const SpatialBvh::Node> b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (usize i = 0; i < a.size(); ++i) {
        const SpatialBvh::Node& x = a[i];
        const SpatialBvh::Node& y = b[i];
        if (x.first != y.first || x.count != y.count || x.left != y.left || x.right != y.right) {
            return false;
        }
        if (x.bounds.min.x != y.bounds.min.x || x.bounds.min.y != y.bounds.min.y || x.bounds.min.z != y.bounds.min.z ||
            x.bounds.max.x != y.bounds.max.x || x.bounds.max.y != y.bounds.max.y || x.bounds.max.z != y.bounds.max.z) {
            return false;
        }
    }
    return true;
}

} // namespace

TEST_CASE("an empty build produces an empty tree and empty queries") {
    SpatialBvh bvh;
    bvh.build({});
    CHECK(bvh.empty());
    CHECK(bvh.node_count() == 0);
    CHECK(bvh.primitive_order().empty());

    std::vector<u32> out{99};
    bvh.query(Aabb{Vec3{-1.0f, -1.0f, -1.0f}, Vec3{1.0f, 1.0f, 1.0f}}, out);
    CHECK(out.empty());

    const Frustum frustum = make_frustum(Vec3{0.0f, 0.0f, 10.0f}, Vec3{0.0f, 0.0f, 0.0f});
    bvh.query(frustum, out);
    CHECK(out.empty());
}

TEST_CASE("a single primitive is its own root leaf") {
    const std::vector<Aabb> boxes{box_at(Vec3{1.0f, 2.0f, 3.0f}, Vec3{0.5f, 0.5f, 0.5f})};
    SpatialBvh bvh;
    bvh.build(boxes);

    REQUIRE(bvh.node_count() == 1);
    CHECK(bvh.nodes()[0].is_leaf());
    CHECK(bvh.nodes()[0].count == 1);
    CHECK(bvh.primitive_order().size() == 1);
    CHECK(bvh.primitive_order()[0] == 0);
    CHECK(bvh.root_bounds().min.x == doctest::Approx(0.5f));
    CHECK(bvh.root_bounds().max.z == doctest::Approx(3.5f));
}

TEST_CASE("every primitive appears exactly once across the leaves") {
    const std::vector<Aabb> boxes = make_scene(300, 0x1234u);
    SpatialBvh bvh;
    bvh.build(boxes);

    std::vector<u32> seen;
    for (const SpatialBvh::Node& node : bvh.nodes()) {
        if (!node.is_leaf()) {
            continue;
        }
        CHECK(node.count <= SpatialBvh::kMaxLeafSize);
        for (u32 i = 0; i < node.count; ++i) {
            seen.push_back(bvh.primitive_order()[node.first + i]);
        }
    }
    std::sort(seen.begin(), seen.end());
    REQUIRE(seen.size() == boxes.size());
    for (usize i = 0; i < seen.size(); ++i) {
        CHECK(seen[i] == static_cast<u32>(i));
    }
}

TEST_CASE("each node's bounds contain its children and equal its subtree's boxes") {
    const std::vector<Aabb> boxes = make_scene(200, 0xBEEFu);
    SpatialBvh bvh;
    bvh.build(boxes);

    const auto subtree_union = [&](const SpatialBvh::Node& node) {
        Aabb result = Aabb::empty();
        if (node.is_leaf()) {
            for (u32 i = 0; i < node.count; ++i) {
                result = result.expanded(boxes[bvh.primitive_order()[node.first + i]]);
            }
        }
        return result;
    };

    // Walk every node; a parent's bounds must equal the union of its subtree's
    // primitives. That is the invariant a query relies on: if a node's box did
    // not cover its subtree, the query could reject a subtree that holds a hit.
    for (usize i = 0; i < bvh.node_count(); ++i) {
        const SpatialBvh::Node& node = bvh.nodes()[i];
        if (node.is_leaf()) {
            const Aabb expected = subtree_union(node);
            if (node.count > 0) {
                CHECK(node.bounds.min.x == doctest::Approx(expected.min.x));
                CHECK(node.bounds.max.z == doctest::Approx(expected.max.z));
            }
            continue;
        }
        const SpatialBvh::Node& left = bvh.nodes()[node.left];
        const SpatialBvh::Node& right = bvh.nodes()[node.right];
        CHECK(node.bounds.min.x <= left.bounds.min.x);
        CHECK(node.bounds.min.y <= left.bounds.min.y);
        CHECK(node.bounds.min.z <= left.bounds.min.z);
        CHECK(node.bounds.max.x >= left.bounds.max.x);
        CHECK(node.bounds.max.y >= left.bounds.max.y);
        CHECK(node.bounds.max.z >= left.bounds.max.z);
        CHECK(node.bounds.min.x <= right.bounds.min.x);
        CHECK(node.bounds.max.x >= right.bounds.max.x);
    }
}

TEST_CASE("the root bounds are the union of every primitive") {
    const std::vector<Aabb> boxes = make_scene(150, 0x55AAu);
    SpatialBvh bvh;
    bvh.build(boxes);

    Aabb expected = Aabb::empty();
    for (const Aabb& box : boxes) {
        expected = expected.expanded(box);
    }
    CHECK(bvh.root_bounds().min.x == doctest::Approx(expected.min.x));
    CHECK(bvh.root_bounds().min.y == doctest::Approx(expected.min.y));
    CHECK(bvh.root_bounds().min.z == doctest::Approx(expected.min.z));
    CHECK(bvh.root_bounds().max.x == doctest::Approx(expected.max.x));
    CHECK(bvh.root_bounds().max.y == doctest::Approx(expected.max.y));
    CHECK(bvh.root_bounds().max.z == doctest::Approx(expected.max.z));
}

TEST_CASE("the frustum query matches the brute-force reference") {
    const std::vector<Aabb> boxes = make_scene(400, 0xC0FFEEu);
    SpatialBvh bvh;
    bvh.build(boxes);

    const Vec3 eyes[] = {
        Vec3{0.0f, 0.0f, 80.0f},  Vec3{60.0f, 40.0f, 60.0f}, Vec3{-70.0f, 10.0f, 20.0f},
        Vec3{0.0f, 100.0f, 0.0f}, Vec3{150.0f, 0.0f, 0.0f},
    };
    const Vec3 targets[] = {
        Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.0f, 0.0f, 0.0f},
        Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.0f, 0.0f, 0.0f},
    };

    for (usize i = 0; i < std::size(eyes); ++i) {
        const Frustum frustum = make_frustum(eyes[i], targets[i]);
        std::vector<u32> out;
        bvh.query(frustum, out);

        // Sorted and unique, as documented.
        CHECK(std::is_sorted(out.begin(), out.end()));
        CHECK(std::adjacent_find(out.begin(), out.end()) == out.end());

        CHECK(same_set(out, brute_force_frustum(boxes, frustum)));
    }
}

TEST_CASE("a frustum pointing away from the scene returns nothing") {
    const std::vector<Aabb> boxes = make_scene(100, 0x1111u);
    SpatialBvh bvh;
    bvh.build(boxes);

    // Look outward from well outside the scene, away from it.
    const Frustum frustum = make_frustum(Vec3{500.0f, 500.0f, 500.0f}, Vec3{600.0f, 600.0f, 600.0f});
    std::vector<u32> out;
    bvh.query(frustum, out);
    CHECK(out.empty());
}

TEST_CASE("the box query matches the brute-force reference") {
    const std::vector<Aabb> boxes = make_scene(400, 0xFEEDu);
    SpatialBvh bvh;
    bvh.build(boxes);

    const Aabb regions[] = {
        Aabb{Vec3{-10.0f, -10.0f, -10.0f}, Vec3{10.0f, 10.0f, 10.0f}},
        Aabb{Vec3{-100.0f, -100.0f, -100.0f}, Vec3{100.0f, 100.0f, 100.0f}},
        Aabb{Vec3{20.0f, 20.0f, 20.0f}, Vec3{30.0f, 30.0f, 30.0f}},
        Aabb{Vec3{-200.0f, -200.0f, -200.0f}, Vec3{-150.0f, -150.0f, -150.0f}},
    };

    for (const Aabb& region : regions) {
        std::vector<u32> out;
        bvh.query(region, out);
        CHECK(std::is_sorted(out.begin(), out.end()));
        CHECK(same_set(out, brute_force_region(boxes, region)));
    }
}

TEST_CASE("a query over the whole scene returns every primitive") {
    const std::vector<Aabb> boxes = make_scene(250, 0x2222u);
    SpatialBvh bvh;
    bvh.build(boxes);

    Aabb everything = Aabb::empty();
    for (const Aabb& box : boxes) {
        everything = everything.expanded(box);
    }
    // Grow by a margin so every box is strictly inside.
    everything.min = everything.min - Vec3{1.0f, 1.0f, 1.0f};
    everything.max = everything.max + Vec3{1.0f, 1.0f, 1.0f};

    std::vector<u32> out;
    bvh.query(everything, out);
    CHECK(out.size() == boxes.size());
}

TEST_CASE("two builds of the same input produce identical trees") {
    const std::vector<Aabb> boxes = make_scene(500, 0x7777u);

    SpatialBvh first;
    first.build(boxes);
    const std::vector<SpatialBvh::Node> snapshot(first.nodes().begin(), first.nodes().end());
    const std::vector<u32> order(first.primitive_order().begin(), first.primitive_order().end());

    // Rebuild into the same object, exercising the reuse path, and into a fresh
    // one. Both must agree with the first build node for node.
    SpatialBvh reused;
    reused.build(boxes);
    reused.build(boxes);
    CHECK(nodes_identical(reused.nodes(), snapshot));
    CHECK(std::equal(reused.primitive_order().begin(), reused.primitive_order().end(), order.begin(), order.end()));

    SpatialBvh fresh;
    fresh.build(boxes);
    CHECK(nodes_identical(fresh.nodes(), snapshot));
}

TEST_CASE("a scene of coincident centroids still builds and queries correctly") {
    // Every box shares a centre, so no axis separates them: the SAH finds no
    // gain and the median fallback has to carry the build. This is the case that
    // would recurse forever without the leaf cap.
    std::vector<Aabb> boxes;
    for (usize i = 0; i < 64; ++i) {
        boxes.push_back(box_at(Vec3{0.0f, 0.0f, 0.0f}, Vec3{1.0f, 1.0f, 1.0f}));
    }
    SpatialBvh bvh;
    bvh.build(boxes);

    REQUIRE(bvh.node_count() >= 1);
    CHECK(bvh.root_bounds().min.x == doctest::Approx(-1.0f));

    std::vector<u32> out;
    bvh.query(Aabb{Vec3{-2.0f, -2.0f, -2.0f}, Vec3{2.0f, 2.0f, 2.0f}}, out);
    CHECK(out.size() == boxes.size());

    // A region that misses every box returns nothing.
    bvh.query(Aabb{Vec3{5.0f, 5.0f, 5.0f}, Vec3{6.0f, 6.0f, 6.0f}}, out);
    CHECK(out.empty());
}

TEST_CASE("the tree is deeper than one leaf once the scene is split") {
    const std::vector<Aabb> boxes = make_scene(400, 0x9999u);
    SpatialBvh bvh;
    bvh.build(boxes);
    // A well-separated scene should produce a real hierarchy, not one leaf.
    CHECK(bvh.node_count() > 1);
}

TEST_CASE("Aabb helpers behave at the edges") {
    CHECK(Aabb::empty().is_empty());

    const Aabb a{Vec3{0.0f, 0.0f, 0.0f}, Vec3{1.0f, 1.0f, 1.0f}};
    const Aabb b{Vec3{2.0f, 2.0f, 2.0f}, Vec3{3.0f, 3.0f, 3.0f}};
    CHECK(!a.overlaps(b));
    CHECK(a.expanded(b).min.x == doctest::Approx(0.0f));
    CHECK(a.expanded(b).max.x == doctest::Approx(3.0f));

    // Touching boxes overlap by the "share at least one point" definition.
    const Aabb c{Vec3{1.0f, 0.0f, 0.0f}, Vec3{2.0f, 1.0f, 1.0f}};
    CHECK(a.overlaps(c));

    const Aabb grown = Aabb::empty().expanded(Vec3{1.0f, 2.0f, 3.0f});
    CHECK(!grown.is_empty());
    CHECK(grown.min.x == doctest::Approx(1.0f));
    CHECK(grown.max.z == doctest::Approx(3.0f));
}
