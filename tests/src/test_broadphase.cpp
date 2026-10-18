// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include <doctest/doctest.h>

#include "fe/jobs/JobSystem.h"
#include "fe/physics2d/BroadPhase.h"
#include "fe/physics2d/Collider2D.h"
#include "fe/physics2d/Geometry.h"
#include "fe/physics2d/Physics2DSystem.h"
#include "fe/physics2d/RigidBody2D.h"
#include "fe/scene/Node.h"
#include "fe/scene/Scene.h"
#include "fe/systems/FrameContext.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

using namespace fe;

namespace {

physics2d::Aabb2D bounds_box(f32 cx, f32 cy, f32 hx, f32 hy) {
    return physics2d::Aabb2D{Vec2{cx, cy}, Vec2{hx, hy}};
}

FrameContext step(f32 delta) {
    FrameContext context;
    context.delta_time = delta;
    context.elapsed_time = static_cast<f64>(delta);
    return context;
}

// A dynamic box body, mirroring the helper in test_physics2d.cpp.
Node* add_dynamic_box(Scene& scene, const Vec2& position, f32 half, f32 mass) {
    Node* node = scene.create_node("Body");
    auto* collider = node->add_component<physics2d::Collider2D>();
    collider->shape = physics2d::ShapeType::Box;
    collider->half_extent = Vec2{half, half};
    auto* body = node->add_component<physics2d::RigidBody2D>();
    body->mass = mass;
    node->transform().position = Vec3{position.x, position.y, 0.0f};
    return node;
}

Node* add_static_box(Scene& scene, const Vec2& position, f32 half_x, f32 half_y) {
    Node* node = scene.create_node("Wall");
    auto* collider = node->add_component<physics2d::Collider2D>();
    collider->shape = physics2d::ShapeType::Box;
    collider->half_extent = Vec2{half_x, half_y};
    node->transform().position = Vec3{position.x, position.y, 0.0f};
    return node;
}

// A point mass with no collider: a joint-only body, which must never enter the
// broad phase.
Node* add_point_mass(Scene& scene, const Vec2& position, f32 mass) {
    Node* node = scene.create_node("Mass");
    auto* body = node->add_component<physics2d::RigidBody2D>();
    body->mass = mass;
    node->transform().position = Vec3{position.x, position.y, 0.0f};
    return node;
}

// True when `pair` is present in the candidate set. The set is sorted and
// de-duplicated, so a binary search is enough.
bool has_pair(const std::vector<physics2d::BroadPhasePair>& candidates, usize a, usize b) {
    if (a > b) {
        std::swap(a, b);
    }
    return std::binary_search(candidates.begin(), candidates.end(), physics2d::BroadPhasePair{a, b});
}

// Every pair whose bounds actually overlap, found the way the pre-broad-phase
// solver did: a double loop over all bodies with the bounds prefilter.
std::vector<physics2d::BroadPhasePair> brute_force_overlaps(const std::vector<physics2d::Aabb2D>& bounds) {
    std::vector<physics2d::BroadPhasePair> result;
    for (usize i = 0; i < bounds.size(); ++i) {
        for (usize j = i + 1; j < bounds.size(); ++j) {
            if (physics2d::bounds_overlap(bounds[i], bounds[j])) {
                result.push_back(physics2d::BroadPhasePair{i, j});
            }
        }
    }
    std::sort(result.begin(), result.end());
    return result;
}

// A collider and the world position it had at the moment of the snapshot. The
// brute-force reference must use the *pre-solve* positions of the frame under
// test: the solver moves bodies as it resolves them, so re-reading the scene
// afterwards can disagree with the count the system computed at the top of the
// step.
struct ColliderRef {
    physics2d::Collider2D* collider = nullptr;
    Vec2 position{};
};

std::vector<ColliderRef> snapshot_colliders(Scene& scene) {
    std::vector<ColliderRef> refs;
    scene.for_each_node([&](Node& node) {
        auto* collider = node.get_component<physics2d::Collider2D>();
        if (collider != nullptr) {
            refs.push_back(ColliderRef{collider, Vec2{node.transform().position.x, node.transform().position.y}});
        }
    });
    return refs;
}

// The number of overlapping collider pairs in a snapshot: the exact count the
// system's `contact_count()` must equal for that frame.
usize brute_force_contact_count(const std::vector<ColliderRef>& refs) {
    usize count = 0;
    for (usize i = 0; i < refs.size(); ++i) {
        for (usize j = i + 1; j < refs.size(); ++j) {
            const physics2d::Aabb2D a = refs[i].collider->bounds(refs[i].position);
            const physics2d::Aabb2D b = refs[j].collider->bounds(refs[j].position);
            if (!physics2d::bounds_overlap(a, b)) {
                continue;
            }
            const physics2d::Contact2D contact = physics2d::intersect_shapes(
                refs[i].collider->shape, refs[i].collider->box(refs[i].position),
                refs[i].collider->circle(refs[i].position), refs[j].collider->shape,
                refs[j].collider->box(refs[j].position), refs[j].collider->circle(refs[j].position));
            if (contact.overlapping) {
                ++count;
            }
        }
    }
    return count;
}

// A stable, parseable id from a small counter, so two independently built
// scenes can be given the same ids. Node ids are otherwise generated at random,
// which makes an id-level comparison across two scenes meaningless.
Uuid stable_uuid(int n) {
    char buffer[37];
    std::snprintf(buffer, sizeof(buffer), "00000000-0000-0000-0000-%012d", n);
    Uuid id;
    (void)Uuid::try_parse(buffer, id);
    return id;
}

void assign_stable_ids(Scene& scene) {
    int n = 1;
    scene.for_each_node([&](Node& node) { node.set_id(stable_uuid(n++)); });
    scene.rebuild_index();
}

} // namespace

// ---------------------------------------------------------------------------
// BroadPhase: the pure grid, tested without a scene.
// ---------------------------------------------------------------------------

TEST_CASE("broad phase candidate set is a superset of the true overlapping pairs") {
    // A seeded, randomized-but-reproducible layout. The seed is fixed so a
    // failure is repeatable; the assertion is the safety property, not a golden
    // value: every pair the brute-force double loop finds overlapping must be in
    // the candidate set. This is exactly the test that fails if the broad phase
    // is made to drop a pair.
    std::mt19937 rng(20261013u);
    std::uniform_real_distribution<f32> position(-20.0f, 20.0f);
    std::uniform_real_distribution<f32> extent(0.1f, 2.0f);

    std::vector<physics2d::Aabb2D> bounds;
    bounds.reserve(300);
    for (int i = 0; i < 300; ++i) {
        const f32 hx = extent(rng);
        const f32 hy = extent(rng);
        bounds.push_back(bounds_box(position(rng), position(rng), hx, hy));
    }

    physics2d::BroadPhase grid;
    grid.build(bounds);
    const std::vector<physics2d::BroadPhasePair> candidates(grid.pairs().begin(), grid.pairs().end());

    const std::vector<physics2d::BroadPhasePair> truth = brute_force_overlaps(bounds);

    // Every true overlap is a candidate.
    for (const physics2d::BroadPhasePair& pair : truth) {
        CHECK(has_pair(candidates, pair.first, pair.second));
    }
    CHECK(truth.size() > 0); // the layout must actually exercise overlaps

    // The candidate set is canonical: sorted, and free of duplicates.
    CHECK(std::is_sorted(candidates.begin(), candidates.end()));
    CHECK(std::adjacent_find(candidates.begin(), candidates.end()) == candidates.end());
    for (const physics2d::BroadPhasePair& pair : candidates) {
        CHECK(pair.first < pair.second);
    }

    // It prunes: far fewer candidates than the full pair count.
    const usize all_pairs = bounds.size() * (bounds.size() - 1) / 2;
    CHECK(candidates.size() < all_pairs);
}

TEST_CASE("a pair straddling a cell boundary is still found") {
    // Two half-unit boxes that overlap across x = 0, which is a cell boundary
    // for this layout: two boxes of extent 1 give a cell size of 2, so cells
    // meet at multiples of 2 and x = 0 is one of them. Keying a body by its
    // centre cell alone would put A in cell -1 and B in cell 0 and miss the
    // pair; inserting the whole AABB puts both in the boundary cells.
    const std::vector<physics2d::Aabb2D> bounds = {
        bounds_box(-0.4f, 0.0f, 0.5f, 0.5f), // spans x in [-0.9, 0.1]
        bounds_box(0.4f, 0.0f, 0.5f, 0.5f),  // spans x in [-0.1, 0.9]
    };
    CHECK(physics2d::bounds_overlap(bounds[0], bounds[1]));

    physics2d::BroadPhase grid;
    grid.build(bounds);
    CHECK(grid.cell_size() == doctest::Approx(2.0f));
    const std::vector<physics2d::BroadPhasePair> candidates(grid.pairs().begin(), grid.pairs().end());
    CHECK(has_pair(candidates, 0, 1));
}

TEST_CASE("coincident bounds are paired and far-apart bounds are not") {
    const std::vector<physics2d::Aabb2D> bounds = {
        bounds_box(0.0f, 0.0f, 1.0f, 1.0f),
        bounds_box(0.0f, 0.0f, 1.0f, 1.0f), // exactly coincident with body 0
        bounds_box(100.0f, 100.0f, 1.0f, 1.0f),
        bounds_box(200.0f, -50.0f, 1.0f, 1.0f),
    };
    physics2d::BroadPhase grid;
    grid.build(bounds);
    const std::vector<physics2d::BroadPhasePair> candidates(grid.pairs().begin(), grid.pairs().end());

    CHECK(has_pair(candidates, 0, 1));       // coincident
    CHECK_FALSE(has_pair(candidates, 0, 2)); // far apart
    CHECK_FALSE(has_pair(candidates, 2, 3)); // far apart
    CHECK(candidates.size() == 1);
}

TEST_CASE("no bodies and a single body produce no candidates") {
    physics2d::BroadPhase grid;
    grid.build({});
    CHECK(grid.candidate_count() == 0);

    const std::vector<physics2d::Aabb2D> one = {bounds_box(0.0f, 0.0f, 1.0f, 1.0f)};
    grid.build(one);
    CHECK(grid.candidate_count() == 0);
}

TEST_CASE("the cell size is floored for point-sized bodies") {
    // Extents near zero would pick a zero cell size; the floor keeps the grid
    // well defined.
    const std::vector<physics2d::Aabb2D> bounds = {
        bounds_box(0.0f, 0.0f, 0.0f, 0.0f),
        bounds_box(0.0f, 0.0f, 0.0f, 0.0f),
    };
    physics2d::BroadPhase grid;
    grid.build(bounds);
    CHECK(grid.cell_size() == doctest::Approx(physics2d::BroadPhase::kMinCellSize));
    // Two coincident points are still a candidate.
    CHECK(grid.candidate_count() == 1);
}

TEST_CASE("rebuilding the grid reuses scratch and stays deterministic") {
    std::mt19937 rng(7u);
    std::uniform_real_distribution<f32> position(-10.0f, 10.0f);
    std::uniform_real_distribution<f32> extent(0.2f, 1.5f);
    std::vector<physics2d::Aabb2D> bounds;
    for (int i = 0; i < 120; ++i) {
        bounds.push_back(bounds_box(position(rng), position(rng), extent(rng), extent(rng)));
    }

    physics2d::BroadPhase grid;
    grid.build(bounds);
    const std::vector<physics2d::BroadPhasePair> first(grid.pairs().begin(), grid.pairs().end());

    // A second build with the same input must reproduce the exact order.
    grid.build(bounds);
    const std::vector<physics2d::BroadPhasePair> second(grid.pairs().begin(), grid.pairs().end());
    CHECK(first == second);
}

// ---------------------------------------------------------------------------
// Integration: the system's candidate set and contact events.
// ---------------------------------------------------------------------------

TEST_CASE("the system's candidate set is a superset of the true overlaps") {
    // A scripted scenario: a pile of boxes resting on a static floor. The broad
    // phase is a superset by construction, so the candidate count can never be
    // smaller than the number of pairs whose bounds truly overlap at the top of
    // the step. This is the integration-level form of the safety property: it
    // fails if the grid is made to drop a pair.
    Scene scene;
    add_static_box(scene, Vec2{0.0f, -0.5f}, 20.0f, 0.5f);
    for (int i = 0; i < 12; ++i) {
        const f32 x = -1.0f + 0.5f * static_cast<f32>(i % 4);
        const f32 y = 0.5f + 0.5f * static_cast<f32>(i / 4);
        add_dynamic_box(scene, Vec2{x, y}, 0.25f, 1.0f);
    }

    physics2d::Physics2DSystem system;
    system.iterations = 4;

    for (int frame = 0; frame < 40; ++frame) {
        system.update(step(1.0f / 60.0f), scene);
    }

    // Snapshot the pre-solve state of the frame under test, run one step, and
    // compare. The reference is the pairs whose bounds (and shapes) overlap in
    // that snapshot; every one of them must be a candidate.
    const std::vector<ColliderRef> refs = snapshot_colliders(scene);
    const usize true_overlaps = brute_force_contact_count(refs);
    system.update(step(1.0f / 60.0f), scene);

    CHECK(true_overlaps > 0);
    CHECK(system.candidate_count() >= true_overlaps);
    CHECK(system.contact_count() <= system.candidate_count());
    CHECK(system.contact_count() > 0);
    // The grid must prune: fewer candidates than the full pair count.
    CHECK(system.candidate_count() < refs.size() * (refs.size() - 1) / 2);
}

TEST_CASE("contact events are deterministic across identical runs") {
    // Two identical scenes stepped identically must emit identical event
    // sequences, frame for frame. The broad phase is order-stable, so the event
    // order (which is sorted by pair key) is reproducible. Both scenes are given
    // the same node ids, because ids are otherwise generated at random and an
    // id-level comparison across two scenes would be meaningless.
    const auto run = []() {
        Scene scene;
        add_static_box(scene, Vec2{0.0f, -0.5f}, 10.0f, 0.5f);
        add_dynamic_box(scene, Vec2{0.0f, 3.0f}, 0.5f, 1.0f);
        add_dynamic_box(scene, Vec2{0.2f, 6.0f}, 0.5f, 1.0f);
        assign_stable_ids(scene);

        physics2d::Physics2DSystem system;
        std::vector<int> log; // phase per event, in order
        std::vector<std::pair<Uuid, Uuid>> pairs;
        for (int frame = 0; frame < 120; ++frame) {
            system.update(step(1.0f / 60.0f), scene);
            for (const physics2d::ContactEvent& event : system.contact_events()) {
                log.push_back(static_cast<int>(event.phase));
                pairs.emplace_back(event.a->id(), event.b->id());
            }
        }
        return std::make_pair(log, pairs);
    };

    const auto first = run();
    const auto second = run();
    CHECK(first.first == second.first);
    CHECK(first.second == second.second);
    // The scenario must produce both a Begin and an End to be meaningful.
    bool saw_begin = false;
    bool saw_end = false;
    for (const int phase : first.first) {
        saw_begin = saw_begin || phase == static_cast<int>(physics2d::ContactPhase::Begin);
        saw_end = saw_end || phase == static_cast<int>(physics2d::ContactPhase::End);
    }
    CHECK(saw_begin);
    CHECK(saw_end);
}

TEST_CASE("Begin events equal the brute-force overlaps in a static layout") {
    // The strongest form of the determinism requirement, done A/B: the pairs that
    // begin touching this frame must be exactly the pairs a full double loop
    // would find overlapping. The layout is static (no gravity, no velocities),
    // so the solver's positional correction cannot move a body between
    // narrow-phase tests and the comparison is exact -- it is not sampling a
    // moving target the way a falling pile does.
    const auto count_begins = [](Scene& scene) {
        physics2d::Physics2DSystem system;
        system.gravity = Vec2{0.0f, 0.0f};
        system.update(step(1.0f / 60.0f), scene);
        usize begins = 0;
        for (const physics2d::ContactEvent& event : system.contact_events()) {
            if (event.phase == physics2d::ContactPhase::Begin) {
                ++begins;
            }
        }
        return begins;
    };

    // A grid of overlapping static boxes, so there are many true overlaps.
    Scene scene;
    for (int y = 0; y < 6; ++y) {
        for (int x = 0; x < 6; ++x) {
            add_static_box(scene, Vec2{static_cast<f32>(x) * 0.75f, static_cast<f32>(y) * 0.75f}, 0.5f, 0.5f);
        }
    }

    const std::vector<ColliderRef> refs = snapshot_colliders(scene);
    const usize true_overlaps = brute_force_contact_count(refs);
    CHECK(true_overlaps > 0);
    CHECK(count_begins(scene) == true_overlaps);
}

TEST_CASE("a contact that begins and ends fires exactly once each") {
    // A ball dropped onto a floor: it must emit exactly one Begin when it lands
    // and exactly one End when it is removed from under the floor. The End is
    // checked by moving the floor away, which is the deterministic way to end a
    // contact without relying on a bounce.
    Scene scene;
    Node* floor_node = add_static_box(scene, Vec2{0.0f, -0.5f}, 5.0f, 0.5f);
    Node* ball = add_dynamic_box(scene, Vec2{0.0f, 1.0f}, 0.25f, 1.0f);

    physics2d::Physics2DSystem system;
    usize begins = 0;
    usize ends = 0;
    bool touching = false;
    for (int frame = 0; frame < 60; ++frame) {
        system.update(step(1.0f / 60.0f), scene);
        for (const physics2d::ContactEvent& event : system.contact_events()) {
            if (event.phase == physics2d::ContactPhase::Begin) {
                ++begins;
                touching = true;
            } else {
                ++ends;
                touching = false;
            }
        }
    }
    CHECK(touching);
    CHECK(begins == 1);
    CHECK(ends == 0);

    // Teleport the floor far away; the contact must end exactly once.
    floor_node->transform().position = Vec3{1000.0f, -0.5f, 0.0f};
    system.update(step(1.0f / 60.0f), scene);
    for (const physics2d::ContactEvent& event : system.contact_events()) {
        if (event.phase == physics2d::ContactPhase::End) {
            ++ends;
        }
    }
    CHECK(ends == 1);
    (void)ball;
}

TEST_CASE("joint-only bodies never enter the broad phase") {
    // A scene of point masses with no collider: no bounds, no candidates, no
    // contacts. This is the case that must not regress when the grid is wired
    // in, because a colliderless body has no AABB to insert.
    Scene scene;
    for (int i = 0; i < 20; ++i) {
        add_point_mass(scene, Vec2{static_cast<f32>(i) * 0.1f, 0.0f}, 1.0f);
    }
    physics2d::Physics2DSystem system;
    system.update(step(1.0f / 60.0f), scene);
    CHECK(system.candidate_count() == 0);
    CHECK(system.contact_count() == 0);
}

TEST_CASE("a joint-only mass ahead of colliders does not mis-map candidate indices") {
    // Regression: `bodies_` holds colliderless masses, but the grid holds only
    // bodies with a collider. If the grid's indices were used to index `bodies_`
    // directly, a point mass ahead of the colliders in scene order would shift
    // every index and the solver would resolve the wrong bodies -- here it would
    // miss the overlapping pair entirely (the point mass has no collider, so the
    // collider check would reject the pair). The owner map is what keeps the
    // indices aligned; this scene fails without it.
    Scene scene;
    add_point_mass(scene, Vec2{50.0f, 50.0f}, 1.0f);  // first, no collider
    add_point_mass(scene, Vec2{-50.0f, 20.0f}, 1.0f); // second, no collider
    add_dynamic_box(scene, Vec2{0.0f, 0.0f}, 0.5f, 1.0f);
    add_dynamic_box(scene, Vec2{0.4f, 0.0f}, 0.5f, 1.0f); // overlaps the box above

    physics2d::Physics2DSystem system;
    system.gravity = Vec2{0.0f, 0.0f};

    const std::vector<ColliderRef> refs = snapshot_colliders(scene);
    const usize true_overlaps = brute_force_contact_count(refs);
    system.update(step(1.0f / 60.0f), scene);

    CHECK(refs.size() == 2); // only the two boxes are colliders
    CHECK(true_overlaps == 1);
    CHECK(system.contact_count() == 1); // the overlapping box pair, not dropped
    CHECK(system.candidate_count() >= 1);
}

TEST_CASE("two hundred separated bodies produce no candidates and no contacts") {
    // A structural check, not a timing check: 200 well-separated bodies must
    // yield zero contacts, and the grid must prune the pair set rather than
    // enumerating all 19900 pairs.
    Scene scene;
    int index = 0;
    for (int y = 0; y < 10; ++y) {
        for (int x = 0; x < 20; ++x) {
            add_dynamic_box(scene, Vec2{static_cast<f32>(x) * 5.0f, static_cast<f32>(y) * 5.0f}, 0.25f, 1.0f);
            ++index;
        }
    }
    CHECK(index == 200);

    physics2d::Physics2DSystem system;
    system.gravity = Vec2{0.0f, 0.0f}; // keep the layout fixed
    system.update(step(1.0f / 60.0f), scene);

    CHECK(system.contact_count() == 0);
    const usize all_pairs = 200 * 199 / 2;
    CHECK(system.candidate_count() < all_pairs);
}

TEST_CASE("two hundred dense bodies keep the candidate set a superset") {
    // A dense cluster: many overlaps, the stress case for the grid. The
    // candidate set must still contain every pre-solve overlap, and the contacts
    // found must be a subset of the candidates.
    //
    // Note: `contact_count()` is *not* asserted equal to the brute-force count.
    // The solver corrects positions between narrow-phase tests within the first
    // iteration, so the count it reports is of a slightly different world than
    // the snapshot; that is pre-existing behaviour, unchanged by the broad phase.
    // The superset relation is the property the broad phase is responsible for.
    Scene scene;
    std::mt19937 rng(99u);
    std::uniform_real_distribution<f32> position(-4.0f, 4.0f);
    for (int i = 0; i < 200; ++i) {
        add_dynamic_box(scene, Vec2{position(rng), position(rng)}, 0.5f, 1.0f);
    }

    physics2d::Physics2DSystem system;
    system.gravity = Vec2{0.0f, 0.0f};

    const std::vector<ColliderRef> refs = snapshot_colliders(scene);
    const usize true_overlaps = brute_force_contact_count(refs);
    system.update(step(1.0f / 60.0f), scene);

    CHECK(true_overlaps > 0);
    CHECK(system.candidate_count() >= true_overlaps);
    CHECK(system.contact_count() <= system.candidate_count());
    CHECK(system.contact_count() > 0);
    // The grid must prune: fewer candidates than the full pair count.
    CHECK(system.candidate_count() < refs.size() * (refs.size() - 1) / 2);
}

TEST_CASE("contact events are deterministic across identical runs") {
    // Two identical scenes stepped identically must emit identical event
    // sequences, frame for frame. The broad phase is order-stable, so the event
    // order (which is sorted by pair key) is reproducible. Both scenes are given
    // the same node ids, because ids are otherwise generated at random and an
    // id-level comparison across two scenes would be meaningless.
    const auto run = []() {
        Scene scene;
        add_static_box(scene, Vec2{0.0f, -0.5f}, 10.0f, 0.5f);
        add_dynamic_box(scene, Vec2{0.0f, 3.0f}, 0.5f, 1.0f);
        add_dynamic_box(scene, Vec2{0.2f, 6.0f}, 0.5f, 1.0f);
        assign_stable_ids(scene);

        physics2d::Physics2DSystem system;
        std::vector<int> log; // phase per event, in order
        std::vector<std::pair<Uuid, Uuid>> pairs;
        for (int frame = 0; frame < 120; ++frame) {
            system.update(step(1.0f / 60.0f), scene);
            for (const physics2d::ContactEvent& event : system.contact_events()) {
                log.push_back(static_cast<int>(event.phase));
                pairs.emplace_back(event.a->id(), event.b->id());
            }
        }
        return std::make_pair(log, pairs);
    };

    const auto first = run();
    const auto second = run();
    CHECK(first.first == second.first);
    CHECK(first.second == second.second);
    // The scenario must produce both a Begin and an End to be meaningful.
    bool saw_begin = false;
    bool saw_end = false;
    for (const int phase : first.first) {
        saw_begin = saw_begin || phase == static_cast<int>(physics2d::ContactPhase::Begin);
        saw_end = saw_end || phase == static_cast<int>(physics2d::ContactPhase::End);
    }
    CHECK(saw_begin);
    CHECK(saw_end);
}

TEST_CASE("Begin events equal the brute-force overlaps in a static layout") {
    // The strongest form of the determinism requirement, done A/B: the pairs that
    // begin touching this frame must be exactly the pairs a full double loop
    // would find overlapping. The layout is static (no gravity, no velocities),
    // so the solver's positional correction cannot move a body between
    // narrow-phase tests and the comparison is exact -- it is not sampling a
    // moving target the way a falling pile does.
    const auto count_begins = [](Scene& scene) {
        physics2d::Physics2DSystem system;
        system.gravity = Vec2{0.0f, 0.0f};
        system.update(step(1.0f / 60.0f), scene);
        usize begins = 0;
        for (const physics2d::ContactEvent& event : system.contact_events()) {
            if (event.phase == physics2d::ContactPhase::Begin) {
                ++begins;
            }
        }
        return begins;
    };

    // A grid of overlapping static boxes, so there are many true overlaps.
    Scene scene;
    for (int y = 0; y < 6; ++y) {
        for (int x = 0; x < 6; ++x) {
            add_static_box(scene, Vec2{static_cast<f32>(x) * 0.75f, static_cast<f32>(y) * 0.75f}, 0.5f, 0.5f);
        }
    }

    const std::vector<ColliderRef> refs = snapshot_colliders(scene);
    const usize true_overlaps = brute_force_contact_count(refs);
    CHECK(true_overlaps > 0);
    CHECK(count_begins(scene) == true_overlaps);
}

TEST_CASE("a contact that begins and ends fires exactly once each") {
    // A ball dropped onto a floor: it must emit exactly one Begin when it lands
    // and exactly one End when it is removed from under the floor. The End is
    // checked by moving the floor away, which is the deterministic way to end a
    // contact without relying on a bounce.
    Scene scene;
    Node* floor_node = add_static_box(scene, Vec2{0.0f, -0.5f}, 5.0f, 0.5f);
    Node* ball = add_dynamic_box(scene, Vec2{0.0f, 1.0f}, 0.25f, 1.0f);

    physics2d::Physics2DSystem system;
    usize begins = 0;
    usize ends = 0;
    bool touching = false;
    for (int frame = 0; frame < 60; ++frame) {
        system.update(step(1.0f / 60.0f), scene);
        for (const physics2d::ContactEvent& event : system.contact_events()) {
            if (event.phase == physics2d::ContactPhase::Begin) {
                ++begins;
                touching = true;
            } else {
                ++ends;
                touching = false;
            }
        }
    }
    CHECK(touching);
    CHECK(begins == 1);
    CHECK(ends == 0);

    // Teleport the floor far away; the contact must end exactly once.
    floor_node->transform().position = Vec3{1000.0f, -0.5f, 0.0f};
    system.update(step(1.0f / 60.0f), scene);
    for (const physics2d::ContactEvent& event : system.contact_events()) {
        if (event.phase == physics2d::ContactPhase::End) {
            ++ends;
        }
    }
    CHECK(ends == 1);
    (void)ball;
}

TEST_CASE("joint-only bodies never enter the broad phase") {
    // A scene of point masses with no collider: no bounds, no candidates, no
    // contacts. This is the case that must not regress when the grid is wired
    // in, because a colliderless body has no AABB to insert.
    Scene scene;
    for (int i = 0; i < 20; ++i) {
        add_point_mass(scene, Vec2{static_cast<f32>(i) * 0.1f, 0.0f}, 1.0f);
    }
    physics2d::Physics2DSystem system;
    system.update(step(1.0f / 60.0f), scene);
    CHECK(system.candidate_count() == 0);
    CHECK(system.contact_count() == 0);
}

TEST_CASE("a joint-only mass ahead of colliders does not mis-map candidate indices") {
    // Regression: `bodies_` holds colliderless masses, but the grid holds only
    // bodies with a collider. If the grid's indices were used to index `bodies_`
    // directly, a point mass ahead of the colliders in scene order would shift
    // every index and the solver would resolve the wrong bodies -- here it would
    // miss the overlapping pair entirely (the point mass has no collider, so the
    // collider check would reject the pair). The owner map is what keeps the
    // indices aligned; this scene fails without it.
    Scene scene;
    add_point_mass(scene, Vec2{50.0f, 50.0f}, 1.0f);  // first, no collider
    add_point_mass(scene, Vec2{-50.0f, 20.0f}, 1.0f); // second, no collider
    add_dynamic_box(scene, Vec2{0.0f, 0.0f}, 0.5f, 1.0f);
    add_dynamic_box(scene, Vec2{0.4f, 0.0f}, 0.5f, 1.0f); // overlaps the box above

    physics2d::Physics2DSystem system;
    system.gravity = Vec2{0.0f, 0.0f};

    const std::vector<ColliderRef> refs = snapshot_colliders(scene);
    const usize true_overlaps = brute_force_contact_count(refs);
    system.update(step(1.0f / 60.0f), scene);

    CHECK(refs.size() == 2); // only the two boxes are colliders
    CHECK(true_overlaps == 1);
    CHECK(system.contact_count() == 1); // the overlapping box pair, not dropped
    CHECK(system.candidate_count() >= 1);
}

TEST_CASE("two hundred separated bodies produce no candidates and no contacts") {
    // A structural check, not a timing check: 200 well-separated bodies must
    // yield zero contacts, and the grid must prune the pair set rather than
    // enumerating all 19900 pairs.
    Scene scene;
    int index = 0;
    for (int y = 0; y < 10; ++y) {
        for (int x = 0; x < 20; ++x) {
            add_dynamic_box(scene, Vec2{static_cast<f32>(x) * 5.0f, static_cast<f32>(y) * 5.0f}, 0.25f, 1.0f);
            ++index;
        }
    }
    CHECK(index == 200);

    physics2d::Physics2DSystem system;
    system.gravity = Vec2{0.0f, 0.0f}; // keep the layout fixed
    system.update(step(1.0f / 60.0f), scene);

    CHECK(system.contact_count() == 0);
    const usize all_pairs = 200 * 199 / 2;
    CHECK(system.candidate_count() < all_pairs);
}

TEST_CASE("two hundred dense bodies keep the candidate set a superset") {
    // A dense cluster: many overlaps, the stress case for the grid. The
    // candidate set must still contain every pre-solve overlap, and the contacts
    // found must be a subset of the candidates.
    //
    // Note: `contact_count()` is *not* asserted equal to the brute-force count.
    // The solver corrects positions between narrow-phase tests within the first
    // iteration, so the count it reports is of a slightly different world than
    // the snapshot; that is pre-existing behaviour, unchanged by the broad phase.
    // The superset relation is the property the broad phase is responsible for.
    Scene scene;
    std::mt19937 rng(99u);
    std::uniform_real_distribution<f32> position(-4.0f, 4.0f);
    for (int i = 0; i < 200; ++i) {
        add_dynamic_box(scene, Vec2{position(rng), position(rng)}, 0.5f, 1.0f);
    }

    physics2d::Physics2DSystem system;
    system.gravity = Vec2{0.0f, 0.0f};

    const std::vector<ColliderRef> refs = snapshot_colliders(scene);
    const usize true_overlaps = brute_force_contact_count(refs);
    system.update(step(1.0f / 60.0f), scene);

    CHECK(true_overlaps > 0);
    CHECK(system.candidate_count() >= true_overlaps);
    CHECK(system.contact_count() <= system.candidate_count());
    CHECK(system.contact_count() > 0);
}

// ---------------------------------------------------------------------------
// Parallel build (M9): the parallel insert must produce the serial pair set.
// ---------------------------------------------------------------------------

TEST_CASE("a parallel broad-phase build produces exactly the serial pair set") {
    // Well past `kParallelBuildThreshold`, with a spread that puts many bodies in
    // the same cells (so the pair emission and dedup are exercised, not just the
    // inserts) and some bodies straddling cell boundaries.
    std::mt19937 rng(4242u);
    std::uniform_real_distribution<f32> position(-30.0f, 30.0f);
    std::uniform_real_distribution<f32> half(0.2f, 1.5f);

    std::vector<physics2d::Aabb2D> bounds;
    bounds.reserve(3000);
    for (int i = 0; i < 3000; ++i) {
        const f32 hx = half(rng);
        const f32 hy = half(rng);
        bounds.push_back(bounds_box(position(rng), position(rng), hx, hy));
    }

    physics2d::BroadPhase serial;
    serial.build(bounds, nullptr);
    const std::vector<physics2d::BroadPhasePair> serial_pairs(serial.pairs().begin(), serial.pairs().end());

    JobSystem jobs(4);
    physics2d::BroadPhase parallel;
    parallel.build(bounds, &jobs);
    const std::vector<physics2d::BroadPhasePair> parallel_pairs(parallel.pairs().begin(), parallel.pairs().end());

    // Same cell size (it is a function of the bounds only) and the same pair set
    // in the same canonical order. Byte-identical, not merely equivalent: the
    // solver iterates this array, so a different order is a different step.
    CHECK(parallel.cell_size() == serial.cell_size());
    REQUIRE(parallel_pairs.size() == serial_pairs.size());
    for (usize i = 0; i < serial_pairs.size(); ++i) {
        CHECK(parallel_pairs[i] == serial_pairs[i]);
    }

    // And the set is still a superset of the truly-overlapping pairs, which is
    // the broad phase's actual contract.
    const std::vector<physics2d::BroadPhasePair> truth = brute_force_overlaps(bounds);
    for (const physics2d::BroadPhasePair& pair : truth) {
        CHECK(std::binary_search(serial_pairs.begin(), serial_pairs.end(), pair));
    }
}

TEST_CASE("a single-threaded JobSystem takes the serial build path") {
    std::mt19937 rng(7u);
    std::uniform_real_distribution<f32> position(-20.0f, 20.0f);
    std::vector<physics2d::Aabb2D> bounds;
    for (int i = 0; i < 2000; ++i) {
        bounds.push_back(bounds_box(position(rng), position(rng), 0.5f, 0.5f));
    }

    physics2d::BroadPhase serial;
    serial.build(bounds, nullptr);

    // worker_count == 0: `is_threaded()` is false, so the parallel branch is not
    // entered even though a JobSystem pointer is supplied.
    JobSystem inline_jobs;
    physics2d::BroadPhase from_inline;
    from_inline.build(bounds, &inline_jobs);

    REQUIRE(from_inline.candidate_count() == serial.candidate_count());
    for (usize i = 0; i < serial.pairs().size(); ++i) {
        CHECK(from_inline.pairs()[i] == serial.pairs()[i]);
    }
}

TEST_CASE("a scene below the build threshold ignores the pool") {
    // Fewer bounds than `kParallelBuildThreshold`: the serial path runs, and the
    // result must be identical to the pooled call all the same.
    std::vector<physics2d::Aabb2D> bounds;
    for (int i = 0; i < 100; ++i) {
        bounds.push_back(bounds_box(static_cast<f32>(i) * 1.5f, 0.0f, 0.6f, 0.6f));
    }

    JobSystem jobs(4);
    physics2d::BroadPhase pooled;
    pooled.build(bounds, &jobs);

    physics2d::BroadPhase serial;
    serial.build(bounds, nullptr);

    REQUIRE(pooled.candidate_count() == serial.candidate_count());
    for (usize i = 0; i < serial.pairs().size(); ++i) {
        CHECK(pooled.pairs()[i] == serial.pairs()[i]);
    }
}
