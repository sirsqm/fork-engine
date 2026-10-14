// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include <doctest/doctest.h>

#include "fe/core/FileSystem.h"
#include "fe/resources/Resource.h"
#include "fe/resources/ResourceManager.h"
#include "fe/resources/Texture2DLoader.h"
#include "fe/scene/Camera2D.h"
#include "fe/scene/Node.h"
#include "fe/scene/Scene.h"
#include "fe/scene/Sprite2D.h"
#include "fe/serialization/SceneSerializer.h"

#include <filesystem>
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

TEST_CASE("component identity survives a round trip") {
    Scene original;
    Node* node = original.create_node("Thing");
    auto* sprite = node->add_component<Sprite2D>();
    const Uuid sprite_id = sprite->id();

    // Identity is assigned at creation, so it is never nil and two components
    // are never the same component.
    CHECK_FALSE(sprite_id.is_nil());
    CHECK(sprite_id != node->id());

    auto text = serialization::scene::to_json(original);
    REQUIRE(text);

    Scene restored;
    REQUIRE_FALSE(serialization::scene::from_json(text.value(), restored));

    Node* reloaded = restored.find_node_by_name("Thing");
    REQUIRE(reloaded != nullptr);
    const auto* reloaded_sprite = reloaded->get_component<Sprite2D>();
    REQUIRE(reloaded_sprite != nullptr);
    CHECK(reloaded_sprite->id() == sprite_id);

    // The scene can find it by that id, which is what an operation that targets
    // this exact component (the collaborative-editing log, ADR-0019) relies on.
    CHECK(restored.find_component(sprite_id) == reloaded_sprite);
    CHECK(restored.find_component(Uuid::generate()) == nullptr);
}

TEST_CASE("a component's id is distinct per component and stable across a move") {
    Scene scene;
    Node* node = scene.create_node("Thing");
    auto* first = node->add_component<Sprite2D>();
    auto* second = node->add_component<Sprite2D>();
    CHECK(first->id() != second->id());

    // Taking a component out of its node and putting it back must not change its
    // identity: an operation log that names it by id has to keep working.
    const Uuid id = first->id();
    auto taken = node->detach_component(0);
    REQUIRE(taken != nullptr);
    CHECK(taken->id() == id);
    node->insert_component(0, std::move(taken));
    CHECK(node->components()[0]->id() == id);
}

TEST_CASE("a version-2 document loads and assigns fresh component ids") {
    // A version-2 component has a type and properties but no "id". The reader
    // must still load it, keeping the id the component was created with rather
    // than failing or leaving a nil id.
    const std::string document = R"({
        "format": "fork_scene",
        "format_version": 2,
        "name": "Old",
        "roots": [
            {
                "id": "11111111-1111-1111-1111-111111111111",
                "name": "Hero",
                "type": "Node",
                "transform": {"position": [0, 0, 0], "rotation": [0, 0, 0], "scale": [1, 1, 1]},
                "components": [{"type": "Sprite2D", "size": [8, 8]}],
                "children": []
            }
        ]
    })";

    Scene scene;
    REQUIRE_FALSE(serialization::scene::from_json(document, scene));

    Node* hero = scene.find_node_by_name("Hero");
    REQUIRE(hero != nullptr);
    const auto* sprite = hero->get_component<Sprite2D>();
    REQUIRE(sprite != nullptr);
    CHECK_FALSE(sprite->id().is_nil());
    CHECK(scene.find_component(sprite->id()) == sprite);
}

TEST_CASE("the document records a format and version") {
    Scene scene;
    [[maybe_unused]] Node* root = scene.create_node("Root");

    auto text = serialization::scene::to_json(scene);
    REQUIRE(text);
    CHECK(text.value().find("\"format\": \"fork_scene\"") != std::string::npos);
    CHECK(text.value().find("\"format_version\": 3") != std::string::npos);
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
    const Error error = serialization::scene::from_json(R"({"format":"fork_scene","format_version":999})", scene);
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

namespace {

// A unique temporary asset root per test case, removed on destruction.
class TempDir {
public:
    explicit TempDir(std::string name) {
        path_ = std::filesystem::temp_directory_path() / ("fe_ser_test_" + name + "_" + std::to_string(counter_++));
        std::filesystem::remove_all(path_);
        std::filesystem::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    [[nodiscard]] std::string str() const { return path_.string(); }
    [[nodiscard]] std::string file(const std::string& name) const { return (path_ / name).string(); }

private:
    std::filesystem::path path_;
    static inline int counter_ = 0;
};

// A stand-in GPU texture. The serializer stores a reference, not the pixels, so
// a path and a type name are all it needs.
class StubTexture final : public Resource {
public:
    explicit StubTexture(std::string path) : Resource(std::move(path)) {}
    [[nodiscard]] const char* type_name() const noexcept override { return "Texture2D"; }
};

// A loader that serves StubTexture for ".png" under the "tex" scheme, so a
// loaded scene can resolve its references without a GPU.
class StubTextureLoader final : public ResourceLoader {
public:
    [[nodiscard]] std::string_view scheme() const noexcept override { return "tex"; }
    [[nodiscard]] bool supports(std::string_view extension) const noexcept override { return extension == ".png"; }
    [[nodiscard]] Result<std::shared_ptr<Resource>> load(std::string_view path) override {
        return std::static_pointer_cast<Resource>(std::make_shared<StubTexture>(std::string(path)));
    }
};

} // namespace

TEST_CASE("with an asset database a texture reference is saved as a stable guid") {
    TempDir dir("guid");
    REQUIRE_FALSE(fs::write_text_file(dir.file("assets/textures/checker.png"), "x"));

    ResourceManager manager;
    manager.assets().set_root(dir.str());
    auto imported = manager.assets().import("assets/textures/checker.png", "tex");
    REQUIRE(imported);

    Scene scene;
    Node* node = scene.create_node("Hero");
    auto* sprite = node->add_component<Sprite2D>();
    sprite->texture = std::make_shared<StubTexture>("assets/textures/checker.png");

    auto text = serialization::scene::to_json(scene, &manager);
    REQUIRE(text);
    CHECK(text.value().find(to_reference(imported.value().id)) != std::string::npos);
    CHECK(text.value().find("tex:assets/textures/checker.png") == std::string::npos);
}

TEST_CASE("without an asset database a texture reference is saved as a path key") {
    Scene scene;
    Node* node = scene.create_node("Hero");
    auto* sprite = node->add_component<Sprite2D>();
    sprite->texture = std::make_shared<StubTexture>("assets/textures/checker.png");

    auto text = serialization::scene::to_json(scene);
    REQUIRE(text);
    CHECK(text.value().find("\"tex:assets/textures/checker.png\"") != std::string::npos);
}

TEST_CASE("a version-1 document migrates path keys to guids and resolves") {
    TempDir dir("migrate");
    REQUIRE_FALSE(fs::write_text_file(dir.file("assets/textures/checker.png"), "x"));

    ResourceManager manager;
    manager.register_loader(std::make_unique<StubTextureLoader>());
    manager.assets().set_root(dir.str());
    auto imported = manager.assets().import("assets/textures/checker.png", "tex");
    REQUIRE(imported);

    // A hand-written version-1 document, as an older editor would have produced.
    const std::string document = R"({
        "format": "fork_scene",
        "format_version": 1,
        "name": "Old",
        "roots": [
            {
                "name": "Hero",
                "type": "Node",
                "components": [
                    { "type": "Sprite2D", "texture": "tex:assets/textures/checker.png" }
                ]
            }
        ]
    })";

    Scene scene;
    REQUIRE_FALSE(serialization::scene::from_json(document, scene, &manager));
    Node* hero = scene.find_node_by_name("Hero");
    REQUIRE(hero != nullptr);
    const auto* sprite = hero->get_component<Sprite2D>();
    REQUIRE(sprite != nullptr);
    REQUIRE(sprite->texture != nullptr);
    CHECK(sprite->texture->path() == "assets/textures/checker.png");
    CHECK(sprite->texture->asset_id() == imported.value().id);
}

TEST_CASE("a version-1 reference with no asset entry fails clearly instead of loading broken") {
    ResourceManager manager;
    const std::string document = R"({
        "format": "fork_scene",
        "format_version": 1,
        "roots": [
            { "name": "Hero", "type": "Node",
              "components": [ { "type": "Sprite2D", "texture": "tex:assets/missing.png" } ] }
        ]
    })";

    Scene scene;
    const Error error = serialization::scene::from_json(document, scene, &manager);
    CHECK(error);
    CHECK(error.message().find("cannot be migrated") != std::string::npos);
}
