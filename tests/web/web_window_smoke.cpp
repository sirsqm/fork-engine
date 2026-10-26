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

    // --- Canvas resize and the input coordinate space -----------------------
    //
    // A browser page owns the canvas's CSS box, so the engine must follow it: the
    // drawing buffer — what the renderer's viewport uses — and the space DOM
    // event coordinates are converted into both change when the page resizes the
    // element. The page is resized here and a real `resize` event dispatched,
    // which is exactly what a phone rotation or a desktop window drag produces.
    // This is the acceptance test for `web-resize-and-dpr` in a browser.
    const fe::u32 buffer_before = window.framebuffer_width();
    EM_ASM({
        var canvas = document.getElementById('canvas');
        canvas.style.width = '640px';
        canvas.style.height = '400px';
        window.dispatchEvent(new Event('resize'));
    });
    report.check("the drawing buffer follows the canvas CSS box after a resize",
                 window.framebuffer_width() == 640 && window.framebuffer_height() == 400);
    report.check("the resize changed the drawing buffer", buffer_before != 640);

    // A DOM coordinate must land in the resized buffer, not in the old one. The
    // pointer listener reports canvas-relative CSS pixels and the backend scales
    // them into the drawing buffer; on this headless display the ratio is 1, so
    // the point is the CSS coordinate and the test asserts it is inside the new
    // 640x400 buffer rather than the stale 320x200 one.
    window.input().begin_frame();
    EM_ASM({
        var canvas = document.getElementById('canvas');
        canvas.dispatchEvent(new MouseEvent('mousemove', {clientX : 320, clientY : 200, bubbles : true}));
    });
    const fe::Vec2 moved = window.input().mouse_position();
    report.check("a DOM coordinate maps into the resized drawing buffer",
                 moved.x > 300.0f && moved.x < 340.0f && moved.y > 180.0f && moved.y < 220.0f);

    // The action layer's regions are normalised against the drawing buffer, so a
    // contact on the right half of the resized canvas must drive an action bound
    // to the right half. That ties the resize to gameplay, which is the point.
    window.input().begin_frame();
    EM_ASM({
        var canvas = document.getElementById('canvas');
        var touch = new Touch({identifier : 7, target : canvas, clientX : 480, clientY : 200});
        canvas.dispatchEvent(new TouchEvent(
            'touchstart',
            {touches : [touch], targetTouches : [touch], changedTouches : [touch], bubbles : true, cancelable : true}));
    });
    fe::ActionMap resized_actions;
    const fe::ActionMap::Id right = resized_actions.add("Right");
    resized_actions.bind_touch_region(right, 0.5f, 0.0f, 1.0f, 1.0f);
    resized_actions.update(window.input(), fe::Vec2{static_cast<fe::f32>(window.framebuffer_width()),
                                                    static_cast<fe::f32>(window.framebuffer_height())});
    report.check("a touch on the resized canvas drives the action for its half", resized_actions.is_down(right));

    // The frame-loop calls must be safe on this backend: the loop does not branch
    // on which backend it drives.
    window.input().begin_frame();
    window.poll_events();
    window.swap_buffers();
    window.set_vsync(true);
    report.check("the frame-loop calls are safe on the web backend", true);

    // --- Page visibility ----------------------------------------------------
    //
    // A backgrounded tab receives no animation frames, so the wall clock runs on
    // while the engine is not stepped; on return the frame that spans the hidden
    // span must be rebaselined, not charged to one step (docs/adr/0082). The
    // backend records the transition so the host can report it to the engine.
    //
    // This drives the real `visibilitychange` event with the document's state
    // flipped, which is what the browser does when a tab is backgrounded and
    // restored. The listener is bound to the *document*, so that is where the
    // event is dispatched. Crucially it does it the way the browser does — hidden
    // then visible with no frame in between — so the check proves both
    // transitions are reported, in order, not just the latest state: a single
    // "is it hidden now?" flag would lose the hidden one and the engine would
    // never rebaseline.
    bool hidden = false;
    report.check("no page-visibility change is pending at rest", !window.take_page_visibility_change(hidden));
    EM_ASM({
        Object.defineProperty(document, 'hidden', {value : true, configurable : true});
        document.dispatchEvent(new Event('visibilitychange'));
        Object.defineProperty(document, 'hidden', {value : false, configurable : true});
        document.dispatchEvent(new Event('visibilitychange'));
    });
    report.check("the hidden transition is reported first", window.take_page_visibility_change(hidden));
    report.check("the first report says the page is hidden", hidden);
    report.check("the visible transition follows it", window.take_page_visibility_change(hidden));
    report.check("the second report says the page is visible", !hidden);
    report.check("the queue is drained", !window.take_page_visibility_change(hidden));

    report.check("the web window does not claim to be closing", !window.should_close());
    window.request_close();
    report.check("request_close() is recorded", window.should_close());

    const std::string text = report.text();
    EM_ASM({ document.title = UTF8ToString($0); }, text.c_str());
    return report.passed() ? 0 : 1;
}
