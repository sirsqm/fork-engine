// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Error.h"
#include "fe/core/Types.h"
#include "fe/platform/InputState.h"

#include <memory>
#include <string>

namespace fe {

// Parameters for creating a window. Sensible defaults let a caller create a
// usable window with `WindowDesc{}`.
struct WindowDesc {
    std::string title = "Fork Engine";
    u32 width = 1280;
    u32 height = 720;
    bool resizable = true;
    bool visible = true;
    // Vertical synchronisation. Disable for benchmarks and for headless tests
    // that render as fast as possible.
    bool vsync = true;
    // Request an OpenGL 3.3 core context. The GL backend is the only backend in
    // this milestone; the field exists so a future backend can request a
    // different version without changing this interface.
    u32 gl_major = 3;
    u32 gl_minor = 3;
};

// An operating-system window with an associated graphics context.
//
// This is the only place in the engine that talks to a windowing library.
// GLFW is a private implementation detail: it never appears in a public header,
// so swapping windowing backends (or adding a second one) does not ripple
// through the rest of the codebase.
//
// A Window is move-only. Create one with Window::create().
class Window {
public:
    virtual ~Window();

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    // Creates a window and its graphics context. Returns an Error if the
    // platform or context could not be initialised; the engine reports this
    // rather than aborting so a headless host can fall back cleanly.
    [[nodiscard]] static Result<std::unique_ptr<Window>> create(const WindowDesc& desc);

    // Processes pending OS events. Must be called once per frame before reading
    // input. Dispatches events into the InputState and invokes user callbacks.
    void poll_events();

    // Presents the back buffer to the screen.
    void swap_buffers();

    // True once the user (or the OS) has requested the window close.
    [[nodiscard]] bool should_close() const;

    void request_close();

    [[nodiscard]] u32 width() const noexcept;
    [[nodiscard]] u32 height() const noexcept;

    // Pixel dimensions, which can differ from width()/height() on displays with
    // a scale factor. The viewport must use these values.
    [[nodiscard]] u32 framebuffer_width() const noexcept;
    [[nodiscard]] u32 framebuffer_height() const noexcept;

    [[nodiscard]] f32 aspect_ratio() const noexcept;

    void set_title(const std::string& title);
    void set_vsync(bool enabled);

    [[nodiscard]] InputState& input() noexcept { return input_; }
    [[nodiscard]] const InputState& input() const noexcept { return input_; }

    // Resolves an OpenGL entry point. Used by the graphics backend to load its
    // function pointers; not part of the general engine API.
    [[nodiscard]] void* get_proc_address(const char* name) const;

    // Opaque native window handle (e.g. GLFWwindow*). Exposed only for the
    // graphics backend, which must not be given a typed platform header.
    [[nodiscard]] void* native_handle() const noexcept { return native_handle_; }

    // A human-readable description of the platform/windowing backend in use.
    [[nodiscard]] static const char* backend_name() noexcept;

private:
    Window();

    void* native_handle_ = nullptr;
    InputState input_{};
    // Set by the resize callback so the runtime can react to framebuffer
    // changes without polling the OS.
    bool framebuffer_resized_ = false;
};

} // namespace fe
