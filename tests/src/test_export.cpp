// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
//
// Tests for the export path (ADR-0064): the manifest value, and the end-to-end
// shipping flow the fork_export tool and the fork_player runtime perform. The
// end-to-end case packs a project tree, mounts the pack with nothing but the
// pack left on disk, and loads the startup scene - with a gameplay script bound
// - through the pack, which is exactly what a shipped build does at startup.
#include <doctest/doctest.h>

#include "fe/core/FileSystem.h"
#include "fe/pack/AndroidExportTemplate.h"
#include "fe/pack/ExportManifest.h"
#include "fe/pack/IosExportTemplate.h"
#include "fe/pack/PackSource.h"
#include "fe/pack/PackWriter.h"
#include "fe/pack/WebExportTemplate.h"
#include "fe/reflect/PropertyBuilders.h"
#include "fe/resources/AssetDatabase.h"
#include "fe/resources/ImageLoader.h"
#include "fe/resources/ResourceManager.h"
#include "fe/scene/Node.h"
#include "fe/scene/Scene.h"
#include "fe/script/Script.h"
#include "fe/script/ScriptBinding.h"
#include "fe/script/ScriptHost.h"
#include "fe/script/ScriptLibrary.h"
#include "fe/script/ScriptRegistry.h"
#include "fe/serialization/SceneSerializer.h"
#include "fe/systems/FrameContext.h"
#include "fe/version/Version.h"

#include <filesystem>
#include <memory>
#include <string>
#include <utility>

using namespace fe;
using namespace fe::pack;

namespace {

// A unique temporary directory per test case, removed on destruction.
class TempDir {
public:
    explicit TempDir(std::string name) {
        path_ = std::filesystem::temp_directory_path() / ("fe_export_test_" + name + "_" + std::to_string(counter_++));
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

// A gameplay behaviour with one authored field, so the exported scene carries a
// script binding with data in it.
class SpinScript final : public Script {
public:
    [[nodiscard]] std::string_view name() const noexcept override { return "SpinScript"; }

    Error update(const FrameContext& context) override {
        (void)context;
        if (node() != nullptr) {
            node()->transform().rotation.z += speed_;
        }
        return Error::none();
    }

    f32 speed_ = 1.0f;
};

ScriptRegistry make_registry() {
    ScriptRegistry registry;
    ScriptDesc desc;
    desc.name = "SpinScript";
    desc.factory = [] { return std::unique_ptr<Script>(new SpinScript()); };
    desc.properties.push_back(
        reflect::make_scalar<SpinScript, f32>("speed", &SpinScript::speed_, reflect::ValueKind::Float));
    registry.register_desc(std::move(desc));
    return registry;
}

} // namespace

TEST_CASE("an export manifest round-trips through its document") {
    ExportManifest manifest;
    manifest.name = "Demo";
    manifest.pack = "game.fepack";
    manifest.scene = "scenes/main.fescene";

    auto text = manifest.to_json();
    REQUIRE(text);
    // The document is self-describing: a reader can tell what it is before it
    // trusts any field.
    CHECK(text.value().find("fork_export") != std::string::npos);

    ExportManifest parsed;
    REQUIRE_FALSE(ExportManifest::from_json(text.value(), parsed));
    CHECK(parsed.name == "Demo");
    CHECK(parsed.pack == "game.fepack");
    CHECK(parsed.scene == "scenes/main.fescene");
}

TEST_CASE("an export manifest rejects a document it should not read") {
    ExportManifest out;

    // Malformed JSON.
    CHECK(ExportManifest::from_json("{ not json", out));
    // JSON, but not a fork export.
    CHECK(ExportManifest::from_json(R"({"format":"fork_project","format_version":1})", out));
    // A version from the future.
    CHECK(ExportManifest::from_json(R"({"format":"fork_export","format_version":99})", out));
    // A document missing what it must name.
    CHECK(ExportManifest::from_json(R"({"format":"fork_export","format_version":1,"pack":"g.fepack"})", out));
    CHECK(ExportManifest::from_json(R"({"format":"fork_export","format_version":1,"scene":"s.fescene"})", out));
}

TEST_CASE("an export manifest saves and loads from disk") {
    TempDir dir("manifest");
    const std::string path = dir.file(ExportManifest::kDefaultFileName);

    ExportManifest manifest;
    manifest.name = "SaveLoad";
    manifest.pack = "assets.fepack";
    manifest.scene = "scenes/start.fescene";
    REQUIRE_FALSE(manifest.save(path));
    REQUIRE(fs::exists(path));

    auto loaded = ExportManifest::load(path);
    REQUIRE(loaded);
    CHECK(loaded.value().name == "SaveLoad");
    CHECK(loaded.value().pack == "assets.fepack");
    CHECK(loaded.value().scene == "scenes/start.fescene");

    // A missing file is an error, not a default-constructed manifest.
    CHECK_FALSE(ExportManifest::load(dir.file("absent.json")));
}

TEST_CASE("an export manifest records the engine version that made it") {
    ExportManifest manifest;
    manifest.name = "Versioned";
    manifest.pack = "game.fepack";
    manifest.scene = "scenes/main.fescene";
    // The export step stamps the version from the one source of truth, so a
    // downloaded build can say what produced it. The test names the value the
    // same way the tool does, so a drift between the two is caught here.
    manifest.engine_version = fe::version::kEngineVersionString;

    auto text = manifest.to_json();
    REQUIRE(text);
    CHECK(text.value().find(fe::version::kEngineVersionString) != std::string::npos);

    ExportManifest parsed;
    REQUIRE_FALSE(ExportManifest::from_json(text.value(), parsed));
    CHECK(parsed.engine_version == fe::version::kEngineVersionString);

    // A manifest written before the field existed has none, which reads as an
    // unrecorded version rather than a guess, and a wrong type is rejected
    // rather than coerced.
    ExportManifest old;
    REQUIRE_FALSE(ExportManifest::from_json(
        R"({"format":"fork_export","format_version":1,"pack":"g.fepack","scene":"s.fescene"})", old));
    CHECK(old.engine_version.empty());
    CHECK(ExportManifest::from_json(
        R"({"format":"fork_export","format_version":1,"pack":"g.fepack","scene":"s.fescene","engine_version":7})",
        old));
}

TEST_CASE("an export manifest round-trips its module list") {
    ExportManifest manifest;
    manifest.name = "WithModules";
    manifest.pack = "game.fepack";
    manifest.scene = "scenes/main.fescene";
    manifest.modules = {"gameplay.so", "libextra.so"};

    auto text = manifest.to_json();
    REQUIRE(text);

    ExportManifest parsed;
    REQUIRE_FALSE(ExportManifest::from_json(text.value(), parsed));
    REQUIRE(parsed.modules.size() == 2);
    CHECK(parsed.modules[0] == "gameplay.so");
    CHECK(parsed.modules[1] == "libextra.so");

    // An empty list is omitted from the document, so a data-only build's
    // manifest is unchanged from before modules existed.
    ExportManifest plain;
    plain.pack = "game.fepack";
    plain.scene = "scenes/main.fescene";
    auto plain_text = plain.to_json();
    REQUIRE(plain_text);
    CHECK(plain_text.value().find("modules") == std::string::npos);

    // A wrong shape is rejected, not silently treated as "no modules".
    ExportManifest out;
    CHECK(ExportManifest::from_json(
        R"({"format":"fork_export","format_version":1,"pack":"g.fepack","scene":"s.fescene","modules":"one.so"})",
        out));
    CHECK(ExportManifest::from_json(
        R"({"format":"fork_export","format_version":1,"pack":"g.fepack","scene":"s.fescene","modules":[1]})", out));
}

// The web export template: the shell a browser loads. The test checks the one
// thing that can silently break a build - the file names the shell fetches
// against the names the export writes - and that a manifest from before targets
// existed still reads as desktop.
TEST_CASE("an export manifest records the target it was built for") {
    ExportManifest desktop;
    desktop.pack = "game.fepack";
    desktop.scene = "scenes/main.fescene";
    auto desktop_text = desktop.to_json();
    REQUIRE(desktop_text);
    // A desktop build is the default and is not written, so its document is what
    // it was before targets existed.
    CHECK(desktop_text.value().find("target") == std::string::npos);

    ExportManifest web;
    web.pack = "game.fepack";
    web.scene = "scenes/main.fescene";
    web.target = ExportManifest::kTargetWeb;
    auto web_text = web.to_json();
    REQUIRE(web_text);
    CHECK(web_text.value().find("\"target\": \"web\"") != std::string::npos);

    ExportManifest parsed;
    REQUIRE_FALSE(ExportManifest::from_json(web_text.value(), parsed));
    CHECK(parsed.target == ExportManifest::kTargetWeb);

    // The Android target round-trips too, and is written because it is not the
    // default: an export step that wrote the project but dropped the target from
    // the manifest would leave a build no launcher could tell apart.
    ExportManifest android;
    android.pack = "game.fepack";
    android.scene = "scenes/main.fescene";
    android.target = ExportManifest::kTargetAndroid;
    auto android_text = android.to_json();
    REQUIRE(android_text);
    CHECK(android_text.value().find("\"target\": \"android\"") != std::string::npos);
    ExportManifest android_parsed;
    REQUIRE_FALSE(ExportManifest::from_json(android_text.value(), android_parsed));
    CHECK(android_parsed.target == ExportManifest::kTargetAndroid);

    // The iOS target round-trips too, for the same reason: an export step that
    // wrote the project but dropped the target would leave a build no launcher
    // could tell apart.
    ExportManifest ios;
    ios.pack = "game.fepack";
    ios.scene = "scenes/main.fescene";
    ios.target = ExportManifest::kTargetIos;
    auto ios_text = ios.to_json();
    REQUIRE(ios_text);
    CHECK(ios_text.value().find("\"target\": \"ios\"") != std::string::npos);
    ExportManifest ios_parsed;
    REQUIRE_FALSE(ExportManifest::from_json(ios_text.value(), ios_parsed));
    CHECK(ios_parsed.target == ExportManifest::kTargetIos);

    // A document with no target is a desktop build: that is the only target that
    // could have produced it.
    ExportManifest legacy;
    REQUIRE_FALSE(ExportManifest::from_json(
        R"({"format":"fork_export","format_version":1,"pack":"g.fepack","scene":"s.fescene"})", legacy));
    CHECK(legacy.target == ExportManifest::kTargetDesktop);

    // A target from the future is rejected rather than guessed at.
    ExportManifest unknown;
    CHECK(ExportManifest::from_json(
        R"({"format":"fork_export","format_version":1,"pack":"g.fepack","scene":"s.fescene","target":"dreamcast"})",
        unknown));
}

TEST_CASE("the web shell fetches exactly the files the export writes") {
    // The names are the contract between the shell's fetch and the export's
    // writes. A mismatch is a 404 at load, which looks like a broken game rather
    // than a broken build, so it is asserted directly.
    const std::string js = web_shell_js();
    CHECK(js.find(std::string("\"") + WebExportNames::kRuntimeJs + "\"") != std::string::npos);
    CHECK(js.find(std::string("\"") + ExportManifest::kDefaultFileName + "\"") != std::string::npos);

    ExportManifest manifest;
    manifest.name = "ShellTest";
    manifest.pack = "game.fepack";
    manifest.scene = "scenes/main.fescene";
    manifest.target = ExportManifest::kTargetWeb;

    const std::string html = web_index_html(manifest);
    // The page loads the shell, and the shell is what the export writes.
    CHECK(html.find(std::string(WebExportNames::kShellJs)) != std::string::npos);
    // The canvas the engine's web backend defaults to must exist in the page, or
    // the backend creates one at the wrong size.
    CHECK(html.find("id=\"canvas\"") != std::string::npos);
    // The project's name reaches the tab title.
    CHECK(html.find("ShellTest") != std::string::npos);

    // The shell refuses a build that is not its own, so serving a desktop export
    // from a web server fails with a sentence instead of a blank canvas.
    CHECK(js.find("manifest.target") != std::string::npos);
}

TEST_CASE("the web shell is mobile-shaped: full-bleed canvas and viewport meta") {
    // The acceptance test for `mobile-viewport-meta`. A phone browser without a
    // viewport meta renders at desktop scale, and a fixed-aspect box centred in a
    // scrollable page letterboxes the game and puts its edges outside the element
    // the backend binds touch to. Both are asserted here because both fail
    // silently: the game loads and looks merely wrong.
    ExportManifest manifest;
    manifest.name = "Phone";
    manifest.pack = "game.fepack";
    manifest.scene = "scenes/main.fescene";
    manifest.target = ExportManifest::kTargetWeb;

    const std::string html = web_index_html(manifest);

    // The viewport meta must exist and must ask for the phone's own width. Without
    // `width=device-width` the page is laid out at a nominal desktop width.
    const std::string viewport_meta = "<meta name=\"viewport\"";
    const auto meta_at = html.find(viewport_meta);
    REQUIRE(meta_at != std::string::npos);
    const std::string meta = html.substr(meta_at, html.find('>', meta_at) - meta_at);
    CHECK(meta.find("width=device-width") != std::string::npos);
    CHECK(meta.find("initial-scale=1") != std::string::npos);
    // `viewport-fit=cover` is what makes the safe-area insets non-zero on a
    // notched phone; without it the insets are always zero and the safe-area item
    // has nothing to offset.
    CHECK(meta.find("viewport-fit=cover") != std::string::npos);

    // The canvas fills the viewport: a fixed inset of 0 on every edge and a full
    // width/height, with no max-width that would letterbox a wide phone.
    CHECK(html.find("#canvas { position: fixed; inset: 0; width: 100%; height: 100%") != std::string::npos);
    // The page does not scroll, or a touch-drag would scroll the page instead of
    // reaching the game.
    CHECK(html.find("overflow: hidden") != std::string::npos);
    // The browser must not claim a touch as a scroll or a pinch before the game
    // sees it.
    CHECK(html.find("touch-action: none") != std::string::npos);

    // The page's title carries the version the build was made with, when the
    // manifest recorded it, so a tab and a bookmark say which engine produced
    // the build (docs/SHIPPING_QUEUE.md, `release-version-stamping`). A manifest without
    // one leaves the title exactly as it was.
    ExportManifest versioned = manifest;
    versioned.engine_version = "9.8.7";
    CHECK(web_index_html(versioned).find("<title>Phone 9.8.7</title>") != std::string::npos);
    CHECK(web_index_html(manifest).find("<title>Phone</title>") != std::string::npos);

    // The page carries the project's mark as an SVG favicon, inline so the export
    // stays one page plus its module and pack. It follows the browser's colour
    // scheme, because a white mark on a white tab bar would be invisible.
    CHECK(html.find("<link rel=\"icon\" type=\"image/svg+xml\" href=\"data:image/svg+xml,") != std::string::npos);
    CHECK(html.find("prefers-color-scheme:dark") != std::string::npos);

    // The shell reports its own layout so the headless check can prove the canvas
    // fills a phone-sized window, and reads the safe-area insets from CSS.
    const std::string js = web_shell_js();
    CHECK(js.find("FE_SHELL_LAYOUT") != std::string::npos);
    CHECK(js.find("env(safe-area-inset-top") != std::string::npos);
    // The report must be re-taken when the window's CSS box changes, not written
    // once at startup: a phone rotates and a browser bar moves, and a stale
    // measurement would report the wrong canvas box exactly when the
    // phone-shaped check reads it.
    CHECK(js.find("addEventListener('resize'") != std::string::npos);
    CHECK(js.find("addEventListener('orientationchange'") != std::string::npos);
    // The insets reach the runtime as the direct-form flag, and the flag exists on
    // the player. A shell that measured them and never passed them would be the
    // same silent failure as the bindings drop this queue started from.
    CHECK(js.find("'--safe-area'") != std::string::npos);
    // The name and the engine version travel as argv too, because the runtime is
    // handed the pack and the scene rather than the manifest. Without them a
    // browser build's title is the engine's default and cannot say what made it.
    CHECK(js.find("'--name'") != std::string::npos);
    CHECK(js.find("'--engine-version'") != std::string::npos);
    // `env()` reports CSS pixels but a touch region is a fraction of the
    // viewport, so the shell must divide before it passes them down. A raw pixel
    // value would be read as a fraction and could clamp the whole axis away.
    CHECK(js.find("/ h") != std::string::npos);
    CHECK(js.find("/ w") != std::string::npos);
}

TEST_CASE("the web shell asks for fullscreen from the first touch and swallows a refusal") {
    // The acceptance test for `touch-fullscreen-prompt`. A phone browser's window
    // is not the whole screen: the address bar and tab strip take a band above a
    // full-bleed canvas, and a landscape thumb can hit that band instead of the
    // game. The browser only grants fullscreen from a user gesture, so the shell
    // asks on the earliest one a game reliably gets and treats every refusal as a
    // no-op: the game is already playable in the frame it has.
    //
    // A headless Chromium in CI cannot grant fullscreen to a synthetic event, so
    // the behaviour is proved by asserting the shell asks from the gesture and
    // that each failure path is caught rather than surfaced as an error.
    const std::string js = web_shell_js();

    // The request is a named function that resolves the canvas the backend binds
    // its own listeners to, so the gesture is taken on the element a touch that
    // reaches gameplay also reaches.
    CHECK(js.find("function requestFullscreenOnFirstTouch()") != std::string::npos);
    CHECK(js.find("document.getElementById(CANVAS_ID)") != std::string::npos);
    // The standard API and its `webkit`-prefixed spelling, because an older
    // WebKit-based mobile browser reports only the latter.
    CHECK(js.find("requestFullscreen") != std::string::npos);
    CHECK(js.find("webkitRequestFullscreen") != std::string::npos);

    // The gesture binding: the first `touchstart` and the first `pointerdown` on
    // the canvas, each `{ once: true }` so the request happens at most once, and
    // passive so the listener cannot delay the first frame's input.
    CHECK(js.find("canvas.addEventListener('touchstart'") != std::string::npos);
    CHECK(js.find("canvas.addEventListener('pointerdown'") != std::string::npos);
    CHECK(js.find("{ once: true, passive: true }") != std::string::npos);
    // The function must actually run at load, or the listener is never bound.
    CHECK(js.find("requestFullscreenOnFirstTouch();") != std::string::npos);

    // Each refusal path is caught. A missing API is a guard rather than a throw;
    // a rejected promise (the common desktop case) is caught on the result; a
    // synchronous throw is caught by the try. None of them reaches `fail`.
    CHECK(js.find("typeof request !== 'function'") != std::string::npos);
    CHECK(js.find(".catch(") != std::string::npos);
    CHECK(js.find("catch (error)") != std::string::npos);
}

TEST_CASE("writing the web shell produces the page and its loader") {
    TempDir dir("web_shell");

    ExportManifest manifest;
    manifest.name = "Written";
    manifest.pack = "game.fepack";
    manifest.scene = "scenes/main.fescene";
    manifest.target = ExportManifest::kTargetWeb;

    // A manifest that names no pack or scene is refused: the shell would have
    // nothing to load, and writing a page that fetches an empty name is worse
    // than not writing one.
    ExportManifest incomplete;
    incomplete.target = ExportManifest::kTargetWeb;
    CHECK(write_web_shell(dir.str(), incomplete));

    REQUIRE_FALSE(write_web_shell(dir.str(), manifest));
    CHECK(fs::exists(fs::join(dir.str(), WebExportNames::kIndexHtml)));
    CHECK(fs::exists(fs::join(dir.str(), WebExportNames::kShellJs)));

    auto index = fs::read_text_file(fs::join(dir.str(), WebExportNames::kIndexHtml));
    REQUIRE(index);
    CHECK(index.value().find("Written") != std::string::npos);

    auto shell = fs::read_text_file(fs::join(dir.str(), WebExportNames::kShellJs));
    REQUIRE(shell);
    CHECK(shell.value().find(WebExportNames::kRuntimeJs) != std::string::npos);
}

// The Android export template: the Gradle project a phone build is. The tests
// check the contract that fails only on a device - the names Gradle, the
// manifest, and the JNI CMakeLists each resolve - and that a manifest from
// before the target existed is unaffected. They never invoke Gradle or the NDK:
// the template is pure, so the shape is testable on a desktop CI, which is the
// point of landing it before the runtime backend.
TEST_CASE("the android project names the runtime the manifest loads") {
    ExportManifest manifest;
    manifest.name = "PhoneGame";
    manifest.pack = "game.fepack";
    manifest.scene = "scenes/main.fescene";
    manifest.target = ExportManifest::kTargetAndroid;

    AndroidProjectOptions options;

    // The manifest declares a native activity, which is the app's entry point
    // when there is no Java code. Without it the APK has no launcher activity and
    // the app installs but does not start.
    const std::string xml = android_manifest_xml(manifest, options);
    CHECK(xml.find("android.app.NativeActivity") != std::string::npos);
    CHECK(xml.find("android:hasCode=\"false\"") != std::string::npos);
    CHECK(xml.find("android.intent.category.LAUNCHER") != std::string::npos);
    // The library the activity loads must be the one the JNI CMakeLists builds,
    // or the activity starts and finds no library. Both come from the same
    // option, so this asserts the option reaches both files.
    const std::string jni = android_jni_cmake_lists(options);
    CHECK(xml.find("android:value=\"" + options.native_library + "\"") != std::string::npos);
    CHECK(jni.find("add_library(" + options.native_library + " SHARED") != std::string::npos);
    // The library is SHARED because it is loaded by name; a static library would
    // be linked into nothing.
    CHECK(jni.find("SHARED") != std::string::npos);
    // The engine is built from source for the phone, not copied: the JNI
    // CMakeLists adds the engine tree the export was pointed at.
    CHECK(jni.find("add_subdirectory(") != std::string::npos);
    CHECK(jni.find("FE::engine") != std::string::npos);
    // A game ships a runtime, not an editor or a test suite.
    CHECK(jni.find("set(FE_BUILD_EDITOR OFF") != std::string::npos);
    CHECK(jni.find("set(FE_BUILD_TESTS OFF") != std::string::npos);

    // The application id reaches both the Gradle namespace and the app id, so
    // Gradle does not derive a different one from a directory name.
    const std::string app = android_app_build_gradle(manifest, options);
    CHECK(app.find("namespace \"" + options.application_id + "\"") != std::string::npos);
    CHECK(app.find("applicationId \"" + options.application_id + "\"") != std::string::npos);
    // The native build is wired to the JNI CMakeLists the export writes.
    CHECK(app.find("externalNativeBuild") != std::string::npos);
    CHECK(app.find("src/main/jni/CMakeLists.txt") != std::string::npos);
    // The SDK levels are the options', not hardcoded.
    CHECK(app.find("minSdk " + std::to_string(options.min_sdk)) != std::string::npos);
    CHECK(app.find("targetSdk " + std::to_string(options.target_sdk)) != std::string::npos);
}

TEST_CASE("the android application id is derived, valid, and lowercase") {
    AndroidProjectOptions options;
    options.application_id.clear(); // ask for the derived default

    ExportManifest manifest;
    manifest.pack = "game.fepack";
    manifest.scene = "scenes/main.fescene";
    manifest.name = "My Game!";
    // A project name with a space and punctuation must still yield a valid,
    // all-lowercase application id: the Play Store rejects anything else, and a
    // build that installs locally but cannot be published is a trap.
    const std::string app = android_app_build_gradle(manifest, options);
    CHECK(app.find("com.forkengine.my.game") != std::string::npos);

    // A name that starts with a digit cannot start a Java identifier segment, so
    // it is prefixed rather than emitted as an invalid id.
    manifest.name = "3dGame";
    const std::string digit = android_app_build_gradle(manifest, options);
    CHECK(digit.find("com.forkengine.x3dgame") != std::string::npos);

    // A project that names no name at all still produces a valid id.
    manifest.name.clear();
    const std::string unnamed = android_app_build_gradle(manifest, options);
    CHECK(unnamed.find("com.forkengine.game") != std::string::npos);

    // An explicit id is used verbatim, so a caller can set the package a store
    // expects.
    options.application_id = "com.example.shooter";
    const std::string explicit_id = android_app_build_gradle(manifest, options);
    CHECK(explicit_id.find("com.example.shooter") != std::string::npos);
    CHECK(explicit_id.find("com.forkengine.game") == std::string::npos);
}

TEST_CASE("writing the android project produces a Gradle tree and its assets directory") {
    TempDir dir("android_project");

    ExportManifest manifest;
    manifest.name = "Written";
    manifest.pack = "game.fepack";
    manifest.scene = "scenes/main.fescene";
    manifest.target = ExportManifest::kTargetAndroid;

    AndroidProjectOptions options;
    options.engine_source_dir = "/opt/forkengine";
    options.player_source = "/opt/forkengine/tools/player/src/main.cpp";

    // A manifest that names no pack or scene is refused: the project would
    // package nothing, and a build that fails on a phone is worse than one that
    // fails here.
    ExportManifest incomplete;
    incomplete.target = ExportManifest::kTargetAndroid;
    CHECK(write_android_project(dir.str(), incomplete, options));

    REQUIRE_FALSE(write_android_project(dir.str(), manifest, options));
    for (const char* relative : {AndroidExportNames::kSettingsGradle, AndroidExportNames::kRootBuildGradle,
                                 AndroidExportNames::kGradleProperties, AndroidExportNames::kAppBuildGradle,
                                 AndroidExportNames::kAndroidManifest, AndroidExportNames::kJniCMakeLists}) {
        CHECK(fs::exists(fs::join(dir.str(), relative)));
    }
    // The assets directory exists even before the pack is copied in, so a Gradle
    // run that finds it empty is a valid project rather than a missing source
    // set.
    CHECK(fs::is_directory(fs::join(dir.str(), AndroidExportNames::kAssetsDir)));

    // The engine source directory reaches the JNI CMakeLists, so the project
    // builds the checkout it was pointed at rather than a path guessed from the
    // working directory.
    auto jni = fs::read_text_file(fs::join(dir.str(), AndroidExportNames::kJniCMakeLists));
    REQUIRE(jni);
    CHECK(jni.value().find("/opt/forkengine") != std::string::npos);
    CHECK(jni.value().find("/opt/forkengine/tools/player/src/main.cpp") != std::string::npos);
}

// The iOS export template: the CMake/Xcode project an app-bundle build is. The
// tests check the contract that fails only on a Mac - the names CMake and Xcode
// resolve, the plist keys iOS requires to launch, and that the pack travels in
// the bundle - and that a manifest from before the target existed is unaffected.
// They never invoke CMake or Xcode: the template is pure, so the shape is
// testable on a Linux or Windows CI, which is the point of landing it before the
// runtime backend.
TEST_CASE("the ios project builds the runtime as an app bundle carrying the pack") {
    ExportManifest manifest;
    manifest.name = "PhoneGame";
    manifest.pack = "game.fepack";
    manifest.scene = "scenes/main.fescene";
    manifest.target = ExportManifest::kTargetIos;
    manifest.engine_version = "0.3.1";

    IosProjectOptions options;
    options.engine_source_dir = "/opt/forkengine";
    options.player_source = "/opt/forkengine/tools/player/src/main.cpp";

    const std::string cmake = ios_project_cmake_lists(manifest, options);
    // The runtime is built as an app bundle, which is what makes CMake produce a
    // .app rather than a bare executable under the iOS toolchain.
    CHECK(cmake.find("add_executable(fork_player MACOSX_BUNDLE") != std::string::npos);
    CHECK(cmake.find("target_link_libraries(fork_player PRIVATE FE::engine)") != std::string::npos);
    // The engine is built from source for the target, not copied.
    CHECK(cmake.find("add_subdirectory(\"/opt/forkengine\"") != std::string::npos);
    // A game ships a runtime, not an editor or a test suite.
    CHECK(cmake.find("set(FE_BUILD_EDITOR OFF") != std::string::npos);
    CHECK(cmake.find("set(FE_BUILD_TESTS OFF") != std::string::npos);
    // The plist the project points at must be the one the export writes, or the
    // bundle has no Info.plist and iOS refuses to install it.
    CHECK(cmake.find(std::string("MACOSX_BUNDLE_INFO_PLIST") + " \"${CMAKE_CURRENT_SOURCE_DIR}/" +
                     IosExportNames::kInfoPlist + "\"") != std::string::npos);
    // The pack and manifest travel in the bundle's Resources, from the assets
    // directory the export fills.
    CHECK(cmake.find("copy_directory") != std::string::npos);
    CHECK(cmake.find(std::string("${CMAKE_CURRENT_SOURCE_DIR}/") + IosExportNames::kAssetsDir) != std::string::npos);
    CHECK(cmake.find("TARGET_BUNDLE_CONTENT_DIR:fork_player") != std::string::npos);
    // The version reaches the bundle from the manifest, not a literal.
    CHECK(cmake.find("MACOSX_BUNDLE_BUNDLE_VERSION \"0.3.1\"") != std::string::npos);
}

TEST_CASE("the ios Info.plist carries the keys iOS requires to launch") {
    ExportManifest manifest;
    manifest.name = "PhoneGame";
    manifest.pack = "game.fepack";
    manifest.scene = "scenes/main.fescene";
    manifest.target = ExportManifest::kTargetIos;

    IosProjectOptions options;
    const std::string plist = ios_info_plist(manifest, options);

    // CFBundleExecutable must name the bundle's executable, which CMake names
    // after the target; a mismatch means iOS cannot find the binary to run.
    CHECK(plist.find("<key>CFBundleExecutable</key>") != std::string::npos);
    CHECK(plist.find("<string>fork_player</string>") != std::string::npos);
    // The identifier and package type are what make it an installable app.
    CHECK(plist.find("<key>CFBundleIdentifier</key>") != std::string::npos);
    CHECK(plist.find("<string>" + options.bundle_id + "</string>") != std::string::npos);
    CHECK(plist.find("<string>APPL</string>") != std::string::npos);
    // The deployment target comes from the options, so the plist and the
    // toolchain agree about the minimum OS.
    CHECK(plist.find("<key>MinimumOSVersion</key>") != std::string::npos);
    CHECK(plist.find("<string>" + options.deployment_target + "</string>") != std::string::npos);
    // A game is fullscreen and landscape: a windowed plist is rejected or shows
    // a status bar over the canvas.
    CHECK(plist.find("<key>UILaunchScreen</key>") != std::string::npos);
    CHECK(plist.find("UIStatusBarHidden") != std::string::npos);
    CHECK(plist.find("UIInterfaceOrientationLandscapeLeft") != std::string::npos);
    // The document is well-formed enough to parse: the XML declaration, a
    // doctype, and one closing plist tag.
    CHECK(plist.find("<?xml version=\"1.0\" encoding=\"UTF-8\"?>") != std::string::npos);
    CHECK(plist.find("<!DOCTYPE plist") != std::string::npos);
    CHECK(plist.find("</plist>") != std::string::npos);
}

TEST_CASE("the ios bundle id is derived, valid, and lowercase") {
    IosProjectOptions options;
    options.bundle_id.clear(); // ask for the derived default

    ExportManifest manifest;
    manifest.pack = "game.fepack";
    manifest.scene = "scenes/main.fescene";
    manifest.name = "My Game!";
    // A project name with a space and punctuation must still yield a valid,
    // lowercase bundle id: the signing tools reject anything else, and a build
    // that runs locally but cannot be signed is a trap.
    const std::string plist = ios_info_plist(manifest, options);
    CHECK(plist.find("com.forkengine.my.game") != std::string::npos);

    // A name that starts with a digit cannot start an identifier segment, so it
    // is prefixed rather than emitted as an invalid id.
    manifest.name = "3dGame";
    const std::string digit = ios_info_plist(manifest, options);
    CHECK(digit.find("com.forkengine.x3dgame") != std::string::npos);

    // A project that names no name at all still produces a valid id.
    manifest.name.clear();
    const std::string unnamed = ios_info_plist(manifest, options);
    CHECK(unnamed.find("com.forkengine.game") != std::string::npos);

    // An explicit id is used verbatim, so a caller can set the id a store
    // expects.
    options.bundle_id = "com.example.shooter";
    const std::string explicit_id = ios_info_plist(manifest, options);
    CHECK(explicit_id.find("com.example.shooter") != std::string::npos);
    CHECK(explicit_id.find("com.forkengine.game") == std::string::npos);
}

TEST_CASE("a game name with XML characters cannot break the plist") {
    ExportManifest manifest;
    manifest.name = "Tom & Jerry <Deluxe>";
    manifest.pack = "game.fepack";
    manifest.scene = "scenes/main.fescene";
    manifest.target = ExportManifest::kTargetIos;

    IosProjectOptions options;
    const std::string plist = ios_info_plist(manifest, options);
    // The raw characters must not appear unescaped, or the plist is not XML and
    // the app does not launch; the escaped forms must.
    CHECK(plist.find("Tom &amp; Jerry &lt;Deluxe&gt;") != std::string::npos);
    CHECK(plist.find("Tom & Jerry <Deluxe>") == std::string::npos);
}

TEST_CASE("writing the ios project produces a CMake tree and its assets directory") {
    TempDir dir("ios_project");

    ExportManifest manifest;
    manifest.name = "Written";
    manifest.pack = "game.fepack";
    manifest.scene = "scenes/main.fescene";
    manifest.target = ExportManifest::kTargetIos;

    IosProjectOptions options;
    options.engine_source_dir = "/opt/forkengine";
    options.player_source = "/opt/forkengine/tools/player/src/main.cpp";

    // A manifest that names no pack or scene is refused: the project would
    // package nothing, and a build that fails on a Mac is worse than one that
    // fails here.
    ExportManifest incomplete;
    incomplete.target = ExportManifest::kTargetIos;
    CHECK(write_ios_project(dir.str(), incomplete, options));

    REQUIRE_FALSE(write_ios_project(dir.str(), manifest, options));
    for (const char* relative : {IosExportNames::kProjectCMakeLists, IosExportNames::kInfoPlist}) {
        CHECK(fs::exists(fs::join(dir.str(), relative)));
    }
    // The assets directory exists even before the pack is copied in, so a build
    // that finds it empty is a valid project rather than a missing copy source.
    CHECK(fs::is_directory(fs::join(dir.str(), IosExportNames::kAssetsDir)));

    // The engine source directory reaches the project, so it builds the checkout
    // it was pointed at rather than a path guessed from the working directory.
    auto cmake = fs::read_text_file(fs::join(dir.str(), IosExportNames::kProjectCMakeLists));
    REQUIRE(cmake);
    CHECK(cmake.value().find("/opt/forkengine") != std::string::npos);
    CHECK(cmake.value().find("/opt/forkengine/tools/player/src/main.cpp") != std::string::npos);
}

// The shipping flow, in process: author a scene with a bound script, pack the
// project tree, delete the loose files, then start the way fork_player does -
// mount the pack and load the startup scene with a reading ScriptBinding. This
// is the claim ADR-0064 makes, tested without spawning the tool and the runtime.
TEST_CASE("an exported pack starts a scene with its script bindings") {
    TempDir dir("end_to_end");
    const std::string assets_root = dir.file("assets");
    const std::string scenes_root = dir.file("scenes");
    REQUIRE_FALSE(fs::create_directories(fs::join(assets_root, "textures")));
    REQUIRE_FALSE(fs::create_directories(scenes_root));

    // A real asset so the pack has content beyond the scene, imported so it
    // carries a stable id (the shipped build resolves it from the pack alone).
    REQUIRE_FALSE(fs::write_binary_file(fs::join(assets_root, "textures/tile.txt"), "tile", 4));

    // Author the scene, binding a behaviour with a non-default property value.
    const std::string scene_key = "scenes/main.fescene";
    const std::string scene_path = fs::join(scenes_root, "main.fescene");
    {
        ScriptRegistry registry = make_registry();
        Scene authored;
        Node* root = authored.create_node("Spinner");
        ScriptHost host;
        REQUIRE_FALSE(host.attach_script(*root, std::make_unique<SpinScript>()));

        // Set the bound script's speed through its reflected property, the way
        // the editor's inspector writes it (ADR-0063), so the exported document
        // carries a value that differs from the behaviour's default.
        const ScriptDesc* desc = registry.find("SpinScript");
        REQUIRE(desc != nullptr);
        const reflect::Property* speed = reflect::resolve_property(desc->properties, {"speed"});
        REQUIRE(speed != nullptr);
        REQUIRE(reflect::write_property(*speed, host.script(root->id()), reflect::Value{4.5}));

        script::ScriptBinding writer(host, registry);
        REQUIRE_FALSE(serialization::scene::save(authored, scene_path, nullptr, &writer));
    }

    // Pack the tree exactly as fork_export does, then remove the loose files.
    AssetDatabase assets;
    assets.set_root(dir.str());
    auto built = PackWriter::build(assets, scene_path, scene_key);
    REQUIRE(built);
    auto bytes = built.value().write();
    REQUIRE(bytes);
    const std::string pack_path = dir.file("game.fepack");
    REQUIRE_FALSE(fs::write_binary_file(pack_path, bytes.value().data(), bytes.value().size()));
    std::filesystem::remove_all(assets_root);
    std::filesystem::remove_all(scenes_root);

    // Write the manifest beside the pack, as the export step does.
    ExportManifest manifest;
    manifest.name = "ExportTest";
    manifest.pack = "game.fepack";
    manifest.scene = scene_key;
    REQUIRE_FALSE(manifest.save(dir.file(ExportManifest::kDefaultFileName)));

    // Start the way the runtime does: read the manifest, mount the pack, load
    // the scene through a reading binding.
    auto loaded_manifest = ExportManifest::load(dir.file(ExportManifest::kDefaultFileName));
    REQUIRE(loaded_manifest);

    auto source = PackSource::open(fs::join(dir.str(), loaded_manifest.value().pack));
    REQUIRE(source);
    ResourceManager manager;
    manager.mount(source.value());

    ScriptRegistry registry = make_registry();
    ScriptHost host;
    script::ScriptBinding reader(host, registry);

    Scene loaded;
    REQUIRE_FALSE(serialization::scene::load(loaded_manifest.value().scene, loaded, &manager, &reader));

    // The scene came back scripted, from the pack, with the authored value.
    Node* spinner = loaded.find_node_by_name("Spinner");
    REQUIRE(spinner != nullptr);
    REQUIRE(host.script_count() == 1);
    const auto* script = dynamic_cast<const SpinScript*>(host.script(spinner->id()));
    REQUIRE(script != nullptr);
    CHECK(script->speed_ == doctest::Approx(4.5f));
}

// The runtime's module step, in process: a manifest records a gameplay module
// beside it by file name, the player resolves that name against the manifest's
// directory, loads the image, and the behaviour it registers is then available
// to the scene's bindings. This is what makes an exported build of a game with
// gameplay code complete (ADR-0061, ADR-0064).
TEST_CASE("a manifest's module is resolved and loaded the way the player does") {
    TempDir dir("modules");
    const std::string module_source = FE_TEST_MODULE_A;
    const std::string module_name = fs::filename(module_source);
    REQUIRE_FALSE(fs::copy_file(module_source, dir.file(module_name)));

    // The export step records the module by file name, relative to the manifest.
    ExportManifest manifest;
    manifest.name = "ModuleTest";
    manifest.pack = "game.fepack";
    manifest.scene = "scenes/main.fescene";
    manifest.modules = {module_name};
    REQUIRE_FALSE(manifest.save(dir.file(ExportManifest::kDefaultFileName)));

    // The runtime reads the manifest, resolves the module against its directory,
    // and loads it. This is load_export() in fork_player, spelled out.
    auto loaded = ExportManifest::load(dir.file(ExportManifest::kDefaultFileName));
    REQUIRE(loaded);
    REQUIRE(loaded.value().modules.size() == 1);
    const std::string module_path = fs::join(dir.str(), loaded.value().modules[0]);
    REQUIRE(fs::exists(module_path));

    ScriptLibrary library;
    REQUIRE_FALSE(library.load_module(module_path));
    // The module's behaviour is now registered, so a scene binding it by name
    // gets an instance rather than an "unknown behaviour" error.
    CHECK(library.registry().contains("SpinBehaviour"));

    // A module the manifest names but that is not beside it fails loudly, rather
    // than starting the game with the behaviour silently missing.
    ScriptLibrary empty_library;
    CHECK(empty_library.load_module(fs::join(dir.str(), "absent_module.so")));
}

// The player runs with the export directory as its working directory and the
// manifest names the module by a bare file name, which resolves (through
// fork_player's resolve_against) to just that name. On Unix the loader does not
// search the working directory, so the bare name has to be resolved here or a
// shipped build fails to start - which is exactly how this regressed once
// (ADR-0064). The test reproduces the player's shape: working directory is the
// export directory, module named with no directory at all.
TEST_CASE("a module named with no directory loads from the working directory") {
    TempDir dir("bare_module");
    const std::string module_name = fs::filename(FE_TEST_MODULE_A);
    REQUIRE_FALSE(fs::copy_file(FE_TEST_MODULE_A, dir.file(module_name)));

    const std::filesystem::path previous = std::filesystem::current_path();
    std::filesystem::current_path(dir.str());
    // Restore the working directory before the TempDir destructor runs, and even
    // if an assertion below throws.
    struct Restore {
        const std::filesystem::path& to;
        ~Restore() {
            std::error_code ec;
            std::filesystem::current_path(to, ec);
        }
    } restore{previous};

    // resolve_against(".", name) normalizes "./name" to a bare "name", which is
    // what the player hands the loader.
    CHECK(fs::normalize(fs::join(".", module_name)) == module_name);

    ScriptLibrary library;
    REQUIRE_FALSE(library.load_module(module_name));
    CHECK(library.registry().contains("SpinBehaviour"));
}
