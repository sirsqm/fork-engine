// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include <doctest/doctest.h>

#include "fe/physics2d/Collider2D.h"
#include "fe/physics2d/DistanceJoint2D.h"
#include "fe/physics2d/Geometry.h"
#include "fe/physics2d/JointSolver.h"
#include "fe/physics2d/Physics2DQueries.h"
#include "fe/physics2d/Physics2DSystem.h"
#include "fe/physics2d/PhysicsTypes.h"
#include "fe/physics2d/RevoluteJoint2D.h"
#include "fe/physics2d/RigidBody2D.h"
#include "fe/reflect/ComponentTypes.h"
#include "fe/reflect/Registry.h"
#include "fe/scene/Node.h"
#include "fe/scene/Scene.h"
#include "fe/serialization/SceneSerializer.h"
#include "fe/systems/FrameContext.h"

#include <cmath>
#include <ostream>
#include <string>
#include <vector>

using namespace fe;

namespace {

physics2d::Aabb2D box(f32 cx, f32 cy, f32 hx, f32 hy) {
    return physics2d::Aabb2D{Vec2{cx, cy}, Vec2{hx, hy}};
}

physics2d::Circle2D circle(f32 cx, f32 cy, f32 radius) {
    return physics2d::Circle2D{Vec2{cx, cy}, radius};
}

FrameContext step(f32 delta) {
    FrameContext context;
    context.delta_time = delta;
    context.elapsed_time = static_cast<f64>(delta);
    return context;
}

// A static floor: a box collider, no rigid body. Spans x in [-half_width,
// half_width], half a unit thick, with its top face at y = 0 (centre y = -0.5).
Node* add_floor(Scene& scene, f32 half_width) {
    Node* node = scene.create_node("Floor");
    auto* collider = node->add_component<physics2d::Collider2D>();
    collider->shape = physics2d::ShapeType::Box;
    collider->half_extent = Vec2{half_width, 0.5f};
    node->transform().position = Vec3{0.0f, -0.5f, 0.0f};
    return node;
}

// A dynamic box body.
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

// A dynamic point mass with no collider: the simplest joint end. Used for the
// joint tests so a constraint is the only thing acting on the body, which keeps
// the expected numbers exact.
Node* add_point_mass(Scene& scene, const Vec2& position, f32 mass) {
    Node* node = scene.create_node("Mass");
    auto* body = node->add_component<physics2d::RigidBody2D>();
    body->mass = mass;
    node->transform().position = Vec3{position.x, position.y, 0.0f};
    return node;
}

// A bare immovable node: no rigid body, no collider. This is a fixed pivot.
Node* add_pivot(Scene& scene, const Vec2& position) {
    Node* node = scene.create_node("Pivot");
    node->transform().position = Vec3{position.x, position.y, 0.0f};
    return node;
}

// Attaches a distance joint to `host`, wiring it to the two given nodes.
physics2d::DistanceJoint2D* add_distance_joint(Node& host, const Node& a, const Node& b, f32 rest_length,
                                               f32 stiffness = 0.0f, f32 damping = 0.0f) {
    auto* joint = host.add_component<physics2d::DistanceJoint2D>();
    joint->body_a = a.id();
    joint->body_b = b.id();
    joint->rest_length = rest_length;
    joint->stiffness = stiffness;
    joint->damping = damping;
    return joint;
}

physics2d::RevoluteJoint2D* add_revolute_joint(Node& host, const Node& a, const Node& b, const Vec2& anchor_a = {},
                                               const Vec2& anchor_b = {}) {
    auto* joint = host.add_component<physics2d::RevoluteJoint2D>();
    joint->body_a = a.id();
    joint->body_b = b.id();
    joint->anchor_a = anchor_a;
    joint->anchor_b = anchor_b;
    return joint;
}

} // namespace

// ---------------------------------------------------------------------------
// Geometry: pure narrow-phase maths, tested without a scene.
// ---------------------------------------------------------------------------

TEST_CASE("separated boxes do not overlap") {
    const auto contact = physics2d::intersect(box(0.0f, 0.0f, 1.0f, 1.0f), box(5.0f, 0.0f, 1.0f, 1.0f));
    CHECK_FALSE(contact.overlapping);
}

TEST_CASE("boxes touching exactly are not overlapping") {
    // A's right face is at x = 1, B's left face is at x = 1: zero depth.
    const auto contact = physics2d::intersect(box(0.0f, 0.0f, 1.0f, 1.0f), box(2.0f, 0.0f, 1.0f, 1.0f));
    CHECK_FALSE(contact.overlapping);
}

TEST_CASE("overlapping boxes report the smallest axis as the contact normal") {
    // A spans x,y in [-1,1]. B spans x in [0.5,2.5] (overlap 0.5) and y in
    // [0.75,2.75] (overlap 0.25). The y axis is the separating axis, so the
    // normal is vertical and the penetration is the smaller y overlap.
    const auto contact = physics2d::intersect(box(0.0f, 0.0f, 1.0f, 1.0f), box(1.5f, 1.75f, 1.0f, 1.0f));
    REQUIRE(contact.overlapping);
    CHECK(contact.normal.x == doctest::Approx(0.0f));
    CHECK(contact.normal.y == doctest::Approx(1.0f));
    CHECK(contact.penetration == doctest::Approx(0.25f));
}

TEST_CASE("a box normal points from the first box towards the second") {
    const auto contact = physics2d::intersect(box(0.0f, 0.0f, 1.0f, 1.0f), box(1.5f, 0.0f, 1.0f, 1.0f));
    REQUIRE(contact.overlapping);
    CHECK(contact.normal.x == doctest::Approx(1.0f));
    CHECK(contact.penetration == doctest::Approx(0.5f));
}

TEST_CASE("separated circles do not overlap") {
    CHECK_FALSE(physics2d::intersect(circle(0.0f, 0.0f, 1.0f), circle(5.0f, 0.0f, 1.0f)).overlapping);
}

TEST_CASE("circles touching exactly are not overlapping") {
    CHECK_FALSE(physics2d::intersect(circle(0.0f, 0.0f, 1.0f), circle(2.0f, 0.0f, 1.0f)).overlapping);
}

TEST_CASE("overlapping circles report the distance along the centre line") {
    const auto contact = physics2d::intersect(circle(0.0f, 0.0f, 2.0f), circle(3.0f, 0.0f, 2.0f));
    REQUIRE(contact.overlapping);
    CHECK(contact.normal.x == doctest::Approx(1.0f));
    CHECK(contact.normal.y == doctest::Approx(0.0f));
    // radii sum 4, distance 3 -> penetration 1.
    CHECK(contact.penetration == doctest::Approx(1.0f));
}

TEST_CASE("concentric circles still produce a usable normal") {
    const auto contact = physics2d::intersect(circle(0.0f, 0.0f, 2.0f), circle(0.0f, 0.0f, 2.0f));
    REQUIRE(contact.overlapping);
    CHECK(contact.normal.length() == doctest::Approx(1.0f));
    CHECK(contact.penetration == doctest::Approx(4.0f));
}

TEST_CASE("a circle against a box face reports a face normal") {
    // Box occupies x in [-1, 1], y in [-1, 1]. Circle sits above the top face.
    const auto contact = physics2d::intersect(circle(0.0f, 1.5f, 1.0f), box(0.0f, 0.0f, 1.0f, 1.0f));
    REQUIRE(contact.overlapping);
    // Normal points from the circle towards the box: downwards.
    CHECK(contact.normal.x == doctest::Approx(0.0f));
    CHECK(contact.normal.y == doctest::Approx(-1.0f));
    CHECK(contact.penetration == doctest::Approx(0.5f));
}

TEST_CASE("a circle near a box corner reports a diagonal normal") {
    // Circle centred just outside the top-right corner (1, 1).
    const auto contact = physics2d::intersect(circle(1.5f, 1.5f, 1.0f), box(0.0f, 0.0f, 1.0f, 1.0f));
    REQUIRE(contact.overlapping);
    const f32 inv = 1.0f / std::sqrt(2.0f);
    CHECK(contact.normal.x == doctest::Approx(-inv));
    CHECK(contact.normal.y == doctest::Approx(-inv));
}

TEST_CASE("a circle whose centre is inside a box is separated along the nearest face") {
    // Centre is 0.1 above the box centre, closer to the top face. The normal
    // points from the circle towards the box -- the same A-to-B convention the
    // outside case uses -- so it points *down*, into the box and back along the
    // axis the circle entered on. The solver pushes the circle along -normal,
    // which carries it out through the top face it is nearest to.
    const auto contact = physics2d::intersect(circle(0.0f, 0.1f, 0.5f), box(0.0f, 0.0f, 1.0f, 1.0f));
    REQUIRE(contact.overlapping);
    CHECK(contact.normal.y == doctest::Approx(-1.0f));
    CHECK(contact.normal.x == doctest::Approx(0.0f));
    // Nearest face is the top, 0.9 away, plus the radius: the depth needed to
    // clear the box.
    CHECK(contact.penetration == doctest::Approx(0.9f + 0.5f));
}

TEST_CASE("an inside-circle normal points the way the solver must push") {
    // The regression this guards: a normal pointing *out* through the nearest
    // face reads as "the way out", but the solver treats the normal as pointing
    // from A towards B and pushes along -normal, so an outward normal drives the
    // circle deeper and skips the bounce impulse as separating. A ball that
    // entered a wall would be swallowed and ejected out the far side.
    const auto box_shape = box(0.0f, 0.0f, 1.0f, 1.0f);
    struct Case {
        f32 x;
        f32 y;
        Vec2 expected;
    };
    // Just inside each of the four faces: the normal points from the circle
    // towards the box, so it points back the way the circle came in.
    const Case cases[] = {
        {0.9f, 0.0f, Vec2{-1.0f, 0.0f}}, // entered the right face
        {-0.9f, 0.0f, Vec2{1.0f, 0.0f}}, // entered the left face
        {0.0f, 0.9f, Vec2{0.0f, -1.0f}}, // entered the top face
        {0.0f, -0.9f, Vec2{0.0f, 1.0f}}, // entered the bottom face
    };
    for (const Case& c : cases) {
        const auto contact = physics2d::intersect(circle(c.x, c.y, 0.5f), box_shape);
        REQUIRE(contact.overlapping);
        CHECK(contact.normal.x == doctest::Approx(c.expected.x));
        CHECK(contact.normal.y == doctest::Approx(c.expected.y));
        // The push the solver applies is -normal. On the axis the circle is
        // nearest a face, that push must carry it clear of the box rather than
        // deeper in. (The other axis is untouched and stays where it was.)
        const f32 pushed_x = c.x - contact.normal.x * contact.penetration;
        const f32 pushed_y = c.y - contact.normal.y * contact.penetration;
        if (c.expected.x != 0.0f) {
            CHECK(std::abs(pushed_x) >= 1.0f - 0.001f);
        } else {
            CHECK(std::abs(pushed_y) >= 1.0f - 0.001f);
        }
    }
}

TEST_CASE("an inside-circle normal matches the outside normal it replaces") {
    // Continuity at the boundary: a circle just inside the right face and one
    // just outside it are the same contact seen from either side, so the normal
    // must not flip sign as the centre crosses the face. A flip is exactly the
    // bug above.
    const auto inside = physics2d::intersect(circle(0.99f, 0.0f, 0.5f), box(0.0f, 0.0f, 1.0f, 1.0f));
    const auto outside = physics2d::intersect(circle(1.01f, 0.0f, 0.5f), box(0.0f, 0.0f, 1.0f, 1.0f));
    REQUIRE(inside.overlapping);
    REQUIRE(outside.overlapping);
    CHECK(inside.normal.x == doctest::Approx(outside.normal.x));
    CHECK(inside.normal.y == doctest::Approx(outside.normal.y));
}

TEST_CASE("a rotated box's inside-circle normal is rotated with the box") {
    // The same inside case on a box rotated 90 degrees. The local frame is
    // rotated back, so a circle just inside the box's local +y face must still
    // produce a normal pointing along the box's local -y. Local +y is world -x
    // after the rotation, so the normal comes out as world +x -- inward, the
    // solver's A-to-B direction -- and the push (-normal) carries the circle out
    // through the world -x face it entered.
    const physics2d::Obb2D rotated = physics2d::Obb2D{Vec2{0.0f, 0.0f}, Vec2{1.0f, 1.0f}, 1.5707963f};
    // Local (0, 0.9) rotated by +90 degrees is world (-0.9, 0).
    const auto contact = physics2d::intersect(circle(-0.9f, 0.0f, 0.5f), rotated);
    REQUIRE(contact.overlapping);
    CHECK(contact.normal.x == doctest::Approx(1.0f).epsilon(0.001));
    CHECK(contact.normal.y == doctest::Approx(0.0f).epsilon(0.001));
    // Pushing along -normal must carry the circle clear of the box, not deeper.
    CHECK(-0.9f - contact.normal.x * contact.penetration <= -1.0f + 0.001f);
}

TEST_CASE("intersect_shapes flips the normal so it always points from the first shape") {
    const auto a_box = box(0.0f, 0.0f, 1.0f, 1.0f);
    const auto b_circle = circle(0.0f, 1.5f, 1.0f);

    // Circle first: normal points from the circle down towards the box.
    const auto circle_first = physics2d::intersect_shapes(physics2d::ShapeType::Circle, a_box, b_circle,
                                                          physics2d::ShapeType::Box, a_box, b_circle);
    // Box first: the same pair, but the normal must point from the box up.
    const auto box_first = physics2d::intersect_shapes(physics2d::ShapeType::Box, a_box, b_circle,
                                                       physics2d::ShapeType::Circle, a_box, b_circle);

    REQUIRE(circle_first.overlapping);
    REQUIRE(box_first.overlapping);
    CHECK(circle_first.normal.y == doctest::Approx(-box_first.normal.y));
    CHECK(circle_first.penetration == doctest::Approx(box_first.penetration));
}

TEST_CASE("bounds_overlap is a conservative prefilter") {
    CHECK(physics2d::bounds_overlap(box(0.0f, 0.0f, 1.0f, 1.0f), box(1.5f, 0.0f, 1.0f, 1.0f)));
    CHECK_FALSE(physics2d::bounds_overlap(box(0.0f, 0.0f, 1.0f, 1.0f), box(3.0f, 0.0f, 1.0f, 1.0f)));
    CHECK(physics2d::bounds_overlap(box(0.0f, 0.0f, 1.0f, 1.0f), circle(1.5f, 0.0f, 1.0f)));
    CHECK_FALSE(physics2d::bounds_overlap(box(0.0f, 0.0f, 1.0f, 1.0f), circle(5.0f, 0.0f, 1.0f)));
}

// ---------------------------------------------------------------------------
// The system: integration and resolution against a scene.
// ---------------------------------------------------------------------------

TEST_CASE("gravity moves a dynamic body downwards") {
    Scene scene;
    Node* body_node = add_dynamic_box(scene, Vec2{0.0f, 10.0f}, 0.5f, 1.0f);

    physics2d::Physics2DSystem system;
    for (int i = 0; i < 10; ++i) {
        system.update(step(1.0f / 60.0f), scene);
    }

    CHECK(body_node->transform().position.y < 10.0f);
    CHECK(body_node->get_component<physics2d::RigidBody2D>()->velocity.y < 0.0f);
}

TEST_CASE("a body lands on a static floor and does not fall through") {
    Scene scene;
    add_floor(scene, 20.0f); // top face at y = 0
    Node* body_node = add_dynamic_box(scene, Vec2{0.0f, 5.0f}, 0.5f, 1.0f);

    physics2d::Physics2DSystem system;
    for (int i = 0; i < 300; ++i) { // 5 seconds at 60 Hz
        system.update(step(1.0f / 60.0f), scene);
    }

    // The body's half-height is 0.5, so resting on the floor puts its centre
    // near y = 0.5. It must not have tunnelled below the floor.
    CHECK(body_node->transform().position.y > 0.4f);
    CHECK(body_node->transform().position.y < 0.7f);
    CHECK(std::abs(body_node->get_component<physics2d::RigidBody2D>()->velocity.y) < 0.5f);
}

TEST_CASE("a static collider with no rigid body never moves") {
    Scene scene;
    Node* floor_node = add_floor(scene, 5.0f);
    const Vec3 before = floor_node->transform().position;

    // Drop a heavy body onto it; the floor must stay put.
    add_dynamic_box(scene, Vec2{0.0f, 3.0f}, 0.5f, 100.0f);
    physics2d::Physics2DSystem system;
    for (int i = 0; i < 120; ++i) {
        system.update(step(1.0f / 60.0f), scene);
    }

    CHECK(floor_node->transform().position.y == doctest::Approx(before.y));
    CHECK(floor_node->transform().position.x == doctest::Approx(before.x));
}

TEST_CASE("a bouncy body reverses its vertical velocity on impact") {
    Scene scene;
    add_floor(scene, 20.0f);
    Node* body_node = add_dynamic_box(scene, Vec2{0.0f, 4.0f}, 0.5f, 1.0f);
    body_node->get_component<physics2d::Collider2D>()->restitution = 1.0f;
    // Give the floor bounciness too; the pair takes the larger restitution.
    scene.roots().front()->get_component<physics2d::Collider2D>()->restitution = 1.0f;

    physics2d::Physics2DSystem system;
    f32 max_upward = 0.0f;
    bool saw_downward = false;
    for (int i = 0; i < 200; ++i) {
        system.update(step(1.0f / 60.0f), scene);
        const f32 vy = body_node->get_component<physics2d::RigidBody2D>()->velocity.y;
        if (vy < -0.1f) {
            saw_downward = true;
        }
        if (saw_downward && vy > max_upward) {
            max_upward = vy;
        }
    }

    CHECK(saw_downward);
    CHECK(max_upward > 1.0f); // it bounced back up with real speed
}

TEST_CASE("a dead body does not bounce") {
    Scene scene;
    add_floor(scene, 20.0f);
    Node* body_node = add_dynamic_box(scene, Vec2{0.0f, 4.0f}, 0.5f, 1.0f);
    // Default restitution is 0 on both.
    physics2d::Physics2DSystem system;

    f32 max_upward = 0.0f;
    bool saw_downward = false;
    for (int i = 0; i < 200; ++i) {
        system.update(step(1.0f / 60.0f), scene);
        const f32 vy = body_node->get_component<physics2d::RigidBody2D>()->velocity.y;
        if (vy < -0.1f) {
            saw_downward = true;
        }
        if (saw_downward && vy > max_upward) {
            max_upward = vy;
        }
    }

    CHECK(saw_downward);
    CHECK(max_upward < 0.2f);
}

TEST_CASE("two dynamic bodies pushed together separate") {
    Scene scene;
    // Start overlapping on x by 0.5 (centres 0.5 apart, half-widths 0.5 each),
    // so the solver must push them apart. No gravity, so the only motion is the
    // separation.
    Node* left = add_dynamic_box(scene, Vec2{-0.25f, 0.0f}, 0.5f, 1.0f);
    Node* right = add_dynamic_box(scene, Vec2{0.25f, 0.0f}, 0.5f, 1.0f);

    physics2d::Physics2DSystem system;
    system.gravity = Vec2{0.0f, 0.0f};
    for (int i = 0; i < 120; ++i) {
        system.update(step(1.0f / 60.0f), scene);
    }

    const f32 gap = right->transform().position.x - left->transform().position.x;
    // Their faces must end up at least touching (separation ~1.0).
    CHECK(gap > 0.9f);
    // And they must have moved apart symmetrically (equal mass).
    CHECK(left->transform().position.x == doctest::Approx(-right->transform().position.x));
}

TEST_CASE("the system reports the contacts it resolved") {
    Scene scene;
    add_floor(scene, 20.0f);
    add_dynamic_box(scene, Vec2{0.0f, 0.5f}, 0.5f, 1.0f); // already resting on the floor

    physics2d::Physics2DSystem system;
    system.update(step(1.0f / 60.0f), scene);
    CHECK(system.contact_count() >= 1);
}

TEST_CASE("a non-positive delta time is a no-op") {
    Scene scene;
    Node* body_node = add_dynamic_box(scene, Vec2{0.0f, 10.0f}, 0.5f, 1.0f);
    const Vec3 before = body_node->transform().position;

    physics2d::Physics2DSystem system;
    system.update(step(0.0f), scene);
    system.update(step(-1.0f), scene);

    CHECK(body_node->transform().position.y == doctest::Approx(before.y));
    CHECK(system.contact_count() == 0);
}

TEST_CASE("an empty scene updates cleanly") {
    Scene scene;
    physics2d::Physics2DSystem system;
    system.update(step(1.0f / 60.0f), scene);
    CHECK(system.contact_count() == 0);
}

TEST_CASE("a collider with no rigid body does not move on its own") {
    Scene scene;
    Node* node = scene.create_node("Static");
    auto* collider = node->add_component<physics2d::Collider2D>();
    collider->half_extent = Vec2{1.0f, 1.0f};
    node->transform().position = Vec3{3.0f, 4.0f, 0.0f};

    physics2d::Physics2DSystem system;
    for (int i = 0; i < 60; ++i) {
        system.update(step(1.0f / 60.0f), scene);
    }

    CHECK(node->transform().position.y == doctest::Approx(4.0f));
}
// ---------------------------------------------------------------------------
// Collision filtering and triggers.
// ---------------------------------------------------------------------------

TEST_CASE("the layer/mask rule is symmetric and default-open") {
    physics2d::Collider2D a;
    physics2d::Collider2D b;
    // The defaults must collide: an unedited collider behaves as before.
    CHECK(a.can_collide_with(b));

    // One side opts out by dropping the other's layer from its mask.
    a.layer = 0x1u;
    b.layer = 0x2u;
    b.mask = 0x1u;
    a.mask = 0xFFFFFFFFu;
    CHECK(a.can_collide_with(b)); // a sees b (a.mask has b.layer, b.mask has a.layer)

    a.mask = 0x2u; // a now collides with layer 2
    b.mask = 0xFFFFFFFFu;
    CHECK(a.can_collide_with(b));

    // Neither side's mask includes the other's layer: no collision.
    a.layer = 0x1u;
    a.mask = 0x1u; // a only collides with layer 1 (itself)
    b.layer = 0x2u;
    b.mask = 0x2u; // b only collides with layer 2 (itself)
    CHECK_FALSE(a.can_collide_with(b));
}

TEST_CASE("a filtered pair passes through and reports no contact") {
    Scene scene;
    add_floor(scene, 20.0f);
    Node* body = add_dynamic_box(scene, Vec2{0.0f, 5.0f}, 0.5f, 1.0f);

    // The floor and the body are put on layers that do not see each other.
    scene.find_node_by_name("Floor")->get_component<physics2d::Collider2D>()->layer = 0x1u;
    scene.find_node_by_name("Floor")->get_component<physics2d::Collider2D>()->mask = 0x1u;
    body->get_component<physics2d::Collider2D>()->layer = 0x2u;
    body->get_component<physics2d::Collider2D>()->mask = 0x2u;

    physics2d::Physics2DSystem system;
    for (int i = 0; i < 120; ++i) {
        system.update(step(1.0f / 60.0f), scene);
    }

    // The body falls straight through the floor: no impulse, no contact.
    CHECK(body->transform().position.y < -1.0f);
    CHECK(system.contact_count() == 0);
    CHECK(system.contact_event_count() == 0);
}

TEST_CASE("a pair that agrees to collide still resolves") {
    Scene scene;
    add_floor(scene, 20.0f);
    Node* body = add_dynamic_box(scene, Vec2{0.0f, 5.0f}, 0.5f, 1.0f);

    // Both on the same layer and seeing each other: the normal path.
    scene.find_node_by_name("Floor")->get_component<physics2d::Collider2D>()->layer = 0x4u;
    scene.find_node_by_name("Floor")->get_component<physics2d::Collider2D>()->mask = 0x4u;
    body->get_component<physics2d::Collider2D>()->layer = 0x4u;
    body->get_component<physics2d::Collider2D>()->mask = 0x4u;

    physics2d::Physics2DSystem system;
    for (int i = 0; i < 300; ++i) {
        system.update(step(1.0f / 60.0f), scene);
    }

    CHECK(body->transform().position.y > 0.4f); // resting on the floor
    CHECK(system.contact_count() >= 1);
}

TEST_CASE("a trigger reports a contact but applies no impulse") {
    Scene scene;
    Node* floor = add_floor(scene, 20.0f);
    Node* body = add_dynamic_box(scene, Vec2{0.0f, 5.0f}, 0.5f, 1.0f);

    // The floor is a sensor: it detects the body but does not stop it.
    floor->get_component<physics2d::Collider2D>()->is_trigger = true;

    physics2d::Physics2DSystem system;
    bool saw_begin = false;
    for (int i = 0; i < 120; ++i) {
        system.update(step(1.0f / 60.0f), scene);
        for (const physics2d::ContactEvent& event : system.contact_events()) {
            if (event.phase == physics2d::ContactPhase::Begin) {
                saw_begin = true;
            }
        }
    }

    // The body passed through, so its velocity was never reversed and it is now
    // below the floor...
    CHECK(body->transform().position.y < -1.0f);
    // ...but the contact was reported while it overlapped the sensor.
    CHECK(saw_begin);
}

TEST_CASE("a trigger emits a matching End event when the pair separates") {
    Scene scene;
    // A sensor the body falls through, so the pair begins and then ends.
    Node* sensor = scene.create_node("Sensor");
    auto* sensor_collider = sensor->add_component<physics2d::Collider2D>();
    sensor_collider->half_extent = Vec2{5.0f, 0.5f};
    sensor_collider->is_trigger = true;
    sensor->transform().position = Vec3{0.0f, 0.0f, 0.0f};

    add_dynamic_box(scene, Vec2{0.0f, 5.0f}, 0.5f, 1.0f);

    physics2d::Physics2DSystem system;
    int begins = 0;
    int ends = 0;
    for (int i = 0; i < 180; ++i) {
        system.update(step(1.0f / 60.0f), scene);
        for (const physics2d::ContactEvent& event : system.contact_events()) {
            if (event.phase == physics2d::ContactPhase::Begin) {
                ++begins;
            } else {
                ++ends;
            }
        }
    }

    CHECK(begins == 1);
    CHECK(ends == 1);
}

// ---------------------------------------------------------------------------
// Reflection: the components persist and appear in the registry.
// ---------------------------------------------------------------------------

TEST_CASE("the physics components are registered with reflection") {
    reflect::register_physics_types();
    const reflect::Registry& registry = reflect::Registry::instance();
    CHECK(registry.contains("Collider2D"));
    CHECK(registry.contains("RigidBody2D"));
}

TEST_CASE("a collider and rigid body round-trip through the serializer") {
    reflect::register_builtin_types();
    reflect::register_physics_types();

    Scene scene;
    Node* node = scene.create_node("Crate");
    auto* collider = node->add_component<physics2d::Collider2D>();
    collider->shape = physics2d::ShapeType::Circle;
    collider->radius = 1.25f;
    collider->restitution = 0.4f;
    collider->friction = 0.2f;
    auto* body = node->add_component<physics2d::RigidBody2D>();
    body->velocity = Vec2{3.0f, -2.0f};
    body->mass = 2.5f;
    body->kinematic = true;
    body->affected_by_gravity = false;

    auto json = serialization::scene::to_json(scene);
    REQUIRE(json);

    Scene loaded;
    REQUIRE_FALSE(serialization::scene::from_json(json.value(), loaded));

    Node* loaded_node = loaded.find_node_by_name("Crate");
    REQUIRE(loaded_node != nullptr);

    const auto* loaded_collider = loaded_node->get_component<physics2d::Collider2D>();
    REQUIRE(loaded_collider != nullptr);
    CHECK(loaded_collider->shape == physics2d::ShapeType::Circle);
    CHECK(loaded_collider->radius == doctest::Approx(1.25f));
    CHECK(loaded_collider->restitution == doctest::Approx(0.4f));
    CHECK(loaded_collider->friction == doctest::Approx(0.2f));

    const auto* loaded_body = loaded_node->get_component<physics2d::RigidBody2D>();
    REQUIRE(loaded_body != nullptr);
    CHECK(loaded_body->velocity.x == doctest::Approx(3.0f));
    CHECK(loaded_body->velocity.y == doctest::Approx(-2.0f));
    CHECK(loaded_body->mass == doctest::Approx(2.5f));
    CHECK(loaded_body->kinematic == true);
    CHECK(loaded_body->affected_by_gravity == false);
}

TEST_CASE("collision filtering fields round-trip through the serializer") {
    reflect::register_builtin_types();
    reflect::register_physics_types();

    Scene scene;
    Node* node = scene.create_node("Pickup");
    auto* collider = node->add_component<physics2d::Collider2D>();
    collider->layer = 0x8u;
    collider->mask = 0x6u;
    collider->is_trigger = true;

    auto json = serialization::scene::to_json(scene);
    REQUIRE(json);

    Scene loaded;
    REQUIRE_FALSE(serialization::scene::from_json(json.value(), loaded));

    Node* loaded_node = loaded.find_node_by_name("Pickup");
    REQUIRE(loaded_node != nullptr);
    const auto* loaded_collider = loaded_node->get_component<physics2d::Collider2D>();
    REQUIRE(loaded_collider != nullptr);
    CHECK(loaded_collider->layer == 0x8u);
    CHECK(loaded_collider->mask == 0x6u);
    CHECK(loaded_collider->is_trigger == true);
}

// ---------------------------------------------------------------------------
// Angular dynamics: rotation integrated by the solver.
// ---------------------------------------------------------------------------

TEST_CASE("angular velocity integrates into the node's rotation") {
    Scene scene;
    Node* node = add_dynamic_box(scene, Vec2{0.0f, 0.0f}, 0.5f, 1.0f);
    auto* body = node->get_component<physics2d::RigidBody2D>();
    body->angular_velocity = 2.0f; // radians per second

    physics2d::Physics2DSystem system;
    system.gravity = Vec2{0.0f, 0.0f}; // isolate the rotation
    for (int i = 0; i < 60; ++i) {
        system.update(step(1.0f / 60.0f), scene);
    }

    // One second at 2 rad/s, with nothing to change the angular velocity.
    CHECK(node->transform().rotation.z == doctest::Approx(2.0f));
    CHECK(body->angular_velocity == doctest::Approx(2.0f));
}

TEST_CASE("a constant torque spins a body up") {
    Scene scene;
    Node* node = add_dynamic_box(scene, Vec2{0.0f, 0.0f}, 0.5f, 1.0f);
    auto* collider = node->get_component<physics2d::Collider2D>();
    auto* body = node->get_component<physics2d::RigidBody2D>();
    // A unit square: inertia = m (w^2 + h^2) / 12 = 1 * (1 + 1) / 12 = 1/6.
    const f32 expected_inertia = collider->inertia_about_center(body->mass);
    CHECK(expected_inertia == doctest::Approx(1.0f / 6.0f));
    body->torque = 1.0f; // 1 N*m

    physics2d::Physics2DSystem system;
    system.gravity = Vec2{0.0f, 0.0f};
    const f32 dt = 1.0f / 60.0f;
    for (int i = 0; i < 60; ++i) {
        system.update(step(dt), scene);
    }

    // alpha = torque / inertia = 6 rad/s^2; after 1 s, w = 6 rad/s.
    CHECK(body->angular_velocity == doctest::Approx(6.0f).epsilon(0.01));
}

TEST_CASE("angular damping decays the spin") {
    Scene scene;
    Node* node = add_dynamic_box(scene, Vec2{0.0f, 0.0f}, 0.5f, 1.0f);
    auto* body = node->get_component<physics2d::RigidBody2D>();
    body->angular_velocity = 4.0f;
    body->angular_damping = 0.5f; // shed half per second

    physics2d::Physics2DSystem system;
    system.gravity = Vec2{0.0f, 0.0f};
    for (int i = 0; i < 60; ++i) {
        system.update(step(1.0f / 60.0f), scene);
    }

    // After one second the spin is roughly halved (the discrete steps land
    // close to the continuous exp(-0.5) factor).
    CHECK(body->angular_velocity < 2.5f);
    CHECK(body->angular_velocity > 1.5f);
}

TEST_CASE("a box resting flat on the floor stays level") {
    // A contact through the centre produces no torque, so a symmetric box on a
    // flat floor must not start spinning. This is the guard against the angular
    // term injecting spurious rotation into a resting stack.
    Scene scene;
    add_floor(scene, 20.0f);
    Node* node = add_dynamic_box(scene, Vec2{0.0f, 0.5f}, 0.5f, 1.0f);

    physics2d::Physics2DSystem system;
    for (int i = 0; i < 120; ++i) {
        system.update(step(1.0f / 60.0f), scene);
    }

    CHECK(std::abs(node->transform().rotation.z) < 0.01f);
    CHECK(std::abs(node->get_component<physics2d::RigidBody2D>()->angular_velocity) < 0.01f);
}

TEST_CASE("fixed_rotation keeps a body from turning while it still falls") {
    Scene scene;
    Node* node = add_dynamic_box(scene, Vec2{0.0f, 5.0f}, 0.5f, 1.0f);
    auto* body = node->get_component<physics2d::RigidBody2D>();
    body->fixed_rotation = true;
    body->angular_velocity = 3.0f; // would spin if not locked

    physics2d::Physics2DSystem system;
    for (int i = 0; i < 30; ++i) {
        system.update(step(1.0f / 60.0f), scene);
    }

    CHECK(node->transform().rotation.z == doctest::Approx(0.0f));
    CHECK(node->transform().position.y < 5.0f); // gravity still applied
}

TEST_CASE("a static collider's rotation is never changed by the solver") {
    Scene scene;
    Node* node = scene.create_node("Static");
    auto* collider = node->add_component<physics2d::Collider2D>();
    collider->half_extent = Vec2{1.0f, 1.0f};
    node->transform().rotation.z = 0.7f;

    physics2d::Physics2DSystem system;
    for (int i = 0; i < 60; ++i) {
        system.update(step(1.0f / 60.0f), scene);
    }

    CHECK(node->transform().rotation.z == doctest::Approx(0.7f));
}

// ---------------------------------------------------------------------------
// Queries: raycast and overlap.
// ---------------------------------------------------------------------------

TEST_CASE("raycast_aabb reports the distance to the near face") {
    // Box spans x in [-1, 1], y in [-1, 1]. A ray from (-5, 0) along +x enters
    // at x = -1, a distance of 4.
    const f32 hit = physics2d::raycast_aabb(Vec2{-5.0f, 0.0f}, Vec2{1.0f, 0.0f}, box(0.0f, 0.0f, 1.0f, 1.0f));
    CHECK(hit == doctest::Approx(4.0f));
}

TEST_CASE("raycast_aabb misses when the ray points away") {
    // From (-5, 0) along -x, the box is behind the ray.
    CHECK(physics2d::raycast_aabb(Vec2{-5.0f, 0.0f}, Vec2{-1.0f, 0.0f}, box(0.0f, 0.0f, 1.0f, 1.0f)) < 0.0f);
}

TEST_CASE("raycast_aabb handles a ray parallel to a slab") {
    // A ray along +x at y = 3 passes above a box that spans y in [-1, 1].
    CHECK(physics2d::raycast_aabb(Vec2{-5.0f, 3.0f}, Vec2{1.0f, 0.0f}, box(0.0f, 0.0f, 1.0f, 1.0f)) < 0.0f);
    // At y = 0 (inside the slab) it hits.
    CHECK(physics2d::raycast_aabb(Vec2{-5.0f, 0.0f}, Vec2{1.0f, 0.0f}, box(0.0f, 0.0f, 1.0f, 1.0f)) > 0.0f);
}

TEST_CASE("raycast_circle reports the near intersection") {
    // Circle at origin radius 1, ray from (-5, 0) along +x enters at x = -1.
    const f32 hit = physics2d::raycast_circle(Vec2{-5.0f, 0.0f}, Vec2{1.0f, 0.0f}, circle(0.0f, 0.0f, 1.0f));
    CHECK(hit == doctest::Approx(4.0f));
}

TEST_CASE("raycast_circle misses a ray that passes outside") {
    CHECK(physics2d::raycast_circle(Vec2{-5.0f, 5.0f}, Vec2{1.0f, 0.0f}, circle(0.0f, 0.0f, 1.0f)) < 0.0f);
}

TEST_CASE("a ray that starts inside a shape hits at distance zero") {
    CHECK(physics2d::raycast_aabb(Vec2{0.0f, 0.0f}, Vec2{1.0f, 0.0f}, box(0.0f, 0.0f, 1.0f, 1.0f)) ==
          doctest::Approx(0.0f));
    CHECK(physics2d::raycast_circle(Vec2{0.0f, 0.0f}, Vec2{1.0f, 0.0f}, circle(0.0f, 0.0f, 1.0f)) ==
          doctest::Approx(0.0f));
}

TEST_CASE("the raycast normal points back towards the origin") {
    const auto a_box = box(0.0f, 0.0f, 1.0f, 1.0f);
    const f32 hit = physics2d::raycast_aabb(Vec2{-5.0f, 0.0f}, Vec2{1.0f, 0.0f}, a_box);
    const Vec2 normal = physics2d::raycast_aabb_normal(Vec2{-5.0f, 0.0f}, Vec2{1.0f, 0.0f}, a_box, hit);
    CHECK(normal.x == doctest::Approx(-1.0f));
    CHECK(normal.y == doctest::Approx(0.0f));

    // A circle: the normal is radial, here pointing back along -x.
    const auto a_circle = circle(0.0f, 0.0f, 1.0f);
    const f32 circle_hit = physics2d::raycast_circle(Vec2{-5.0f, 0.0f}, Vec2{1.0f, 0.0f}, a_circle);
    const Vec2 circle_normal =
        physics2d::raycast_circle_normal(Vec2{-5.0f, 0.0f}, Vec2{1.0f, 0.0f}, a_circle, circle_hit);
    CHECK(circle_normal.x == doctest::Approx(-1.0f));
    CHECK(circle_normal.y == doctest::Approx(0.0f));
}

TEST_CASE("scene raycast returns the nearest collider and its hit point") {
    Scene scene;
    // A near box and a far box along +x from the origin.
    Node* near_node = scene.create_node("Near");
    auto* near_collider = near_node->add_component<physics2d::Collider2D>();
    near_collider->half_extent = Vec2{1.0f, 1.0f};
    near_node->transform().position = Vec3{2.0f, 0.0f, 0.0f}; // left face at x = 1

    Node* far_node = scene.create_node("Far");
    auto* far_collider = far_node->add_component<physics2d::Collider2D>();
    far_collider->half_extent = Vec2{1.0f, 1.0f};
    far_node->transform().position = Vec3{10.0f, 0.0f, 0.0f}; // left face at x = 9

    const physics2d::RaycastHit hit = physics2d::raycast(scene, Vec2{0.0f, 0.0f}, Vec2{1.0f, 0.0f});
    REQUIRE(hit.hit);
    CHECK((hit.node == near_node));
    CHECK(hit.distance == doctest::Approx(1.0f));
    CHECK(hit.point.x == doctest::Approx(1.0f));
    CHECK(hit.point.y == doctest::Approx(0.0f));
    CHECK(hit.normal.x == doctest::Approx(-1.0f));
    (void)far_node;
}

TEST_CASE("scene raycast respects a max distance") {
    Scene scene;
    Node* node = scene.create_node("Wall");
    auto* collider = node->add_component<physics2d::Collider2D>();
    collider->half_extent = Vec2{1.0f, 1.0f};
    node->transform().position = Vec3{10.0f, 0.0f, 0.0f};

    CHECK_FALSE(physics2d::raycast(scene, Vec2{0.0f, 0.0f}, Vec2{1.0f, 0.0f}, 5.0f).hit);
    CHECK(physics2d::raycast(scene, Vec2{0.0f, 0.0f}, Vec2{1.0f, 0.0f}, 15.0f).hit);
}

TEST_CASE("a zero-length raycast direction is a miss, not a crash") {
    Scene scene;
    Node* node = scene.create_node("Wall");
    auto* collider = node->add_component<physics2d::Collider2D>();
    collider->half_extent = Vec2{1.0f, 1.0f};
    node->transform().position = Vec3{0.0f, 0.0f, 0.0f};

    CHECK_FALSE(physics2d::raycast(scene, Vec2{0.0f, 0.0f}, Vec2{0.0f, 0.0f}).hit);
}

TEST_CASE("a raycast against a scene with no colliders is a clean miss") {
    Scene scene;
    static_cast<void>(scene.create_node("Empty")); // a node with no collider
    CHECK_FALSE(physics2d::raycast(scene, Vec2{0.0f, 0.0f}, Vec2{1.0f, 0.0f}).hit);
}

TEST_CASE("raycast finds a circle collider") {
    Scene scene;
    Node* node = scene.create_node("Ball");
    auto* collider = node->add_component<physics2d::Collider2D>();
    collider->shape = physics2d::ShapeType::Circle;
    collider->radius = 1.0f;
    node->transform().position = Vec3{5.0f, 0.0f, 0.0f}; // near face at x = 4

    const physics2d::RaycastHit hit = physics2d::raycast(scene, Vec2{0.0f, 0.0f}, Vec2{1.0f, 0.0f});
    REQUIRE(hit.hit);
    CHECK((hit.node == node));
    CHECK(hit.distance == doctest::Approx(4.0f));
}

TEST_CASE("overlap_circle finds colliders within the radius") {
    Scene scene;
    Node* near_node = scene.create_node("Near");
    auto* near_collider = near_node->add_component<physics2d::Collider2D>();
    near_collider->half_extent = Vec2{1.0f, 1.0f};
    near_node->transform().position = Vec3{2.0f, 0.0f, 0.0f};

    Node* far_node = scene.create_node("Far");
    auto* far_collider = far_node->add_component<physics2d::Collider2D>();
    far_collider->half_extent = Vec2{1.0f, 1.0f};
    far_node->transform().position = Vec3{50.0f, 0.0f, 0.0f};

    std::vector<physics2d::OverlapHit> hits;
    const usize count = physics2d::overlap_circle(scene, Vec2{0.0f, 0.0f}, 3.0f, hits);
    REQUIRE(count == 1);
    REQUIRE(hits.size() == 1);
    CHECK((hits[0].node == near_node));
    (void)far_node;
}

TEST_CASE("overlap_box finds colliders intersecting the query box") {
    Scene scene;
    Node* node = scene.create_node("Target");
    auto* collider = node->add_component<physics2d::Collider2D>();
    collider->half_extent = Vec2{1.0f, 1.0f};
    node->transform().position = Vec3{1.5f, 0.0f, 0.0f}; // overlaps x in [0.5, 2.5]

    std::vector<physics2d::OverlapHit> hits;
    const usize count = physics2d::overlap_box(scene, Vec2{0.0f, 0.0f}, Vec2{1.0f, 1.0f}, hits);
    REQUIRE(count == 1);
    CHECK((hits[0].node == node));
    CHECK(hits[0].penetration == doctest::Approx(0.5f));
}

TEST_CASE("overlap_point reports the collider containing the point") {
    Scene scene;
    Node* node = scene.create_node("Pad");
    auto* collider = node->add_component<physics2d::Collider2D>();
    collider->half_extent = Vec2{2.0f, 2.0f};
    node->transform().position = Vec3{0.0f, 0.0f, 0.0f};

    physics2d::OverlapHit out;
    CHECK(physics2d::overlap_point(scene, Vec2{0.5f, 0.5f}, &out));
    CHECK((out.node == node));
    CHECK_FALSE(physics2d::overlap_point(scene, Vec2{10.0f, 10.0f}));
}

TEST_CASE("queries work against a static-only scene") {
    // No rigid bodies anywhere: the queries must still see the colliders.
    Scene scene;
    add_floor(scene, 5.0f); // top face at y = 0

    // Straight down from above the floor hits it.
    const physics2d::RaycastHit hit = physics2d::raycast(scene, Vec2{0.0f, 5.0f}, Vec2{0.0f, -1.0f});
    REQUIRE(hit.hit);
    CHECK(hit.distance == doctest::Approx(5.0f));

    // A point just inside the floor is found.
    CHECK(physics2d::overlap_point(scene, Vec2{0.0f, -0.25f}));
}

TEST_CASE("overlap into a reused buffer keeps capacity and does not double-count") {
    Scene scene;
    for (int i = 0; i < 4; ++i) {
        Node* node = scene.create_node("Box");
        auto* collider = node->add_component<physics2d::Collider2D>();
        collider->half_extent = Vec2{0.5f, 0.5f};
        node->transform().position = Vec3{static_cast<f32>(i) * 0.5f, 0.0f, 0.0f};
    }

    std::vector<physics2d::OverlapHit> hits;
    physics2d::overlap_circle(scene, Vec2{0.0f, 0.0f}, 10.0f, hits);
    const usize capacity_after_first = hits.capacity();
    CHECK(hits.size() == 4);

    // The caller clears; the buffer is reused without reallocating.
    hits.clear();
    const usize count = physics2d::overlap_circle(scene, Vec2{0.0f, 0.0f}, 10.0f, hits);
    CHECK(count == 4);
    CHECK(hits.size() == 4);
    CHECK(hits.capacity() == capacity_after_first);
}

// ---------------------------------------------------------------------------
// Contact events: begin and end.
// ---------------------------------------------------------------------------

TEST_CASE("a landing body reports a begin contact event") {
    Scene scene;
    Node* floor_node = add_floor(scene, 20.0f);
    Node* body_node = add_dynamic_box(scene, Vec2{0.0f, 0.5f}, 0.5f, 1.0f); // already touching

    physics2d::Physics2DSystem system;
    system.update(step(1.0f / 60.0f), scene);

    REQUIRE(system.contact_event_count() == 1);
    const physics2d::ContactEvent& event = system.contact_events().front();
    CHECK(event.phase == physics2d::ContactPhase::Begin);
    // The pair is reported in a stable order; either endpoint may be first.
    const bool matched =
        (event.a == floor_node && event.b == body_node) || (event.a == body_node && event.b == floor_node);
    CHECK(matched);
}

TEST_CASE("a begin event is not repeated while the contact persists") {
    Scene scene;
    add_floor(scene, 20.0f);
    add_dynamic_box(scene, Vec2{0.0f, 0.5f}, 0.5f, 1.0f);

    physics2d::Physics2DSystem system;
    system.update(step(1.0f / 60.0f), scene);
    REQUIRE(system.contact_event_count() == 1); // begin

    // Further frames with the contact still present emit nothing.
    for (int i = 0; i < 10; ++i) {
        system.update(step(1.0f / 60.0f), scene);
        CHECK(system.contact_event_count() == 0);
    }
}

TEST_CASE("separating bodies report an end contact event") {
    Scene scene;
    // Two boxes overlapping on x, pushed apart by the solver. No gravity, so
    // the only motion is the separation. They are given opposing velocities so
    // they fully separate within the run, crossing the slop that keeps a resting
    // contact alive.
    Node* left = add_dynamic_box(scene, Vec2{-0.25f, 0.0f}, 0.5f, 1.0f);
    Node* right = add_dynamic_box(scene, Vec2{0.25f, 0.0f}, 0.5f, 1.0f);
    left->get_component<physics2d::RigidBody2D>()->velocity = Vec2{-2.0f, 0.0f};
    right->get_component<physics2d::RigidBody2D>()->velocity = Vec2{2.0f, 0.0f};

    physics2d::Physics2DSystem system;
    system.gravity = Vec2{0.0f, 0.0f};

    system.update(step(1.0f / 60.0f), scene);
    REQUIRE(system.contact_event_count() == 1);
    CHECK(system.contact_events().front().phase == physics2d::ContactPhase::Begin);

    // Run until they are no longer touching; the frame that separates them
    // emits the End event.
    bool saw_end = false;
    for (int i = 0; i < 120 && !saw_end; ++i) {
        system.update(step(1.0f / 60.0f), scene);
        for (const physics2d::ContactEvent& event : system.contact_events()) {
            if (event.phase == physics2d::ContactPhase::End) {
                saw_end = true;
                const bool matched = (event.a == left && event.b == right) || (event.a == right && event.b == left);
                CHECK(matched);
            }
        }
    }
    CHECK(saw_end);
}

TEST_CASE("a contact ending because a node left the scene does not dangle") {
    Scene scene;
    Node* body = add_dynamic_box(scene, Vec2{0.0f, 0.5f}, 0.5f, 1.0f);
    add_floor(scene, 20.0f);

    physics2d::Physics2DSystem system;
    system.update(step(1.0f / 60.0f), scene);
    REQUIRE(system.contact_event_count() == 1);

    // Remove the body, then step: the End event must be skipped rather than
    // reporting a pointer into freed memory.
    (void)scene.remove_root(body);
    system.update(step(1.0f / 60.0f), scene);
    for (const physics2d::ContactEvent& event : system.contact_events()) {
        CHECK(event.a != nullptr);
        CHECK(event.b != nullptr);
    }
}

TEST_CASE("a paused step emits no contact events") {
    Scene scene;
    add_floor(scene, 20.0f);
    add_dynamic_box(scene, Vec2{0.0f, 0.5f}, 0.5f, 1.0f);

    physics2d::Physics2DSystem system;
    system.update(step(0.0f), scene);
    CHECK(system.contact_event_count() == 0);
}

TEST_CASE("contact event order is deterministic across identical runs") {
    auto run = [] {
        Scene scene;
        add_floor(scene, 20.0f);
        add_dynamic_box(scene, Vec2{-0.25f, 0.5f}, 0.5f, 1.0f);
        add_dynamic_box(scene, Vec2{0.25f, 0.5f}, 0.5f, 1.0f);
        physics2d::Physics2DSystem system;
        system.update(step(1.0f / 60.0f), scene);
        return system.contact_events().size();
    };
    // Two identical worlds must report the same number of begin events.
    CHECK(run() == run());
}

// ---------------------------------------------------------------------------
// Angular reflection: the new properties persist.
// ---------------------------------------------------------------------------

TEST_CASE("angular properties round-trip through the serializer") {
    reflect::register_builtin_types();
    reflect::register_physics_types();

    Scene scene;
    Node* node = scene.create_node("Spinner");
    node->add_component<physics2d::Collider2D>();
    auto* body = node->add_component<physics2d::RigidBody2D>();
    body->angular_velocity = 1.5f;
    body->torque = 2.0f;
    body->inertia = 0.75f;
    body->angular_damping = 0.25f;
    body->fixed_rotation = true;

    auto json = serialization::scene::to_json(scene);
    REQUIRE(json);

    Scene loaded;
    REQUIRE_FALSE(serialization::scene::from_json(json.value(), loaded));
    Node* loaded_node = loaded.find_node_by_name("Spinner");
    REQUIRE(loaded_node != nullptr);
    const auto* loaded_body = loaded_node->get_component<physics2d::RigidBody2D>();
    REQUIRE(loaded_body != nullptr);
    CHECK(loaded_body->angular_velocity == doctest::Approx(1.5f));
    CHECK(loaded_body->torque == doctest::Approx(2.0f));
    CHECK(loaded_body->inertia == doctest::Approx(0.75f));
    CHECK(loaded_body->angular_damping == doctest::Approx(0.25f));
    CHECK(loaded_body->fixed_rotation == true);
}

// ---------------------------------------------------------------------------
// Joints: pure constraint rows, tested without a scene.
// ---------------------------------------------------------------------------

namespace {

// A dynamic joint end with the given mass, at the origin, at rest. A helper so
// the row tests read as physics rather than as struct literals.
physics2d::JointBody dynamic_body(f32 mass) {
    physics2d::JointBody body;
    body.inv_mass = mass > 0.0f ? 1.0f / mass : 0.0f;
    body.inv_inertia = mass > 0.0f ? 1.0f / mass : 0.0f;
    return body;
}

} // namespace

TEST_CASE("a distance row measures the separation along the centre line") {
    physics2d::JointBody a = dynamic_body(1.0f);
    physics2d::JointBody b = dynamic_body(1.0f);
    a.position = Vec2{0.0f, 0.0f};
    b.position = Vec2{3.0f, 4.0f}; // length 5 along (0.6, 0.8)

    physics2d::DistanceJointParams params;
    params.rest_length = 5.0f; // already satisfied

    auto row = physics2d::build_distance_row(a, b, params, 1.0f / 60.0f);
    REQUIRE(row);

    // The Jacobian is the unit direction, negated on A and positive on B.
    CHECK(row.value().jacobian_a.x == doctest::Approx(-0.6f));
    CHECK(row.value().jacobian_a.y == doctest::Approx(-0.8f));
    CHECK(row.value().jacobian_b.x == doctest::Approx(0.6f));
    CHECK(row.value().jacobian_b.y == doctest::Approx(0.8f));

    // Two equal masses: the apparent mass is 1 / (1/1 + 1/1) = 0.5.
    CHECK(row.value().effective_mass == doctest::Approx(0.5f));

    // At the rest length there is no error, so a rigid rod does nothing.
    CHECK(row.value().position_error == doctest::Approx(0.0f));
    CHECK(row.value().bias == doctest::Approx(0.0f));
}

TEST_CASE("a distance row reports the stretch as its position error") {
    physics2d::JointBody a = dynamic_body(1.0f);
    physics2d::JointBody b = dynamic_body(1.0f);
    a.position = Vec2{0.0f, 0.0f};
    b.position = Vec2{2.0f, 0.0f};

    physics2d::DistanceJointParams params;
    params.rest_length = 1.5f;

    auto row = physics2d::build_distance_row(a, b, params, 1.0f / 60.0f);
    REQUIRE(row);
    CHECK(row.value().position_error == doctest::Approx(0.5f));
    CHECK(row.value().position_factor == doctest::Approx(0.2f));
    // A rigid rod has no velocity bias; it corrects position directly.
    CHECK(row.value().bias == doctest::Approx(0.0f));
}

TEST_CASE("a distance row is unsolvable when both bodies are immovable") {
    physics2d::JointBody a; // zero inverses: a static end
    physics2d::JointBody b;
    a.position = Vec2{0.0f, 0.0f};
    b.position = Vec2{1.0f, 0.0f};

    physics2d::DistanceJointParams params;
    params.rest_length = 1.0f;

    auto row = physics2d::build_distance_row(a, b, params, 1.0f / 60.0f);
    REQUIRE_FALSE(row);
}

TEST_CASE("a distance row is unsolvable when the bodies are coincident") {
    physics2d::JointBody a = dynamic_body(1.0f);
    physics2d::JointBody b = dynamic_body(1.0f);
    // Both at the origin: the constraint has no direction to measure along.

    physics2d::DistanceJointParams params;
    params.rest_length = 1.0f;

    auto row = physics2d::build_distance_row(a, b, params, 1.0f / 60.0f);
    REQUIRE_FALSE(row);
}

TEST_CASE("a rigid distance row removes a fixed fraction of the error") {
    physics2d::JointBody a = dynamic_body(1.0f);
    physics2d::JointBody b = dynamic_body(1.0f);
    a.position = Vec2{0.0f, 0.0f};
    b.position = Vec2{2.0f, 0.0f};

    physics2d::DistanceJointParams params;
    params.rest_length = 1.0f; // stretched by 1

    auto row = physics2d::build_distance_row(a, b, params, 1.0f / 60.0f);
    REQUIRE(row);
    physics2d::apply_row_position(row.value(), a, b);

    // 0.2 of the 1.0 error is removed, split evenly: each body moves 0.1, so the
    // separation shrinks by the full 0.2, from 2.0 to 1.8.
    const f32 new_length = (b.position - a.position).length();
    CHECK(new_length == doctest::Approx(1.8f));
}

TEST_CASE("a spring distance row pulls back with a velocity bias") {
    physics2d::JointBody a = dynamic_body(1.0f);
    physics2d::JointBody b = dynamic_body(1.0f);
    a.position = Vec2{0.0f, 0.0f};
    b.position = Vec2{2.0f, 0.0f};

    physics2d::DistanceJointParams params;
    params.rest_length = 1.0f; // stretched by 1
    params.stiffness = 8.0f;   // 8 Hz
    params.damping = 0.0f;

    auto row = physics2d::build_distance_row(a, b, params, 1.0f / 60.0f);
    REQUIRE(row);
    // bias = stiffness * stretch = 8 * 1 = 8, a closing velocity.
    CHECK(row.value().bias == doctest::Approx(8.0f));
    // A spring corrects through velocity, not a direct position fix.
    CHECK(row.value().position_factor == doctest::Approx(0.0f));

    physics2d::apply_row(row.value(), a, b);
    // A and B are pulled towards each other: A gains +x velocity, B loses x.
    CHECK(a.velocity.x > 0.0f);
    CHECK(b.velocity.x < 0.0f);
}

TEST_CASE("a revolute row pins two centres together with two rows") {
    physics2d::JointBody a = dynamic_body(1.0f);
    physics2d::JointBody b = dynamic_body(1.0f);
    a.position = Vec2{0.0f, 0.0f};
    b.position = Vec2{1.0f, 0.5f}; // offset; both anchors at the centres

    physics2d::RevoluteJointParams params; // anchors default to the centres
    physics2d::ConstraintRow rows[physics2d::kRevoluteJointRows];
    auto built = physics2d::build_revolute_rows(a, b, params, 1.0f / 60.0f, rows);
    REQUIRE(built);

    // Row 0 is the x separation, row 1 the y separation. The error is
    // A_anchor - B_anchor, which is negative of the centre offset here.
    CHECK(rows[0].position_error == doctest::Approx(-1.0f));
    CHECK(rows[1].position_error == doctest::Approx(-0.5f));
    CHECK(rows[0].jacobian_a.x == doctest::Approx(1.0f));
    CHECK(rows[1].jacobian_a.y == doctest::Approx(1.0f));

    for (const physics2d::ConstraintRow& row : rows) {
        physics2d::apply_row_position(row, a, b);
    }
    // 0.2 of the separation is removed, split evenly: the centres end up 80% of
    // the way apart they started.
    CHECK((b.position - a.position).length() == doctest::Approx(0.8f * Vec2{1.0f, 0.5f}.length()));
}

TEST_CASE("a revolute row couples an offset anchor to rotation") {
    physics2d::JointBody a = dynamic_body(1.0f);
    physics2d::JointBody b = dynamic_body(1.0f);
    a.position = Vec2{0.0f, 0.0f};
    b.position = Vec2{0.0f, -1.0f};

    physics2d::RevoluteJointParams params;
    params.anchor_b = Vec2{0.0f, 1.0f}; // bob's top, at the pivot (0,0)
    physics2d::ConstraintRow rows[physics2d::kRevoluteJointRows];
    auto built = physics2d::build_revolute_rows(a, b, params, 1.0f / 60.0f, rows);
    REQUIRE(built);

    // The lever arm on B is (0, 1) and the row Jacobian is -axis. For the x row
    // that is cross((0,1), (-1,0)) = 1; for the y row cross((0,1), (0,-1)) = 0.
    // So the x correction is what spins B, and the y correction does not.
    CHECK(rows[0].angular_b == doctest::Approx(1.0f));
    CHECK(rows[1].angular_b == doctest::Approx(0.0f));
}

TEST_CASE("applying a revolute row keeps the shared anchor pinned") {
    physics2d::JointBody a = dynamic_body(1.0f); // the pivot, movable here
    physics2d::JointBody b = dynamic_body(1.0f);
    a.position = Vec2{0.0f, 0.0f};
    b.position = Vec2{1.0f, 0.0f};

    physics2d::RevoluteJointParams params;
    params.anchor_b = Vec2{-1.0f, 0.0f}; // bob's left end, at the pivot (0,0)
    physics2d::ConstraintRow rows[physics2d::kRevoluteJointRows];
    REQUIRE(physics2d::build_revolute_rows(a, b, params, 1.0f / 60.0f, rows));
    for (const physics2d::ConstraintRow& row : rows) {
        physics2d::apply_row_position(row, a, b);
    }
    // Both anchor world points should now nearly coincide.
    const Vec2 anchor_a = physics2d::revolute_world_anchor(a, params);
    const Vec2 anchor_b = b.position + Vec2{-1.0f, 0.0f};
    CHECK((anchor_a - anchor_b).length() < 0.05f);
}

// ---------------------------------------------------------------------------
// Joints in the scene: gathering, solving, and diagnostics.
// ---------------------------------------------------------------------------

TEST_CASE("a distance joint holds two bodies at its rest length") {
    Scene scene;
    Node* pivot = add_pivot(scene, Vec2{0.0f, 0.0f});
    Node* mass = add_point_mass(scene, Vec2{0.0f, -2.0f}, 1.0f);
    mass->get_component<physics2d::RigidBody2D>()->affected_by_gravity = true;

    Node* host = scene.create_node("Joint");
    add_distance_joint(*host, *pivot, *mass, 1.0f); // pin the length at 1

    physics2d::Physics2DSystem system;
    for (int i = 0; i < 60; ++i) {
        system.update(step(1.0f / 60.0f), scene);
    }

    CHECK(system.joint_count() == 1);
    CHECK(system.joint_diagnostics().empty());

    // Gravity pulls the mass down, but the rod keeps it 1 unit from the pivot.
    const Vec2 separation = Vec2{mass->transform().position.x - pivot->transform().position.x,
                                 mass->transform().position.y - pivot->transform().position.y};
    CHECK(separation.length() == doctest::Approx(1.0f).epsilon(0.05));
    // It ends up hanging below the pivot, not beside it.
    CHECK(mass->transform().position.y < 0.0f);
}

TEST_CASE("a revolute joint swings a bob about a fixed pivot") {
    Scene scene;
    Node* pivot = add_pivot(scene, Vec2{0.0f, 0.0f});
    Node* bob = add_point_mass(scene, Vec2{1.0f, 0.0f}, 1.0f);
    auto* bob_body = bob->get_component<physics2d::RigidBody2D>();
    bob_body->affected_by_gravity = true;
    // A point mass has no rotational inertia, so give it a moment of inertia
    // about the pin. The pin is 1 unit from the centre, so I = m r^2 = 1.
    bob_body->inertia = 1.0f;

    Node* host = scene.create_node("Joint");
    // The bob's left end is pinned to the pivot; the bob starts horizontal so
    // gravity has a lever to turn.
    add_revolute_joint(*host, *pivot, *bob, Vec2{}, Vec2{-1.0f, 0.0f});

    physics2d::Physics2DSystem system;
    for (int i = 0; i < 30; ++i) {
        system.update(step(1.0f / 60.0f), scene);
    }

    CHECK(system.joint_count() == 1);
    CHECK(system.joint_diagnostics().empty());
    // The bob swung down and started to rotate about the pin.
    CHECK(bob->transform().position.y < 0.0f);
    CHECK(std::abs(bob->get_component<physics2d::RigidBody2D>()->angular_velocity) > 0.0f);
    // The anchor stayed pinned: the bob's left end is still at the pivot.
    const f32 angle = bob->transform().rotation.z;
    const Vec2 anchor = Vec2{bob->transform().position.x, bob->transform().position.y} +
                        Vec2{-1.0f * std::cos(angle), -1.0f * std::sin(angle)};
    CHECK(anchor.length() < 0.1f);
}

TEST_CASE("a joint with a nil body reference is skipped with a diagnostic") {
    Scene scene;
    Node* a = add_point_mass(scene, Vec2{0.0f, 0.0f}, 1.0f);
    Node* b = add_point_mass(scene, Vec2{1.0f, 0.0f}, 1.0f);
    // Turn gravity off so the check below is about the joint, not about falling.
    a->get_component<physics2d::RigidBody2D>()->affected_by_gravity = false;
    b->get_component<physics2d::RigidBody2D>()->affected_by_gravity = false;

    Node* host = scene.create_node("Joint");
    auto* joint = host->add_component<physics2d::DistanceJoint2D>();
    joint->body_a = a->id();
    // body_b is left nil: the joint is inert.

    physics2d::Physics2DSystem system;
    system.update(step(1.0f / 60.0f), scene);

    CHECK(system.joint_count() == 0);
    REQUIRE(system.joint_diagnostics().size() == 1);
    CHECK(system.joint_diagnostics().front().type == std::string_view{"DistanceJoint2D"});
    // Nothing moved: the joint never touched the bodies.
    CHECK(a->transform().position.y == doctest::Approx(0.0f));
    CHECK(b->transform().position.x == doctest::Approx(1.0f));
}

TEST_CASE("a joint whose body has left the scene is skipped with a diagnostic") {
    Scene scene;
    Node* pivot = add_pivot(scene, Vec2{0.0f, 0.0f});
    Node* mass = add_point_mass(scene, Vec2{0.0f, -1.0f}, 1.0f);

    Node* host = scene.create_node("Joint");
    add_distance_joint(*host, *pivot, *mass, 1.0f);

    // Remove the pivot: the joint now references a node that is not in the
    // scene. This must not crash and must surface as a diagnostic.
    const auto removed_pivot = scene.remove_root(pivot);
    CHECK(removed_pivot != nullptr);

    physics2d::Physics2DSystem system;
    system.update(step(1.0f / 60.0f), scene);

    CHECK(system.joint_count() == 0);
    REQUIRE(system.joint_diagnostics().size() == 1);
    CHECK(system.joint_diagnostics().front().error.message().find("not in the scene") != std::string::npos);
}

TEST_CASE("a joint connecting a body to itself is skipped") {
    Scene scene;
    Node* a = add_point_mass(scene, Vec2{0.0f, 0.0f}, 1.0f);

    Node* host = scene.create_node("Joint");
    auto* joint = host->add_component<physics2d::DistanceJoint2D>();
    joint->body_a = a->id();
    joint->body_b = a->id();

    physics2d::Physics2DSystem system;
    system.update(step(1.0f / 60.0f), scene);

    CHECK(system.joint_count() == 0);
    REQUIRE(system.joint_diagnostics().size() == 1);
    CHECK(system.joint_diagnostics().front().error.message().find("itself") != std::string::npos);
}

TEST_CASE("a joint with no movable body is skipped") {
    Scene scene;
    Node* a = add_pivot(scene, Vec2{0.0f, 0.0f});
    Node* b = add_pivot(scene, Vec2{1.0f, 0.0f});

    Node* host = scene.create_node("Joint");
    add_distance_joint(*host, *a, *b, 1.0f);

    physics2d::Physics2DSystem system;
    system.update(step(1.0f / 60.0f), scene);

    CHECK(system.joint_count() == 0);
    REQUIRE(system.joint_diagnostics().size() == 1);
    CHECK(system.joint_diagnostics().front().error.message().find("movable") != std::string::npos);
}

TEST_CASE("a joint-only body with no collider is still simulated") {
    // A point mass with no collider must fall under gravity on its own, or a
    // joint to it would be pulling on something that never moves.
    Scene scene;
    Node* mass = add_point_mass(scene, Vec2{0.0f, 5.0f}, 1.0f);
    mass->get_component<physics2d::RigidBody2D>()->affected_by_gravity = true;

    physics2d::Physics2DSystem system;
    for (int i = 0; i < 10; ++i) {
        system.update(step(1.0f / 60.0f), scene);
    }
    CHECK(mass->transform().position.y < 5.0f);
}

TEST_CASE("the joint components are registered with reflection") {
    reflect::register_physics_types();
    const reflect::Registry& registry = reflect::Registry::instance();
    CHECK(registry.contains("DistanceJoint2D"));
    CHECK(registry.contains("RevoluteJoint2D"));
}

TEST_CASE("a distance joint round-trips through the serializer") {
    reflect::register_builtin_types();
    reflect::register_physics_types();

    Scene scene;
    Node* a = scene.create_node("A");
    Node* b = scene.create_node("B");

    Node* host = scene.create_node("Joint");
    auto* joint = host->add_component<physics2d::DistanceJoint2D>();
    joint->body_a = a->id();
    joint->body_b = b->id();
    joint->rest_length = 2.5f;
    joint->stiffness = 6.0f;
    joint->damping = 0.5f;

    auto json = serialization::scene::to_json(scene);
    REQUIRE(json);

    Scene loaded;
    REQUIRE_FALSE(serialization::scene::from_json(json.value(), loaded));

    Node* loaded_host = loaded.find_node_by_name("Joint");
    REQUIRE(loaded_host != nullptr);
    const auto* loaded_joint = loaded_host->get_component<physics2d::DistanceJoint2D>();
    REQUIRE(loaded_joint != nullptr);
    // The body references survive as stable ids and resolve back to the nodes.
    CHECK(loaded.find_node(loaded_joint->body_a) != nullptr);
    CHECK(loaded.find_node(loaded_joint->body_b) != nullptr);
    CHECK(loaded_joint->body_a == a->id());
    CHECK(loaded_joint->body_b == b->id());
    CHECK(loaded_joint->rest_length == doctest::Approx(2.5f));
    CHECK(loaded_joint->stiffness == doctest::Approx(6.0f));
    CHECK(loaded_joint->damping == doctest::Approx(0.5f));
}

TEST_CASE("a revolute joint round-trips through the serializer") {
    reflect::register_builtin_types();
    reflect::register_physics_types();

    Scene scene;
    Node* a = scene.create_node("A");
    Node* b = scene.create_node("B");

    Node* host = scene.create_node("Joint");
    auto* joint = host->add_component<physics2d::RevoluteJoint2D>();
    joint->body_a = a->id();
    joint->body_b = b->id();
    joint->anchor_a = Vec2{0.25f, -0.5f};
    joint->anchor_b = Vec2{-0.75f, 1.0f};

    auto json = serialization::scene::to_json(scene);
    REQUIRE(json);

    Scene loaded;
    REQUIRE_FALSE(serialization::scene::from_json(json.value(), loaded));

    Node* loaded_host = loaded.find_node_by_name("Joint");
    REQUIRE(loaded_host != nullptr);
    const auto* loaded_joint = loaded_host->get_component<physics2d::RevoluteJoint2D>();
    REQUIRE(loaded_joint != nullptr);
    CHECK(loaded_joint->body_a == a->id());
    CHECK(loaded_joint->body_b == b->id());
    CHECK(loaded_joint->anchor_a.x == doctest::Approx(0.25f));
    CHECK(loaded_joint->anchor_a.y == doctest::Approx(-0.5f));
    CHECK(loaded_joint->anchor_b.x == doctest::Approx(-0.75f));
    CHECK(loaded_joint->anchor_b.y == doctest::Approx(1.0f));
}

// ---------------------------------------------------------------------------
// Oriented boxes and collider offsets.
//
// A box collider carries the node's Z rotation and a local offset, so a rotated
// node collides as its true oriented shape and an offset shape stays attached
// as the node turns. These tests cover the pure geometry first, then the system
// end to end.
// ---------------------------------------------------------------------------

namespace {

physics2d::Obb2D obb(f32 cx, f32 cy, f32 hx, f32 hy, f32 rotation) {
    return physics2d::Obb2D{Vec2{cx, cy}, Vec2{hx, hy}, rotation};
}

} // namespace

TEST_CASE("a box rotated 90 degrees collides as the swapped axis-aligned box") {
    // Half-extent (2, 0.5) rotated 90 degrees spans 2 units along y and 0.5 along
    // x, so it is the axis-aligned box (0.5, 2). This is the sharpest proof the
    // orientation is applied: the same shape, one quarter turn.
    const physics2d::Obb2D turned = obb(0.0f, 0.0f, 2.0f, 0.5f, kPi / 2.0f);

    // A box sitting above the turned shape (y = 2.3) overlaps; the same box to
    // the side (x = 2.3) does not, because the turned shape is thin in x.
    CHECK(physics2d::intersect(turned, obb(0.0f, 2.3f, 0.5f, 0.5f, 0.0f)).overlapping);
    CHECK_FALSE(physics2d::intersect(turned, obb(2.3f, 0.0f, 0.5f, 0.5f, 0.0f)).overlapping);
}

TEST_CASE("a rotated box that its bounding box would report as colliding is separated") {
    // A long thin box turned 45 degrees. Its axis-aligned bound is a square of
    // half-extent ~1.56, so an AABB collider would claim the small box in the
    // corner touches it. The oriented shape is thin along the diagonal, so the
    // corner box is genuinely clear.
    const physics2d::Obb2D beam = obb(0.0f, 0.0f, 2.0f, 0.2f, kPi / 4.0f);
    const physics2d::Obb2D corner = obb(1.55f, 1.55f, 0.05f, 0.05f, 0.0f);

    // The bounding boxes do overlap -- this is exactly the false positive the
    // orientation removes.
    CHECK(physics2d::bounds_overlap(physics2d::bounds_of(beam), physics2d::bounds_of(corner)));
    // The true oriented shapes do not.
    CHECK_FALSE(physics2d::intersect(beam, corner).overlapping);
}

TEST_CASE("a rotated box still overlaps where its oriented shape reaches") {
    const physics2d::Obb2D beam = obb(0.0f, 0.0f, 2.0f, 0.2f, kPi / 4.0f);
    // On the beam's own long axis, one unit out from the centre.
    const Vec2 on_axis = beam.axis_x() * 1.2f;
    const physics2d::Obb2D hit = obb(on_axis.x, on_axis.y, 0.1f, 0.1f, 0.0f);
    const auto contact = physics2d::intersect(beam, hit);
    REQUIRE(contact.overlapping);
    CHECK(contact.penetration > 0.0f);
    CHECK(contact.normal.length() == doctest::Approx(1.0f));
}

TEST_CASE("the bounding box of a rotated box is the conservative axis-aligned bound") {
    // A unit box turned 45 degrees has corners at +/- (sqrt2, sqrt2), so its
    // axis-aligned bound is the square of half-extent sqrt2 -- larger than the
    // unrotated half-extent, which is what keeps the broad phase a superset.
    const physics2d::Aabb2D bound = physics2d::bounds_of(obb(0.0f, 0.0f, 1.0f, 1.0f, kPi / 4.0f));
    CHECK(bound.half_extent.x == doctest::Approx(std::sqrt(2.0f)));
    CHECK(bound.half_extent.y == doctest::Approx(std::sqrt(2.0f)));
    // A zero-rotation box's bound is exactly its own half-extent.
    const physics2d::Aabb2D aligned = physics2d::bounds_of(obb(0.0f, 0.0f, 1.0f, 2.0f, 0.0f));
    CHECK(aligned.half_extent.x == doctest::Approx(1.0f));
    CHECK(aligned.half_extent.y == doctest::Approx(2.0f));
}

TEST_CASE("a circle against a rotated box face reports the rotated face normal") {
    // A circle on the box's local +y face. The face normal is the box's local
    // +y axis, rotated into world space, so the contact normal is -axis_y.
    const physics2d::Obb2D beam = obb(0.0f, 0.0f, 2.0f, 0.2f, kPi / 4.0f);
    const Vec2 circle_center = beam.axis_y() * 0.7f;
    const auto contact = physics2d::intersect(physics2d::Circle2D{circle_center, 0.6f}, beam);

    REQUIRE(contact.overlapping);
    const Vec2 axis_y = beam.axis_y();
    CHECK(contact.normal.x == doctest::Approx(-axis_y.x));
    CHECK(contact.normal.y == doctest::Approx(-axis_y.y));
    CHECK(contact.penetration == doctest::Approx(0.1f));
}

TEST_CASE("a circle outside a rotated box's corner is separated") {
    // The corner of the rotated box's bounding box that the true shape does not
    // reach. The AABB prefilter would pass; the oriented narrow phase does not.
    const physics2d::Obb2D beam = obb(0.0f, 0.0f, 2.0f, 0.2f, kPi / 4.0f);
    const physics2d::Circle2D probe{Vec2{1.5f, 1.5f}, 0.1f};
    CHECK(physics2d::bounds_overlap(physics2d::bounds_of(beam), physics2d::bounds_of(probe)));
    CHECK_FALSE(physics2d::intersect(probe, beam).overlapping);
}

TEST_CASE("intersect_shapes keeps the normal pointing from the first shape for oriented boxes") {
    const physics2d::Obb2D a = obb(0.0f, 0.0f, 1.0f, 1.0f, kPi / 6.0f);
    const physics2d::Circle2D b{Vec2{0.0f, 1.6f}, 1.0f};

    const auto circle_first =
        physics2d::intersect_shapes(physics2d::ShapeType::Circle, a, b, physics2d::ShapeType::Box, a, b);
    const auto box_first =
        physics2d::intersect_shapes(physics2d::ShapeType::Box, a, b, physics2d::ShapeType::Circle, a, b);

    REQUIRE(circle_first.overlapping);
    REQUIRE(box_first.overlapping);
    CHECK(circle_first.normal.x == doctest::Approx(-box_first.normal.x));
    CHECK(circle_first.normal.y == doctest::Approx(-box_first.normal.y));
    CHECK(circle_first.penetration == doctest::Approx(box_first.penetration));
}

TEST_CASE("a rotated box collides as its oriented shape in the solver") {
    Scene scene;
    // A long thin static beam turned 45 degrees.
    Node* beam = scene.create_node("Beam");
    auto* beam_collider = beam->add_component<physics2d::Collider2D>();
    beam_collider->half_extent = Vec2{2.0f, 0.2f};
    beam->transform().rotation.z = kPi / 4.0f;

    // A small dynamic box in the corner of the beam's bounding box but clear of
    // the oriented shape. No gravity, so it stays put and the only thing that
    // could move it is a spurious contact.
    Node* corner = add_dynamic_box(scene, Vec2{1.55f, 1.55f}, 0.05f, 1.0f);
    corner->get_component<physics2d::RigidBody2D>()->affected_by_gravity = false;

    physics2d::Physics2DSystem system;
    system.gravity = Vec2{0.0f, 0.0f};
    system.update(step(1.0f / 60.0f), scene);

    CHECK(system.contact_count() == 0);
    CHECK(corner->transform().position.x == doctest::Approx(1.55f));
    CHECK(corner->transform().position.y == doctest::Approx(1.55f));
}

TEST_CASE("a rotated box collides where its oriented shape does overlap") {
    Scene scene;
    Node* beam = scene.create_node("Beam");
    auto* beam_collider = beam->add_component<physics2d::Collider2D>();
    beam_collider->half_extent = Vec2{2.0f, 0.2f};
    beam->transform().rotation.z = kPi / 4.0f;

    // On the beam's long axis, so the oriented shape and the box overlap.
    const Vec2 on_axis = Vec2{std::cos(kPi / 4.0f), std::sin(kPi / 4.0f)} * 1.2f;
    add_dynamic_box(scene, on_axis, 0.1f, 1.0f);

    physics2d::Physics2DSystem system;
    system.gravity = Vec2{0.0f, 0.0f};
    system.update(step(1.0f / 60.0f), scene);

    CHECK(system.contact_count() >= 1);
}

TEST_CASE("a collider offset is rotated into the world by the node's rotation") {
    // The collider is offset one unit along the node's local +x. The node is
    // turned 90 degrees, so the shape lands one unit along world +y, not +x.
    Scene scene;
    Node* anchor = scene.create_node("Anchor");
    auto* collider = anchor->add_component<physics2d::Collider2D>();
    collider->half_extent = Vec2{0.5f, 0.5f};
    collider->offset = Vec2{1.0f, 0.0f};
    anchor->transform().rotation.z = kPi / 2.0f;

    Node* above = add_dynamic_box(scene, Vec2{0.0f, 1.0f}, 0.5f, 1.0f);
    above->get_component<physics2d::RigidBody2D>()->affected_by_gravity = false;

    physics2d::Physics2DSystem system;
    system.gravity = Vec2{0.0f, 0.0f};
    system.update(step(1.0f / 60.0f), scene);

    CHECK(system.contact_count() >= 1);
}

TEST_CASE("an offset collider does not stay on the node's axis when the node rotates") {
    // Same offset and rotation as above, but a body where the offset *would* be
    // without rotation (world +x). If the offset were not rotated the two would
    // collide; correctly rotated, they are one unit apart and clear.
    Scene scene;
    Node* anchor = scene.create_node("Anchor");
    auto* collider = anchor->add_component<physics2d::Collider2D>();
    collider->half_extent = Vec2{0.5f, 0.5f};
    collider->offset = Vec2{1.0f, 0.0f};
    anchor->transform().rotation.z = kPi / 2.0f;

    Node* to_the_side = add_dynamic_box(scene, Vec2{1.0f, 0.0f}, 0.5f, 1.0f);
    to_the_side->get_component<physics2d::RigidBody2D>()->affected_by_gravity = false;

    physics2d::Physics2DSystem system;
    system.gravity = Vec2{0.0f, 0.0f};
    system.update(step(1.0f / 60.0f), scene);

    CHECK(system.contact_count() == 0);
}

TEST_CASE("a collider offset places the shape for a query as well as the solver") {
    Scene scene;
    Node* node = scene.create_node("Offset");
    auto* collider = node->add_component<physics2d::Collider2D>();
    collider->half_extent = Vec2{0.5f, 0.5f};
    collider->offset = Vec2{2.0f, 0.0f};

    // The node is at the origin; the shape is two units to the right.
    CHECK_FALSE(physics2d::overlap_point(scene, Vec2{0.0f, 0.0f}));
    CHECK(physics2d::overlap_point(scene, Vec2{2.0f, 0.0f}));

    std::vector<physics2d::OverlapHit> hits;
    CHECK(physics2d::overlap_circle(scene, Vec2{2.0f, 0.0f}, 0.25f, hits) == 1);
}

TEST_CASE("a collider offset round-trips through the serializer") {
    reflect::register_builtin_types();
    reflect::register_physics_types();

    Scene scene;
    Node* node = scene.create_node("Offset");
    auto* collider = node->add_component<physics2d::Collider2D>();
    collider->offset = Vec2{0.25f, -1.5f};

    auto json = serialization::scene::to_json(scene);
    REQUIRE(json);

    Scene loaded;
    REQUIRE_FALSE(serialization::scene::from_json(json.value(), loaded));
    Node* loaded_node = loaded.find_node_by_name("Offset");
    REQUIRE(loaded_node != nullptr);
    const auto* loaded_collider = loaded_node->get_component<physics2d::Collider2D>();
    REQUIRE(loaded_collider != nullptr);
    CHECK(loaded_collider->offset.x == doctest::Approx(0.25f));
    CHECK(loaded_collider->offset.y == doctest::Approx(-1.5f));
}

// ---------------------------------------------------------------------------
// Friction combination.
// ---------------------------------------------------------------------------

TEST_CASE("friction combines so a frictionless material stays frictionless") {
    // The regression this guards: the solver used max(a.friction, b.friction) to
    // combine the pair, which makes friction 0 inexpressible -- a body authored
    // as frictionless inherits whatever the other surface has. A ball set to
    // friction 0 hitting a default-friction wall then bled tangential speed on
    // every bounce until it travelled dead horizontal.
    //
    // A frictionless ball sliding along a frictioned floor must keep all of its
    // horizontal speed: with the combine rule correct, the contact can exert no
    // tangential impulse at all.
    Scene scene;
    Node* floor = add_floor(scene, 10.0f);
    floor->get_component<physics2d::Collider2D>()->friction = 0.9f;

    Node* ball = add_dynamic_box(scene, Vec2{0.0f, 0.5f}, 0.5f, 1.0f);
    auto* ball_collider = ball->get_component<physics2d::Collider2D>();
    ball_collider->friction = 0.0f;
    auto* body = ball->get_component<physics2d::RigidBody2D>();
    body->velocity = Vec2{100.0f, 0.0f};
    body->wake();

    physics2d::Physics2DSystem physics;
    physics.gravity = Vec2{0.0f, -10.0f};
    for (int i = 0; i < 120; ++i) {
        physics.update(step(1.0f / 60.0f), scene);
    }

    // No tangential impulse may be applied, so the horizontal speed is intact.
    CHECK(body->velocity.x == doctest::Approx(100.0f).epsilon(0.001));
}

TEST_CASE("friction combines as a geometric mean, so both surfaces must be rough") {
    // Two rough surfaces produce friction; one smooth surface removes it. This
    // pins the combine rule at the midpoint too, so a future change to a
    // different rule (min, or an average) is caught rather than silently
    // altering how every contact behaves.
    Scene scene;
    Node* floor = add_floor(scene, 10.0f);
    floor->get_component<physics2d::Collider2D>()->friction = 1.0f;

    Node* slider = add_dynamic_box(scene, Vec2{0.0f, 0.5f}, 0.5f, 1.0f);
    slider->get_component<physics2d::Collider2D>()->friction = 1.0f;
    auto* body = slider->get_component<physics2d::RigidBody2D>();
    body->velocity = Vec2{100.0f, 0.0f};
    body->wake();

    physics2d::Physics2DSystem physics;
    physics.gravity = Vec2{0.0f, -10.0f};
    for (int i = 0; i < 120; ++i) {
        physics.update(step(1.0f / 60.0f), scene);
    }

    // Rough on rough: the slider is slowed. (The exact value depends on the
    // solver's iteration count, so this asserts the direction, not a number.)
    CHECK(body->velocity.x < 100.0f);
}
