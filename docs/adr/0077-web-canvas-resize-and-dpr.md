# ADR-0077 — The web canvas size and the input coordinate space

## Status

Accepted.

## Context

A browser gives a `<canvas>` two independent sizes, and the web backend
(ADR-0074) originally knew about only one of them:

* the **CSS box** is the element's laid-out size in CSS pixels. The page owns
  it, and the export shell makes the canvas fluid (`width: 100%`, a 16:9
  `aspect-ratio`), so it changes when the browser window is resized or a phone is
  rotated. A DOM event reports its coordinates relative to this box.
* the **drawing buffer** is the canvas's `width`/`height` attributes in device
  pixels. The engine owns it, and it is what the renderer's viewport must match.

At creation the backend set the drawing buffer once from `WindowDesc::width` and
`height`, and `framebuffer_width()`/`height()` read it back. Two things then went
wrong, and both are exactly the case the web target exists for:

1. A fluid shell lays the canvas out at a size the descriptor never knew, so the
   buffer was the wrong resolution from the first frame and never followed a
   resize.
2. `devicePixelRatio` was ignored. On a HiDPI display or a phone a CSS pixel is
   several device pixels, so a canvas that filled the viewport was rendered at a
   fraction of its real resolution.

Worse, the input path compared the two spaces directly: DOM events report CSS
pixels, `InputState` was fed those numbers, and the action layer normalises touch
regions against the drawing buffer. On any display where the two differ, every
touch and click was offset — a game that is unplayable on the very phones phase 2
targets.

## Decision

Introduce `fe::CanvasViewport` (`fe/platform/CanvasScale.h`), a pure value that
maps a CSS box and a device pixel ratio to a drawing-buffer size and the scale DOM
coordinates convert through. The web backend:

* registers the browser's `resize` event and calls `sync_canvas_size()`, which
  reads the canvas's CSS box (`emscripten_get_element_css_size`) and the display's
  ratio (`emscripten_get_device_pixel_ratio`), then sets the drawing buffer to the
  box in device pixels. `framebuffer_width()`/`height()` therefore follow the page
  with no frame-loop change;
* calls the same function once at creation, so the first frame is already correct
  rather than a frame behind;
* converts every DOM event coordinate — pointer and touch alike — through the
  recorded scale before handing it to `InputState`, so gameplay, the action
  layer's normalised regions, and the viewport all share one space;
* keeps the last good mapping when the canvas has no CSS size (a hidden tab, a
  `display:none` element) rather than collapsing the viewport to zero.

The ratio is clamped to at least 1, and the scale is derived from the *rounded*
buffer rather than the raw ratio so a converted point always lands inside the
buffer that was actually set.

## Consequences

* The engine follows a page that resizes the canvas, and renders at the display's
  real resolution, with no wiring on the page beyond the resize event the browser
  already sends.
* Touch and pointer input land where they look on any display, which is what makes
  a phone playable rather than merely responsive.
* The arithmetic is pure and lives in one tested place (`test_canvas_scale.cpp`),
  so the backend's resize and input paths cannot drift from each other. The
  browser-level acceptance check is in `tests/web/run_web_smoke.sh`, which resizes
  the canvas in a real browser and observes both the buffer and a converted
  coordinate.
* `width()`/`height()` now report the CSS box rather than the requested size, so a
  caller that wanted the requested size must keep it itself. Nothing in the engine
  reads them for layout.

## Alternatives considered

* **Scale in the input callbacks only, leaving the buffer fixed.** Fixes the
  coordinate offset but not the resolution: a phone would still render at the
  canvas's default 300x150 buffer stretched over the screen.
* **Scale the buffer by the ratio only, ignoring the CSS box.** Renders sharp but
  keeps the wrong size whenever the page lays the canvas out differently from the
  descriptor, which the fluid export shell always does.
* **A resize handler in the export shell's JavaScript.** Puts engine policy in a
  generated page, cannot be unit-tested, and every embedder would have to repeat
  it. The backend is the one place that already owns the canvas.

## References

* ADR-0071 — the web-first 2D shipping path.
* ADR-0074 — the web window backend this refines.
* `tests/src/test_canvas_scale.cpp` — the unit tests for the mapping.
* `tests/web/run_web_smoke.sh` — the in-browser acceptance test.
