# ADR-0032: Near-term platform scope

- **Status:** Accepted
- **Date:** 2026-10
- **Related:** [ADR-0018](0018-multi-platform-shipping.md),
  [ADR-0010](0010-3d-rendering-foundation.md)

## Context

"Games ship anywhere" is one of the project's three pillars
([ADR-0018](0018-multi-platform-shipping.md)): a game built in Fork Engine
should reach every platform its players use. That is the right long-term goal,
and it is what the RHI abstraction and the platform layer exist to make possible.

The risk is that a long-term goal is read as a present-tense claim. Today the
engine renders through **OpenGL 3.3 core** on desktop. That backend is fine for
Linux and Windows, but it is deprecated on macOS, and it does not run on the web
or on consoles at all. A README that says "reaches every platform its players
use" without qualification promises more than the engine can currently deliver,
and an aspirational claim that a reader can falsify in five minutes costs more
credibility than it buys.

Two failure modes to avoid: **over-claiming** (the README promises ports that do
not exist) and **over-building** (adding a WebGPU backend now, before a single
game has been built, to justify the claim). Both are scope errors.

## Decision

Scope the shipping claim to what the engine actually does, and make the wider
goal a roadmap item with a concrete plan rather than a present-tense promise.

- **The near-term target is desktop.** Linux and Windows through the OpenGL 3.3
  core backend are the supported path. macOS is *buildable* but not yet a
  supported target, because GL 3.3 is deprecated there.
- **The wider goal stays a goal.** Web (Emscripten/WASM + WebGL2/WebGPU), mobile
  (GLES/Vulkan/Metal), additional desktop backends (Vulkan/Metal/WebGPU), and
  consoles remain the direction of
  [ADR-0018](0018-multi-platform-shipping.md) and the M8 milestones — sequenced,
  not claimed.
- **Documentation must match reality.** The README and the status table state the
  current platform support plainly and link to the plan; they do not assert
  ports that do not exist.
- **No new graphics backend until a game needs one.** The RHI abstraction is
  already the seam; proving it with a second backend is valuable but is
  sequenced *after* a vertical slice, not before it. Adding WebGPU now would be
  building for a promise instead of a need.

## Consequences

- The README is falsifiable and true: a reader can build and run on desktop and
  find exactly what is described.
- The "ship anywhere" pillar is preserved as an explicit, planned direction, so
  the ambition is not lost — only the false present-tense framing.
- Portability work (S4 web, S5 mobile) waits behind the vertical slice and the
  scripting seam, which is the same ordering the roadmap's near-term priorities
  set.
- If a second backend is added later, it validates the RHI abstraction as
  intended; this ADR does not forbid it, it only refuses to *require* it before
  there is a game to run on it.

## Alternatives considered

- **Keep the unqualified "ship anywhere" claim.** Rejected. It is the kind of
  claim a reader disproves quickly, and honesty about scope is part of the
  project's stated philosophy.
- **Add a WebGPU backend now to make the claim true.** Rejected. It is real,
  ongoing cost (a second backend to maintain, test, and debug) paid before any
  game exists to need it — breadth before depth, which the guiding rule warns
  against.
- **Drop the multi-platform pillar entirely.** Rejected. The abstraction and the
  platform layer are already built for it, and shipping is a genuine
  differentiator; the fix is to *sequence* the claim, not abandon the goal.
