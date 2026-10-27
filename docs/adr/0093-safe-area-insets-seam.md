# 0093 — Safe-area insets as a normalised seam

Date: 2026-10-27

Status: Accepted

Related: [ADR-0091](0091-mobile-viewport-and-full-bleed-canvas.md),
[ADR-0075](0075-gameplay-actions-input-seam.md),
[ADR-0077](0077-web-canvas-resize-and-dpr.md),
[ADR-0090](0090-declarative-input-bindings.md)

## Context

[ADR-0091](0091-mobile-viewport-and-full-bleed-canvas.md) makes the exported
page paint under the system UI, which is what a full-bleed game wants and also
what puts controls under it. A phone's notch, rounded corners and home indicator
reserve bands of the screen that the OS draws over. A touch region placed in one
of those bands is either covered — a thumb cannot reach it — or fires when the
player grips the phone.

The insets are known to exactly one layer. CSS exposes them as
`env(safe-area-inset-top|right|bottom|left)`, a length in CSS pixels, and only
the page can read it: there is no engine call and no window flag that reports
them. The regions, by contrast, are the engine's, and
[ADR-0090](0090-declarative-input-bindings.md) writes them as normalised
fractions of the viewport so they survive a resize and an orientation change.
Two facts, in two layers, in two units.

## Decision

`ActionMap` carries four safe-area insets as normalised fractions of the
viewport, one per edge, set through `set_safe_area_insets(top, right, bottom,
left)` and readable per edge. They default to zero, which is the identity
mapping, so every backend that is not a phone is unaffected.

The insets *shrink the space a region is measured in* rather than being
subtracted from each region. A region written `[0, 0, 0.34, 1.0]` means "the
left third of the usable area", which is what an author means and keeps one
layout valid on a notched and an unnotched phone alike. A touch inside a
reserved band remaps to a coordinate outside `0..1`, so it matches no region and
fires nothing — which is the point of reserving the band.

The unit conversion lives at the boundary that measures it. The shell reads
`env()` in CSS pixels — there is no other way to read it, since `env()` resolves
only inside a declaration — and divides by the window's width or height before
passing the values to the runtime as the `--safe-area top,right,bottom,left`
flag. The runtime therefore only ever sees fractions, the same units the regions
are written in.

A report that would reserve a whole axis is discarded rather than honoured. If
the clamped top and bottom sum to one or more, both are set to zero; the two
failure modes are not equal, since ignoring a broken measurement leaves the game
playable while honouring it makes the whole axis dead. Falling back to zero also
keeps the usable area strictly positive, so the normalised division in `update`
cannot be by zero.

The player passes the insets through and logs them when they are non-zero. It
does not clamp or interpret them; that belongs to `ActionMap`, where the regions
are.

## Consequences

- A declared layout survives a notched and an unnotched phone without a second
  document, because the insets change the space, not the regions.
- The engine learns nothing about CSS. `ActionMap` sees four floats; the shell
  is the only place that knows `env()` exists.
- The seam is testable without a browser. The unit tests drive a touch into a
  reserved band and into the usable area's own edge and assert which fires, and
  a nonsense report is asserted to fall back to the identity mapping.
- The web check cannot prove the insets are non-zero on a desktop headless
  Chromium, where they are always zero; what it proves is the value the runtime
  was given, which the shell prints into the same layout line the full-bleed
  check reads. The behaviour is proved by the unit tests, the plumbing by the
  browser.
- A game that wants a control *inside* the system UI — rare, and usually wrong —
  cannot have one through a region. It would have to read raw touch points,
  which the action seam still allows.

## Alternatives considered

- **Subtract the insets from each region in the shell.** Rejected: the shell
  would have to know every region, so it would need the bindings document, which
  is exactly the coupling [ADR-0090](0090-declarative-input-bindings.md) removed.
- **Pass the insets in CSS pixels and convert in the runtime.** Rejected: the
  runtime would need the window size at the moment the flag is parsed, and the
  conversion would be one more place for the two units to disagree. The layer
  that measured the pixels is the layer that knows how to drop them.
- **Expose the insets through the platform window interface.** Rejected for now:
  the insets are a property of the *page* the game runs in, not of the window
  the engine opened, and the web backend already has the shell as its page.
  Threading a page-only fact through the window interface would put it on every
  backend that can never have it.
- **Clamp each inset to `[0, 1)` and let the sum be whatever it is.** Rejected:
  a sum of exactly one leaves a zero-width usable area, which is the division
  the fallback exists to prevent.
