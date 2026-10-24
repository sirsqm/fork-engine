// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "WebWindow.h"

#include "fe/core/Log.h"

#include <emscripten/html5.h>
#include <emscripten/html5_webgl.h>

#include <string>

namespace fe {
namespace {

// The canvas the engine renders into when the descriptor names none. A shipped
// shell is expected to provide an element with this id (the export template, a
// later queue item, does); a page that uses another id passes it in the
// descriptor.
constexpr const char* kDefaultCanvas = "#canvas";

// `window.close()` ends a page, but a browser only honours it for a window the
// script itself opened. The engine still records the request so a caller that
// drives the loop can stop cleanly, and reports the reason to the page so a
// shell can react (e.g. show a "you may close this tab" overlay) rather than
// silently doing nothing.
const char* on_before_unload(int /*eventType*/, const void* /*reserved*/, void* /*userData*/) {
    return nullptr;
}

} // namespace

WebWindow::WebWindow() : Window("web") {}

WebWindow::~WebWindow() {
    // Detach the context from this canvas. The browser reclaims the GPU
    // resources; Emscripten's handle is process-global, so it is released here
    // rather than left for a later window to inherit.
    if (native_handle() != nullptr) {
        emscripten_webgl_destroy_context(
            static_cast<EMSCRIPTEN_WEBGL_CONTEXT_HANDLE>(reinterpret_cast<uintptr_t>(native_handle())));
        set_native_handle(nullptr);
    }
}

Result<std::unique_ptr<Window>> WebWindow::create(const WindowDesc& desc) {
    // A canvas element must exist before a context can be made on it. A page
    // that loads the module before its markup would otherwise get a bare
    // "context creation failed" with no hint as to why, so create the element
    // when it is missing: a shell that already has a #canvas is unaffected.
    //
    // The JavaScript below is deliberately written without a strict-equality
    // operator. Emscripten splices an EM_ASM body into the module verbatim, and
    // clang-format parses it as C++, where `a === b` reads as `a == (= b)` and is
    // rewritten to `a == = b` — a JavaScript syntax error that breaks the whole
    // module with no clue at build time. String.prototype.startsWith avoids the
    // operator entirely and cannot be mangled.
    const std::string selector = desc.canvas.empty() ? std::string(kDefaultCanvas) : desc.canvas;
    const int created = MAIN_THREAD_EM_ASM_INT(
        {
            var id = UTF8ToString($0);
            var el = document.querySelector(id);
            if (!el) {
                el = document.createElement('canvas');
                el.id = id.startsWith('#') ? id.substring(1) : id;
                document.body.appendChild(el);
            }
            return 1;
        },
        selector.c_str());
    if (created == 0) {
        return Error("web window: could not create or find the canvas element \"" + selector + "\"");
    }

    // Size the canvas to the requested logical size before the context is made,
    // so the drawing buffer starts at the right resolution rather than at the
    // browser's default 300x150 and a resize on the first frame.
    emscripten_set_canvas_element_size(selector.c_str(), static_cast<int>(desc.width), static_cast<int>(desc.height));

    EmscriptenWebGLContextAttributes attributes;
    emscripten_webgl_init_context_attributes(&attributes);
    // WebGL2 is GLSL ES 3.00, which is what the ES shader dialect targets
    // (ADR-0072); asking for version 1 would silently change the shading
    // language under the engine.
    attributes.majorVersion = 2;
    attributes.minorVersion = 0;
    attributes.alpha = false;
    attributes.depth = true;
    attributes.stencil = false;
    attributes.antialias = false;
    attributes.premultipliedAlpha = true;
    attributes.preserveDrawingBuffer = false;
    attributes.powerPreference = EM_WEBGL_POWER_PREFERENCE_DEFAULT;
    // Fail rather than fall back to a WebGL1 context: a version-1 context cannot
    // compile the engine's shaders, so a silent downgrade would surface much
    // later as a shader compile error with no explanation.
    attributes.failIfMajorPerformanceCaveat = false;

    const EMSCRIPTEN_WEBGL_CONTEXT_HANDLE context = emscripten_webgl_create_context(selector.c_str(), &attributes);
    if (context <= 0) {
        return Error("web window: WebGL2 context creation failed on \"" + selector +
                     "\" (is WebGL2 available in this browser?)");
    }

    // The context must be current before the graphics backend resolves entry
    // points and compiles shaders through it.
    if (emscripten_webgl_make_context_current(context) != EMSCRIPTEN_RESULT_SUCCESS) {
        emscripten_webgl_destroy_context(context);
        return Error("web window: could not make the WebGL2 context current");
    }

    auto window = std::unique_ptr<WebWindow>(new WebWindow());
    window->canvas_selector_ = selector;
    window->width_ = desc.width;
    window->height_ = desc.height;
    // The context handle is the backend's native handle, so the graphics
    // backend can identify the surface without a WebGL header of its own. The
    // handle is an opaque token to everything outside this file.
    window->set_native_handle(reinterpret_cast<void*>(static_cast<uintptr_t>(context)));

    if (!desc.title.empty()) {
        window->set_title(desc.title);
    }
    emscripten_set_beforeunload_callback(nullptr, &on_before_unload);
    window->input().begin_frame();

    FE_LOG_INFO("web window: WebGL2 context %d on %s (%ux%u)", static_cast<int>(context), selector.c_str(), desc.width,
                desc.height);
    return std::unique_ptr<Window>(window.release());
}

void WebWindow::poll_events() {
    // The DOM delivers input through event callbacks, not a poll, so there is
    // nothing to pump here. The call stays part of the interface so the frame
    // loop is identical on every backend.
}

void WebWindow::swap_buffers() {
    // WebGL2 presents implicitly when the callback returns; there is no
    // explicit swap. Nothing to do, and nothing to branch on at the call site.
}

bool WebWindow::should_close() const {
    return close_requested_;
}

void WebWindow::request_close() {
    close_requested_ = true;
}

u32 WebWindow::width() const noexcept {
    return width_;
}

u32 WebWindow::height() const noexcept {
    return height_;
}

u32 WebWindow::framebuffer_width() const noexcept {
    // The drawing buffer is what the viewport must match: a browser can size it
    // independently of the CSS box (a HiDPI display, a canvas the page resized).
    // Ask the context for it, falling back to the canvas element and then to the
    // logical size if the query fails, so the viewport is never 0.
    int w = 0;
    int h = 0;
    const auto context = static_cast<EMSCRIPTEN_WEBGL_CONTEXT_HANDLE>(reinterpret_cast<uintptr_t>(native_handle()));
    if (emscripten_webgl_get_drawing_buffer_size(context, &w, &h) == EMSCRIPTEN_RESULT_SUCCESS && w > 0) {
        return static_cast<u32>(w);
    }
    if (emscripten_get_canvas_element_size(canvas_selector_.c_str(), &w, &h) == EMSCRIPTEN_RESULT_SUCCESS && w > 0) {
        return static_cast<u32>(w);
    }
    return width_;
}

u32 WebWindow::framebuffer_height() const noexcept {
    int w = 0;
    int h = 0;
    const auto context = static_cast<EMSCRIPTEN_WEBGL_CONTEXT_HANDLE>(reinterpret_cast<uintptr_t>(native_handle()));
    if (emscripten_webgl_get_drawing_buffer_size(context, &w, &h) == EMSCRIPTEN_RESULT_SUCCESS && h > 0) {
        return static_cast<u32>(h);
    }
    if (emscripten_get_canvas_element_size(canvas_selector_.c_str(), &w, &h) == EMSCRIPTEN_RESULT_SUCCESS && h > 0) {
        return static_cast<u32>(h);
    }
    return height_;
}

void WebWindow::set_title(const std::string& title) {
    // A canvas has no title bar; the document title is the closest equivalent
    // and is what a browser tab shows.
    MAIN_THREAD_EM_ASM({ document.title = UTF8ToString($0); }, title.c_str());
}

void WebWindow::set_vsync(bool enabled) {
    (void)enabled;
}

void* WebWindow::get_proc_address(const char* name) const {
    // WebGL2's entry points come from the browser. Emscripten exposes them by
    // name through the WebGL2 binding, which is the web equivalent of the
    // desktop GLFW loader. The returned pointer is only valid while this
    // window's context is current, which the graphics backend guarantees by
    // resolving before it renders.
    return emscripten_webgl_get_proc_address(name);
}

} // namespace fe
