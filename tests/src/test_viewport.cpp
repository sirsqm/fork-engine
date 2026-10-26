// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
//
// The viewport's headless half: camera pan/zoom and sprite picking. No window,
// no GPU, no ImGui - just the transform maths and the hit-test, which is the
// part of the viewport worth testing because it can be wrong in ways a
// screenshot would not make obvious.

#include <doctest/doctest.h>

#include "fe/editor/Viewport.h"
#include "fe/scene/Camera2D.h"
#include "fe/scene/Node.h"
#include "fe/scene/Scene.h"
#include "fe/scene/Sprite2D.h"

#include <array>

using namespace fe;
using namespace fe::editor;

namespace {

// A 100x100 viewport with a default (origin, zoom 1) camera, plus the scene the
// camera lives in so camera.node() is valid.
struct View {
    Scene scene;
    Node* camera_node = nullptr;
    Camera2D* camera = nullptr;
    Viewport viewport;

    View() {
        camera_node = scene.create_node("Camera");
        camera = camera_node->add_component<Camera2D>();
        viewport.set_size(100, 100);
    }

    Node* add_sprite(const char* name, const Vec2& position, const Vec2& size) {
        Node* node = scene.create_node(name);
        node->transform().position = Vec3{position.x, position.y, 0.0f};
        auto* sprite = node->add_component<Sprite2D>();
        sprite->size = size;
        return node;
    }
};

} // namespace

TEST_CASE("screen_to_world maps the viewport centre to the camera position") {
    View view;
    view.camera_node->transform().position = Vec3{10.0f, 20.0f, 0.0f};

    // The centre of a 100x100 viewport is the camera's world position.
    const Vec2 centre = view.viewport.screen_to_world(*view.camera, Vec2{50.0f, 50.0f});
    CHECK(centre.x == doctest::Approx(10.0f));
    CHECK(centre.y == doctest::Approx(20.0f));

    // Screen Y grows downward, world Y grows upward, so moving down the image
    // (larger screen Y) means smaller world Y.
    const Vec2 below = view.viewport.screen_to_world(*view.camera, Vec2{50.0f, 100.0f});
    CHECK(below.y == doctest::Approx(20.0f - 50.0f));

    const Vec2 right = view.viewport.screen_to_world(*view.camera, Vec2{100.0f, 50.0f});
    CHECK(right.x == doctest::Approx(10.0f + 50.0f));
}

TEST_CASE("zoom scales the world-to-screen mapping") {
    View view;
    view.camera->zoom = 2.0f;

    // At zoom 2, the 100px viewport spans only 50 world units.
    const Vec2 right_edge = view.viewport.screen_to_world(*view.camera, Vec2{100.0f, 50.0f});
    CHECK(right_edge.x == doctest::Approx(25.0f));
    CHECK(right_edge.y == doctest::Approx(0.0f));
}

TEST_CASE("panning moves the camera opposite to the drag") {
    View view;
    view.viewport.pan_by_pixels(*view.camera, Vec2{10.0f, -5.0f});

    // Dragging right and up reveals content to the left and below, so the
    // camera moves left and down.
    CHECK(view.camera_node->transform().position.x == doctest::Approx(-10.0f));
    CHECK(view.camera_node->transform().position.y == doctest::Approx(-5.0f));
}

TEST_CASE("zoom about the cursor keeps the world point under the cursor") {
    View view;
    const Vec2 cursor{75.0f, 25.0f};
    const Vec2 before = view.viewport.screen_to_world(*view.camera, cursor);

    view.viewport.zoom_at_cursor(*view.camera, cursor, 1.0f); // one wheel notch
    CHECK(view.camera->zoom > 1.0f);

    const Vec2 after = view.viewport.screen_to_world(*view.camera, cursor);
    // The anchor stays put: the same world point is still under the cursor.
    CHECK(after.x == doctest::Approx(before.x).epsilon(0.001));
    CHECK(after.y == doctest::Approx(before.y).epsilon(0.001));
}

TEST_CASE("a sprite hit-test uses its world AABB") {
    View view;
    Node* sprite = view.add_sprite("Sprite", Vec2{0.0f, 0.0f}, Vec2{10.0f, 10.0f});

    CHECK(Viewport::node_contains_world_point(*sprite, Vec2{0.0f, 0.0f}));
    CHECK(Viewport::node_contains_world_point(*sprite, Vec2{4.9f, 4.9f}));
    CHECK_FALSE(Viewport::node_contains_world_point(*sprite, Vec2{5.1f, 0.0f}));
    CHECK_FALSE(Viewport::node_contains_world_point(*sprite, Vec2{0.0f, -5.1f}));
}

TEST_CASE("picking returns the smallest sprite under the cursor") {
    View view;
    // A large backdrop and a small icon, both centred at the origin. The icon
    // must win even though the backdrop also contains the point.
    Node* backdrop = view.add_sprite("Backdrop", Vec2{0.0f, 0.0f}, Vec2{80.0f, 80.0f});
    Node* icon = view.add_sprite("Icon", Vec2{0.0f, 0.0f}, Vec2{10.0f, 10.0f});

    CHECK(view.viewport.pick(view.scene, *view.camera, Vec2{50.0f, 50.0f}) == icon);

    // Away from the icon, only the backdrop is under the cursor.
    CHECK(view.viewport.pick(view.scene, *view.camera, Vec2{80.0f, 50.0f}) == backdrop);

    // Outside every sprite, nothing is picked.
    CHECK(view.viewport.pick(view.scene, *view.camera, Vec2{1.0f, 1.0f}) == nullptr);
}

TEST_CASE("picking follows a moved camera") {
    View view;
    Node* sprite = view.add_sprite("Sprite", Vec2{40.0f, 0.0f}, Vec2{10.0f, 10.0f});

    // At the origin camera the sprite's world (40,0) is off the right edge of a
    // 100px viewport (which spans -50..50) but still inside; the centre of the
    // viewport (screen 50,50) is world (0,0), away from the sprite.
    CHECK(view.viewport.pick(view.scene, *view.camera, Vec2{50.0f, 50.0f}) == nullptr);

    // Move the camera onto the sprite; now the viewport centre picks it.
    view.camera_node->transform().position = Vec3{40.0f, 0.0f, 0.0f};
    CHECK(view.viewport.pick(view.scene, *view.camera, Vec2{50.0f, 50.0f}) == sprite);
}

TEST_CASE("world bounds project back to screen corners") {
    View view;
    Node* sprite = view.add_sprite("Sprite", Vec2{0.0f, 0.0f}, Vec2{10.0f, 10.0f});

    Vec2 min;
    Vec2 max;
    REQUIRE(Viewport::node_world_bounds(*sprite, min, max));
    CHECK(min.x == doctest::Approx(-5.0f));
    CHECK(max.x == doctest::Approx(5.0f));

    std::array<Vec2, 4> quad{};
    view.viewport.world_bounds_to_screen(*view.camera, min, max, quad);

    // The world origin is the viewport centre, so the quad straddles (50, 50).
    // World Y grows up but screen Y grows down, so the world-min corner (below
    // centre) is *below* centre on screen (larger Y).
    CHECK(quad[0].x == doctest::Approx(45.0f)); // min.x, min.y
    CHECK(quad[0].y == doctest::Approx(55.0f));
    CHECK(quad[2].x == doctest::Approx(55.0f)); // max.x, max.y
    CHECK(quad[2].y == doctest::Approx(45.0f));
}

TEST_CASE("a zero-sized viewport never picks") {
    View view;
    view.add_sprite("Sprite", Vec2{0.0f, 0.0f}, Vec2{10.0f, 10.0f});
    view.viewport.set_size(0, 0);

    CHECK(view.viewport.pick(view.scene, *view.camera, Vec2{0.0f, 0.0f}) == nullptr);
}

// --- Drag to move -----------------------------------------------------------
//
// The drag-to-move maths is pure, so it is tested here rather than through the
// panel: a drag right and down must move the node right and *down* on screen,
// which is world -Y, and the movement must shrink as the zoom grows.

TEST_CASE("a screen drag maps to a world delta with the vertical axis flipped") {
    View view;
    view.viewport.set_size(100, 100);

    // Zoom 1: one screen pixel is one world unit, and screen Y (down) is world
    // -Y, so a drag right and down moves the node right and down in the world.
    const Vec2 delta = view.viewport.drag_to_world_delta(*view.camera, Vec2{10.0f, 20.0f});
    CHECK(delta.x == doctest::Approx(10.0f));
    CHECK(delta.y == doctest::Approx(-20.0f));
}

TEST_CASE("a screen drag is divided by the zoom") {
    View view;
    view.viewport.set_size(100, 100);
    view.camera->zoom = 2.0f;

    const Vec2 delta = view.viewport.drag_to_world_delta(*view.camera, Vec2{10.0f, 20.0f});
    CHECK(delta.x == doctest::Approx(5.0f));
    CHECK(delta.y == doctest::Approx(-10.0f));
}

TEST_CASE("a dragged node keeps its grab offset and tracks the cursor") {
    View view;
    view.viewport.set_size(100, 100);

    // The node starts at the world origin, which is the viewport centre. Grab it
    // 10 world units to the right of where it sits, then move the cursor: the
    // node must keep that 10-unit offset rather than snap under the cursor.
    const Vec2 origin{0.0f, 0.0f};
    const Vec2 cursor_world_start{10.0f, 0.0f};

    // Cursor travels to (30, 5) in the world: a +20 in X and a +5 in Y.
    const Vec2 cursor_world_now{30.0f, 5.0f};
    const Vec2 travel{cursor_world_now.x - cursor_world_start.x, cursor_world_now.y - cursor_world_start.y};
    const Vec2 moved = Vec2{origin.x + travel.x, origin.y + travel.y};

    CHECK(moved.x == doctest::Approx(20.0f));
    CHECK(moved.y == doctest::Approx(5.0f));

    // The same result through `dragged_position`, which is the helper the panel
    // uses: a mouse delta of the same travel (screen Y flipped) lands identically.
    const Vec2 via_helper = view.viewport.dragged_position(*view.camera, origin, Vec2{travel.x, -travel.y});
    CHECK(via_helper.x == doctest::Approx(20.0f));
    CHECK(via_helper.y == doctest::Approx(5.0f));
}

TEST_CASE("drag_to_world_delta tolerates a zero or negative zoom") {
    View view;
    view.viewport.set_size(100, 100);
    view.camera->zoom = 0.0f;

    // A degenerate zoom is treated as 1 rather than dividing by zero.
    const Vec2 delta = view.viewport.drag_to_world_delta(*view.camera, Vec2{4.0f, 4.0f});
    CHECK(delta.x == doctest::Approx(4.0f));
    CHECK(delta.y == doctest::Approx(-4.0f));
}
