// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
//
// The Fork Engine editor shell.
//
// A thin entry point over `fe::editor::EditorHost`, which embeds an Engine,
// holds the document, and runs the ImGui UI. This file only parses arguments
// and starts the host; all editor behaviour lives in the editor libraries so it
// stays testable.
//
// Usage:
//   fork_editor_shell                 open the built-in example scene
//   fork_editor_shell --project DIR   open the project in DIR (or a project
//                                     file); indexes its asset root
//   fork_editor_shell --project-file F  open the project file F
//   fork_editor_shell --scene PATH    open a scene file
//   fork_editor_shell --module PATH   load a gameplay module (repeatable); it is
//                                     hot-reloaded when its file is rebuilt
//
// Controls: the Hierarchy panel lists the scene; the Play panel starts and
// stops play-in-editor; close the window to exit.

#include "fe/core/FileSystem.h"
#include "fe/core/Log.h"
#include "fe/editor/EditorHost.h"
#include "fe/editor/ProjectSettings.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct Options {
    std::string project_file;
    std::string project_root;
    std::string scene_to_open;
    // Gameplay module shared libraries to load and hot-reload while the editor
    // runs (ADR-0061, ADR-0066). Repeatable: --module a --module b.
    std::vector<std::string> modules;
    fe::LogLevel log_level = fe::LogLevel::Info;
    fe::u32 max_frames = 0;
    bool headless = false;
    // The editor caches imports so a re-import of an unchanged asset (a scene
    // reload, a restart) is a read instead of a re-decode. It is on by default
    // when a project is open, rooted at "<project>/.fork/cache". --no-cache
    // turns it off; --cache-dir overrides the location. See
    // docs/adr/0029-import-cache.md.
    bool import_cache = true;
    std::string import_cache_dir;
};

// The directory a project lives in, used to place its import cache beside it.
// A project file's directory is its parent; a --project directory is itself.
// Empty when neither is given, in which case there is no project to cache for.
[[nodiscard]] std::string project_directory(const Options& options) {
    if (!options.project_root.empty()) {
        return options.project_root;
    }
    if (!options.project_file.empty()) {
        return fe::fs::parent_path(options.project_file);
    }
    return {};
}

// A `--project` argument is either a project file or a directory. A file that
// ends in ".json" is opened as-is; anything else is treated as a directory and
// the default project file name is looked for inside it.
[[nodiscard]] std::string resolve_project_argument(const std::string& argument) {
    if (argument.size() >= 5 && argument.compare(argument.size() - 5, 5, ".json") == 0) {
        return argument;
    }
    return fe::fs::join(argument, fe::editor::ProjectSettings::kDefaultFileName);
}

[[nodiscard]] Options parse_options(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--project" && i + 1 < argc) {
            options.project_file = resolve_project_argument(argv[++i]);
        } else if (arg == "--project-file" && i + 1 < argc) {
            options.project_file = argv[++i];
        } else if (arg == "--scene" && i + 1 < argc) {
            options.scene_to_open = argv[++i];
        } else if (arg == "--module" && i + 1 < argc) {
            options.modules.emplace_back(argv[++i]);
        } else if (arg == "--frames" && i + 1 < argc) {
            options.max_frames = static_cast<fe::u32>(std::strtoul(argv[++i], nullptr, 10));
        } else if (arg == "--headless") {
            // Run with a hidden window and stop after a few frames; useful for a
            // smoke test where no display is available.
            options.headless = true;
            if (options.max_frames == 0) {
                options.max_frames = 3;
            }
        } else if (arg == "--no-cache") {
            options.import_cache = false;
        } else if (arg == "--cache-dir" && i + 1 < argc) {
            options.import_cache_dir = argv[++i];
        } else if (arg == "--verbose") {
            options.log_level = fe::LogLevel::Debug;
        } else if (arg == "--quiet") {
            options.log_level = fe::LogLevel::Warn;
        } else if (arg == "--help" || arg == "-h") {
            std::puts(
                "usage: fork_editor_shell [--project DIR|FILE] [--project-file FILE] [--scene PATH] [--module PATH] "
                "[--headless] [--frames N] [--cache-dir DIR] [--no-cache] [--verbose] [--quiet]");
            std::exit(0);
        }
    }
    return options;
}

} // namespace

int main(int argc, char** argv) {
    const Options options = parse_options(argc, argv);

    fe::editor::EditorHost::Config config;
    config.engine.window.title = "Fork Engine Editor";
    config.engine.window.width = 1280;
    config.engine.window.height = 720;
    config.engine.window.visible = !options.headless;
    config.engine.log_level = options.log_level;
    // Cache imports by default. The root is the project's ".fork/cache" so it is
    // disposable with the project; an explicit --cache-dir wins, and with no
    // project and no --cache-dir the root stays empty and the cache is inert.
    if (options.import_cache) {
        std::string root = options.import_cache_dir;
        if (root.empty()) {
            const std::string dir = project_directory(options);
            if (!dir.empty()) {
                root = fe::fs::join(fe::fs::join(dir, ".fork"), "cache");
            }
        }
        config.engine.cache_root = root;
    }
    config.project_file = options.project_file;
    config.project_root = options.project_root;
    config.scene_to_open = options.scene_to_open;
    config.modules = options.modules;
    config.max_frames = options.max_frames;

    fe::editor::EditorHost host(std::move(config));
    return host.run();
}
