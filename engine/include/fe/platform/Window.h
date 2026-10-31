// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Error.h"
#include "fe/core/Types.h"
#include "fe/platform/InputState.h"
#include "fe/platform/WindowIcon.h"

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

    // The icon shown in the title bar and the task bar. The default is the
    // engine's own mark, so a window looks like the engine without a caller
    // having to ask; an empty icon leaves the OS default. A backend that cannot
    // set an icon ignores it.
    WindowIcon icon = WindowIcon::white();
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

    // Sets the window's icon. The default does nothing: a backend with no OS has
    // no icon to set. Called by Window::create() with the descriptor's icon, so
    // a caller normally sets WindowDesc::icon rather than calling this.
    virtual void set_icon(const WindowIcon& icon);

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

    // --- Page visibility (web) ---------------------------------------------
    //
    // A hidden page is one the platform may stop drawing and stop scheduling
    // frames for: a backgrounded browser tab receives no animation frames at all.
    // The wall clock keeps running while it is hidden, so on the way back the gap
    // is not simulation time. The platform records each transition here as it
    // happens, and the host drains them at the frame boundary and reports each to
    // the engine (Engine::notify_page_visibility), which rebaselines the clock so
    // the gap is not charged to one step. See
    // docs/adr/0082-page-visibility-wiring.md.
    //
    // Transitions are *queued*, not collapsed to the current state. On the web no
    // frame runs while the page is hidden, so a tab that is backgrounded and
    // restored between two frames produces two transitions — hidden, then
    // visible — with no frame in between to observe the first. A single
    // "latest state" flag would report only "visible" and the engine would never
    // learn a hidden span occurred, so the resume would not be rebaselined. The
    // host drains the queue, so it sees both, in order, on the next frame.
    //
    // Only the web backend drives note_page_visibility(); a desktop window never
    // does, so its queue stays empty and its clock is untouched. Both methods are
    // public for the same reason InputState's on_key() is: the platform reports
    // events through the object they belong to, and a test can inject one to
    // exercise the path.

    // The most transitions that can be pending. A page hides and returns between
    // frames at most once in practice; the extra room is slack for a pathological
    // flip, and the cap keeps the queue a fixed, allocation-free array.
    static constexpr u32 kMaxPendingVisibilityChanges = 8;

    // Records a page-visibility transition. `hidden` is the visibility the page
    // changed to. Called by the web backend's visibilitychange callback. On a full
    // queue the pending transitions are collapsed to this one: a page flipping
    // faster than the frame loop cannot be replayed transition by transition, and
    // reporting the current state keeps the engine consistent with reality rather
    // than a stale sequence.
    void note_page_visibility(bool hidden) noexcept {
        if (pending_visibility_count_ == kMaxPendingVisibilityChanges) {
            pending_visibility_head_ = 0;
            pending_visibility_count_ = 0;
        }
        const u32 tail = (pending_visibility_head_ + pending_visibility_count_) % kMaxPendingVisibilityChanges;
        pending_visibility_hidden_[tail] = hidden;
        ++pending_visibility_count_;
    }

    // Removes and reports the oldest pending page-visibility transition: true with
    // `hidden` set to the visibility it changed to, or false when none is pending.
    // A host drains this in a loop, forwarding each transition to the engine, so a
    // hidden span that both began and ended between frames is reported in full.
    [[nodiscard]] bool take_page_visibility_change(bool& hidden) noexcept {
        if (pending_visibility_count_ == 0) {
            return false;
        }
        hidden = pending_visibility_hidden_[pending_visibility_head_];
        pending_visibility_head_ = (pending_visibility_head_ + 1) % kMaxPendingVisibilityChanges;
        --pending_visibility_count_;
        return true;
    }

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
    // The page-visibility transitions recorded but not yet drained by a host, as
    // a small ring: a hidden span that began and ended between frames is two
    // transitions, so they are queued rather than collapsed. See
    // note_page_visibility().
    bool pending_visibility_hidden_[kMaxPendingVisibilityChanges] = {};
    u32 pending_visibility_head_ = 0;
    u32 pending_visibility_count_ = 0;
};

} // namespace fe
