# 0095 — Verifying a web run for GL errors, not just pixels

Date: 2026-10-25

Status: Accepted

Related: [ADR-0074](0074-web-window-backend.md), [ADR-0076](0076-3d-in-the-browser.md),
[ADR-0071](0071-web-first-2d-shipping-path.md)

## Context

`3d-in-the-browser` (docs/SHIPPING_QUEUE.md) is done when "an exported 3D project renders
a non-blank frame in headless Chromium **with no GL error**". The web game check
proved the first half and could not prove the second.

The check reads the player's verdict out of the page title and asserts a live
canvas, a live loop, and a non-blank frame — at least two distinct colours. That
last check is strong against a dead renderer: a scene that never drew, a shader
that failed to compile, a null renderer all produce one colour. But it is blind
to a *rejected* draw. A WebGL2 context enforces rules a desktop GL 3.3 context
tolerates, and it reports a violation by recording a `glGetError` code and
dropping the call, not by failing the frame. The frame that follows can still be
non-blank — the main pass keeps drawing after the shadow pass has failed — so a
browser-only GL error left the run green.

This is not hypothetical. ADR-0076 fixed two such defects: the shadow map's depth
texture used a format/type pair GLSL ES 3.00 rejects, and the shadow pass drew
into a framebuffer with a colour attachment enabled. Both were invisible to the
pixel check and were found by reading the browser console by hand.

## Decision

**Add a diagnostics seam to the RHI device, and assert on it in the web check.**

`rhi::Device` gains:

```cpp
[[nodiscard]] virtual std::vector<std::string> drain_errors();
```

It drains the backend's pending error queue and returns one human-readable
description per error, clearing them, so a later call sees only errors raised
after this one. The default returns nothing.

The properties that make it a seam rather than an OpenGL detail:

- **The default is honest, not a stub.** A backend with no error queue has no
  errors to report, so "none" is the true answer, not a placeholder. The null
  device never touches a graphics API; a caller that drains a device it did not
  create reads an empty list and does not branch on which backend it holds.
- **It is a drain, not a flag.** `glGetError` is a queue: each call returns one
  code and clears it. A drain loops until `GL_NO_ERROR`, bounded so a lost
  context — which reports `GL_CONTEXT_LOST` forever — cannot spin.
- **An unknown code is reported, not dropped.** A code the name table does not
  know is still a real error, so it is returned by number. Silently discarding
  it is the failure the drain exists to prevent.

The player drains the device after its last frame and puts the count in the
report the check reads: `glerr=N`. Each error is also logged, so a failure names
what the driver rejected rather than only that it did. The web game check fails
when `glerr` is non-zero.

Two tests pin the seam:

- a headless unit test asserts the null device's drain is empty, so the default
  cannot drift into a fabricated error a caller would have to special-case;
- a graphics test renders a 3D scene through the OpenGL device and asserts the
  drain is empty afterwards. This is the desktop half: a correct render leaves
  the queue clean, so a change that makes the drain report a false positive
  fails on a desktop context rather than as a confusing red web job.

## Consequences

- The web check now proves the second half of `3d-in-the-browser`'s acceptance:
  the frame is non-blank *and* the driver accepted every call that produced it.
- A browser-only GL error fails the run at the point it is introduced, with the
  rejected call named in the log, instead of being found by eye later.
- The drain is bounded and cheap, and runs once, after the last frame. It costs
  one frame's worth of `glGetError` calls, which is nothing.
- The web report gains a field. The check parses it with `sed` like the others,
  and the smoke test does not use the player, so neither needs a coordinated
  change beyond the one field.
- Draining only after the last frame means an error raised on an earlier frame
  and *cleared* by a later one would be missed. Nothing clears the queue except
  a drain, and nothing else drains, so in practice the queue accumulates every
  error since the context was created — which is the behaviour wanted.
- The seam reports *that* a call was rejected, not which call. Naming the call
  would need either a `glGetError` after every command (a real cost on the hot
  path) or a debug-group marker (`KHR_debug`, not guaranteed under WebGL2). The
  count plus the frame the error appeared is enough to localise a defect, and the
  per-pass structure of the renderer makes "after the last frame" a small window.

## Alternatives considered

- **Assert `glGetError()` inside the OpenGL backend on every call.** Rejected: it
  is a synchronous round-trip to the driver, and the render path issues many
  calls per frame. It would turn a shipping runtime's frame cost into a debugging
  cost. A drain at one point, in the verification run, keeps the hot path clean.
- **Parse the browser console for GL errors in the check.** Rejected: the console
  is not in the DOM dump the harness reads, so it would need the debugging
  protocol — a second, heavier harness for a fact the engine already knows.
- **A `--check-gl-errors` flag on the player.** Rejected: the report already
  carries the verdict, and a flag makes the check opt-in, which is how a check
  rots. The drain runs whenever the player reports a frame, which is exactly when
  the web check observes it.
- **Fail the frame at the first error inside the engine.** Rejected: an error is
  often recoverable and the game should keep running; a shipping runtime that
  aborted on a recoverable driver quirk would be worse than one that draws the
  rest of the frame. Surfacing the error to the verifier, not aborting the game,
  is the right split.
