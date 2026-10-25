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
#include "fe/pack/HttpFetchSource.h"
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

// A web build mounts its pack over HTTP and lets the browser own the frame
// schedule, so it needs Emscripten's loop entry point instead of the blocking
// desktop loop. See docs/adr/0071-web-first-2d-shipping-path.md.
#if defined(FE_PLATFORM_WEB)
#include <emscripten.h>
#endif

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <set>
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
    // Stop after this many frames and exit cleanly. 0 means run until the window
    // closes. It is the interactive counterpart of --headless N: the web build
    // needs it because a browser page owns the frame schedule and a check has to
    // bound a run to observe it (a game otherwise runs until the tab closes).
    fe::u32 frames = 0;
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
        if (fe::Error error = mount_pack(); error) {
            return error;
        }

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
        // A bounded interactive run stops through the same door a window close
        // does: request_exit(), which step_frame() (and so run()) tests at the top
        // of the next frame. The frame just counted is therefore still drawn and
        // presented before the loop ends.
        if (options_.frames > 0 && frame_count_ >= options_.frames) {
            request_exit();
        }
    }

    void on_render(fe::f32) override {
        if (!engine().has_graphics() && !engine().headless_render()) {
            return;
        }
        // Application's frame draws 2D; 3D is the app's own step, and only when
        // the scene actually has a 3D camera. A 2D game therefore pays nothing for
        // the 3D renderer being present, and a 3D game gets its pass without the
        // player having to be told which it is.
        if (fe::renderer3d::Renderer3D::find_active_camera(scene()) != nullptr) {
            auto drawn =
                engine().renderer_3d().render(engine().device(), scene(), engine().window().framebuffer_width(),
                                              engine().window().framebuffer_height());
            if (!drawn) {
                FE_LOG_ERROR("fork_player: 3D render failed: %s", drawn.error().message().c_str());
            }
        }
        // Read the frame back while it is still the current framebuffer. A web
        // canvas is presented implicitly and, with preserveDrawingBuffer off (the
        // default this window asks for), its contents are undefined after the
        // callback returns -- so the readback has to happen here, in the frame
        // that drew, and not later in shutdown(). See report_frame().
        if (options_.frames > 0 && frame_count_ >= options_.frames) {
            report_frame();
        }
    }

    void on_stop() override {
        // Machine-checkable: what ran, and that it ran from a pack. A CI job greps
        // these lines, exactly as it does for the headless example.
        FE_LOG_INFO("fork_player: %llu frame(s), %llu script(s)", static_cast<unsigned long long>(frame_count_),
                    static_cast<unsigned long long>(host_.script_count()));
    }

private:
    // Reads the middle of the last frame and reports how varied it is, then hands
    // the verdict to the page on web. A blank frame has one colour; a rendered 2D
    // scene has several, so the distinct-colour count is a blankness check that
    // needs no golden image and does not change when a game's art changes.
    void report_frame() {
        const fe::u32 width = engine().window().framebuffer_width();
        const fe::u32 height = engine().window().framebuffer_height();
        if (width == 0 || height == 0) {
            return;
        }
        // A centred window of the frame, small enough to read cheaply on a
        // software renderer and large enough to cover the middle of the screen.
        const fe::u32 sample_w = std::min<fe::u32>(width, 64);
        const fe::u32 sample_h = std::min<fe::u32>(height, 64);
        const fe::u32 x = (width - sample_w) / 2;
        const fe::u32 y = (height - sample_h) / 2;
        std::vector<fe::u8> pixels(static_cast<std::size_t>(sample_w) * sample_h * 4);
        engine().device().read_pixels(x, y, sample_w, sample_h, pixels);

        std::set<std::uint32_t> distinct;
        for (std::size_t i = 0; i + 3 < pixels.size(); i += 4) {
            const std::uint32_t packed =
                static_cast<std::uint32_t>(pixels[i]) | (static_cast<std::uint32_t>(pixels[i + 1]) << 8) |
                (static_cast<std::uint32_t>(pixels[i + 2]) << 16) | (static_cast<std::uint32_t>(pixels[i + 3]) << 24);
            distinct.insert(packed);
        }

        FE_LOG_INFO("fork_player: frame readback %ux%u, %llu distinct colour(s)", static_cast<unsigned int>(sample_w),
                    static_cast<unsigned int>(sample_h), static_cast<unsigned long long>(distinct.size()));

#if defined(FE_PLATFORM_WEB)
        // The report a headless-browser check reads back out of the page. The
        // engine's log goes to the browser console, which --dump-dom does not
        // show, so the verdict is written into the title where the harness can
        // see it, the same way the web smoke test reports. `canvas` is the
        // window's live-context flag: a WebGL2 context can only be obtained from
        // a canvas, so a 1 here is the check that the shell provided one.
        EM_ASM(
            { document.title = 'FE_PLAYER_RESULT canvas=' + $0 + ' frames=' + $1 + ' distinct=' + $2; },
            engine().window().has_context() ? 1 : 0, static_cast<int>(frame_count_), static_cast<int>(distinct.size()));
#endif
    }

    // Mounts the pack the run was pointed at. On desktop that is a file read
    // through the filesystem; on web there is no filesystem, so the pack is
    // fetched over HTTP from the page's own origin. The choice is made here, once,
    // so everything after it -- the loaders, the scene reader, the script
    // bindings -- is identical on both platforms and never asks which it is.
    [[nodiscard]] fe::Error mount_pack() {
#if defined(FE_PLATFORM_WEB)
        // The shell passes the manifest's pack path, which is relative to the
        // page, so an empty base URL lets the browser resolve it against the
        // page's own origin.
        auto source = fe::pack::HttpFetchSource::open(std::string{}, options_.pack);
#else
        auto source = fe::pack::PackSource::open(options_.pack);
#endif
        if (!source) {
            return source.error();
        }
        engine().resources().mount(source.value());
        FE_LOG_INFO("fork_player: mounted '%s' (%llu entries)", options_.pack.c_str(),
                    static_cast<unsigned long long>(source.value()->pack().entry_count()));
        return fe::Error::none();
    }

    Options options_;
    fe::ScriptHost host_;
    fe::ScriptLibrary library_;
    std::unique_ptr<fe::script::ScriptBinding> binding_;
    fe::u64 frame_count_ = 0;
};

void print_usage() {
    std::puts("usage: fork_player --export DIR [--frames N] [--headless N]\n"
              "       fork_player --pack FILE --scene KEY [--module PATH ...] [--frames N] [--headless N]\n"
              "\n"
              "  --export DIR    play the export in DIR, reading its fork.export.json\n"
              "                  (its pack, scene, and gameplay modules)\n"
              "  --pack FILE     a pack file to mount (direct form)\n"
              "  --scene KEY     the startup scene's key inside the pack (direct form)\n"
              "  --module PATH   an extra gameplay module to load (repeatable)\n"
              "  --frames N      run N frames and exit (the web build's bounded run)\n"
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
        } else if (arg == "--frames" && has_value) {
            out.frames = static_cast<fe::u32>(std::strtoul(argv[++i], nullptr, 10));
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

// Builds the engine configuration for a validated option set. Shared by the
// desktop entry point and the web one, so a web run and a desktop run differ only
// in how the loop is driven, never in how the engine is configured.
[[nodiscard]] fe::Engine::Config make_config(const Options& options) {
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
    return config;
}

#if defined(FE_PLATFORM_WEB)

namespace {

// The application the browser's frame callback drives. A browser page owns the
// frame schedule, so the player cannot block in a `while` loop the way the
// desktop entry point does: the module returns to the page and
// requestAnimationFrame calls back into this object once per frame. The
// Application primitives (initialize/step_frame/shutdown) exist precisely for a
// host like this.
std::unique_ptr<PlayerApp> g_app;

void web_frame(void* /*arg*/) {
    // step_frame() returns false once the run is over (an exit request, a
    // headless frame count, or a window close). Cancel the loop and tear down
    // there, so shutdown() runs exactly once and no frame follows it.
    if (g_app == nullptr || !g_app->step_frame()) {
        emscripten_cancel_main_loop();
        if (g_app != nullptr) {
            g_app->shutdown();
            g_app.reset();
        }
    }
}

} // namespace

int main(int argc, char** argv) {
    Options options;
    if (!parse_options(argc, argv, options)) {
        return 2;
    }
    fe::log::set_min_level(options.verbose ? fe::LogLevel::Debug
                                           : (options.quiet ? fe::LogLevel::Warn : fe::LogLevel::Info));

    if (options.pack.empty() || options.scene.empty()) {
        std::fprintf(stderr, "fork_player: a pack and a scene are required (--pack FILE --scene KEY)\n");
        return 2;
    }

    // No filesystem to probe on web: the pack is fetched at mount time, and a
    // missing one fails there with a clear message. Checking `fs::exists` here
    // would always answer false (Emscripten's MEMFS is empty), so it is not done.

    g_app = std::make_unique<PlayerApp>(make_config(options), std::move(options));
    if (fe::Error error = g_app->initialize(); error) {
        std::fprintf(stderr, "fork_player: %s\n", error.message().c_str());
        g_app.reset();
        return 1;
    }

    // Frame rate 0 means "as fast as the browser schedules", which is
    // requestAnimationFrame's own cadence. The loop runs until step_frame() says
    // the run is done; the callback above cancels it.
    emscripten_set_main_loop_arg(web_frame, nullptr, 0, 0);
    return 0;
}

#else // desktop

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

    PlayerApp app(make_config(options), std::move(options));
    if (options.headless) {
        app.set_headless(options.headless_frames);
    }
    return app.run();
}

#endif // FE_PLATFORM_WEB
