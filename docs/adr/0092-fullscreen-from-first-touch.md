# 0092 — A fullscreen request from the first touch

Date: 2026-10-27

Status: Accepted

Related: [ADR-0091](0091-mobile-viewport-and-full-bleed-canvas.md),
[ADR-0071](0071-web-first-2d-shipping-path.md)

## Context

[ADR-0091](0091-mobile-viewport-and-full-bleed-canvas.md) makes the exported
page a full-bleed surface: the canvas covers the window and the page never
scrolls. That is the whole of the *page's* box, but a phone browser's window is
not the whole screen. The address bar and the tab strip sit above it, and they
are not merely cosmetic — they take vertical space a full-bleed canvas would
otherwise use, and on a landscape phone they take a band that a thumb resting on
the screen edge can hit instead of the game.

The browser's answer is the Fullscreen API, and its rule is that a page may only
enter fullscreen from a user gesture. A page cannot ask on load, and a page that
asks from a timer or from the first frame is refused. So the shell needs a
gesture, and it needs to be the *earliest* one that a game reliably receives.

## Decision

The shell requests fullscreen from the first touch on the canvas, and from the
first pointer press as a fallback for a browser that reports one but not the
other. The listener is `{ once: true }`, so the request is made at most once
rather than on every touch, and passive, so it cannot delay the first frame's
input.

The request is best-effort and its failure is swallowed:

- if the API is absent (`requestFullscreen` and the `webkit`-prefixed spelling
  both missing), the shell does nothing;
- if the request returns a promise that rejects — the common case on a desktop
  browser, an embedded webview, or a page under a policy that forbids fullscreen
  — the rejection is caught and ignored;
- if the call throws synchronously, the throw is caught and ignored.

A refusal is not an error. The game is already playable in the frame it has;
fullscreen only makes it larger. Reporting a refusal as an error would turn the
normal desktop experience into a visible failure, which is worse than the band
of browser chrome it is trying to remove.

The gesture is taken on the canvas rather than on the document, matching where
the web backend binds its own touch and pointer listeners
([ADR-0074](0074-web-window-backend.md)): a touch that reaches the game is a
touch that can also grant fullscreen.

## Consequences

- A phone game leaves the browser chrome on the first tap, so the canvas reaches
  the screen edges and the full-bleed layout is actually full.
- Nothing changes on a desktop: the request is made, refused, and ignored, and
  the game keeps playing in its tab.
- The first touch does double duty — it is both the fullscreen gesture and a
  gameplay input — which is the intended behaviour but worth stating, because a
  game that reads the first touch as "start" now also changes the screen shape
  under the player.
- The check is a source assertion, not a browser one: a headless Chromium in CI
  cannot grant fullscreen without a user gesture it does not have, so the shell's
  promise to *ask* is asserted, and its promise to *degrade* is asserted by the
  catch. A behavioural test would need a browser that grants fullscreen to a
  synthetic event, which no browser does.

## Alternatives considered

- **A visible fullscreen button.** Rejected: it adds a control to every game,
  needs its own layout and its own safe-area handling, and makes the common case
  (a phone, where fullscreen is always wanted) require a tap before it looks
  right.
- **Request fullscreen from the first key press as well.** Rejected for now: a
  keyboard is a desktop signal, and a desktop browser refuses anyway, so the
  listener would be dead code on the only target it could fire on. The pointer
  fallback already covers a browser that reports pointer events but not touch.
- **Keep the state in a variable and re-try later.** Rejected: a browser only
  grants fullscreen from a live gesture, so a retry outside one is refused for
  the same reason the first attempt on load was. `{ once: true }` states the
  intent plainly.
