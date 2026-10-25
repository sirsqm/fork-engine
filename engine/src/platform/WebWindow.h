// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/platform/CanvasScale.h"
#include "fe/platform/Window.h"

#include <memory>

namespace fe {

// The web window backend: an HTML canvas with a WebGL2 context, no GLFW.
//
// A browser has no OS window and no GLFW, so this backend stands in for the
// desktop one on the web target. It is a Window like any other — same interface,
// same frame-loop contract — with two differences that are the whole point:
//
//   * it *owns* a graphics context (a WebGL2 one), so `has_context()` is true
//     and the runtime creates a device for it, exactly as on desktop. This is
//     what separates it from the null backend, which owns nothing;
//   * its surface is a `<canvas>` element and its entry points come from the
//     browser, so `get_proc_address()` resolves through Emscripten's WebGL2
//     binding rather than a desktop GL loader.
//
// Internal to the platform module: a caller selects it with
// `WindowDesc::backend = WindowBackend::Web` and receives a `Window`. In a web
// build the factory also serves the default (GLFW) request with this backend, so
// a game that never names a backend works unchanged in a browser. See
// docs/adr/0074-web-window-backend.md.
class WebWindow final : public Window {
public:
    // Creates the WebGL2 context on `desc.canvas` (defaulting to "#canvas") and
    // sizes the canvas from the descriptor. Returns an Error if the context
    // could not be created — a browser with WebGL2 disabled, for instance —
    // rather than aborting, so the engine reports it like any other failure.
    [[nodiscard]] static Result<std::unique_ptr<Window>> create(const WindowDesc& desc);

    ~WebWindow() override;

    WebWindow(const WebWindow&) = delete;
    WebWindow& operator=(const WebWindow&) = delete;

    // A browser delivers input through DOM events rather than a poll, so the
    // event callbacks installed at creation are what fill InputState and there
    // are no pending OS events to pump here. The call stays part of the
    // interface so the frame loop does not branch on the backend.
    void poll_events() override;

    // WebGL2 presents implicitly at the end of a frame, so there is no buffer to
    // swap. Kept as a no-op so the frame loop does not branch on the backend.
    void swap_buffers() override;

    [[nodiscard]] bool should_close() const override;
    void request_close() override;

    [[nodiscard]] u32 width() const noexcept override;
    [[nodiscard]] u32 height() const noexcept override;
    [[nodiscard]] u32 framebuffer_width() const noexcept override;
    [[nodiscard]] u32 framebuffer_height() const noexcept override;

    void set_title(const std::string& title) override;

    // WebGL2 has no swap interval to set; the browser's compositor decides when
    // a frame is presented. Accept and ignore so callers do not branch.
    void set_vsync(bool enabled) override;

    // Resolves a WebGL2 entry point as Emscripten exposes it. This is what the
    // graphics backend loads its function pointers with on the web target.
    [[nodiscard]] void* get_proc_address(const char* name) const override;

    // The distinguishing fact of this backend: a canvas-backed surface *does*
    // have a context, so unlike the null backend the runtime creates a device.
    [[nodiscard]] bool has_context() const noexcept override { return true; }

    // Converts a DOM event coordinate — CSS pixels relative to the canvas, which
    // is what the browser reports — into drawing-buffer pixels, the space the
    // viewport and the action layer's touch regions are in. Public because the
    // event callbacks are free functions; WebWindow itself is internal to the
    // platform module, so this is not part of any public API.
    [[nodiscard]] Vec2 buffer_point(f32 css_x, f32 css_y) const noexcept {
        return viewport_.css_to_buffer_point(css_x, css_y);
    }

    // Brings the drawing buffer back in line with the canvas's laid-out CSS box
    // and the display's device pixel ratio, and records the scale DOM event
    // coordinates must be converted through. The page owns the CSS box (the
    // export shell makes it fluid), so this runs on every resize and once at
    // creation, and is a no-op when the box has not changed. Public because the
    // resize callback is a free function. See
    // docs/adr/0077-web-canvas-resize-and-dpr.md.
    void sync_canvas_size();

private:
    WebWindow();

    // Registers (or removes) the DOM event listeners that feed InputState. A
    // browser delivers input as events rather than as a poll, so these callbacks
    // are what `poll_events()` would otherwise do. Kept together so the target
    // each event is bound to is visible in one place: the two choices are
    // deliberate and were measured, not guessed (see the .cpp).
    void install_input_callbacks();
    void remove_input_callbacks();

    // The CSS selector of the canvas this window renders into. Kept so a
    // resize or a title change can find the same element the context was made
    // on.
    std::string canvas_selector_ = "#canvas";
    u32 width_ = 0;
    u32 height_ = 0;
    bool close_requested_ = false;

    // The drawing buffer's size in device pixels, recorded by sync_canvas_size().
    //
    // Cached rather than queried because framebuffer_width()/height() are called
    // several times every frame (the action viewport, the renderer's viewport,
    // the readback) and the only way to ask the browser is an Emscripten call
    // that crosses into JavaScript and back — `emscripten_webgl_get_drawing_buffer_size`
    // and `emscripten_get_canvas_element_size` are not free. The buffer changes
    // only when the page changes the canvas, which is exactly when the resize
    // callback runs, so the cache cannot go stale behind the engine's back.
    u32 buffer_width_ = 0;
    u32 buffer_height_ = 0;

    // The last canvas-to-buffer mapping applied, and what DOM event coordinates
    // are converted through. Empty until the first sync_canvas_size().
    CanvasViewport viewport_{};
};

} // namespace fe
