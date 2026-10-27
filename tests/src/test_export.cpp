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
#include "fe/pack/ExportManifest.h"
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

    // The shell reports its own layout so the headless check can prove the canvas
    // fills a phone-sized window rather than trusting the stylesheet alone.
    const std::string js = web_shell_js();
    CHECK(js.find("FE_SHELL_LAYOUT") != std::string::npos);

    // A phone game wants the whole screen, so the shell asks for fullscreen from
    // the first touch and swallows a refusal: a browser that declines leaves the
    // game playable, so a rejection must not surface as an error.
    CHECK(js.find("requestFullscreen") != std::string::npos);
    CHECK(js.find("'touchstart'") != std::string::npos);
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
