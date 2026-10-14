# ADR-0042: A null RHI device for headless rendering

- **Status:** Accepted
- **Date:** 2026-10
- **Related:** [ADR-0041](0041-platform-contract-and-null-backend.md),
  [ADR-0018](0018-multi-platform-shipping.md),
  [ADR-0032](0032-near-term-platform-scope.md),
  [ADR-0017](0017-performance-strategy.md)

## Context

[ADR-0041](0041-platform-contract-and-null-backend.md) added the null platform
backend so the engine's frame loop runs with no display. It deliberately stopped
short of a null *graphics* backend and recorded it as the follow-up: "it would
let the render extraction and draw submission run headless ... a distinct piece
of work with its own design (what a no-op `draw_indexed` means, how tests assert
submission without pixels)."

That gap has a concrete cost today. `Engine::initialize` skips the device and the
renderers when the window has no context, so a headless run executes the systems
but never the render path. Everything above the RHI that is not a pixel --
walking the scene, collecting instances, sorting into batches, choosing the
active camera, computing the frustum, issuing draws, and the profiler's
`frame/render` counters -- is unexercised with no display. A CI runner or a
dedicated server can run gameplay but not a single frame of rendering, and a
render-path regression that needs no pixels (a batching bug that produces too
many draw calls, a culling bug that drops a visible instance) cannot be caught
without a GL context.

The RHI is already the seam for this: `rhi::Device` is a command-oriented
interface, and the OpenGL 3.3 backend is one implementation. A second
implementation that does nothing is the smallest way to close the gap.

## Decision

**Add a null RHI backend: a `Device` whose resources are inert and whose commands
are no-ops, selected by the runtime for a contextless window when headless
rendering is requested.**

### The backend

`rhi::null::create_null_device()` returns a `Device` that:

- **Creates inert resources.** `create_shader`, `create_vertex_buffer`,
  `create_index_buffer`, `create_texture`, and `create_render_target` return
  objects that satisfy the interface and report the real shape the caller asked
  for (names, element counts, dimensions) but hold no GPU state. The validation
  matches the OpenGL backend's, so a malformed request is refused headless
  exactly as it is interactive.
- **No-ops every command.** `begin_frame`, `set_*`, `bind_*`, `draw_indexed`, and
  `end_frame` do nothing observable. `draw_indexed` is a no-op, not a counter:
  the proof that submission happened comes from the renderer's own
  `last_draw_calls()`, which is the value an interactive run reports too.
- **Rasterises nothing.** `read_pixels` leaves the caller's buffer untouched on
  purpose, so a caller can tell "nothing was read" from "the frame was black".
  A test that needs pixels belongs in the graphics suite, which requires a real
  context.
- **Reports `backend_name() == "null"`.**

### Selection: an explicit opt-in, not a fallback

The backend is chosen through a new `Engine::Config::headless_render` field,
default off. The three outcomes are:

| Window context | `headless_render` | Result |
| --- | --- | --- |
| real | (any) | OpenGL device and renderers (unchanged) |
| none | false | no device, no renderers (the ADR-0041 behaviour) |
| none | true | null device and renderers; render path runs, nothing rasterises |

`Engine::has_graphics()` stays false in both headless cases (there is still no
surface). A new `Engine::headless_render()` reports the third case, and the two
are never both true. `Application`'s render step runs when either is true, so the
one branch that decides "is there anything to draw into?" is honest about which
kind of target it has.

### Why opt-in rather than automatic

A headless run that only needs gameplay (a dedicated server stepping a
simulation) should not pay for a device and two renderers it will never use, and
the ADR-0041 behaviour -- no device at all -- is the right default for it.
Making the null device automatic would silently change what "headless" means for
every existing caller. A field keeps the two intents distinct and greppable.

## Consequences

- **The render path runs with no display.** Extraction, batching, camera
  selection, culling, and draw submission execute against an inert device, and
  the renderers' existing counters report what they submitted. CI can assert
  render-path behaviour headless, without `xvfb-run` and without a GPU.
- **The engine can render into a null device from a test directly**, without an
  `Engine`: `rhi::create_null_device()` then
  `Renderer2D::initialize(device)` and `render(device, scene, w, h)`. That is how
  the unit tests prove submission, and the same shape works for `Renderer3D`.
- **Both render paths are covered.** The tests drive `Renderer2D` (extraction,
  batching, one draw call for shared-texture sprites) and `Renderer3D` -- whose
  shadow pass binds the render target the null device created, clears it, draws
  the caster, and restores the previous target. That bind/restore sequence is
  the deepest part of the render path, and it is why the null device must return
  a usable render target rather than `nullptr`. `fork_headless --render` runs
  both renderers in one process and prints both counters.
- **The interactive path is unchanged.** A real window still selects the OpenGL
  backend through `rhi::create_device`; the new factory is a second branch on the
  same seam, not a change to the first.
- **The null device is a backend, not a platform concern.** It lives in `rhi`
  under `src/rhi/null/`, mirroring `src/rhi/opengl/`, so the module graph does
  not change.
- **Pixels remain out of scope.** The backend is explicit that it proves
  submission, not output; the graphics suite still owns pixel assertions and
  still skips cleanly with no context.

## Alternatives considered

- **A no-op `draw_indexed` that counts calls on the device.** Rejected. The
  renderers already expose `last_draw_calls()`, `last_culled()`, and the sprite
  count, which are the values an interactive run reports; a second counter on the
  device would be a parallel source of truth that could drift from them, and it
  would widen the `Device` interface for one backend's benefit.
- **Make the null device automatic for any contextless window.** Rejected for
  the reason above: it would silently add a device and two renderers to every
  headless run, changing the cost and the meaning of the existing default.
- **A null *window* that also fakes a context, so `create_device` returns a null
  device.** Rejected. It would make `has_context()` lie, and the whole point of
  ADR-0041 is that the runtime can honestly ask "is there a surface?". A separate
  factory and an explicit config field keep the question answerable.
- **Assert on `read_pixels` output (all-black).** Rejected. Black is a valid
  rendered frame, so a black readback cannot distinguish "rendered to black" from
  "did not render"; leaving the buffer untouched is unambiguous.
