// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include <doctest/doctest.h>

#include "fe/reflect/ComponentTypes.h"
#include "fe/scene/Node.h"
#include "fe/scene/Scene.h"
#include "fe/scene/Sprite2D.h"
#include "fe/scene/SpriteAnimator.h"
#include "fe/serialization/SceneSerializer.h"
#include "fe/systems/SceneSchedule.h"
#include "fe/systems/SpriteAnimationSystem.h"

#include <cmath>
#include <ostream>
#include <string>

using namespace fe;

namespace {

// A recording system that appends its own tag to a shared order string, plus
// the context it last saw. Used to prove schedule ordering and context
// propagation without depending on any real system.
class RecordingSystem final : public System {
public:
    RecordingSystem(std::string tag, std::string& order) : tag_(std::move(tag)), order_(order) {}

    [[nodiscard]] std::string_view name() const noexcept override { return tag_; }

    void update(const FrameContext& context, Scene& scene) override {
        (void)scene;
        last_delta = context.delta_time;
        last_elapsed = context.elapsed_time;
        last_resources = context.resources;
        update_count += 1;
        order_ += tag_;
        order_ += '|';
    }

    f32 last_delta = 0.0f;
    f64 last_elapsed = 0.0;
    ResourceManager* last_resources = nullptr;
    int update_count = 0;

private:
    std::string tag_;
    std::string& order_;
};

SpriteAnimator* add_animated_sprite(Scene& scene, u32 x, u32 y, f32 fps, bool loop = true) {
    Node* node = scene.create_node("Animated");
    auto* sprite = node->add_component<Sprite2D>();
    auto* animator = node->add_component<SpriteAnimator>();
    animator->frames_x = x;
    animator->frames_y = y;
    animator->fps = fps;
    animator->loop = loop;
    (void)sprite;
    return animator;
}

FrameContext frame(f32 delta) {
    FrameContext context;
    context.delta_time = delta;
    context.elapsed_time = static_cast<f64>(delta);
    return context;
}

} // namespace

TEST_CASE("a schedule runs systems in the order they were added") {
    std::string order;
    SceneSchedule schedule;
    schedule.add_system<RecordingSystem>("A", order);
    schedule.add_system<RecordingSystem>("B", order);
    schedule.add_system<RecordingSystem>("C", order);

    Scene scene;
    schedule.update(frame(0.016f), scene);

    CHECK(order == "A|B|C|");
    CHECK(schedule.system_count() == 3);
}

TEST_CASE("every system sees the same frame context") {
    std::string order;
    SceneSchedule schedule;
    auto* a = schedule.add_system<RecordingSystem>("A", order);
    auto* b = schedule.add_system<RecordingSystem>("B", order);

    Scene scene;
    FrameContext context = frame(0.5f);
    context.elapsed_time = 12.0;
    schedule.update(context, scene);

    CHECK(a->last_delta == doctest::Approx(0.5f));
    CHECK(b->last_delta == doctest::Approx(0.5f));
    CHECK(a->last_elapsed == doctest::Approx(12.0));
    CHECK(b->last_elapsed == doctest::Approx(12.0));
}

TEST_CASE("a schedule can be searched by system name") {
    std::string order;
    SceneSchedule schedule;
    schedule.add_system<RecordingSystem>("SpriteAnimationSystem", order);
    schedule.add_system<RecordingSystem>("Other", order);

    CHECK(schedule.find("Other") != nullptr);
    CHECK(schedule.find("Other")->name() == "Other");
    CHECK(schedule.find("Missing") == nullptr);
}

TEST_CASE("an empty schedule updates cleanly") {
    SceneSchedule schedule;
    Scene scene;
    schedule.update(frame(0.016f), scene);
    CHECK(schedule.system_count() == 0);
}

TEST_CASE("clearing a schedule leaves it reusable") {
    std::string order;
    SceneSchedule schedule;
    schedule.add_system<RecordingSystem>("A", order);
    schedule.clear();
    CHECK(schedule.system_count() == 0);

    schedule.add_system<RecordingSystem>("B", order);
    Scene scene;
    schedule.update(frame(0.016f), scene);
    CHECK(order == "B|");
}

TEST_CASE("the animation system advances a frame after enough time elapses") {
    Scene scene;
    auto* animator = add_animated_sprite(scene, 4, 1, 10.0f); // 10 fps -> 0.1s per frame

    SpriteAnimationSystem system;
    CHECK(animator->current_frame == 0);

    // Half a frame of time: nothing advances yet.
    system.update(frame(0.05f), scene);
    CHECK(animator->current_frame == 0);
    CHECK(system.last_advanced_count() == 0);

    // Another half finishes the frame.
    system.update(frame(0.05f), scene);
    CHECK(animator->current_frame == 1);
    CHECK(system.last_advanced_count() == 1);
}

TEST_CASE("a large delta advances several frames at once") {
    Scene scene;
    auto* animator = add_animated_sprite(scene, 4, 1, 10.0f);

    SpriteAnimationSystem system;
    system.update(frame(0.35f), scene); // 3.5 frames worth
    CHECK(animator->current_frame == 3);

    // The leftover 0.5 frame is kept: 0.5 + 0.6 -> one more frame.
    system.update(frame(0.06f), scene);
    CHECK(animator->current_frame == 0); // wrapped (looping, 4 frames)
}

TEST_CASE("looping wraps and non-looping clamps to the last frame") {
    Scene looping;
    auto* wrap = add_animated_sprite(looping, 2, 2, 8.0f, /*loop=*/true);
    SpriteAnimationSystem system;
    // 4 frames at 8 fps: two seconds drives more than one full cycle.
    for (int i = 0; i < 20; ++i) {
        system.update(frame(0.125f), looping);
    }
    CHECK(wrap->current_frame < wrap->count());

    Scene once;
    auto* clamp = add_animated_sprite(once, 2, 2, 8.0f, /*loop=*/false);
    for (int i = 0; i < 20; ++i) {
        system.update(frame(0.125f), once);
    }
    CHECK(clamp->current_frame == clamp->count() - 1);
}

TEST_CASE("a paused animator holds its frame but still publishes uvs") {
    Scene scene;
    Node* node = scene.create_node("Animated");
    auto* sprite = node->add_component<Sprite2D>();
    auto* animator = node->add_component<SpriteAnimator>();
    animator->frames_x = 4;
    animator->frames_y = 1;
    animator->fps = 10.0f;
    animator->playing = false;
    animator->current_frame = 2;

    SpriteAnimationSystem system;
    system.update(frame(5.0f), scene);

    CHECK(animator->current_frame == 2);
    CHECK(system.last_advanced_count() == 0);
    // Cell 2 of 4 is the third quarter of the texture; the sprite must show it
    // even though time is not advancing.
    CHECK(sprite->uv.x == doctest::Approx(0.5f));
    CHECK(sprite->uv.z == doctest::Approx(0.75f));
}

TEST_CASE("the animation system writes the current cell's uvs onto the sprite") {
    Scene scene;
    Node* node = scene.create_node("Animated");
    auto* sprite = node->add_component<Sprite2D>();
    auto* animator = node->add_component<SpriteAnimator>();
    animator->frames_x = 2;
    animator->frames_y = 2; // 4 cells in a 2x2 grid
    animator->fps = 1.0f;
    animator->playing = false;

    SpriteAnimationSystem system;

    animator->current_frame = 0; // top-left
    system.update(frame(0.0f), scene);
    CHECK(sprite->uv.x == doctest::Approx(0.0f));
    CHECK(sprite->uv.y == doctest::Approx(0.0f));
    CHECK(sprite->uv.z == doctest::Approx(0.5f));
    CHECK(sprite->uv.w == doctest::Approx(0.5f));

    animator->current_frame = 3; // bottom-right
    system.update(frame(0.0f), scene);
    CHECK(sprite->uv.x == doctest::Approx(0.5f));
    CHECK(sprite->uv.y == doctest::Approx(0.5f));
    CHECK(sprite->uv.z == doctest::Approx(1.0f));
    CHECK(sprite->uv.w == doctest::Approx(1.0f));
}

TEST_CASE("a degenerate layout disables animation and draws the whole texture") {
    Scene scene;
    Node* node = scene.create_node("Animated");
    auto* sprite = node->add_component<Sprite2D>();
    auto* animator = node->add_component<SpriteAnimator>();
    animator->frames_x = 0;
    animator->frames_y = 4;
    animator->fps = 10.0f;

    CHECK(animator->count() == 0);

    SpriteAnimationSystem system;
    system.update(frame(1.0f), scene);

    CHECK(animator->current_frame == 0);
    CHECK(system.last_advanced_count() == 0);
    CHECK(sprite->uv.x == doctest::Approx(0.0f));
    CHECK(sprite->uv.z == doctest::Approx(1.0f));
}

TEST_CASE("a stale out-of-range frame is normalised") {
    Scene scene;
    auto* animator = add_animated_sprite(scene, 2, 2, 10.0f, /*loop=*/false);
    animator->current_frame = 99; // as if hand-edited in a document

    SpriteAnimationSystem system;
    system.update(frame(0.0f), scene);

    CHECK(animator->current_frame == animator->count() - 1);
}

TEST_CASE("the system ignores animators without a sprite and sprites without an animator") {
    Scene scene;
    Node* only_animator = scene.create_node("OnlyAnimator");
    auto* animator = only_animator->add_component<SpriteAnimator>();
    animator->frames_x = 4;
    animator->fps = 10.0f;
    animator->current_frame = 1;

    Node* only_sprite = scene.create_node("OnlySprite");
    auto* sprite = only_sprite->add_component<Sprite2D>();
    sprite->uv = Vec4{0.1f, 0.2f, 0.3f, 0.4f};

    SpriteAnimationSystem system;
    system.update(frame(1.0f), scene);

    // The animator with no sprite is left untouched, and the sprite with no
    // animator keeps the UVs it was authored with.
    CHECK(animator->current_frame == 1);
    CHECK(system.last_advanced_count() == 0);
    CHECK(sprite->uv.x == doctest::Approx(0.1f));
    CHECK(sprite->uv.w == doctest::Approx(0.4f));
}

TEST_CASE("a sprite animator round-trips through the serializer via reflection") {
    // The registry is process-global; populate it explicitly so this test does
    // not depend on another file having registered the built-in types first.
    reflect::register_builtin_types();

    Scene scene;
    Node* node = scene.create_node("Animated");
    auto* animator = node->add_component<SpriteAnimator>();
    animator->frames_x = 8;
    animator->frames_y = 2;
    animator->fps = 12.5f;
    animator->playing = false;
    animator->loop = false;
    animator->current_frame = 5;

    auto json = serialization::scene::to_json(scene);
    REQUIRE(json);

    Scene loaded;
    REQUIRE_FALSE(serialization::scene::from_json(json.value(), loaded)); // Error is falsy on success

    Node* loaded_node = loaded.find_node_by_name("Animated");
    REQUIRE(loaded_node != nullptr);
    const auto* loaded_animator = loaded_node->get_component<SpriteAnimator>();
    REQUIRE(loaded_animator != nullptr);
    CHECK(loaded_animator->frames_x == 8);
    CHECK(loaded_animator->frames_y == 2);
    CHECK(loaded_animator->fps == doctest::Approx(12.5f));
    CHECK(loaded_animator->playing == false);
    CHECK(loaded_animator->loop == false);
    CHECK(loaded_animator->current_frame == 5);
}

TEST_CASE("a system that leaves the scene does not leak its accumulator") {
    Scene scene;
    Node* holder = scene.create_node("Holder");
    Node* node = holder->create_child<Node>("Animated");
    auto* sprite = node->add_component<Sprite2D>();
    auto* animator = node->add_component<SpriteAnimator>();
    animator->frames_x = 4;
    animator->frames_y = 1;
    animator->fps = 10.0f;
    (void)sprite;

    SpriteAnimationSystem system;
    system.update(frame(0.05f), scene); // half a frame, not yet advanced

    // Detach the animated node: the fractional progress it had accumulated
    // must not survive the round trip, and the system must not keep a pointer
    // to it.
    std::unique_ptr<Node> detached = holder->remove_child(node);
    REQUIRE(detached != nullptr);

    system.update(frame(0.05f), scene); // the node is gone
    CHECK(system.last_advanced_count() == 0);

    holder->add_child(std::move(detached));
    system.update(frame(0.05f), scene); // half a frame from a clean slate
    CHECK(animator->current_frame == 0);
    CHECK(system.last_advanced_count() == 0);
}
