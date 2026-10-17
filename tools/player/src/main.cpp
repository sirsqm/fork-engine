// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
//
// fork_player - the shipping runtime: mount a pack and play it.
//
// The second half of M8's export path (docs/adr/0064-export-runtime-and-player.md).
// fork_pack turns a project into one pack file; this program mounts that pack and
// runs the ordinary game loop, so a pack is something a player can be handed
// rather than an artifact only a test can read.
//
// It is deliberately generic. A build produced by fork_export is this binary, a
// pack, and a manifest - the game's identity is its data, not a per-game compiled
// entry point. That keeps a compiler out of the export step and means an exported
// build is reproducible from the pack alone.
//
// It links FE::engine and nothing else: no editor, no ImGui. A shipped runtime
// should carry only what it runs.
//
// Usage:
//   fork_player --export DIR             play the export in DIR (reads its manifest,
//                                        including the modules it records)
//   fork_player --pack FILE --scene KEY  play a pack directly
//   fork_player ... --module PATH        load an extra gameplay module (repeatable)
//   fork_player ... --headless N         run N frames with no window
//   fork_player ... --render             run the render path with the null device
//
// The manifest (fork.export.json) names the pack, the startup scene, and the
// gameplay modules, so the --export form is what a launcher or a packager uses;
// --pack/--scene is for a developer running a pack in place.

#include "fe/core/FileSystem.h"
#include "fe/core/Log.h"
#include "fe/pack/ExportManifest.h"
#include "fe/pack/PackSource.h"
#include "fe/renderer3d/Renderer3D.h"
#include "fe/runtime/Application.h"
#include "fe/scene/Scene.h"
#include "fe/script/ScriptBinding.h"
#include "fe/script/ScriptHost.h"
#include "fe/script/ScriptLibrary.h"
#include "fe/script/ScriptRegistry.h"
#include "fe/script/ScriptSystem.h"
#include "fe/serialization/SceneSerializer.h"
#include "fe/systems/SceneSchedule.h"

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

// Everything the run needs to find its content, resolved from the command line.
struct Options {
    std::string pack;
    std::string scene;
    std::vector<std::string> modules;
    std::string name;
    fe::u32 headless_frames = 0;
    bool headless = false;
    bool render = false;
    bool verbose = false;
    bool quiet = false;
};

// Resolves a manifest-relative path against the directory the manifest lives in,
// so an export directory can be moved or copied whole. An absolute path is left
// alone.
[[nodiscard]] std::string resolve_against(const std::string& directory, const std::string& path) {
    if (path.empty() || fe::fs::is_absolute(path)) {
        return path;
    }
    return fe::fs::normalize(fe::fs::join(directory, path));
}

// Reads an export manifest and fills in the pack, scene, and name it records. A
// directory (or a path with no extension) is taken to be the export directory,
// and the manifest's fixed file name is looked for inside it.
[[nodiscard]] fe::Error load_export(const std::string& path, Options& out) {
    std::string file = path;
    if (fe::fs::extension(path).empty()) {
        file = fe::fs::join(path, fe::pack::ExportManifest::kDefaultFileName);
    }

    auto manifest = fe::pack::ExportManifest::load(file);
    if (!manifest) {
        return manifest.error();
    }
    const std::string directory = fe::fs::parent_path(fe::fs::normalize(file));
    out.pack = resolve_against(directory, manifest.value().pack);
    out.scene = manifest.value().scene;
    out.name = manifest.value().name;
    // A manifest's modules are recorded beside it, so resolve each against the
    // manifest's directory. Modules named on the command line are appended after
    // these, so a developer can add one to an exported build without re-exporting.
    for (const std::string& module : manifest.value().modules) {
        out.modules.push_back(resolve_against(directory, module));
    }
    return fe::Error::none();
}

class PlayerApp final : public fe::Application {
public:
    PlayerApp(fe::Engine::Config config, Options options) : fe::Application(config), options_(std::move(options)) {}

protected:
    fe::Error on_start() override {
        // Mount the pack before any content load. Every loader the engine
        // registered during initialize() reads through the manager's source, so
        // from here on the scene and its assets come from the pack and nothing
        // reads a loose file (ADR-0030).
        auto source = fe::pack::PackSource::open(options_.pack);
        if (!source) {
            return source.error();
        }
        engine().resources().mount(source.value());
        FE_LOG_INFO("fork_player: mounted '%s' (%llu entries)", options_.pack.c_str(),
                    static_cast<unsigned long long>(source.value()->pack().entry_count()));

        // Load the gameplay modules named on the command line. Each registers the
        // behaviours it provides into the registry the scene's script bindings
        // resolve through, so a scene that records a behaviour by name gets a live
        // instance back (ADR-0061).
        for (const std::string& module : options_.modules) {
            // A module that will not load is fatal: the scene almost certainly
            // needs its behaviours, and running half a game is worse than saying
            // why it did not start.
            if (fe::Error error = library_.load_module(module); error) {
                return error;
            }
            FE_LOG_INFO("fork_player: loaded module '%s'", module.c_str());
        }

        // The scene's script bindings are read back through the host and the
        // registry, so the behaviours a scene records are attached during the load
        // itself (ADR-0063). This is the same seam the editor persists through; a
        // shipped build just supplies a registry with the game's modules in it.
        binding_ = std::make_unique<fe::script::ScriptBinding>(host_, library_.registry());
        if (fe::Error error =
                fe::serialization::scene::load(options_.scene, scene(), &engine().resources(), binding_.get());
            error) {
            return error;
        }

        // Drive the scripts from the ordinary schedule, exactly as the editor's
        // play mode and the tests do: no new execution model for a shipped build.
        schedule().add_system<fe::ScriptSystem>(host_);

        FE_LOG_INFO("fork_player: scene '%s' has %llu node(s), %llu script(s)", options_.scene.c_str(),
                    static_cast<unsigned long long>(scene().node_count()),
                    static_cast<unsigned long long>(host_.script_count()));
        return fe::Error::none();
    }

    void on_update(fe::f32 delta_time) override {
        (void)delta_time;
        ++frame_count_;
    }

    void on_render(fe::f32) override {
        // Application's frame draws 2D; 3D is the app's own step, and only when
        // the scene actually has a 3D camera. A 2D game therefore pays nothing for
        // the 3D renderer being present, and a 3D game gets its pass without the
        // player having to be told which it is.
        if (!engine().has_graphics() && !engine().headless_render()) {
            return;
        }
        if (fe::renderer3d::Renderer3D::find_active_camera(scene()) == nullptr) {
            return;
        }
        auto drawn = engine().renderer_3d().render(engine().device(), scene(), engine().window().framebuffer_width(),
                                                   engine().window().framebuffer_height());
        if (!drawn) {
            FE_LOG_ERROR("fork_player: 3D render failed: %s", drawn.error().message().c_str());
        }
    }

    void on_stop() override {
        // Machine-checkable: what ran, and that it ran from a pack. A CI job greps
        // these lines, exactly as it does for the headless example.
        FE_LOG_INFO("fork_player: %llu frame(s), %llu script(s)", static_cast<unsigned long long>(frame_count_),
                    static_cast<unsigned long long>(host_.script_count()));
    }

private:
    Options options_;
    fe::ScriptHost host_;
    fe::ScriptLibrary library_;
    std::unique_ptr<fe::script::ScriptBinding> binding_;
    fe::u64 frame_count_ = 0;
};

void print_usage() {
    std::puts("usage: fork_player --export DIR [--headless N]\n"
              "       fork_player --pack FILE --scene KEY [--module PATH ...] [--headless N]\n"
              "\n"
              "  --export DIR    play the export in DIR, reading its fork.export.json\n"
              "                  (its pack, scene, and gameplay modules)\n"
              "  --pack FILE     a pack file to mount (direct form)\n"
              "  --scene KEY     the startup scene's key inside the pack (direct form)\n"
              "  --module PATH   an extra gameplay module to load (repeatable)\n"
              "  --headless N    run N frames with no window\n"
              "  --render        run the render path against the null RHI device\n"
              "  --verbose       log at debug level\n"
              "  --quiet         log warnings and above only");
}

[[nodiscard]] bool parse_options(int argc, char** argv, Options& out) {
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        const bool has_value = i + 1 < argc;
        if (arg == "--export" && has_value) {
            if (fe::Error error = load_export(argv[++i], out); error) {
                std::fprintf(stderr, "fork_player: %s\n", error.message().c_str());
                return false;
            }
        } else if (arg == "--pack" && has_value) {
            out.pack = argv[++i];
        } else if (arg == "--scene" && has_value) {
            out.scene = argv[++i];
        } else if (arg == "--module" && has_value) {
            out.modules.emplace_back(argv[++i]);
        } else if (arg == "--headless" && has_value) {
            out.headless = true;
            out.headless_frames = static_cast<fe::u32>(std::strtoul(argv[++i], nullptr, 10));
        } else if (arg == "--render") {
            out.render = true;
        } else if (arg == "--verbose") {
            out.verbose = true;
        } else if (arg == "--quiet") {
            out.quiet = true;
        } else if (arg == "--help" || arg == "-h") {
            print_usage();
            std::exit(0);
        } else {
            std::fprintf(stderr, "fork_player: unknown or incomplete argument '%s'\n", std::string(arg).c_str());
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
    fe::log::set_min_level(options.verbose ? fe::LogLevel::Debug
                                           : (options.quiet ? fe::LogLevel::Warn : fe::LogLevel::Info));

    if (options.pack.empty() || options.scene.empty()) {
        std::fprintf(stderr,
                     "fork_player: a pack and a scene are required (--export DIR, or --pack FILE --scene KEY)\n");
        return 2;
    }
    if (!fe::fs::exists(options.pack)) {
        std::fprintf(stderr, "fork_player: pack '%s' does not exist\n", options.pack.c_str());
        return 1;
    }

    fe::Engine::Config config;
    config.log_level = fe::log::min_level();
    config.window.title = options.name.empty() ? "Fork Engine" : options.name;
    config.window.width = 1280;
    config.window.height = 720;
    if (options.headless) {
        // No OS window and no GL context: the null platform backend (ADR-0041).
        // With --render the render path still runs against the null RHI device
        // (ADR-0042), so a shipped build can be exercised in CI.
        config.window.backend = fe::WindowBackend::Null;
        config.window.visible = false;
        config.window.vsync = false;
        config.headless_render = options.render;
    }

    const bool headless = options.headless;
    const fe::u32 headless_frames = options.headless_frames;
    PlayerApp app(config, std::move(options));
    if (headless) {
        app.set_headless(headless_frames);
    }
    return app.run();
}
