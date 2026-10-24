# 0074. The web window backend

Date: 2026-10-24

Status: Accepted

## Context

ADR-0073 brought the engine's *build* to the web: an Emscripten toolchain, a
`FE_PLATFORM_WEB` macro, and the promise that a web build would one day link. It
did not bring the engine's *runtime* there, because the platform layer had a hole
in it that only became visible once the toolchain existed:

* `Window.cpp` contained the GLFW backend directly — `#include <GLFW/glfw3.h>`,
  `glfwCreateWindow`, the key translation, the lot. GLFW cannot be compiled to
  wasm at all, so a web build could not merely avoid *calling* it; it could not
  compile it. The toolchain item had added `FE_PLATFORM_WEB` and then had nothing
  to do with it in the one file that needed it.
* `WindowBackend` offered `GLFW` and `Null` and nothing else. `Null` is
  deliberately context-less (`has_context() == false`), so selecting it on the web
  would give a window with no surface and no device — an engine that runs but
  draws nothing, which is not shipping a game to a browser.
* The engine linked `FE::glad` unconditionally, and glad is a desktop loader:
  there is no driver to resolve against in a browser, and the symbol would not
  link for wasm.

The web target therefore configured but did not generate, and once the link line
was fixed it did not compile. The gap was structural, not a missing flag: the
platform layer had one window implementation baked into the base class, and the
web needs a second one.

## Decision

Make the window backend a real seam, with three implementations behind one
interface, chosen at compile time from the platform.

1. **`Window` is backend-neutral.** The base class holds no platform state and
   names no windowing library. Every OS call moved to a subclass, and the base
   defaults are the "no OS, no surface" behaviour. `Window.cpp` is now
   platform-free and compiles unchanged for desktop and web.

2. **`WindowBackend::Web` joins the enum, and `WebWindow` implements it.** It is a
   canvas-backed WebGL2 context: `has_context()` is true (unlike `Null`), so the
   runtime creates a device for it exactly as on desktop. `get_proc_address()`
   resolves through `emscripten_webgl_get_proc_address`, which is the web
   equivalent of the GLFW loader.

3. **The GLFW code moves to `GlfwWindow.{h,cpp}`, and the two backends are
   mutually exclusive at build time.** `FE_PLATFORM_WEB` selects `WebWindow.cpp`
   or `GlfwWindow.cpp`, both in CMake and in `Window.cpp`'s factory, so a web
   build links no GLFW and a desktop build links no Emscripten.

4. **The GL declarations follow the same split.** The device includes `<glad/gl.h>`
   on desktop and `<GLES3/gl3.h>` on web — Emscripten's own header, which declares
   the same names with the same types. The device code between them is identical;
   only the declarations and the "load the entry points" step differ, and on web
   there is nothing to load because the browser supplies them.

5. **A build-time choice is paired with a runtime test.** `tests/web/run_web_smoke.sh`
   compiles the real backend sources to wasm and runs the result in headless
   Chromium with SwiftShader, asserting a live WebGL2 context and real entry-point
   resolution. Compiling to wasm is not evidence that a browser will give a
   context, so the acceptance test has to actually ask for one.

The web backend is selected by `WindowDesc::backend`, and in a web build the
factory also serves the default (`GLFW`) request with it — so a game that never
names a backend works unchanged in a browser, which is the property the whole
shipping path depends on.

## Consequences

* The engine library builds and links for `wasm32`, and the web window backend is
  proven to obtain a real WebGL2 context in a browser. The shipping path
  (ADR-0071) has a surface to render into.
* GLFW is now genuinely private: it lives in one translation unit, appears in no
  public header, and is absent from a web build entirely rather than present and
  unused. A third backend (a mobile one, say) is a new subclass and one case in
  the factory, not a change to the runtime.
* `WindowDesc` grows a `canvas` field, which only the web backend reads. It is a
  string selector rather than an element handle because the descriptor is a
  value that crosses the public API, and a DOM node is not a thing the engine
  should be handing around by pointer.
* The device still has one `#if defined(FE_PLATFORM_WEB)` for its GL header. That
  is the honest boundary — the declarations really do come from a different place
  — but it does mean a third GL platform would want the include factored into a
  small `fe/rhi/opengl/GLHeaders.h` rather than a third branch. Not worth doing
  for two.
* `has_context()` stays the one runtime branch on "is there a surface?". The web
  backend answers true, so `Engine::initialize()` takes the graphics path with no
  new special case for web.
* Two latent bugs in ADR-0073's CMake were found and fixed: `fe_mark_system()`
  called with an unquoted empty variable became a zero-argument call and failed
  the configure, and the engine linked `FE::glad` unconditionally. Neither was
  caught because that item's test exercised the platform classifier in script
  mode and never ran a web configure.

## References

* ADR-0041 — the platform contract and the null backend.
* ADR-0042 — the null RHI device (why `has_context()` decides the graphics path).
* ADR-0071 — the web-first 2D shipping path.
* ADR-0072 — the GLSL dialect seam (why the web context must be WebGL2/GLSL ES 3.00).
* ADR-0073 — the Emscripten toolchain and the `FE_PLATFORM_WEB` macro.
* `tests/web/run_web_smoke.sh` — the acceptance test for this decision.
