// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
//
// The 3D viewport's headless half: the orbit rig, ray/AABB hit-testing, and
// framing. No window, no GPU, no ImGui - just the maths a screenshot would not
// make obviously wrong.

#include <doctest/doctest.h>

#include "fe/editor/Viewport3D.h"
#include "fe/mesh/Mesh.h"
#include "fe/scene/Node.h"
#include "fe/scene/Scene.h"
#include "fe/scene3d/Camera3D.h"
#include "fe/scene3d/MeshRenderer3D.h"

#include <array>

using namespace fe;
using namespace fe::editor;

namespace {

// A 200x200 viewport, a camera on a node, and helpers to add meshes.
struct View {
    Scene scene;
    Node* camera_node = nullptr;
    Camera3D* camera = nullptr;
    Viewport3D viewport;

    View() {
        camera_node = scene.create_node("Camera");
        camera = camera_node->add_component<Camera3D>();
        viewport.set_size(200, 200);
    }

    Node* add_box(const char* name, const Vec3& position, const Vec3& half_extent) {
        Node* node = scene.create_node(name);
        node->transform().position = position;
        auto* renderer = node->add_component<MeshRenderer3D>();
        renderer->mesh = Mesh::make_box(half_extent);
        return node;
    }
};

} // namespace

TEST_CASE("ray_intersects_aabb hits the near face and misses outside the slab") {
    const Vec3 origin{0.0f, 0.0f, -5.0f};
    const Vec3 direction{0.0f, 0.0f, 1.0f};
    const Vec3 box_min{-1.0f, -1.0f, -1.0f};
    const Vec3 box_max{1.0f, 1.0f, 1.0f};

    f32 t = -1.0f;
    REQUIRE(Viewport3D::ray_intersects_aabb(origin, direction, box_min, box_max, t));
    CHECK(t == doctest::Approx(4.0f));

    // A ray parallel to and outside a slab misses.
    f32 miss_t = 0.0f;
    CHECK_FALSE(Viewport3D::ray_intersects_aabb(Vec3{5.0f, 0.0f, -5.0f}, direction, box_min, box_max, miss_t));

    // A ray pointing away from the box misses even though the line would cross.
    f32 away_t = 0.0f;
    CHECK_FALSE(Viewport3D::ray_intersects_aabb(origin, Vec3{0.0f, 0.0f, -1.0f}, box_min, box_max, away_t));

    // A ray starting inside the box reports t = 0.
    f32 inside_t = -1.0f;
    REQUIRE(Viewport3D::ray_intersects_aabb(Vec3{}, direction, box_min, box_max, inside_t));
    CHECK(inside_t == doctest::Approx(0.0f));
}

TEST_CASE("screen_ray through the centre points where the camera looks") {
    View view;
    // Place the camera at +Z looking back at the origin (forward is -Z).
    view.camera_node->transform().position = Vec3{0.0f, 0.0f, 10.0f};

    Vec3 origin;
    Vec3 direction;
    REQUIRE(view.viewport.screen_ray(*view.camera, Vec2{100.0f, 100.0f}, origin, direction));
    CHECK(direction.x == doctest::Approx(0.0f).epsilon(1e-4));
    CHECK(direction.y == doctest::Approx(0.0f).epsilon(1e-4));
    CHECK(direction.z == doctest::Approx(-1.0f).epsilon(1e-4));
}

TEST_CASE("pick selects the box under the cursor") {
    View view;
    view.camera_node->transform().position = Vec3{0.0f, 0.0f, 10.0f};

    Node* centre = view.add_box("Centre", Vec3{0.0f, 0.0f, 0.0f}, Vec3{1.0f, 1.0f, 1.0f});
    view.add_box("Side", Vec3{5.0f, 0.0f, 0.0f}, Vec3{1.0f, 1.0f, 1.0f});

    // The centre of the viewport ray passes through the origin, so it hits the
    // centre box and not the one off to the side.
    CHECK((view.viewport.pick(view.scene, *view.camera, Vec2{100.0f, 100.0f}) == centre));

    // Empty space to the far left hits nothing.
    CHECK((view.viewport.pick(view.scene, *view.camera, Vec2{2.0f, 2.0f}) == nullptr));
}

TEST_CASE("pick prefers the nearer of two overlapping boxes") {
    View view;
    view.camera_node->transform().position = Vec3{0.0f, 0.0f, 10.0f};

    Node* near_box = view.add_box("Near", Vec3{0.0f, 0.0f, 3.0f}, Vec3{1.0f, 1.0f, 1.0f});
    view.add_box("Far", Vec3{0.0f, 0.0f, -3.0f}, Vec3{1.0f, 1.0f, 1.0f});

    CHECK((view.viewport.pick(view.scene, *view.camera, Vec2{100.0f, 100.0f}) == near_box));
}

TEST_CASE("world_to_screen and screen_ray round-trip a point on the view axis") {
    View view;
    view.camera_node->transform().position = Vec3{0.0f, 0.0f, 10.0f};

    Vec2 screen;
    REQUIRE(view.viewport.world_to_screen(*view.camera, Vec3{0.0f, 0.0f, 0.0f}, screen));
    CHECK(screen.x == doctest::Approx(100.0f).epsilon(0.5f));
    CHECK(screen.y == doctest::Approx(100.0f).epsilon(0.5f));

    // A point behind the camera does not project.
    Vec2 behind;
    CHECK_FALSE(view.viewport.world_to_screen(*view.camera, Vec3{0.0f, 0.0f, 20.0f}, behind));
}

TEST_CASE("frame_bounds puts the box centre at the pivot and a positive distance") {
    Viewport3D viewport;
    viewport.set_size(200, 200);
    const Vec3 box_min{-1.0f, -1.0f, -1.0f};
    const Vec3 box_max{3.0f, 3.0f, 3.0f};

    viewport.frame_bounds(box_min, box_max, 60.0f);
    CHECK(viewport.orbit().pivot.x == doctest::Approx(1.0f));
    CHECK(viewport.orbit().pivot.y == doctest::Approx(1.0f));
    CHECK(viewport.orbit().pivot.z == doctest::Approx(1.0f));
    CHECK(viewport.orbit().distance > 0.0f);
}

TEST_CASE("orbit changes yaw and pitch and clamps pitch short of the pole") {
    Viewport3D viewport;
    const f32 start_yaw = viewport.orbit().yaw;

    viewport.orbit_by_pixels(Vec3{10.0f, 0.0f, 0.0f});
    CHECK(viewport.orbit().yaw != doctest::Approx(start_yaw));

    // A huge vertical drag must not let the pitch reach +-90 degrees, which would
    // degenerate the up vector.
    viewport.orbit_by_pixels(Vec3{0.0f, 100000.0f, 0.0f});
    CHECK(viewport.orbit().pitch < 1.5708f);
    CHECK(viewport.orbit().pitch > 1.5f);
}

TEST_CASE("dolly moves the eye closer for a positive scroll and is clamped") {
    Viewport3D viewport;
    viewport.set_orbit(Viewport3D::OrbitState{Vec3{}, 0.0f, 0.0f, 10.0f});
    const Vec3 before = viewport.eye();

    viewport.dolly_by_scroll(1.0f);
    const Vec3 after = viewport.eye();
    // Closer means the eye moved toward the pivot, so the distance shrank.
    CHECK((after - viewport.orbit().pivot).length() < (before - viewport.orbit().pivot).length());

    // Scrolling far in never reaches the pivot.
    for (int i = 0; i < 500; ++i) {
        viewport.dolly_by_scroll(1.0f);
    }
    CHECK(viewport.orbit().distance > 0.0f);
}

TEST_CASE("apply_to writes the rig into the camera node's transform") {
    View view;
    Viewport3D& viewport = view.viewport;
    viewport.set_orbit(Viewport3D::OrbitState{Vec3{0.0f, 0.0f, 0.0f}, 0.0f, 0.0f, 10.0f});

    viewport.apply_to(*view.camera);
    // Yaw 0, pitch 0 faces -Z, so the eye is at +Z * distance.
    CHECK(view.camera_node->transform().position.z == doctest::Approx(10.0f).epsilon(1e-3));
    CHECK(view.camera->eye().z == doctest::Approx(10.0f).epsilon(1e-3));
    // And the camera now looks at the pivot.
    CHECK(view.camera->forward().z == doctest::Approx(-1.0f).epsilon(1e-3));
}

TEST_CASE("sync_from_camera recovers the orientation apply_to wrote") {
    View view;
    view.camera_node->transform().position = Vec3{3.0f, 2.0f, 7.0f};
    // Aim the camera along a known direction by rotating the node.
    view.camera_node->transform().rotation = Vec3{0.0f, radians(45.0f), 0.0f};

    Viewport3D viewport;
    viewport.set_size(200, 200);
    viewport.set_orbit(Viewport3D::OrbitState{Vec3{}, 0.0f, 0.0f, 5.0f});
    viewport.sync_from_camera(*view.camera);

    // Applying the recovered rig reproduces the camera's forward direction.
    const Vec3 original_forward = view.camera->forward();
    viewport.apply_to(*view.camera);
    const Vec3 recovered_forward = view.camera->forward();
    CHECK(recovered_forward.x == doctest::Approx(original_forward.x).epsilon(1e-3));
    CHECK(recovered_forward.y == doctest::Approx(original_forward.y).epsilon(1e-3));
    CHECK(recovered_forward.z == doctest::Approx(original_forward.z).epsilon(1e-3));
}

TEST_CASE("node_world_bounds follows the node's transform") {
    View view;
    Node* box = view.add_box("Box", Vec3{5.0f, 0.0f, 0.0f}, Vec3{1.0f, 1.0f, 1.0f});

    Vec3 box_min;
    Vec3 box_max;
    REQUIRE(Viewport3D::node_world_bounds(*box, box_min, box_max));
    CHECK(box_min.x == doctest::Approx(4.0f));
    CHECK(box_max.x == doctest::Approx(6.0f));
    CHECK(box_min.y == doctest::Approx(-1.0f));
    CHECK(box_max.y == doctest::Approx(1.0f));
}

TEST_CASE("scene_world_bounds unions every node's bounds") {
    View view;
    view.add_box("A", Vec3{-3.0f, 0.0f, 0.0f}, Vec3{1.0f, 1.0f, 1.0f});
    view.add_box("B", Vec3{3.0f, 0.0f, 0.0f}, Vec3{1.0f, 1.0f, 1.0f});

    Vec3 box_min;
    Vec3 box_max;
    REQUIRE(Viewport3D::scene_world_bounds(view.scene, box_min, box_max));
    CHECK(box_min.x == doctest::Approx(-4.0f));
    CHECK(box_max.x == doctest::Approx(4.0f));
}

TEST_CASE("aabb_corners lists all eight corners") {
    std::array<Vec3, 8> corners{};
    Viewport3D::aabb_corners(Vec3{-1.0f, -2.0f, -3.0f}, Vec3{1.0f, 2.0f, 3.0f}, corners);

    // Every corner must have each coordinate at one of the two bounds; checking
    // the first and last against the min/max corner covers the ordering.
    CHECK(corners[0].x == doctest::Approx(-1.0f));
    CHECK(corners[0].y == doctest::Approx(-2.0f));
    CHECK(corners[0].z == doctest::Approx(-3.0f));
    CHECK(corners[6].x == doctest::Approx(1.0f));
    CHECK(corners[6].y == doctest::Approx(2.0f));
    CHECK(corners[6].z == doctest::Approx(3.0f));
}
