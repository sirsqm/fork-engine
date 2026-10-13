// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include <doctest/doctest.h>

#include "fe/resources/Resource.h"
#include "fe/resources/Texture2DLoader.h"
#include "fe/scene/Camera2D.h"
#include "fe/scene/Node.h"
#include "fe/scene/Scene.h"
#include "fe/scene/Sprite2D.h"
#include "fe/serialization/SceneSerializer.h"

#include <memory>
#include <string>

using namespace fe;

namespace {

// Builds a small scene with a camera and a sprite hierarchy, for round-trip
// tests.
void build_sample_scene(Scene& scene) {
    scene.set_name("Sample");

    Node* camera_node = scene.create_node("Camera");
    auto* camera = camera_node->add_component<Camera2D>();
    camera->zoom = 1.5f;
    camera->primary = true;
    camera_node->transform().position = Vec3{32.0f, 48.0f, 0.0f};

    Node* sprite_node = scene.create_node("Hero");
    auto* sprite = sprite_node->add_component<Sprite2D>();
    sprite->size = Vec2{64.0f, 64.0f};
    sprite->color = Vec4{0.5f, 0.25f, 1.0f, 0.8f};
    sprite->layer = 3;
    sprite_node->transform().position = Vec3{100.0f, 200.0f, 0.0f};
    sprite_node->transform().rotation = Vec3{0.0f, 0.0f, 0.5f};

    Node* child = sprite_node->create_child<Node>("Shadow");
    auto* child_sprite = child->add_component<Sprite2D>();
    child_sprite->size = Vec2{32.0f, 8.0f};
    child->transform().position = Vec3{0.0f, -40.0f, 0.0f};
}

} // namespace

TEST_CASE("a scene round-trips through JSON without losing structure") {
    Scene original;
    build_sample_scene(original);

    auto text = serialization::scene::to_json(original);
    REQUIRE(text);

    Scene restored;
    REQUIRE_FALSE(serialization::scene::from_json(text.value(), restored));

    CHECK(restored.name() == "Sample");
    CHECK(restored.root_count() == 2);
    CHECK(restored.node_count() == 3);

    Node* hero = restored.find_node_by_name("Hero");
    REQUIRE(hero != nullptr);
    CHECK(hero->child_count() == 1);
    CHECK(hero->children()[0]->name() == "Shadow");

    const auto* sprite = hero->get_component<Sprite2D>();
    REQUIRE(sprite != nullptr);
    CHECK(sprite->size.x == doctest::Approx(64.0f));
    CHECK(sprite->layer == 3);
    CHECK(sprite->color.z == doctest::Approx(1.0f));

    CHECK(hero->transform().position.x == doctest::Approx(100.0f));
    CHECK(hero->transform().rotation.z == doctest::Approx(0.5f));

    const auto* camera_node = restored.find_node_by_name("Camera");
    REQUIRE(camera_node != nullptr);
    const auto* camera = camera_node->get_component<Camera2D>();
    REQUIRE(camera != nullptr);
    CHECK(camera->zoom == doctest::Approx(1.5f));
}

TEST_CASE("node identity survives a round trip") {
    Scene original;
    Node* node = original.create_node("Thing");
    const Uuid original_id = node->id();

    auto text = serialization::scene::to_json(original);
    REQUIRE(text);

    Scene restored;
    REQUIRE_FALSE(serialization::scene::from_json(text.value(), restored));

    Node* reloaded = restored.find_node_by_name("Thing");
    REQUIRE(reloaded != nullptr);
    CHECK(reloaded->id() == original_id);
}

TEST_CASE("the document records a format and version") {
    Scene scene;
    scene.create_node("Root");

    auto text = serialization::scene::to_json(scene);
    REQUIRE(text);
    CHECK(text.value().find("\"format\": \"fork_scene\"") != std::string::npos);
    CHECK(text.value().find("\"format_version\": 1") != std::string::npos);
}

TEST_CASE("loading rejects a document that is not a fork scene") {
    Scene scene;
    const Error error = serialization::scene::from_json(R"({"format":"other","format_version":1})", scene);
    CHECK(error);
    CHECK(error.message().find("not a fork scene") != std::string::npos);
}

TEST_CASE("a sprite's texture reference is saved as a resolvable resource key") {
    Scene scene;
    Node* node = scene.create_node("Hero");
    auto* sprite = node->add_component<Sprite2D>();

    // Stand in for a loaded texture. The serializer only needs its path and
    // type name; it stores a *reference*, not the pixels.
    class StubTexture final : public Resource {
    public:
        explicit StubTexture(std::string path) : Resource(std::move(path)) {}
        [[nodiscard]] const char* type_name() const noexcept override { return "Texture2D"; }
    };
    sprite->texture = std::make_shared<StubTexture>("assets/textures/checker.png");

    auto text = serialization::scene::to_json(scene);
    REQUIRE(text);
    CHECK(text.value().find("\"tex:assets/textures/checker.png\"") != std::string::npos);
}

TEST_CASE("Texture2DLoader::reference_key does not double-prefix a scheme") {
    CHECK(Texture2DLoader::reference_key("assets/hero.png") == "tex:assets/hero.png");
    CHECK(Texture2DLoader::reference_key("tex:assets/hero.png") == "tex:assets/hero.png");
}

TEST_CASE("loading rejects a newer format version") {
    Scene scene;
    const Error error =
        serialization::scene::from_json(R"({"format":"fork_scene","format_version":999})", scene);
    CHECK(error);
    CHECK(error.message().find("newer") != std::string::npos);
}

TEST_CASE("loading reports malformed JSON instead of throwing") {
    Scene scene;
    const Error error = serialization::scene::from_json("{ this is not json", scene);
    CHECK(error);
    CHECK(error.message().find("parse error") != std::string::npos);
}

TEST_CASE("loading an empty scene document yields an empty scene") {
    Scene scene;
    REQUIRE_FALSE(serialization::scene::from_json(R"({"format":"fork_scene","format_version":1})", scene));
    CHECK(scene.root_count() == 0);
}
