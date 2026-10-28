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

#include "fe/audio/AudioBackend.h"
#include "fe/audio/AudioSystem.h"
#include "fe/core/FileSystem.h"
#include "fe/core/Log.h"
#include "fe/pack/ExportManifest.h"
#include "fe/pack/HttpFetchSource.h"
#include "fe/pack/PackSource.h"
#include "fe/physics2d/Collider2D.h"
#include "fe/physics2d/Physics2DSystem.h"
#include "fe/physics3d/Collider3D.h"
#include "fe/physics3d/Physics3DSystem.h"
#include "fe/player/ActionBindings.h"
#include "fe/player/BuiltinBehaviours.h"
#include "fe/renderer3d/Renderer3D.h"
#include "fe/runtime/Application.h"
#include "fe/runtime/AudioListenerSystem.h"
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
    // The engine version the build was produced with, read from the manifest so
    // a run can log it (docs/SHIPPING_QUEUE.md, `release-version-stamping`). Empty for a
    // build whose manifest predates the field, or a run pointed at a pack and a
    // scene directly rather than at an export directory.
    std::string engine_version;
    // The pack key of a declarative input-binding document, if the build ships
    // one (docs/SHIPPING_QUEUE.md, `touch-gesture-regions`). Read from the pack after the
    // engine's own declarations, so a game's touch regions are data rather than
    // code. Empty means the build declares no bindings of its own.
    std::string bindings;
    fe::u32 headless_frames = 0;
    // Stop after this many frames and exit cleanly. 0 means run until the window
    // closes. It is the interactive counterpart of --headless N: the web build
    // needs it because a browser page owns the frame schedule and a check has to
    // bound a run to observe it (a game otherwise runs until the tab closes).
    fe::u32 frames = 0;
    // Drive the input path end to end inside the browser: press a movement key
    // with a real synthetic DOM event, then report how far the watched node
    // moved. This is the acceptance test for "input works in the web build" — a
    // browser check can dispatch events but cannot press a key, so the player
    // does it and reports the result into the page. Ignored off the web target.
    bool probe_input = false;
    // The node whose movement the input probe measures. A scene names its camera
    // "Camera" by convention; a game that does not can override it.
    std::string probe_node = "Camera";
    // Drive the probe with a *touch* instead of a key: dispatch a synthetic
    // touchstart on the canvas at a normalised point and report how far the
    // watched node moved. This is the acceptance test for "touch works in the web
    // build" (docs/SHIPPING_QUEUE.md, `touch-gesture-regions`): a browser check can dispatch
    // an event but cannot touch a screen, so the player does it and reports the
    // result. Ignored off the web target.
    bool probe_touch = false;
    // Where the synthetic touch lands, normalised (0..1, origin top-left). The
    // default sits inside the built-in walk pad (the lower-left forward region),
    // so a run with no bindings document still proves the touch path; a project
    // that declares its own regions aims it where only its document covers.
    fe::f32 probe_touch_x = 0.1f;
    fe::f32 probe_touch_y = 0.6f;
    // The OS-reserved bands of the screen, as normalised fractions of the
    // viewport, in top,right,bottom,left order. The web shell measures them from
    // CSS `env(safe-area-inset-*)` and passes them here, because only the page
    // can read them and only the engine can apply them to touch regions
    // (docs/SHIPPING_QUEUE.md, `mobile-safe-area`). All zero on every other target.
    fe::f32 safe_top = 0.0f;
    fe::f32 safe_right = 0.0f;
    fe::f32 safe_bottom = 0.0f;
    fe::f32 safe_left = 0.0f;
    // A fixed simulation step in seconds, or 0 to step by the measured frame
    // time. The input probe pins it, so the distance it measures is a property of
    // the input and the behaviour rather than of how fast the browser happened to
    // run: a run under a headless browser has a wall-clock delta that varies
    // wildly, and a probe measuring it would be flaky by construction.
    fe::f32 fixed_step = 0.0f;
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
    out.engine_version = manifest.value().engine_version;
    // The bindings key is a path *inside the pack*, not beside the manifest, so
    // it is taken verbatim: resolving it against the manifest's directory would
    // turn a pack key into a filesystem path the pack does not use.
    out.bindings = manifest.value().bindings;
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

        // The engine's built-in behaviours are registered before any scene is
        // read, for every target. On desktop a game may also ship its own logic
        // as a gameplay module; on the web there is no dynamic loader, so these
        // are the only behaviours a browser build can run — which is what gives
        // an exported game logic to consume input at all (ADR-0078). A scene
        // binds them by name like any other behaviour, so nothing downstream
        // knows or cares which side of the process boundary they came from.
        fe::player::register_builtin_behaviours(library_.registry());

        // Declare the standard actions and bind them to a keyboard and a
        // touchscreen. This is what makes the built-in behaviours actually
        // respond to a player: the behaviours read actions, not keys, so a scene
        // that binds FlyCamera3D is playable on a desktop and a phone from the
        // same declaration (ADR-0075). A game's own module may add more actions
        // or rebind these; the standard names are the built-ins' contract.
        fe::player::declare_builtin_actions(actions());

        // A game's own bindings, if the build ships a document for them. Applied
        // after the standard set, so a game can add actions, rebind the built-ins
        // to its own layout, or declare touch regions of its own - and it does so
        // in data, without a rebuild (docs/SHIPPING_QUEUE.md, `touch-gesture-regions`).
        //
        // A malformed document is fatal rather than ignored: a game whose regions
        // silently did not load is a game whose touch controls do nothing, which is
        // worse than saying why it did not start. An absent one is the common case
        // and is not an error - most builds declare no bindings of their own.
        if (!options_.bindings.empty()) {
            auto text = engine().resources().read_text(options_.bindings);
            if (!text) {
                return fe::Error("fork_player: the manifest names bindings at '" + options_.bindings +
                                 "', which is not in the pack: " + text.error().message());
            }
            auto bindings = fe::player::ActionBindings::from_json(text.value());
            if (!bindings) {
                return bindings.error();
            }
            bindings.value().apply(actions());
            FE_LOG_INFO("fork_player: applied %llu declared action(s) from '%s'",
                        static_cast<unsigned long long>(bindings.value().actions.size()), options_.bindings.c_str());
        }

        // The OS-reserved screen bands, measured by the web shell and handed
        // down as normalised fractions. Applied after the bindings so they
        // constrain every declared region, the game's own and the built-ins'
        // alike. Zero on every other target, which is the identity mapping
        // (docs/SHIPPING_QUEUE.md, `mobile-safe-area`).
        actions().set_safe_area_insets(options_.safe_top, options_.safe_right, options_.safe_bottom,
                                       options_.safe_left);
        if (options_.safe_top > 0.0f || options_.safe_right > 0.0f || options_.safe_bottom > 0.0f ||
            options_.safe_left > 0.0f) {
            FE_LOG_INFO("fork_player: safe-area insets t=%.3f r=%.3f b=%.3f l=%.3f",
                        static_cast<double>(options_.safe_top), static_cast<double>(options_.safe_right),
                        static_cast<double>(options_.safe_bottom), static_cast<double>(options_.safe_left));
        }

        // The gameplay modules named on the command line. Each registers the
        // behaviours it provides into the registry the scene's script bindings
        // resolve through, so a scene that records a behaviour by name gets a live
        // instance back (ADR-0061). A module may override a built-in by
        // registering the same name, which is the documented replace-on-collision
        // rule.
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
        //
        // Order is semantically significant here, not incidental (ADR-0008): a
        // script runs first and writes the motion it wants (a controller sets its
        // body's velocity), and the physics step runs after it and integrates
        // that motion against the world. Reversed, a script would be overwriting
        // the collision response the step just produced, and a character would
        // walk through a wall.
        schedule().add_system<fe::ScriptSystem>(host_);

        // Physics runs only for the dimensions the scene actually uses. A 2D game
        // must not pay for a 3D broad phase (or the reverse), and the player is
        // generic: it can see the loaded scene before it builds the schedule, so
        // it asks rather than assuming. This is the same choice on_render() makes
        // for the 3D renderer, and it is what lets one player run every game.
        //
        // Without this the player had *no* physics at all: a scene's colliders and
        // rigid bodies were inert, so an exported 3D game could draw but nothing
        // could move or collide. The editor's play mode always had this (it adds
        // both systems in PlaySession); a shipped build did not.
        if (scene_has_collider<fe::physics3d::Collider3D>()) {
            schedule().add_system<fe::physics3d::Physics3DSystem>();
        }
        if (scene_has_collider<fe::physics2d::Collider2D>()) {
            schedule().add_system<fe::physics2d::Physics2DSystem>();
        }

        // Audio, last so a cue a script or a behaviour fired this frame is mixed
        // this frame rather than a frame late. This is the same shape the
        // breakout slice uses (examples/breakout/src/main.cpp): adopt the
        // engine's audio system into the schedule, then attach a real output
        // device if the platform has one.
        //
        // A build with no device is not a failure. A CI machine has no sound
        // card, a browser starts its context suspended until a gesture, and the
        // mixer runs silently in every one of those cases rather than aborting a
        // game that is otherwise fine. That is why a device that will not open is
        // a warning and not an error.
        adopt_audio_system();

        // Report the version the build was made with when the manifest recorded
        // it, so a run says what produced it (docs/SHIPPING_QUEUE.md,
        // `release-version-stamping`). Silent for a build whose manifest predates
        // the field, or a run pointed at a pack and a scene directly.
        if (!options_.engine_version.empty()) {
            FE_LOG_INFO("fork_player: build made with engine v%s", options_.engine_version.c_str());
        }

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
#if defined(FE_PLATFORM_WEB)
        // The input probe. On the first frame the watched node's start transform
        // is recorded and a movement key plus a look key are pressed with real
        // synthetic DOM events, so they travel the whole path a player's
        // keystroke does: DOM event -> the backend's listener -> InputState ->
        // ActionMap -> the behaviour's update. On every later frame the key is
        // still held, so the node keeps moving. Measuring the start here, before
        // any behaviour has run, is what makes the reported delta the effect of
        // the input rather than of the scene's authoring.
        if (options_.probe_input && frame_count_ == 1) {
            begin_input_probe();
        }
#endif
    }

    void on_render(fe::f32) override {
        if (!engine().has_graphics() && !engine().headless_render()) {
            return;
        }
#if defined(FE_PLATFORM_WEB)
        // Sampled after the systems have run (on_render follows the schedule in
        // Application::step_frame), and every frame rather than only the last: a
        // jump is an arc, so its peak happens partway through and the final
        // height would be back on the ground. This is what turns "the character
        // jumped" into a number.
        if (options_.probe_input) {
            sample_probe_height();
        }
#endif
        // Application's frame draws 2D; 3D is the app's own step. render() itself
        // is the single decision point for whether a 3D pass happens: it looks up
        // the scene's active camera and returns 0 immediately when there is none,
        // so a 2D game does no 3D work and a 3D game gets its pass without the
        // player having to be told which it is. Asking here as well would walk the
        // scene a second time every frame to answer a question render() answers
        // again; the two would also be free to disagree. One walk, one answer.
        auto drawn = engine().renderer_3d().render(engine().device(), scene(), engine().window().framebuffer_width(),
                                                   engine().window().framebuffer_height());
        if (!drawn) {
            FE_LOG_ERROR("fork_player: 3D render failed: %s", drawn.error().message().c_str());
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

#if defined(FE_PLATFORM_WEB)
    // Presses the movement keys with real synthetic DOM events and records the
    // watched node's starting transform, so a later report can measure what the
    // input did. The events are dispatched at the same target the backend's
    // listeners are on (the window for keyboard), so this exercises the shipped
    // path rather than a test-only one.
    void begin_input_probe() {
        const fe::Node* watched = scene().find_node_by_name(options_.probe_node);
        if (watched == nullptr) {
            FE_LOG_WARN("fork_player: input probe could not find a node named '%s'", options_.probe_node.c_str());
            return;
        }
        probe_start_position_ = watched->transform().position;
        probe_active_ = true;

        // Movement is driven by whichever device the probe was asked for.
        //
        // The two devices are probed in separate runs on purpose. A key probe
        // presses W, D *and* Space: the movement proves the key path and the jump
        // proves the key edge reaches the controller. A touch probe presses *no*
        // keys, so the only thing that can move the node is the synthetic touch —
        // otherwise a jump would move it and `moved` would be non-zero even if the
        // touch never landed, which is exactly the false pass that makes a probe
        // worthless. Keeping them apart is what lets `moved > 0` mean "the touch
        // reached gameplay" without a second measurement.
        if (options_.probe_touch) {
            dispatch_touch_probe();
            return;
        }
        EM_ASM({
            window.dispatchEvent(new KeyboardEvent('keydown', {code : 'KeyW', bubbles : true}));
            window.dispatchEvent(new KeyboardEvent('keydown', {code : 'KeyD', bubbles : true}));
            window.dispatchEvent(new KeyboardEvent('keydown', {code : 'Space', bubbles : true}));
        });
        FE_LOG_INFO("fork_player: input probe pressed W, D and Space; watching node '%s'", options_.probe_node.c_str());
    }

    // The touch half of the input probe: dispatch a real synthetic touch on the
    // canvas and leave it down for the run. The point is that the event travels
    // the whole shipped path a finger would - a DOM touch on the canvas, the
    // backend's listener, InputState, the action map's region test, the
    // behaviour - rather than calling into the engine directly, which would prove
    // nothing about whether touch reaches gameplay in a browser.
    //
    // The canvas is the dispatch target because that is where the backend binds
    // its touch listener (WebWindow); dispatching on `window` would miss it.
    void dispatch_touch_probe() {
        const int ok = EM_ASM_INT(
            {
                const canvas = document.getElementById('canvas') || document.querySelector('canvas');
                if (!canvas) {
                    return 0;
                }
                const rect = canvas.getBoundingClientRect();
                const x = rect.left + rect.width * $0;
                const y = rect.top + rect.height * $1;
                // A Touch is constructed with client coordinates; the backend converts
                // them to canvas pixels and normalises against the viewport, so the
                // fraction the caller asked for is the fraction the region test sees.
                const touch = new Touch({identifier : 1, target : canvas, clientX : x, clientY : y});
                canvas.dispatchEvent(new TouchEvent('touchstart', {
                    touches : [touch],
                    targetTouches : [touch],
                    changedTouches : [touch],
                    bubbles : true,
                    cancelable : true,
                }));
                return 1;
            },
            static_cast<double>(options_.probe_touch_x), static_cast<double>(options_.probe_touch_y));
        if (ok == 0) {
            FE_LOG_WARN("fork_player: touch probe found no canvas to touch");
            probe_active_ = false;
            return;
        }
        FE_LOG_INFO("fork_player: touch probe touched the canvas at (%.2f, %.2f); watching node '%s'",
                    static_cast<double>(options_.probe_touch_x), static_cast<double>(options_.probe_touch_y),
                    options_.probe_node.c_str());
    }

    // Called each frame while the probe runs, so the report can state how high the
    // watched node rose above where it started. A walk keeps the height flat; a
    // jump raises it, so a non-zero peak is proof the jump edge reached the
    // controller.
    void sample_probe_height() {
        if (!probe_active_) {
            return;
        }
        const fe::Node* watched = scene().find_node_by_name(options_.probe_node);
        if (watched == nullptr) {
            return;
        }
        const fe::f32 rise = watched->transform().position.y - probe_start_position_.y;
        probe_peak_rise_ = rise > probe_peak_rise_ ? rise : probe_peak_rise_;
    }

    // How far the watched node moved *horizontally* since the probe began, in
    // world units, and whether the run proved input reached gameplay at all.
    //
    // Horizontal on purpose. A character standing still still moves a little
    // vertically: it settles onto the ground for a frame or two as the solver
    // resolves its initial penetration, which is a few thousandths of a unit and
    // happens with no input at all. A total-distance check counts that settle and
    // reports a pass for a game whose input never arrived. Walking is horizontal
    // (the character's own controller zeroes the move's y), so measuring only the
    // horizontal part separates "walked" from "settled" — the difference between a
    // real input check and a number that is non-zero whatever the input does.
    [[nodiscard]] fe::f32 probe_moved_distance() const {
        if (!probe_active_) {
            return 0.0f;
        }
        const fe::Node* watched = scene().find_node_by_name(options_.probe_node);
        if (watched == nullptr) {
            return 0.0f;
        }
        const fe::Vec3 delta = watched->transform().position - probe_start_position_;
        return fe::Vec3{delta.x, 0.0f, delta.z}.length();
    }

    // The highest the watched node rose above its start, in world units. Non-zero
    // means a jump happened.
    [[nodiscard]] fe::f32 probe_peak_rise() const { return probe_peak_rise_; }
#endif

private:
    // Reads the last frame and reports how varied it is, then hands the verdict
    // to the page on web. A blank frame has one colour; a rendered scene has
    // several, so the distinct-colour count is a blankness check that needs no
    // golden image and does not change when a game's art changes.
    //
    // The whole frame is read, not a centred patch. A patch is cheaper, but it
    // is only sound when the geometry is guaranteed to cover the middle of the
    // screen: a 3D scene framed with its geometry off-centre -- or simply a
    // camera pointing at the sky -- leaves a centred patch on the clear colour
    // and reports a rendered frame as blank. The cost is one readback at the end
    // of a bounded run, which is negligible next to the frames themselves, and a
    // game that runs unbounded never calls this at all.
    void report_frame() {
        const fe::u32 width = engine().window().framebuffer_width();
        const fe::u32 height = engine().window().framebuffer_height();
        if (width == 0 || height == 0) {
            return;
        }
        std::vector<fe::u8> pixels(static_cast<std::size_t>(width) * height * 4);
        engine().device().read_pixels(0, 0, width, height, pixels);

        std::set<std::uint32_t> distinct;
        for (std::size_t i = 0; i + 3 < pixels.size(); i += 4) {
            const std::uint32_t packed =
                static_cast<std::uint32_t>(pixels[i]) | (static_cast<std::uint32_t>(pixels[i + 1]) << 8) |
                (static_cast<std::uint32_t>(pixels[i + 2]) << 16) | (static_cast<std::uint32_t>(pixels[i + 3]) << 24);
            distinct.insert(packed);
        }

        FE_LOG_INFO("fork_player: frame readback %ux%u, %llu distinct colour(s)", static_cast<unsigned int>(width),
                    static_cast<unsigned int>(height), static_cast<unsigned long long>(distinct.size()));

#if defined(FE_PLATFORM_WEB)
        // Drain the backend's error queue and add anything found to the run's
        // total. "It drew something" and "it drew legally" are different claims:
        // a WebGL2 context rejects calls a desktop GL 3.3 context tolerates, and
        // a rejected pass can still leave a non-blank frame behind -- the shadow
        // map this queue's ADR-0076 was about came back incomplete while the main
        // pass kept drawing. Reading the queue is the only way a headless run
        // sees that, so the count is part of the verdict and the check fails on a
        // non-zero one. Each error is logged, so a failure names what the driver
        // rejected rather than only that it did.
        //
        // The drain *accumulates* rather than replacing. report_frame() runs on
        // every frame from the target onward (the loop keeps stepping until the
        // browser's virtual-time budget ends, and each of those frames reports),
        // so an error raised on one frame would be drained there and then
        // overwritten by the next frame's empty queue -- the run would report
        // clean while the log showed the error. Appending keeps a transient error
        // in the verdict, which is what "the run was legal" has to mean.
        for (const std::string& error : engine().device().drain_errors()) {
            FE_LOG_ERROR("fork_player: GL error after the run: %s", error.c_str());
            gl_errors_seen_.push_back(error);
        }

        // The report a headless-browser check reads back out of the page. The
        // engine's log goes to the browser console, which --dump-dom does not
        // show, so the verdict is written into the title where the harness can
        // see it, the same way the web smoke test reports. `canvas` is the
        // window's live-context flag: a WebGL2 context can only be obtained from
        // a canvas, so a 1 here is the check that the shell provided one.
        //
        // There is deliberately no frame-time field. A headless-browser run is
        // driven with --virtual-time-budget, which advances a virtual clock, so
        // any interval measured here is a property of the budget and not of the
        // engine; reporting it would put a number in the log that reads like a
        // frame time and is not one. Frame cost is measured and regression-gated
        // by fork_bench, which runs the real frame loop against a real clock.
        EM_ASM(
            {
                document.title = 'FE_PLAYER_RESULT canvas=' + $0 + ' frames=' + $1 + ' distinct=' + $2 + ' moved=' +
                                 $3.toFixed(3) + ' probe=' + $4 + ' jumped=' + $5.toFixed(3) + ' glerr=' + $6;
            },
            engine().window().has_context() ? 1 : 0, static_cast<int>(frame_count_), static_cast<int>(distinct.size()),
            static_cast<double>(probe_moved_distance()), options_.probe_input ? 1 : 0,
            static_cast<double>(probe_peak_rise()), static_cast<int>(gl_errors_seen_.size()));
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

    // Whether the loaded scene contains a collider of `T`, which is how the
    // player decides which physics systems the frame needs. Walking the scene once
    // at startup is free next to a frame, and it keeps the decision in one place
    // rather than requiring a project to declare its dimension.
    template <typename T>
    [[nodiscard]] bool scene_has_collider() const {
        bool found = false;
        scene().for_each_node([&](fe::Node& node) {
            if (!found && node.get_component<T>() != nullptr) {
                found = true;
            }
        });
        return found;
    }

    // Adopts the engine's audio system into the schedule and opens the real
    // output device if one is available. A device that will not open is not an
    // error: the mixer still runs, silently, which is what a CI machine with no
    // sound card does, and what a browser does until the user's first gesture
    // (docs/adr/0081-web-audio-autoplay-gesture.md).
    //
    // The listener system is added *before* the audio system so the mix this
    // frame uses the camera's position this frame; added after, a positional
    // source would be attenuated against wherever the camera was last frame, and
    // a fast-moving player would hear every sound from one frame behind.
    void adopt_audio_system() {
        auto system = engine().take_audio_system();
        if (system == nullptr) {
            return;
        }
        audio_system_ = system.get();

        schedule().add_system<fe::AudioListenerSystem>(audio_system_);

        auto device = fe::audio::create_audio_device();
        if (!device) {
            FE_LOG_WARN("fork_player: no audio backend (%s); mixing silently", device.error().message().c_str());
        } else if (auto opened = device.value()->open(48000, 2); !opened) {
            FE_LOG_WARN("fork_player: could not open the audio device (%s); mixing silently",
                        opened.error().message().c_str());
        } else if (fe::Error error = device.value()->start(); error) {
            FE_LOG_WARN("fork_player: could not start playback (%s); mixing silently", error.message().c_str());
        } else {
            audio_device_ = std::move(device.value());
            audio_system_->set_device(audio_device_.get());
            FE_LOG_INFO("fork_player: opened %s audio backend at %u Hz", std::string(audio_device_->backend()).c_str(),
                        audio_device_->format().sample_rate);
        }

        schedule().add_system(std::move(system));
    }

    Options options_;
    fe::ScriptHost host_;
    fe::ScriptLibrary library_;
    std::unique_ptr<fe::script::ScriptBinding> binding_;
    fe::audio::AudioSystem* audio_system_ = nullptr;
    std::unique_ptr<fe::audio::AudioDevice> audio_device_;
    fe::u64 frame_count_ = 0;
#if defined(FE_PLATFORM_WEB)
    fe::Vec3 probe_start_position_{};
    fe::f32 probe_peak_rise_ = 0.0f;
    bool probe_active_ = false;
    // Every GL error the backend reported, accumulated across the frames that
    // reported. Kept so the report's count is the run's total and a later clean
    // drain cannot overwrite an earlier error (see report_frame()).
    std::vector<std::string> gl_errors_seen_;
#endif
};

void print_usage() {
    std::puts("usage: fork_player --export DIR [--frames N] [--headless N]\n"
              "       fork_player --pack FILE --scene KEY [--module PATH ...] [--frames N] [--headless N]\n"
              "\n"
              "  --export DIR    play the export in DIR, reading its fork.export.json\n"
              "                  (its pack, scene, and gameplay modules)\n"
              "  --pack FILE     a pack file to mount (direct form)\n"
              "  --scene KEY     the startup scene's key inside the pack (direct form)\n"
              "  --name NAME     the game's display name, for the window and page title\n"
              "                  (direct form; --export reads it from the manifest)\n"
              "  --engine-version V  the engine version the build was made with, shown in\n"
              "                  the title (direct form; --export reads it from the manifest)\n"
              "  --bindings KEY  a declarative input-binding document's key inside the\n"
              "                  pack (direct form; --export reads it from the manifest)\n"
              "  --module PATH   an extra gameplay module to load (repeatable)\n"
              "  --frames N      run N frames and exit (the web build's bounded run)\n"
              "  --probe-input   (web) press movement keys with synthetic events and\n"
              "                  report how far --probe-node moved; proves input reaches\n"
              "                  gameplay in a browser\n"
              "  --probe-touch   (web) like --probe-input, but dispatch a synthetic\n"
              "                  touch on the canvas instead of a key press; proves a\n"
              "                  declared touch region reaches gameplay\n"
              "  --probe-touch-at X,Y  (web) where the synthetic touch lands, normalised\n"
              "                  (default 0.1,0.6, inside the built-in walk pad)\n"
              "  --probe-node N  the node the input probe watches (default: Camera)\n"
              "  --safe-area T,R,B,L  (web) OS-reserved screen bands as normalised\n"
              "                  fractions, so touch regions stay off a notch or a home\n"
              "                  indicator; the shell reads them from env(safe-area-inset-*)\n"
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
        } else if (arg == "--name" && has_value) {
            // The game's display name. `--export` reads it from the manifest; the
            // web shell, which is handed the pack and scene directly rather than
            // an export directory, passes it here so a browser build's title is
            // the game's rather than the engine's default.
            out.name = argv[++i];
        } else if (arg == "--engine-version" && has_value) {
            // The engine version the build was produced with, for the same
            // reason: the web shell has the manifest but the runtime is handed
            // argv, so the version travels with it (docs/SHIPPING_QUEUE.md,
            // `release-version-stamping`).
            out.engine_version = argv[++i];
        } else if (arg == "--bindings" && has_value) {
            // The pack key of a declarative input-binding document. `--export`
            // reads this from the manifest; the web shell, which is handed the
            // pack and scene directly rather than a directory, passes it here.
            // Without it a web build would silently drop the game's own layout
            // and fall back to the built-ins, which is exactly the failure this
            // option exists to make impossible.
            out.bindings = argv[++i];
        } else if (arg == "--module" && has_value) {
            out.modules.emplace_back(argv[++i]);
        } else if (arg == "--frames" && has_value) {
            out.frames = static_cast<fe::u32>(std::strtoul(argv[++i], nullptr, 10));
        } else if (arg == "--probe-input") {
            out.probe_input = true;
        } else if (arg == "--probe-touch") {
            out.probe_input = true;
            out.probe_touch = true;
        } else if (arg == "--probe-touch-at" && has_value) {
            // "x,y" as normalised fractions, so a project can aim the probe at the
            // region its own bindings document covers without a rebuild.
            const std::string value = argv[++i];
            const std::size_t comma = value.find(',');
            if (comma == std::string::npos) {
                std::fprintf(stderr, "fork_player: --probe-touch-at wants 'x,y' in 0..1\n");
                return false;
            }
            out.probe_touch_x = static_cast<fe::f32>(std::strtod(value.substr(0, comma).c_str(), nullptr));
            out.probe_touch_y = static_cast<fe::f32>(std::strtod(value.substr(comma + 1).c_str(), nullptr));
        } else if (arg == "--probe-node" && has_value) {
            out.probe_node = argv[++i];
        } else if (arg == "--safe-area" && has_value) {
            // "t,r,b,l" as normalised fractions. The web shell measures these
            // from CSS env() and passes them through, so a phone's notch and home
            // indicator push interactive regions inward instead of sitting under
            // the system UI. Malformed input is refused rather than guessed: a
            // silently ignored inset would put a control where a thumb cannot
            // reach it, which is the bug this option exists to prevent.
            const std::string value = argv[++i];
            fe::f32 parsed[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            int field = 0;
            bool ok = true;
            std::size_t start = 0;
            while (field < 4) {
                const std::size_t comma = value.find(',', start);
                const std::string piece =
                    comma == std::string::npos ? value.substr(start) : value.substr(start, comma - start);
                char* end = nullptr;
                parsed[field] = static_cast<fe::f32>(std::strtod(piece.c_str(), &end));
                if (piece.empty() || end == piece.c_str() || *end != '\0') {
                    ok = false;
                    break;
                }
                ++field;
                if (comma == std::string::npos) {
                    break;
                }
                start = comma + 1;
            }
            if (!ok || field != 4) {
                std::fprintf(stderr, "fork_player: --safe-area wants 'top,right,bottom,left' in 0..1\n");
                return false;
            }
            out.safe_top = parsed[0];
            out.safe_right = parsed[1];
            out.safe_bottom = parsed[2];
            out.safe_left = parsed[3];
        } else if (arg == "--fixed-step" && has_value) {
            out.fixed_step = static_cast<fe::f32>(std::strtod(argv[++i], nullptr));
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
    // A build that recorded the engine that made it says so in its title, so a
    // tab, a bookmark and a screenshot carry the version. On the web the title
    // is the document title, which the backend sets from here, so this is also
    // what a player sees. The suffix is left off a build whose manifest predates
    // the field, and a run pointed at a pack and a scene directly
    // (docs/SHIPPING_QUEUE.md, `release-version-stamping`).
    if (!options.engine_version.empty()) {
        config.window.title += " " + options.engine_version;
    }
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
    // A fixed step is what makes a verification run reproducible, so the input
    // probe pins one by default: its reported distance is then a property of the
    // input and the behaviour, not of the browser's frame timing. A caller can
    // override it, and a plain run leaves it at 0 and steps by the measured
    // delta as a game does.
    config.fixed_delta_time = options.fixed_step;
    if (config.fixed_delta_time <= 0.0f && options.probe_input) {
        config.fixed_delta_time = 1.0f / 60.0f;
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
