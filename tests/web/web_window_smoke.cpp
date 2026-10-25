// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
//
// The web window smoke test: proves the web backend really comes up in a
// browser, not just that it compiles.
//
// Compiling to wasm is not evidence that a canvas and a WebGL2 context can be
// obtained, so this program does the thing itself: it constructs a Window with
// the web backend, asserts it owns a live WebGL2 context, and resolves real
// WebGL2 entry points through it — the same call the graphics backend makes when
// it loads its function pointers. It then reports a single machine-readable
// result into the document title, which the harness
// (tests/web/run_web_smoke.sh) reads back with `chromium --dump-dom`.
//
// The result goes into the page rather than the process exit code because a
// headless browser is driven by loading a page: the page's state is the
// observable, and reading it needs no debugging protocol. The process exit code
// is still set, so a run under `node` reports the same verdict twice.
//
// See docs/adr/0074-web-window-backend.md.

#include "fe/platform/InputActions.h"
#include "fe/platform/Window.h"

#include <emscripten.h>

#include <string>
#include <string_view>
#include <vector>

namespace {

// Collects the outcome so the report is one line per check, and the page shows
// exactly what was verified rather than a bare pass/fail.
class Report {
public:
    void check(std::string_view what, bool ok) {
        checks_.push_back(std::string(ok ? "ok   " : "FAIL ") + std::string(what));
        if (!ok) {
            passed_ = false;
        }
    }

    [[nodiscard]] bool passed() const noexcept { return passed_; }

    [[nodiscard]] std::string text() const {
        std::string out = passed_ ? "FE_SMOKE_RESULT: PASS\n" : "FE_SMOKE_RESULT: FAIL\n";
        for (const std::string& line : checks_) {
            out += line;
            out += '\n';
        }
        return out;
    }

private:
    std::vector<std::string> checks_;
    bool passed_ = true;
};

} // namespace

int main() {
    Report report;

    fe::WindowDesc desc;
    // The canvas the shell provides. Asking for the backend explicitly is what
    // this test is about; a web build also serves the default backend with the
    // same window, which is the case a game actually hits.
    desc.backend = fe::WindowBackend::Web;
    desc.width = 320;
    desc.height = 200;
    desc.title = "Fork Engine web smoke test";
    desc.canvas = "#canvas";

    auto created = fe::Window::create(desc);
    report.check("Window::create() with the web backend succeeds", created.ok());
    if (!created.ok()) {
        // Nothing else can be asserted without a window, so report and stop.
        const std::string text = report.text();
        EM_ASM({ document.title = UTF8ToString($0); }, text.c_str());
        return 1;
    }

    fe::Window& window = *created.value();

    // The two facts that make this backend usable: it owns a context (so the
    // runtime creates a device) and it is not the null backend.
    report.check("the web window reports a live context (has_context())", window.has_context());
    report.check("the backend identifies itself as web", std::string_view(window.backend_name()) == "web");
    report.check("the web window has a native handle for its context", window.native_handle() != nullptr);

    // The requested size must be honoured before the first frame, so the viewport
    // maths has something real to work with.
    report.check("the window width matches the descriptor", window.width() == 320);
    report.check("the window height matches the descriptor", window.height() == 200);
    report.check("the framebuffer width is non-zero", window.framebuffer_width() > 0);
    report.check("the framebuffer height is non-zero", window.framebuffer_height() > 0);

    // Resolving entry points is what the graphics backend does with this window.
    // A WebGL2-only name (glVertexAttribDivisor is core in GLES3/WebGL2 and
    // absent from GLES2) is included deliberately: if the browser had quietly
    // given a WebGL1 context, that lookup would fail.
    for (const char* name : {"glClear", "glCreateShader", "glDrawElements", "glVertexAttribDivisor"}) {
        report.check(std::string("get_proc_address resolves ") + name, window.get_proc_address(name) != nullptr);
    }
    report.check("get_proc_address returns null for an unknown entry point",
                 window.get_proc_address("glDefinitelyNotAFunction") == nullptr);

    // --- Input --------------------------------------------------------------
    //
    // The strongest check this test can make: dispatch real DOM events at the
    // page and observe them arrive in InputState through the backend's
    // listeners. Everything else here proves the window came up; this proves the
    // backend is actually wired to the browser's event stream, which is what
    // `touch-input-actions` added. A synthetic event is a real event as far as
    // the listener is concerned, so this exercises the same path a keystroke or a
    // finger does.
    window.input().begin_frame();
    EM_ASM({ window.dispatchEvent(new KeyboardEvent('keydown', {code : 'ArrowLeft', bubbles : true})); });
    report.check("a keydown reaches InputState (Left is down)", window.input().is_key_down(fe::Key::Left));
    report.check("the keydown produced a press edge", window.input().was_key_pressed(fe::Key::Left));

    window.input().begin_frame();
    EM_ASM({ window.dispatchEvent(new KeyboardEvent('keyup', {code : 'ArrowLeft', bubbles : true})); });
    report.check("a keyup releases the key", !window.input().is_key_down(fe::Key::Left));
    report.check("the keyup produced a release edge", window.input().was_key_released(fe::Key::Left));

    // Touch: a real TouchEvent at the canvas, the way a finger arrives.
    window.input().begin_frame();
    EM_ASM({
        var canvas = document.getElementById('canvas');
        var touch = new Touch({identifier : 42, target : canvas, clientX : 40, clientY : 100});
        canvas.dispatchEvent(new TouchEvent(
            'touchstart',
            {touches : [touch], targetTouches : [touch], changedTouches : [touch], bubbles : true, cancelable : true}));
    });
    report.check("a touchstart reaches InputState", window.input().touch_count() == 1);
    const fe::u32 touch_id = window.input().touch_id_at(0);
    report.check("the contact is down",
                 touch_id != fe::InputState::kInvalidTouchId && window.input().is_touch_down(touch_id));
    report.check("the touch began this frame", window.input().was_touch_began(touch_id));

    // The action layer on top of that same contact: the acceptance test for the
    // queue item, in a real browser.
    fe::ActionMap actions;
    const fe::ActionMap::Id steer = actions.add("Steer");
    actions.bind_key(steer, fe::Key::Left);
    actions.bind_touch_region(steer, 0.0f, 0.0f, 0.5f, 1.0f);
    actions.update(window.input(), fe::Vec2{static_cast<fe::f32>(window.framebuffer_width()),
                                            static_cast<fe::f32>(window.framebuffer_height())});
    report.check("a touch drives a gameplay action in the browser", actions.is_down(steer));

    window.input().begin_frame();
    EM_ASM({
        var canvas = document.getElementById('canvas');
        var touch = new Touch({identifier : 42, target : canvas, clientX : 40, clientY : 100});
        canvas.dispatchEvent(new TouchEvent(
            'touchend',
            {touches : [], targetTouches : [], changedTouches : [touch], bubbles : true, cancelable : true}));
    });
    report.check("a touchend releases the contact", !window.input().is_touch_down(touch_id));
    actions.update(window.input(), fe::Vec2{static_cast<fe::f32>(window.framebuffer_width()),
                                            static_cast<fe::f32>(window.framebuffer_height())});
    report.check("the action releases with the contact", !actions.is_down(steer));

    // A key held while the page loses focus must not stick down.
    window.input().begin_frame();
    EM_ASM({ window.dispatchEvent(new KeyboardEvent('keydown', {code : 'Space', bubbles : true})); });
    report.check("a key is down before the blur", window.input().is_key_down(fe::Key::Space));
    EM_ASM({ window.dispatchEvent(new Event('blur')); });
    report.check("losing focus releases every key", !window.input().is_key_down(fe::Key::Space));

    // The frame-loop calls must be safe on this backend: the loop does not branch
    // on which backend it drives.
    window.input().begin_frame();
    window.poll_events();
    window.swap_buffers();
    window.set_vsync(true);
    report.check("the frame-loop calls are safe on the web backend", true);

    report.check("the web window does not claim to be closing", !window.should_close());
    window.request_close();
    report.check("request_close() is recorded", window.should_close());

    const std::string text = report.text();
    EM_ASM({ document.title = UTF8ToString($0); }, text.c_str());
    return report.passed() ? 0 : 1;
}
