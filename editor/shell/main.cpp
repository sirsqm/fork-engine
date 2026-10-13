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
//   fork_editor_shell --scene PATH    open a scene file
//
// Controls: the Hierarchy panel lists the scene; the Play panel starts and
// stops play-in-editor; close the window to exit.

#include "fe/core/Log.h"
#include "fe/editor/EditorHost.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

namespace {

struct Options {
    std::string scene_to_open;
    fe::LogLevel log_level = fe::LogLevel::Info;
    fe::u32 max_frames = 0;
    bool headless = false;
};

[[nodiscard]] Options parse_options(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--scene" && i + 1 < argc) {
            options.scene_to_open = argv[++i];
        } else if (arg == "--frames" && i + 1 < argc) {
            options.max_frames = static_cast<fe::u32>(std::strtoul(argv[++i], nullptr, 10));
        } else if (arg == "--headless") {
            // Run with a hidden window and stop after a few frames; useful for a
            // smoke test where no display is available.
            options.headless = true;
            if (options.max_frames == 0) {
                options.max_frames = 3;
            }
        } else if (arg == "--verbose") {
            options.log_level = fe::LogLevel::Debug;
        } else if (arg == "--quiet") {
            options.log_level = fe::LogLevel::Warn;
        } else if (arg == "--help" || arg == "-h") {
            std::puts("usage: fork_editor_shell [--scene PATH] [--headless] [--frames N] [--verbose] [--quiet]");
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
    config.scene_to_open = options.scene_to_open;
    config.max_frames = options.max_frames;

    fe::editor::EditorHost host(std::move(config));
    return host.run();
}
