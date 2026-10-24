// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
//
// fork_export - turn a project into a runnable build.
//
// The export step of M8 (docs/adr/0064-export-runtime-and-player.md). A project
// directory goes in; a directory a player can be handed comes out:
//
//   build/export/<name>/          --target desktop (the default)
//     fork_player[.exe]     the runtime
//     game.fepack           the assets and the startup scene
//     fork.export.json      the manifest naming the pack and the scene
//
//   build/export/<name>/          --target web
//     index.html            the page a player opens
//     fork_shell.js         the loader that starts the wasm runtime
//     fork_player.js        the wasm runtime's JS glue
//     fork_player.wasm      the wasm binary
//     game.fepack           the assets and the startup scene
//     fork.export.json      the manifest, naming the target and the pack
//
// The pack is written by the engine's PackWriter, the same code fork_pack uses,
// so the two tools cannot disagree about the format. On desktop the player is
// copied, not rebuilt: the runtime is generic and driven by the pack, so export
// has no compiler in it and cannot fail for toolchain reasons. A web build cannot
// copy a runtime, because the player is a wasm module the page loads
// asynchronously; the export assembles the wasm pair Emscripten produced and
// writes a shell beside it (docs/adr/0071-web-first-2d-shipping-path.md).
//
// Usage:
//   fork_export --project DIR --out DIR [--target desktop|web] [--player PATH]
//               [--pack-name NAME] [--scene-key PATH]
//
// --player names the runtime to copy (desktop) or to assemble (web). It defaults
// to the fork_player beside this executable, or fork_player.js for web, which is
// where the build puts it.

#include "fe/core/FileSystem.h"
#include "fe/core/Log.h"
#include "fe/pack/ExportManifest.h"
#include "fe/pack/PackWriter.h"
#include "fe/pack/WebExportTemplate.h"
#include "fe/resources/AssetDatabase.h"
#include "fe/serialization/SceneReferences.h"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct Options {
    std::string project;              // a project directory or file
    std::string out;                  // output directory
    std::string player;               // runtime to copy; default: beside this executable
    std::string pack_name;            // pack file name inside the output directory
    std::string scene_key;            // key the scene is stored under; default: project-relative
    std::string target = "desktop";   // "desktop" (copy the player) or "web" (assemble a shell)
    std::vector<std::string> modules; // gameplay modules to copy in and record
    bool prune = true;                // pack only what the startup scene references
    bool verbose = false;
};

// The subset of the project file the exporter needs. The engine does not know
// what a project is (docs/adr/0025-project-file.md), so this small reader lives
// with the tool that does - the same arrangement fork_pack makes.
struct ProjectFile {
    std::string name;
    std::string directory;     // the directory the project file lives in
    std::string startup_scene; // resolved against `directory`
};

[[nodiscard]] std::string resolve_against(const std::string& directory, const std::string& path) {
    if (path.empty()) {
        return {};
    }
    if (fe::fs::is_absolute(path)) {
        return fe::fs::normalize(path);
    }
    return fe::fs::normalize(fe::fs::join(directory, path));
}

[[nodiscard]] fe::Result<ProjectFile> read_project(const std::string& path) {
    std::string file = path;
    if (fe::fs::extension(path).empty()) {
        file = fe::fs::join(path, "fork.project.json");
    }

    auto text = fe::fs::read_text_file(file);
    if (!text) {
        return text.error();
    }

    nlohmann::json document;
    try {
        document = nlohmann::json::parse(text.value());
    } catch (const nlohmann::json::parse_error& e) {
        return fe::Error("project parse error in '" + file + "': " + e.what());
    }

    const std::string format = document.value("format", std::string{});
    if (format != "fork_project") {
        return fe::Error("not a fork project document (format='" + format + "')");
    }
    const fe::i32 version = document.value("format_version", 0);
    if (version != 1) {
        return fe::Error("project format version " + std::to_string(version) + " is not supported");
    }

    ProjectFile project;
    project.name = document.value("name", std::string{});
    project.directory = fe::fs::parent_path(fe::fs::normalize(file));
    project.startup_scene = resolve_against(project.directory, document.value("startup_scene", std::string{}));
    return project;
}

// Resolves the startup scene's resource references to root-relative asset paths,
// so a pruned pack carries exactly what the scene names. Reads the scene as text
// and traces it through reflection; it does not load it, so no resource is
// decoded.
//
// A reference resolves if the database has indexed it or, failing that, if the
// file is simply present under the pack root - an unimported asset is still
// packed, by path, under the default scheme, exactly as the unpruned writer packs
// it. A reference that resolves to neither is a hard error: shipping a build
// whose scene names an asset that is not in it produces a game that runs until it
// draws, which is worse than a failed export.
[[nodiscard]] fe::Result<std::vector<std::string>>
scene_asset_paths(const std::string& scene_path, const std::string& pack_root, const fe::AssetDatabase& assets) {
    auto text = fe::fs::read_text_file(scene_path);
    if (!text) {
        return text.error();
    }
    auto references = fe::serialization::scene::collect_resource_references(text.value());
    if (!references) {
        return references.error();
    }

    std::vector<std::string> paths;
    for (const std::string& reference : references.value()) {
        const std::string path = fe::serialization::scene::reference_to_asset_path(reference, assets);
        if (path.empty() || (assets.find_by_path(path) == nullptr && !fe::fs::exists(fe::fs::join(pack_root, path)))) {
            return fe::Error("scene reference '" + reference + "' is not an asset under '" + pack_root + "'");
        }
        paths.push_back(path);
    }
    return paths;
}

// The runtime's file name on this platform, so a copy finds it and a manifest
// reader can name it back.
[[nodiscard]] std::string player_file_name() {
#if defined(_WIN32)
    return "fork_player.exe";
#else
    return "fork_player";
#endif
}

// The directory this executable lives in, so the default player path is the one
// the build put beside it.
[[nodiscard]] std::string executable_directory(const char* argv0) {
    if (argv0 == nullptr || *argv0 == '\0') {
        return {};
    }
    return fe::fs::parent_path(fe::fs::normalize(argv0));
}

void print_usage() {
    std::puts("usage: fork_export --project DIR --out DIR [--target desktop|web] [--player PATH]\n"
              "                   [--pack-name NAME] [--scene-key PATH] [--module PATH ...] [--no-prune]\n"
              "\n"
              "  --project DIR    a project directory or fork.project.json file\n"
              "  --out DIR        the directory to write the build into\n"
              "  --target NAME    the platform to build for (default: desktop)\n"
              "                     desktop  copy the native player beside the pack\n"
              "                     web      assemble a wasm runtime, a pack, and an\n"
              "                              HTML/JS shell a browser loads\n"
              "  --player PATH    the runtime to copy or, for --target web, the wasm\n"
              "                   runtime to assemble (default: fork_player beside\n"
              "                   this tool, or fork_player.js for web)\n"
              "  --pack-name NAME the pack file's name (default: game.fepack)\n"
              "  --scene-key PATH the scene's key inside the pack (default: project-relative)\n"
              "  --module PATH    a gameplay module to copy in and record (repeatable)\n"
              "  --no-prune       pack the whole asset tree instead of only the\n"
              "                   assets the startup scene references. Pruning is\n"
              "                   the default (ADR-0065)\n"
              "  --verbose        log at debug level");
}

[[nodiscard]] bool parse_options(int argc, char** argv, Options& out) {
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        const bool has_value = i + 1 < argc;
        if (arg == "--project" && has_value) {
            out.project = argv[++i];
        } else if (arg == "--out" && has_value) {
            out.out = argv[++i];
        } else if (arg == "--player" && has_value) {
            out.player = argv[++i];
        } else if (arg == "--pack-name" && has_value) {
            out.pack_name = argv[++i];
        } else if (arg == "--scene-key" && has_value) {
            out.scene_key = argv[++i];
        } else if (arg == "--target" && has_value) {
            out.target = argv[++i];
        } else if (arg == "--module" && has_value) {
            out.modules.emplace_back(argv[++i]);
        } else if (arg == "--prune") {
            out.prune = true;
        } else if (arg == "--no-prune") {
            out.prune = false;
        } else if (arg == "--verbose") {
            out.verbose = true;
        } else if (arg == "--help" || arg == "-h") {
            print_usage();
            std::exit(0);
        } else {
            std::fprintf(stderr, "fork_export: unknown or incomplete argument '%s'\n", std::string(arg).c_str());
            return false;
        }
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    Options options;
    if (!parse_options(argc, argv, options)) {
        print_usage();
        return 2;
    }
    fe::log::set_min_level(options.verbose ? fe::LogLevel::Debug : fe::LogLevel::Info);

    if (options.project.empty() || options.out.empty()) {
        std::fprintf(stderr, "fork_export: --project and --out are required\n");
        return 2;
    }
    // The target decides what "the runtime" is: a native executable to copy, or
    // a wasm module plus a shell. Rejecting an unknown target here means the
    // rest of the tool has exactly two cases to reason about.
    const bool web = options.target == fe::pack::ExportManifest::kTargetWeb;
    if (!web && options.target != fe::pack::ExportManifest::kTargetDesktop) {
        std::fprintf(stderr, "fork_export: unknown target '%s' (expected 'desktop' or 'web')\n",
                     options.target.c_str());
        return 2;
    }

    auto project = read_project(options.project);
    if (!project) {
        std::fprintf(stderr, "fork_export: %s\n", project.error().message().c_str());
        return 1;
    }
    if (project.value().startup_scene.empty()) {
        std::fprintf(stderr, "fork_export: the project names no startup scene\n");
        return 1;
    }

    // The pack root is the project directory, not the asset root: a scene's asset
    // references are project-relative, so keying the pack the same way is what
    // makes them resolve in a shipped run (docs/adr/0030-asset-pack-format.md).
    const std::string pack_root = project.value().directory;
    const std::string scene_key =
        options.scene_key.empty() ? fe::fs::relative_to(pack_root, project.value().startup_scene) : options.scene_key;
    const std::string pack_name = options.pack_name.empty() ? "game.fepack" : options.pack_name;

    // Resolve the runtime. For desktop the default is the fork_player the build
    // put beside this tool, which is what makes the ordinary invocation a single
    // command with no path bookkeeping. For web there is no native executable to
    // copy - the runtime is the wasm module Emscripten emits - so the default is
    // the JS glue beside this tool, and its .wasm is expected next to it.
    std::string player = options.player;
    if (player.empty()) {
        const std::string directory = executable_directory(argv[0]);
        player = fe::fs::join(directory.empty() ? "." : directory,
                              web ? fe::pack::WebExportNames::kRuntimeJs : player_file_name());
    }
    if (!fe::fs::exists(player)) {
        std::fprintf(stderr, "fork_export: no runtime at '%s'. Build fork_player%s, or pass --player PATH.\n",
                     player.c_str(), web ? " for the web target (emcmake)" : "");
        return 1;
    }
    // For web the .wasm sits beside the JS glue; Emscripten writes both, so a
    // missing one means the build was not a web build and the export should say
    // so now rather than leave a shell that 404s at load.
    std::string wasm_source;
    if (web) {
        wasm_source = fe::fs::join(fe::fs::parent_path(player), fe::pack::WebExportNames::kWasmBinary);
        if (!fe::fs::exists(wasm_source)) {
            std::fprintf(stderr,
                         "fork_export: no '%s' beside '%s'. A web runtime is built with emcmake; "
                         "build fork_player for the web target, or pass --player PATH.\n",
                         fe::pack::WebExportNames::kWasmBinary, player.c_str());
            return 1;
        }
    }

    // Index the project tree so every imported asset goes into the pack with its
    // scheme and stable id; the pack stays self-describing (ADR-0020, ADR-0030).
    fe::AssetDatabase assets;
    assets.set_root(pack_root);
    FE_LOG_INFO("fork_export: indexed %llu asset(s) under '%s'", static_cast<unsigned long long>(assets.asset_count()),
                pack_root.c_str());

    // Build the pack in memory first, then write the whole export. A failure
    // here (a missing scene, an unreadable asset) leaves no half-written build.
    fe::pack::PackOptions pack_options;
    if (options.prune) {
        auto paths = scene_asset_paths(project.value().startup_scene, pack_root, assets);
        if (!paths) {
            std::fprintf(stderr, "fork_export: %s\n", paths.error().message().c_str());
            return 1;
        }
        pack_options.prune = true;
        pack_options.include = std::move(paths.value());
        FE_LOG_INFO("fork_export: pruning to %llu scene-referenced asset(s)",
                    static_cast<unsigned long long>(pack_options.include.size()));
    }

    auto built = fe::pack::PackWriter::build(assets, project.value().startup_scene, scene_key, pack_options);
    if (!built) {
        std::fprintf(stderr, "fork_export: %s\n", built.error().message().c_str());
        return 1;
    }
    auto pack_bytes = built.value().write();
    if (!pack_bytes) {
        std::fprintf(stderr, "fork_export: %s\n", pack_bytes.error().message().c_str());
        return 1;
    }

    if (fe::Error error = fe::fs::create_directories(options.out); error) {
        std::fprintf(stderr, "fork_export: %s\n", error.message().c_str());
        return 1;
    }

    const std::string pack_path = fe::fs::join(options.out, pack_name);
    if (fe::Error error = fe::fs::write_binary_file(pack_path, pack_bytes.value().data(), pack_bytes.value().size());
        error) {
        std::fprintf(stderr, "fork_export: %s\n", error.message().c_str());
        return 1;
    }

    // Assemble the runtime. Desktop copies the native player byte for byte, with
    // its permission bits, so the exported player is executable: the export must
    // not depend on a toolchain, and the same player serves every game
    // (ADR-0064). Web copies the wasm pair instead - there is no executable, and
    // the page loads the JS glue and its .wasm - and then writes the shell.
    if (web) {
        const std::string js_out = fe::fs::join(options.out, fe::pack::WebExportNames::kRuntimeJs);
        if (fe::Error error = fe::fs::copy_file(player, js_out); error) {
            std::fprintf(stderr, "fork_export: %s\n", error.message().c_str());
            return 1;
        }
        const std::string wasm_out = fe::fs::join(options.out, fe::pack::WebExportNames::kWasmBinary);
        if (fe::Error error = fe::fs::copy_file(wasm_source, wasm_out); error) {
            std::fprintf(stderr, "fork_export: %s\n", error.message().c_str());
            return 1;
        }
    } else {
        const std::string player_out = fe::fs::join(options.out, player_file_name());
        if (fe::Error error = fe::fs::copy_file(player, player_out); error) {
            std::fprintf(stderr, "fork_export: %s\n", error.message().c_str());
            return 1;
        }
    }

    // The manifest names what to run, so a launcher does not have to infer it.
    // The target is recorded too: the web shell refuses a build that is not its
    // own, and a desktop launcher can tell the two apart without probing names.
    fe::pack::ExportManifest manifest;
    manifest.name = project.value().name;
    manifest.target = options.target;
    manifest.pack = pack_name;
    manifest.scene = scene_key;

    // Copy each gameplay module in beside the player and record it by its file
    // name, so the player loads it from the export directory. Modules are named
    // on the command line rather than discovered: which libraries a game needs
    // is a property of the game, and a build should record exactly what it was
    // told to ship.
    //
    // A web build has no shared-library loader, so a module cannot be loaded in
    // a browser. Refusing rather than recording a module the shell would never
    // load keeps a build from claiming gameplay it cannot run.
    if (web && !options.modules.empty()) {
        std::fprintf(stderr, "fork_export: gameplay modules are not supported for the web target "
                             "(a browser has no shared-library loader)\n");
        return 1;
    }
    for (const std::string& module : options.modules) {
        if (!fe::fs::exists(module)) {
            std::fprintf(stderr, "fork_export: module '%s' does not exist\n", module.c_str());
            return 1;
        }
        const std::string module_name = fe::fs::filename(module);
        if (fe::Error error = fe::fs::copy_file(module, fe::fs::join(options.out, module_name)); error) {
            std::fprintf(stderr, "fork_export: %s\n", error.message().c_str());
            return 1;
        }
        manifest.modules.push_back(module_name);
    }

    if (fe::Error error = manifest.save(fe::fs::join(options.out, fe::pack::ExportManifest::kDefaultFileName)); error) {
        std::fprintf(stderr, "fork_export: %s\n", error.message().c_str());
        return 1;
    }

    // The shell is written last, after the manifest it reads, so a failure above
    // leaves no page pointing at files that were never written. The shell takes
    // the pack and scene from the manifest, so the two cannot disagree.
    if (web) {
        if (fe::Error error = fe::pack::write_web_shell(options.out, manifest); error) {
            std::fprintf(stderr, "fork_export: %s\n", error.message().c_str());
            return 1;
        }
    }

    FE_LOG_INFO("fork_export: wrote '%s' (%llu pack entries) for the %s target", options.out.c_str(),
                static_cast<unsigned long long>(built.value().entry_count()), options.target.c_str());
    return 0;
}
