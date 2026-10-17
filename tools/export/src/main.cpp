// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
//
// fork_export - turn a project into a runnable build.
//
// The export step of M8 (docs/adr/0064-export-runtime-and-player.md). A project
// directory goes in; a directory a player can be handed comes out:
//
//   build/export/<name>/
//     fork_player[.exe]     the runtime
//     game.fepack           the assets and the startup scene
//     fork.export.json      the manifest naming the pack and the scene
//
// The pack is written by the engine's PackWriter, the same code fork_pack uses,
// so the two tools cannot disagree about the format. The player is copied, not
// rebuilt: the runtime is generic and driven by the pack, so export has no
// compiler in it and cannot fail for toolchain reasons.
//
// Usage:
//   fork_export --project DIR --out DIR [--player PATH] [--pack-name NAME]
//               [--scene-key PATH]
//
// --player names the runtime to copy. It defaults to the fork_player beside this
// executable, which is where the build puts it.

#include "fe/core/FileSystem.h"
#include "fe/core/Log.h"
#include "fe/pack/ExportManifest.h"
#include "fe/pack/PackWriter.h"
#include "fe/resources/AssetDatabase.h"

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
    std::vector<std::string> modules; // gameplay modules to copy in and record
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
    std::puts("usage: fork_export --project DIR --out DIR [--player PATH] [--pack-name NAME]\n"
              "                   [--scene-key PATH] [--module PATH ...]\n"
              "\n"
              "  --project DIR    a project directory or fork.project.json file\n"
              "  --out DIR        the directory to write the build into\n"
              "  --player PATH    the runtime to copy (default: fork_player beside this tool)\n"
              "  --pack-name NAME the pack file's name (default: game.fepack)\n"
              "  --scene-key PATH the scene's key inside the pack (default: project-relative)\n"
              "  --module PATH    a gameplay module to copy in and record (repeatable)\n"
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
        } else if (arg == "--module" && has_value) {
            out.modules.emplace_back(argv[++i]);
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

    // Resolve the runtime to copy. The default is the fork_player the build put
    // beside this tool, which is what makes the ordinary invocation a single
    // command with no path bookkeeping.
    std::string player = options.player;
    if (player.empty()) {
        const std::string directory = executable_directory(argv[0]);
        player = fe::fs::join(directory.empty() ? "." : directory, player_file_name());
    }
    if (!fe::fs::exists(player)) {
        std::fprintf(stderr, "fork_export: no runtime at '%s'. Build fork_player, or pass --player PATH.\n",
                     player.c_str());
        return 1;
    }

    // Index the project tree so every imported asset goes into the pack with its
    // scheme and stable id; the pack stays self-describing (ADR-0020, ADR-0030).
    fe::AssetDatabase assets;
    assets.set_root(pack_root);
    FE_LOG_INFO("fork_export: indexed %llu asset(s) under '%s'", static_cast<unsigned long long>(assets.asset_count()),
                pack_root.c_str());

    // Build the pack in memory first, then write the whole export. A failure
    // here (a missing scene, an unreadable asset) leaves no half-written build.
    auto built = fe::pack::PackWriter::build(assets, project.value().startup_scene, scene_key);
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

    // Copy the runtime in. Copied byte for byte, with its permission bits, so
    // the exported player is executable: the export must not depend on a
    // toolchain, and the same player serves every game (ADR-0064).
    const std::string player_out = fe::fs::join(options.out, player_file_name());
    if (fe::Error error = fe::fs::copy_file(player, player_out); error) {
        std::fprintf(stderr, "fork_export: %s\n", error.message().c_str());
        return 1;
    }

    // The manifest names what to run, so a launcher does not have to infer it.
    fe::pack::ExportManifest manifest;
    manifest.name = project.value().name;
    manifest.pack = pack_name;
    manifest.scene = scene_key;

    // Copy each gameplay module in beside the player and record it by its file
    // name, so the player loads it from the export directory. Modules are named
    // on the command line rather than discovered: which libraries a game needs
    // is a property of the game, and a build should record exactly what it was
    // told to ship.
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

    FE_LOG_INFO("fork_export: wrote '%s' (%llu pack entries) with runtime '%s'", options.out.c_str(),
                static_cast<unsigned long long>(built.value().entry_count()), player_file_name().c_str());
    return 0;
}
