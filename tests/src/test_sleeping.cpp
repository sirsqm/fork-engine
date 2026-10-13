// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
//
// Sleeping and island detection: the pure island builder, and the system's
// sleep/wake behaviour. Everything here is deterministic -- no wall-clock
// thresholds, no frame rates -- so it runs headless and identically on CI.
//
// The pure tests call IslandBuilder directly with hand-written links; the
// system tests build small scenes and assert on state, counts, and positions.

#include <doctest/doctest.h>

#include "fe/physics2d/Collider2D.h"
#include "fe/physics2d/DistanceJoint2D.h"
#include "fe/physics2d/IslandSolver.h"
#include "fe/physics2d/Physics2DSystem.h"
#include "fe/physics2d/PhysicsTypes.h"
#include "fe/physics2d/RigidBody2D.h"
#include "fe/reflect/ComponentTypes.h"
#include "fe/reflect/Registry.h"
#include "fe/scene/Node.h"
#include "fe/scene/Scene.h"
#include "fe/serialization/SceneSerializer.h"
#include "fe/systems/FrameContext.h"

#include <cmath>
#include <vector>

using namespace fe;

namespace {

FrameContext step(f32 delta) {
    FrameContext context;
    context.delta_time = delta;
    context.elapsed_time = static_cast<f64>(delta);
    return context;
}

// The default step used throughout: 60 Hz, so the default 0.5 s sleep_time is
// reached after exactly 30 steps.
constexpr f32 kDt = 1.0f / 60.0f;

// A static floor whose top face is at y = 0.
Node* add_floor(Scene& scene, f32 half_width) {
    Node* node = scene.create_node("Floor");
    auto* collider = node->add_component<physics2d::Collider2D>();
    collider->shape = physics2d::ShapeType::Box;
    collider->half_extent = Vec2{half_width, 0.5f};
    node->transform().position = Vec3{0.0f, -0.5f, 0.0f};
    return node;
}

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

// A dynamic point mass with no collider: the simplest joint end, and an island
// vertex with no contacts.
Node* add_point_mass(Scene& scene, const Vec2& position, f32 mass) {
    Node* node = scene.create_node("Mass");
    auto* body = node->add_component<physics2d::RigidBody2D>();
    body->mass = mass;
    node->transform().position = Vec3{position.x, position.y, 0.0f};
    return node;
}

physics2d::DistanceJoint2D* add_distance_joint(Node& host, const Node& a, const Node& b, f32 rest_length) {
    auto* joint = host.add_component<physics2d::DistanceJoint2D>();
    joint->body_a = a.id();
    joint->body_b = b.id();
    joint->rest_length = rest_length;
    return joint;
}

} // namespace

// ---------------------------------------------------------------------------
// IslandBuilder: the pure graph, tested without a scene.
// ---------------------------------------------------------------------------

TEST_CASE("an empty island build has no islands") {
    physics2d::IslandBuilder builder;
    builder.build(0, {});
    CHECK(builder.island_count() == 0);
    CHECK(builder.islands().empty());
}

TEST_CASE("bodies with no links are each their own island") {
    physics2d::IslandBuilder builder;
    builder.build(3, {});
    REQUIRE(builder.island_count() == 3);
    CHECK(builder.islands()[0] == std::vector<usize>{0});
    CHECK(builder.islands()[1] == std::vector<usize>{1});
    CHECK(builder.islands()[2] == std::vector<usize>{2});
    CHECK_FALSE(builder.same_island(0, 1));
    CHECK(builder.same_island(0, 0));
}

TEST_CASE("a single link merges exactly two bodies") {
    physics2d::IslandBuilder builder;
    builder.build(3, {physics2d::IslandLink{0, 2}});
    REQUIRE(builder.island_count() == 2);
    // The merged island lists its members in ascending order, and the islands
    // are ordered by their lowest member.
    CHECK(builder.islands()[0] == std::vector<usize>{0, 2});
    CHECK(builder.islands()[1] == std::vector<usize>{1});
    CHECK(builder.same_island(0, 2));
    CHECK_FALSE(builder.same_island(0, 1));
}

TEST_CASE("a chain of links forms one island") {
    physics2d::IslandBuilder builder;
    builder.build(4, {physics2d::IslandLink{0, 1}, physics2d::IslandLink{1, 2}, physics2d::IslandLink{2, 3}});
    REQUIRE(builder.island_count() == 1);
    CHECK(builder.islands()[0] == std::vector<usize>{0, 1, 2, 3});
    CHECK(builder.same_island(0, 3));
}

TEST_CASE("two disjoint links are two islands") {
    physics2d::IslandBuilder builder;
    builder.build(4, {physics2d::IslandLink{0, 1}, physics2d::IslandLink{2, 3}});
    REQUIRE(builder.island_count() == 2);
    CHECK(builder.islands()[0] == std::vector<usize>{0, 1});
    CHECK(builder.islands()[1] == std::vector<usize>{2, 3});
    CHECK_FALSE(builder.same_island(0, 2));
}

TEST_CASE("the link direction does not matter") {
    physics2d::IslandBuilder forward;
    forward.build(2, {physics2d::IslandLink{0, 1}});
    physics2d::IslandBuilder backward;
    backward.build(2, {physics2d::IslandLink{1, 0}});
    CHECK(forward.island_count() == backward.island_count());
    CHECK(forward.same_island(0, 1));
    CHECK(backward.same_island(0, 1));
}

TEST_CASE("a link naming a body that does not exist is ignored") {
    physics2d::IslandBuilder builder;
    builder.build(2, {physics2d::IslandLink{0, 5}, physics2d::IslandLink{9, 9}});
    // Neither link joins anything: the two bodies stay separate.
    CHECK(builder.island_count() == 2);
    CHECK_FALSE(builder.same_island(0, 1));
}

TEST_CASE("the same_island query is false for an out-of-range body") {
    physics2d::IslandBuilder builder;
    builder.build(2, {physics2d::IslandLink{0, 1}});
    CHECK_FALSE(builder.same_island(0, 7));
}

TEST_CASE("the island build is deterministic") {
    const std::vector<physics2d::IslandLink> links = {physics2d::IslandLink{0, 3}, physics2d::IslandLink{3, 1},
                                                      physics2d::IslandLink{2, 4}};
    physics2d::IslandBuilder first;
    first.build(6, links);
    physics2d::IslandBuilder second;
    second.build(6, links);
    CHECK(first.island_count() == second.island_count());
    for (usize i = 0; i < first.island_count(); ++i) {
        CHECK(first.islands()[i] == second.islands()[i]);
    }
}

// ---------------------------------------------------------------------------
// A lone body settles and sleeps.
// ---------------------------------------------------------------------------

TEST_CASE("a body that is asleep does not move even if it still has a velocity") {
    // Sleeping means the solver and the integrator skip the body, not merely that
    // it happens to have zero velocity. A sleeping body is loaded with a velocity
    // and must still not move: this is the structural proof that asleep_ gates
    // the integration, not the value of the velocity.
    Scene scene;
    Node* node = add_dynamic_box(scene, Vec2{0.0f, 0.0f}, 0.5f, 1.0f);
    auto* body = node->get_component<physics2d::RigidBody2D>();
    body->affected_by_gravity = false;
    body->sleep(); // asleep, then given a velocity by hand
    body->velocity = Vec2{10.0f, 10.0f};
    body->angular_velocity = 5.0f;

    physics2d::Physics2DSystem system;
    for (int i = 0; i < 60; ++i) {
        system.update(step(kDt), scene);
    }
    CHECK(body->is_sleeping);
    CHECK(node->transform().position.x == doctest::Approx(0.0f));
    CHECK(node->transform().position.y == doctest::Approx(0.0f));
    CHECK(node->transform().rotation.z == doctest::Approx(0.0f));
}

TEST_CASE("a body at rest sleeps after the sustained-time threshold and stays asleep") {
    // No gravity and no contacts, so the body is resting from the first step.
    // The default sleep_time of 0.5 s is reached after exactly 30 steps at
    // 60 Hz; the body must sleep then and still be asleep much later (not a
    // one-frame flicker).
    Scene scene;
    Node* node = add_dynamic_box(scene, Vec2{0.0f, 0.0f}, 0.5f, 1.0f);
    auto* body = node->get_component<physics2d::RigidBody2D>();
    body->affected_by_gravity = false;

    physics2d::Physics2DSystem system;
    for (int i = 0; i < 29; ++i) {
        system.update(step(kDt), scene);
    }
    CHECK_FALSE(body->is_sleeping); // one step short of the threshold

    system.update(step(kDt), scene);
    CHECK(body->is_sleeping); // step 30: timer reached sleep_time
    CHECK(system.sleeping_body_count() == 1);

    for (int i = 0; i < 60; ++i) {
        system.update(step(kDt), scene);
    }
    CHECK(body->is_sleeping); // still asleep, no flicker
    CHECK(node->transform().position.x == doctest::Approx(0.0f));
    CHECK(node->transform().position.y == doctest::Approx(0.0f));
}

TEST_CASE("a body that is still moving does not sleep") {
    Scene scene;
    Node* node = add_dynamic_box(scene, Vec2{0.0f, 10.0f}, 0.5f, 1.0f);
    // Free fall: gravity keeps the speed above the threshold, so the rest timer
    // never accumulates and the body never sleeps.
    physics2d::Physics2DSystem system;
    for (int i = 0; i < 120; ++i) {
        system.update(step(kDt), scene);
        CHECK_FALSE(node->get_component<physics2d::RigidBody2D>()->is_sleeping);
    }
    CHECK(system.sleeping_body_count() == 0);
}

TEST_CASE("a body resting on a static floor sleeps") {
    Scene scene;
    add_floor(scene, 20.0f);
    Node* node = add_dynamic_box(scene, Vec2{0.0f, 0.5f}, 0.5f, 1.0f); // already on the floor

    physics2d::Physics2DSystem system;
    for (int i = 0; i < 120; ++i) {
        system.update(step(kDt), scene);
    }
    auto* body = node->get_component<physics2d::RigidBody2D>();
    CHECK(body->is_sleeping);
    CHECK(system.sleeping_body_count() == 1);
}

TEST_CASE("a sleeping body is not moved by the solver") {
    // The settled body is asleep; its transform must be byte-for-byte stable
    // over hundreds of further steps. This is the structural proof that the
    // solver skips it, not a timing measurement.
    Scene scene;
    add_floor(scene, 20.0f);
    Node* node = add_dynamic_box(scene, Vec2{0.0f, 0.5f}, 0.5f, 1.0f);

    physics2d::Physics2DSystem system;
    for (int i = 0; i < 120; ++i) {
        system.update(step(kDt), scene);
    }
    REQUIRE(node->get_component<physics2d::RigidBody2D>()->is_sleeping);
    const Vec3 resting = node->transform().position;

    for (int i = 0; i < 300; ++i) {
        system.update(step(kDt), scene);
    }
    CHECK(node->transform().position.x == resting.x);
    CHECK(node->transform().position.y == resting.y);
    CHECK(node->transform().rotation.z == resting.z);
}

// ---------------------------------------------------------------------------
// Waking on disturbance.
// ---------------------------------------------------------------------------

TEST_CASE("a moving body wakes the sleeping body it reaches") {
    // A sleeps alone; B slides in from the left and, the moment their bounds
    // overlap, the wake pass wakes A before the solver runs, so B pushes A
    // rather than passing through it.
    Scene scene;
    Node* a_node = add_dynamic_box(scene, Vec2{0.0f, 0.0f}, 0.5f, 1.0f);
    Node* b_node = add_dynamic_box(scene, Vec2{-4.0f, 0.0f}, 0.5f, 1.0f);
    auto* a_body = a_node->get_component<physics2d::RigidBody2D>();
    auto* b_body = b_node->get_component<physics2d::RigidBody2D>();
    a_body->affected_by_gravity = false;
    b_body->affected_by_gravity = false;
    b_body->velocity = Vec2{2.0f, 0.0f};

    physics2d::Physics2DSystem system;

    // Let A sleep while B is still far away.
    for (int i = 0; i < 40; ++i) {
        system.update(step(kDt), scene);
    }
    REQUIRE(a_body->is_sleeping);
    CHECK(a_node->transform().position.x == doctest::Approx(0.0f));

    // Run until the collision; A must be woken and pushed to the right.
    bool a_woke = false;
    for (int i = 0; i < 200; ++i) {
        system.update(step(kDt), scene);
        if (!a_body->is_sleeping) {
            a_woke = true;
        }
    }
    CHECK(a_woke);
    CHECK(a_node->transform().position.x > 0.1f);
    // B did not tunnel through A: it is still to the left of A's centre.
    CHECK(b_node->transform().position.x < a_node->transform().position.x);
}

TEST_CASE("two independent islands do not wake each other") {
    Scene scene;
    Node* a_node = add_dynamic_box(scene, Vec2{0.0f, 0.0f}, 0.5f, 1.0f);
    Node* b_node = add_dynamic_box(scene, Vec2{50.0f, 0.0f}, 0.5f, 1.0f);
    auto* a_body = a_node->get_component<physics2d::RigidBody2D>();
    auto* b_body = b_node->get_component<physics2d::RigidBody2D>();
    a_body->affected_by_gravity = false;
    b_body->affected_by_gravity = false;

    physics2d::Physics2DSystem system;
    for (int i = 0; i < 40; ++i) {
        system.update(step(kDt), scene);
    }
    REQUIRE(a_body->is_sleeping);
    REQUIRE(b_body->is_sleeping);
    CHECK(system.island_count() == 2);
    CHECK_FALSE(system.same_island(a_node->id(), b_node->id()));

    // Disturb A only.
    a_body->wake();
    a_body->velocity = Vec2{1.0f, 0.0f};
    system.update(step(kDt), scene);

    CHECK_FALSE(a_body->is_sleeping);
    CHECK(b_body->is_sleeping); // B is in another island and stays asleep
}

TEST_CASE("a jointed pair is one island and sleeps together") {
    Scene scene;
    Node* a_node = add_point_mass(scene, Vec2{0.0f, 0.0f}, 1.0f);
    Node* b_node = add_point_mass(scene, Vec2{1.0f, 0.0f}, 1.0f);
    auto* a_body = a_node->get_component<physics2d::RigidBody2D>();
    auto* b_body = b_node->get_component<physics2d::RigidBody2D>();
    a_body->affected_by_gravity = false;
    b_body->affected_by_gravity = false;

    Node* host = scene.create_node("Joint");
    add_distance_joint(*host, *a_node, *b_node, 1.0f);

    physics2d::Physics2DSystem system;
    for (int i = 0; i < 60; ++i) {
        system.update(step(kDt), scene);
    }
    CHECK(system.island_count() == 1);
    CHECK(system.same_island(a_node->id(), b_node->id()));
    CHECK(a_body->is_sleeping);
    CHECK(b_body->is_sleeping);
}

TEST_CASE("waking one body of a jointed pair wakes the other") {
    Scene scene;
    Node* a_node = add_point_mass(scene, Vec2{0.0f, 0.0f}, 1.0f);
    Node* b_node = add_point_mass(scene, Vec2{1.0f, 0.0f}, 1.0f);
    auto* a_body = a_node->get_component<physics2d::RigidBody2D>();
    auto* b_body = b_node->get_component<physics2d::RigidBody2D>();
    a_body->affected_by_gravity = false;
    b_body->affected_by_gravity = false;

    Node* host = scene.create_node("Joint");
    add_distance_joint(*host, *a_node, *b_node, 1.0f);

    physics2d::Physics2DSystem system;
    for (int i = 0; i < 60; ++i) {
        system.update(step(kDt), scene);
    }
    REQUIRE(a_body->is_sleeping);
    REQUIRE(b_body->is_sleeping);

    // Wake only A. The next step propagates the wake across the island, so B is
    // awake too.
    a_body->wake();
    system.update(step(kDt), scene);
    CHECK_FALSE(a_body->is_sleeping);
    CHECK_FALSE(b_body->is_sleeping);
}

TEST_CASE("a body that can never sleep keeps its island awake") {
    Scene scene;
    add_floor(scene, 20.0f);
    Node* node = add_dynamic_box(scene, Vec2{0.0f, 0.5f}, 0.5f, 1.0f);
    node->get_component<physics2d::RigidBody2D>()->can_sleep = false;

    physics2d::Physics2DSystem system;
    for (int i = 0; i < 200; ++i) {
        system.update(step(kDt), scene);
        CHECK_FALSE(node->get_component<physics2d::RigidBody2D>()->is_sleeping);
    }
    CHECK(system.sleeping_body_count() == 0);
}

TEST_CASE("a body put to sleep by hand resumes when woken") {
    // The controllable API: sleep() stops the body immediately, and wake()
    // resumes it. A sleeping body is skipped by the solver, so between the two
    // calls its position is frozen even though gravity would otherwise act.
    Scene scene;
    Node* node = add_dynamic_box(scene, Vec2{0.0f, 5.0f}, 0.5f, 1.0f);
    auto* body = node->get_component<physics2d::RigidBody2D>();

    physics2d::Physics2DSystem system;
    system.update(step(kDt), scene);
    REQUIRE_FALSE(body->is_sleeping);

    body->sleep();
    const Vec3 frozen = node->transform().position;
    for (int i = 0; i < 30; ++i) {
        system.update(step(kDt), scene);
    }
    CHECK(body->is_sleeping);
    CHECK(node->transform().position.y == frozen.y); // gravity did not act

    body->wake();
    system.update(step(kDt), scene);
    CHECK_FALSE(body->is_sleeping);
    CHECK(node->transform().position.y < frozen.y); // the fall resumed
}

TEST_CASE("two bodies resting on one floor are separate islands") {
    // Static geometry does not join islands: two bodies that both touch the same
    // floor but not each other are two islands, so one may sleep while the other
    // is still moving.
    Scene scene;
    add_floor(scene, 20.0f);
    Node* left = add_dynamic_box(scene, Vec2{-5.0f, 0.5f}, 0.5f, 1.0f);
    Node* right = add_dynamic_box(scene, Vec2{5.0f, 0.5f}, 0.5f, 1.0f);

    physics2d::Physics2DSystem system;
    system.update(step(kDt), scene);
    CHECK(system.island_count() == 2);
    CHECK_FALSE(system.same_island(left->id(), right->id()));
}

TEST_CASE("a settled stack of bodies sleeps as one island") {
    // Two boxes stacked on a floor. The floor is static, so it is not an island
    // vertex; the two boxes touch each other, so they are one island and must
    // sleep together. This is the definition-of-done case: a settled stack
    // sleeps and stays asleep.
    Scene scene;
    add_floor(scene, 20.0f);
    Node* lower = add_dynamic_box(scene, Vec2{0.0f, 0.5f}, 0.5f, 1.0f);
    Node* upper = add_dynamic_box(scene, Vec2{0.0f, 1.5f}, 0.5f, 1.0f);
    auto* lower_body = lower->get_component<physics2d::RigidBody2D>();
    auto* upper_body = upper->get_component<physics2d::RigidBody2D>();

    physics2d::Physics2DSystem system;
    for (int i = 0; i < 600; ++i) { // ten seconds: plenty to settle
        system.update(step(kDt), scene);
    }
    CHECK(system.island_count() == 1);
    CHECK(system.same_island(lower->id(), upper->id()));
    CHECK(lower_body->is_sleeping);
    CHECK(upper_body->is_sleeping);
    CHECK(system.sleeping_body_count() == 2);

    // And they stay asleep and put: the stack does not creep.
    const Vec3 lower_at = lower->transform().position;
    const Vec3 upper_at = upper->transform().position;
    for (int i = 0; i < 300; ++i) {
        system.update(step(kDt), scene);
    }
    CHECK(lower->transform().position.y == lower_at.y);
    CHECK(upper->transform().position.y == upper_at.y);
}

TEST_CASE("a body falling onto a sleeping body wakes it and the pair rests") {
    Scene scene;
    add_floor(scene, 20.0f);
    Node* sleeper = add_dynamic_box(scene, Vec2{0.0f, 0.5f}, 0.5f, 1.0f); // rests on the floor
    auto* sleeper_body = sleeper->get_component<physics2d::RigidBody2D>();

    physics2d::Physics2DSystem system;

    // Let the lower body settle and sleep before the other one exists, so the
    // sleeper is genuinely asleep when the faller arrives.
    for (int i = 0; i < 120; ++i) {
        system.update(step(kDt), scene);
    }
    REQUIRE(sleeper_body->is_sleeping);

    // Now drop a body from above onto it.
    Node* faller = add_dynamic_box(scene, Vec2{0.0f, 5.0f}, 0.5f, 1.0f);
    bool woke = false;
    for (int i = 0; i < 300; ++i) {
        system.update(step(kDt), scene);
        if (!sleeper_body->is_sleeping) {
            woke = true;
        }
    }
    CHECK(woke); // the falling body woke the sleeper

    // They settled into a stable stack: the faller rests on top of the sleeper
    // (centres about 1.0 apart) and did not tunnel through it.
    const f32 sleeper_y = sleeper->transform().position.y;
    const f32 faller_y = faller->transform().position.y;
    CHECK(faller_y > sleeper_y);        // above, not through
    CHECK(faller_y - sleeper_y > 0.8f); // resting on it, not overlapping
    CHECK(faller_y - sleeper_y < 1.3f);
}

// ---------------------------------------------------------------------------
// Sleeping does not perturb a scene that is not at rest.
// ---------------------------------------------------------------------------

TEST_CASE("a scene that never rests is identical with sleeping enabled or disabled") {
    // A free-falling body never comes to rest, so sleeping must not engage and
    // must not change the trajectory. Run the same scene twice, once with
    // can_sleep false (sleeping impossible) and once with the default, and
    // require identical positions.
    auto run = [](bool can_sleep) {
        Scene scene;
        Node* node = add_dynamic_box(scene, Vec2{0.0f, 10.0f}, 0.5f, 1.0f);
        node->get_component<physics2d::RigidBody2D>()->can_sleep = can_sleep;
        physics2d::Physics2DSystem system;
        for (int i = 0; i < 60; ++i) {
            system.update(step(kDt), scene);
        }
        return node->transform().position.y;
    };
    CHECK(run(true) == doctest::Approx(run(false)));
}

TEST_CASE("sleeping is deterministic across identical runs") {
    auto run = [](f32& out_y, bool& out_slept) {
        Scene scene;
        add_floor(scene, 20.0f);
        Node* node = add_dynamic_box(scene, Vec2{0.0f, 2.0f}, 0.5f, 1.0f);
        physics2d::Physics2DSystem system;
        for (int i = 0; i < 200; ++i) {
            system.update(step(kDt), scene);
        }
        out_y = node->transform().position.y;
        out_slept = node->get_component<physics2d::RigidBody2D>()->is_sleeping;
    };
    f32 y1 = 0.0f;
    f32 y2 = 0.0f;
    bool slept1 = false;
    bool slept2 = false;
    run(y1, slept1);
    run(y2, slept2);
    CHECK(y1 == y2);
    CHECK(slept1 == slept2);
    CHECK(slept1);
}

// ---------------------------------------------------------------------------
// Reflection: the authored sleep properties persist.
// ---------------------------------------------------------------------------

TEST_CASE("the sleep properties are registered and round-trip through the serializer") {
    reflect::register_builtin_types();
    reflect::register_physics_types();

    Scene scene;
    Node* node = scene.create_node("Sleeper");
    node->add_component<physics2d::Collider2D>();
    auto* body = node->add_component<physics2d::RigidBody2D>();
    body->can_sleep = false;
    body->sleep_time = 1.25f;
    body->sleep_linear_threshold = 0.2f;
    body->sleep_angular_threshold = 0.3f;

    auto json = serialization::scene::to_json(scene);
    REQUIRE(json);

    Scene loaded;
    REQUIRE_FALSE(serialization::scene::from_json(json.value(), loaded));
    Node* loaded_node = loaded.find_node_by_name("Sleeper");
    REQUIRE(loaded_node != nullptr);
    const auto* loaded_body = loaded_node->get_component<physics2d::RigidBody2D>();
    REQUIRE(loaded_body != nullptr);
    CHECK(loaded_body->can_sleep == false);
    CHECK(loaded_body->sleep_time == doctest::Approx(1.25f));
    CHECK(loaded_body->sleep_linear_threshold == doctest::Approx(0.2f));
    CHECK(loaded_body->sleep_angular_threshold == doctest::Approx(0.3f));
}
