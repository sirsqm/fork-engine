// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
//
// Tests for the editor document: building, loading, and saving a scene through
// the public serialization API, and the play-request handshake between the UI
// and the host. These are headless and touch the filesystem only through the
// engine's own fs helpers.

#include <doctest/doctest.h>

#include "fe/core/FileSystem.h"
#include "fe/editor/EditorContext.h"
#include "fe/scene/Node.h"

#include <string>

using namespace fe;
using namespace fe::editor;

namespace {

// A path under the build's working directory, unique per test file.
[[nodiscard]] std::string scratch_path(const char* name) {
    return std::string("editor_test_") + name + ".json";
}

} // namespace

TEST_CASE("a new scene has a camera, sprites, and a refreshed hierarchy") {
    EditorContext context;
    context.register_default_component_factories();

    REQUIRE_FALSE(context.new_scene(nullptr));

    CHECK(context.scene().name() == "Untitled");
    CHECK(context.scene().node_count() == 6); // Camera + 3 sprites + Group + child.
    CHECK(context.scene().find_node_by_name("Camera") != nullptr);

    // Sprites are created without a texture when no resource manager is given,
    // which is a valid authoring state; the sprites still exist as nodes.
    CHECK(context.scene().find_node_by_name("Sprite0") != nullptr);
    CHECK(context.scene().find_node_by_name("GroupChild") != nullptr);

    // The hierarchy was rebuilt, so it reflects the new scene.
    CHECK(context.hierarchy().rows().size() == context.scene().node_count());
}

TEST_CASE("a scene round-trips through save and load") {
    EditorContext context;
    context.register_default_component_factories();
    REQUIRE_FALSE(context.new_scene(nullptr));

    const std::string path = scratch_path("roundtrip");
    REQUIRE_FALSE(context.save_scene(path));
    CHECK(context.scene_path() == path);
    CHECK(fs::exists(path));

    EditorContext reloaded;
    REQUIRE_FALSE(reloaded.load_scene(path, nullptr));
    CHECK(reloaded.scene().node_count() == context.scene().node_count());
    CHECK(reloaded.scene().find_node_by_name("GroupChild") != nullptr);
    CHECK(reloaded.scene_path() == path);

    // The test writes into the working directory (the build tree); the file is
    // intentionally left in place, like the other serialization tests.
}

TEST_CASE("loading a missing scene leaves the document untouched") {
    EditorContext context;
    REQUIRE_FALSE(context.new_scene(nullptr));
    const usize before = context.scene().node_count();

    Error error = context.load_scene("this_file_does_not_exist_12345.json", nullptr);
    CHECK(error);

    // The failed load must not have modified the existing document.
    CHECK(context.scene().node_count() == before);
    CHECK(context.scene_path().empty());
}

TEST_CASE("saving with no path and no prior save is an error") {
    EditorContext context;
    REQUIRE_FALSE(context.new_scene(nullptr));
    // A fresh scene has never been saved and no path was given.
    Error error = context.save_scene("");
    CHECK(error);
}

TEST_CASE("play requests are a one-shot handshake") {
    EditorContext context;

    CHECK(context.play_request() == PlayRequest::None);
    CHECK(context.take_play_request() == PlayRequest::None);

    context.request_start_play();
    CHECK(context.play_request() == PlayRequest::Start);
    CHECK(context.take_play_request() == PlayRequest::Start);
    // Taking the request consumes it; a second take sees nothing.
    CHECK(context.take_play_request() == PlayRequest::None);

    context.request_stop_play();
    CHECK(context.take_play_request() == PlayRequest::Stop);
}
