// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Error.h"
#include "fe/core/Types.h"
#include "fe/platform/InputState.h"

#include <memory>
#include <string>

namespace fe {

// Which windowing backend a window is created with.
//
// The concrete backend is selected here, on the descriptor, rather than by a
// separate Platform interface: the window is the only OS entry point the engine
// calls directly, so one factory that dispatches on a field is the smallest seam
// that removes the hardcoded dependency. See
// docs/adr/0041-platform-contract-and-null-backend.md.
enum class WindowBackend : u8 {
    // The real desktop backend (GLFW): an OS window and a graphics context.
    GLFW = 0,
    // No OS and no graphics context. For CI, dedicated servers, and
    // deterministic tests that run with no display.
    Null,
    // The web backend: an HTML canvas with a WebGL2 context, no GLFW. Selected
    // by a web build (FE_PLATFORM_WEB), where GLFW cannot be compiled at all.
    // Unlike Null this *does* own a graphics context, so a device is created for
    // it; see docs/adr/0074-web-window-backend.md.
    Web,
};

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
    // Which backend to create. GLFW (the default) is the interactive path; Null
    // is a window with no OS surface and no graphics context, for a run with no
    // display. A Null window reports `has_context() == false`, so the runtime
    // knows not to create a device. Web is the canvas-backed browser backend,
    // available only in a web build.
    WindowBackend backend = WindowBackend::GLFW;

    // The CSS selector of the canvas to render into, for the web backend only.
    // Ignored by every other backend, which have their own surface. A web build
    // defaults to "#canvas" and creates the element if the page has not, so a
    // shell that provides its own canvas only needs this when it uses another
    // id.
    std::string canvas = "#canvas";
};

// An operating-system window with an associated graphics context.
//
// This is the only place in the engine that talks to a windowing library. GLFW
// is a private implementation detail: it never appears in a public header, and
// it lives entirely in the desktop backend (GlfwWindow), so a platform where
// GLFW cannot be compiled — the web target — simply compiles a different
// backend instead. Swapping windowing backends (or adding a second one) does
// not ripple through the rest of the codebase.
//
// Three backends implement this interface: the GLFW desktop window, the null
// (headless) window, and the web canvas window. The base class itself holds no
// platform state: every OS call lives in a subclass, and the defaults below are
// the no-op behaviour a window with no OS needs. See
// docs/adr/0041-platform-contract-and-null-backend.md and
// docs/adr/0074-web-window-backend.md.
//
// A Window is move-only. Create one with Window::create().
class Window {
public:
    virtual ~Window();

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    // Creates a window and its graphics context, using the backend named by
    // `desc.backend`. Returns an Error if the platform or context could not be
    // initialised; the engine reports this rather than aborting so a headless
    // host can fall back cleanly.
    [[nodiscard]] static Result<std::unique_ptr<Window>> create(const WindowDesc& desc);

    // Processes pending OS events. Must be called once per frame before reading
    // input. Dispatches events into the InputState and invokes user callbacks.
    // The default does nothing: a backend with no OS has no events.
    virtual void poll_events();

    // Presents the back buffer to the screen. The default does nothing.
    virtual void swap_buffers();

    // True once the user (or the OS) has requested the window close.
    [[nodiscard]] virtual bool should_close() const;

    virtual void request_close();

    [[nodiscard]] virtual u32 width() const noexcept;
    [[nodiscard]] virtual u32 height() const noexcept;

    // Pixel dimensions, which can differ from width()/height() on displays with
    // a scale factor. The viewport must use these values.
    [[nodiscard]] virtual u32 framebuffer_width() const noexcept;
    [[nodiscard]] virtual u32 framebuffer_height() const noexcept;

    [[nodiscard]] f32 aspect_ratio() const noexcept;

    virtual void set_title(const std::string& title);
    virtual void set_vsync(bool enabled);

    [[nodiscard]] InputState& input() noexcept { return input_; }
    [[nodiscard]] const InputState& input() const noexcept { return input_; }

    // Resolves an OpenGL entry point. Used by the graphics backend to load its
    // function pointers; not part of the general engine API. Returns nullptr on
    // a backend with no graphics context (the null window).
    [[nodiscard]] virtual void* get_proc_address(const char* name) const;

    // True when this window owns a graphics context (and therefore a device can
    // be created). False for the null window; true for GLFW and for web. The
    // runtime consults this to decide whether to bring up the graphics stack; it
    // is the one honest branch on "is there a surface?", so no code has to
    // pretend a null window can render.
    [[nodiscard]] virtual bool has_context() const noexcept { return true; }

    // Opaque native window handle (e.g. GLFWwindow*). Exposed only for the
    // graphics backend, which must not be given a typed platform header.
    [[nodiscard]] void* native_handle() const noexcept { return native_handle_; }

    // A human-readable description of the platform/windowing backend in use.
    [[nodiscard]] const char* backend_name() const noexcept { return backend_name_; }

protected:
    // Constructs an unbound window and records the backend name. Each backend's
    // factory constructs this; a caller never does.
    explicit Window(const char* backend_name);

    // Records the opaque native handle the backend created. Protected because
    // the handle is backend-private state that only the creating backend knows.
    void set_native_handle(void* handle) noexcept { native_handle_ = handle; }

private:
    void* native_handle_ = nullptr;
    // The backend this window was created with, reported by backend_name(). A
    // plain member (not virtual) so backend_name() stays non-virtual and can be
    // called from a destructor or before the vtable is established.
    const char* backend_name_ = "unknown";
    InputState input_{};
};

} // namespace fe
