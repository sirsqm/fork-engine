# 0091 — A mobile viewport and a full-bleed canvas

Date: 2026-10-27

Status: Accepted

Related: [ADR-0071](0071-web-first-2d-shipping-path.md),
[ADR-0074](0074-web-window-backend.md), [ADR-0076](0076-3d-in-the-browser.md),
[ADR-0077](0077-web-canvas-resize-and-dpr.md)

## Context

Phase 1 put a game in a browser and Phase 2 puts the same game on a phone. The
shell the export writes ([ADR-0071](0071-web-first-2d-shipping-path.md)) was
built for the first, and it shows.

The page centred a fixed-aspect canvas in a scrollable body:

```css
body { display: flex; align-items: center; justify-content: center; }
#canvas { width: 100%; max-width: 1280px; aspect-ratio: 16 / 9; }
```

On a desktop that is a reasonable letterbox. On a phone it is three bugs at
once. The `max-width` and the fixed aspect ratio mean the canvas is smaller than
the window in at least one axis, so the game is letterboxed and does not use the
screen it was given. `html, body { height: 100% }` with a centred child and no
`overflow` rule makes the page taller than the viewport, so a drag can scroll
the page instead of reaching the game. And the viewport meta was the minimum
one — `<meta name="viewport" content="width=device-width, initial-scale=1">` —
with no `viewport-fit`, so a phone that would otherwise paint under a notch or a
home indicator is instead boxed into the safe rectangle, and the safe-area
insets a later item needs are always zero.

There is a second, quieter failure. The canvas is the element the web backend
binds pointer and touch listeners to, because their coordinates are reported
relative to the element the listener is on
([ADR-0074](0074-web-window-backend.md)). A canvas that does not reach the
window edge has a dead band between it and the screen edge; a touch there lands
on the body, which has no listener, and the game never sees it. A player holding
a phone by its edges touches exactly there.

Nothing about this fails loudly. The build succeeds, the game loads, and it
looks merely wrong — which is why the fix needs a test that measures the layout
rather than one that only checks the game ran.

## Decision

The export shell's page is a full-bleed, non-scrolling surface:

- the canvas is `position: fixed; inset: 0; width: 100%; height: 100%`, so it
  covers the window's whole CSS box with no letterbox and no dead band;
- `html, body { overflow: hidden; overscroll-behavior: none }` so a drag cannot
  scroll the page away from the game;
- the viewport meta is
  `width=device-width, initial-scale=1, viewport-fit=cover, user-scalable=no`,
  so the page is laid out at the device width, may paint under the system UI,
  and does not zoom on a double tap;
- `touch-action: none` on the canvas and the body, so the browser does not claim
  a drag as a scroll or a pinch before the game receives it;
- the shell measures its own layout — the canvas box and the window — and writes
  it into a hidden `#layout` element as a single `FE_SHELL_LAYOUT` line.

The measurement is what makes the decision testable. A headless browser cannot
judge "looks right", but it can read a number out of the DOM, so the web game
check runs the 2D slice at a phone's CSS size (844x390, an iPhone 14's box
rotated) and fails unless the canvas box equals the window box. The stylesheet
is asserted directly too, for the properties a screenshot could not distinguish.

The measurement goes into a hidden element rather than the page title because
the runtime already writes its own result into the title, and two writers would
race.

## Consequences

- The game uses the whole screen on a phone and on a desktop, and a touch at the
  screen edge reaches the backend.
- The safe-area insets become non-zero on a notched phone, which is the
  precondition for the `mobile-safe-area` item that follows.
- A game that wants a letterbox (a fixed-aspect game on a wide desktop) now has
  to ask for it in its own page; the shipped default fills the window. That is
  the right default for a game whose target is a phone, and a game with an
  opinion can replace the shell.
- The shell carries a measurement hook that is dead weight in a shipped build
  apart from one string assignment, which is a fair price for a check that
  catches a silent layout regression.

## Alternatives considered

- **Keep the letterbox and add a "fullscreen" button.** Rejected: it makes the
  common case (a phone) the special case, and the dead band at the window edge
  remains until the button is pressed.
- **Set the canvas size from script on resize.** Rejected: the backend already
  follows the CSS box through `WebWindow::sync_canvas_size`
  ([ADR-0077](0077-web-canvas-resize-and-dpr.md)), so the page only has to make
  the box the right size. Scripting the size would be a second source of truth
  for the same fact.
- **Assert the layout by screenshot.** Rejected: it needs a golden image, which
  is brittle across SwiftShader versions, and a canvas that is off by a few
  pixels is exactly what a fuzzy image compare would let through. A measured
  number is exact.
