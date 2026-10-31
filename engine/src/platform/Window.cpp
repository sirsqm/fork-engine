// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/platform/Window.h"

#include "NullWindow.h"

// The context-bearing backend is chosen at compile time, from the platform macro
// that cmake/FEPlatform.cmake defines. On desktop it is GLFW; on web it is the
// canvas backend. The preprocessor branch is what keeps GLFW out of a web build
// entirely, rather than present and unused — GLFW cannot be compiled to wasm, so
// "present" is not an option there.
#if defined(FE_PLATFORM_WEB)
#include "WebWindow.h"
#else
#include "GlfwWindow.h"
#endif

#include <memory>

namespace fe {

// The Window base class: the factory, and the defaults a window with no OS needs.
//
// This file is deliberately platform-free: it names no windowing library, and it
// compiles unchanged for desktop and for web. The concrete backends live in
// their own translation units — GlfwWindow.cpp for desktop, WebWindow.cpp for
// web — so a build only ever compiles the backend it can actually link. See
// docs/adr/0074-web-window-backend.md.
//
// The base-class defaults below are the "no OS, no surface" behaviour. A backend
// overrides only what its platform actually does, so the null window overrides
// almost nothing while the GLFW and web backends override almost everything.

// --- Base-class defaults ---------------------------------------------------

Window::Window(const char* backend_name) : backend_name_(backend_name) {}

Window::~Window() = default;

void Window::poll_events() {}

void Window::swap_buffers() {}

bool Window::should_close() const {
    return false;
}

void Window::request_close() {}

u32 Window::width() const noexcept {
    return 0;
}

u32 Window::height() const noexcept {
    return 0;
}

u32 Window::framebuffer_width() const noexcept {
    return 0;
}

u32 Window::framebuffer_height() const noexcept {
    return 0;
}

f32 Window::aspect_ratio() const noexcept {
    const u32 w = framebuffer_width();
    const u32 h = framebuffer_height();
    return (w > 0 && h > 0) ? static_cast<f32>(w) / static_cast<f32>(h) : 1.0f;
}

void Window::set_title(const std::string& title) {
    (void)title;
}

void Window::set_vsync(bool enabled) {
    (void)enabled;
}

void Window::set_icon(const WindowIcon& icon) {
    (void)icon;
}

void* Window::get_proc_address(const char* name) const {
    (void)name;
    return nullptr;
}

// --- Factory ---------------------------------------------------------------

Result<std::unique_ptr<Window>> Window::create(const WindowDesc& desc) {
    // Backend selection lives here, on the descriptor, and nowhere else. A
    // second backend is a case in this function, not a change to the runtime.
    // The null backend is the first such case; see
    // docs/adr/0041-platform-contract-and-null-backend.md.
    if (desc.backend == WindowBackend::Null) {
        return std::unique_ptr<Window>(new NullWindow(desc));
    }

#if defined(FE_PLATFORM_WEB)
    // On web the canvas backend is the only context-bearing one: there is no OS
    // window and no GLFW. A request for the GLFW backend is still served by the
    // canvas backend, so a game that takes the default backend works unchanged
    // in a browser.
    (void)desc.backend;
    return WebWindow::create(desc);
#else
    if (desc.backend == WindowBackend::Web) {
        // A web window is a canvas, which only exists in a web build. Report it
        // rather than silently handing back a GLFW window, so a misconfigured
        // native build fails loudly instead of quietly doing the wrong thing.
        return Error("the web window backend is only available in a web build "
                     "(configure with the Emscripten toolchain)");
    }
    return GlfwWindow::create(desc);
#endif
}

} // namespace fe
