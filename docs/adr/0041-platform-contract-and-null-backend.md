# ADR-0041: The platform contract and a null/headless backend

- **Status:** Accepted
- **Date:** 2026-10
- **Related:** [ADR-0018](0018-multi-platform-shipping.md),
  [ADR-0032](0032-near-term-platform-scope.md),
  [ADR-0010](0010-3d-rendering-foundation.md)

## Context

"Games ship anywhere" is a pillar, and [ADR-0018](0018-multi-platform-shipping.md)
sets the shape of the work: the `platform` module is the *complete* OS contract,
and a null backend exists to prove nothing leaks past it. The first M8 item is to
make that concrete — audit the contract and add the null backend — because today
the engine cannot run its frame loop without a display.

The gap is narrow and specific. `Window::create()` always goes through GLFW, so
it fails with `glfwInit failed: no display or platform backend available` on a
machine with no display. `Application` already has a `headless_` mode, but it
still creates a real GL context: a "headless" CI run today works only because the
workflow wraps it in `xvfb-run`. That is a virtual display, not headlessness. A
dedicated server, a container, or a CI runner with no X server cannot run the
engine at all.

What the engine actually needs from the OS is small and well bounded, so the
audit is mostly a matter of writing it down and deciding what is in scope now
versus later. The pieces already exist behind the module (`Window`,
`InputState`), or in other modules that hide the OS behind their own seam
(`core::fs`, `core::Log`, `rhi::Device`, the audio device, the import cache).
The audit records which of those are already "platform" responsibilities in
practice and what a second backend must provide.

## Decision

**Define the platform contract explicitly, and add a null backend that
implements the window/input part of it with no OS, selected through a data field
on `WindowDesc` rather than a new interface.**

### The contract

The `platform` module owns every OS-specific behaviour the engine depends on.
The contract is grouped by concern, and each group states where it lives and
whether it is implemented now.

| Concern | Where it lives today | Status |
| --- | --- | --- |
| Window / surface | `platform::Window` (GLFW backend) | Implemented |
| Input: keyboard, mouse, scroll | `platform::InputState`, fed by the window backend | Implemented |
| Input: touch, gamepad | — | Deferred; see "Where touch and gamepad attach" |
| Graphics context | `rhi::Device` + `Window::get_proc_address` | Implemented (OpenGL 3.3 core) |
| Filesystem | `core::fs` (+ `resources::ResourceSource`, `pack::PackSource`) | Implemented |
| Threads | `core` (std only); the `jobs` module (`JobSystem`, ADR-0068) | Implemented |
| Time | `runtime::Engine` (steady clock); `platform` is where a backend would inject it | Implemented, but in the runtime |
| Dynamic libraries | — | Deferred |
| Lifecycle (init/shutdown) | `Window::create`/destructor; `Engine::initialize`/`shutdown` | Implemented |
| Lifecycle (suspend/resume, orientation) | — | Deferred (mobile) |

The contract is deliberately stated in terms of *what the engine depends on*, not
*every OS facility*. A concern is in the contract only when engine code would
otherwise reach for the OS directly. Filesystem and time are listed even though
they live in `core` and `runtime`, because a future target (a browser sandbox, a
console) must supply them; the seam is the existing interface, not a new one.

### Where touch and gamepad attach

Touch and gamepad are the same input model as the keyboard and mouse, so they
attach to the existing seam rather than a new one:

- `InputState` gains `on_touch(...)` and `on_gamepad_*(...)` alongside the
  existing `on_key`/`on_mouse_*`. A backend translates its native events into
  those calls exactly as the GLFW backend translates key codes today.
- Gameplay reads abstract actions (the input layer is already keyed by engine
  enum values, not native codes), so touch and gamepad map onto the same queries.
- Nothing above `platform` branches on which device produced a value.

That is the whole design; this ADR does not implement it. It is recorded here so
the null backend and the GLFW backend are shaped to accept it without a rewrite.

### The null backend

A **null window** implements the `Window` interface with no OS and no GL context:

- `poll_events()` and `swap_buffers()` are no-ops.
- `should_close()` is false until `request_close()` is called.
- `input()` returns an `InputState` that stays empty (no events are ever fed).
- `get_proc_address()` returns `nullptr`; `native_handle()` returns `nullptr`.
- `backend_name()` returns `"null"`.

Because it satisfies the same interface, the rest of the engine does not branch
on "is this real?" at the `Window` level. The one place the *engine* must branch
is where a graphics device would be created: a null window has no context, so
`Engine::initialize` selects the null path and **skips device and renderer
creation explicitly** rather than pretending to succeed.

### Selecting the backend: a field, not a new interface

The concrete backend is chosen through `WindowDesc::backend`
(`WindowBackend::GLFW` or `WindowBackend::Null`), not through a new `Platform`
interface with a factory.

Justification:

- **The window is the only OS entry point the engine calls directly.** The other
  contract concerns are already behind their own seams (`core::fs`, `rhi`,
  audio, pack). A `Platform` interface would be a second indirection over a
  single entry point, which is speculative abstraction.
- **A data field keeps the call sites identical.** `Engine::initialize` already
  calls `Window::create(config.window)`; choosing a backend is one more field on
  the descriptor, not a new object threaded through the runtime. No new lifetime
  to reason about.
- **It extends the existing factory seam.** `Window::create(desc)` is already the
  one place that talks to the windowing library; making it dispatch on `desc` is
  the smallest change that removes the hardcoded GLFW dependency, and it is the
  seam a second desktop backend (Windows/macOS parity, M8) will use too.

If a future target needs a contract concern the window cannot carry (for
example, a platform-provided clock the runtime must use instead of
`std::chrono`), that is the point to introduce a `Platform` interface — with a
concrete need in hand, not before.

## Consequences

- **The engine runs with no display.** A headless run builds the window as the
  null backend, creates no device and no renderers, updates its systems, and
  exits cleanly. CI can drop `xvfb-run` for the headless smoke tests.
- **Nothing above `platform` branches on the backend**, except the one explicit
  "is there a graphics device?" check in `Engine`, which is honest: a headless
  run has no device, and code that renders must say so rather than silently
  succeed.
- **The renderer path is unchanged.** An interactive run still creates a GLFW
  window and an OpenGL context, byte for byte as before.
- **The audit is a living document.** Deferred rows (touch/gamepad, dynamic
  libraries, suspend/resume) are named so they are planned, not forgotten, and
  the module graph does not change: the null backend is still `platform`.
- **The graphics tests still skip cleanly without a context**, and the headless
  path is proven by tests that run with no display.

## Alternatives considered

- **A `Platform` interface with a factory.** Rejected for now. It is the natural
  shape once several contract concerns need per-backend implementations, but the
  engine calls the OS through exactly one object (the window) today; a second
  interface over one entry point is the speculative abstraction the project's
  rules warn against. `WindowDesc::backend` removes the hardcoded dependency with
  less machinery, and the `Platform` interface can be introduced when a second
  concern actually needs it.
- **`#ifdef` the headless path.** Rejected. It scatters OS decisions through the
  code and gives no testable object; the null window is a real implementation of
  the interface, so the headless path is exercised by the same code an
  interactive run uses.
- **A null *graphics* backend (a no-op RHI device) as part of this change.**
  Rejected as scope. It is the right follow-up — it would let the render
  extraction and draw submission run headless — but it is a distinct piece of
  work with its own design (what a no-op `draw_indexed` means, how tests assert
  submission without pixels). This change skips rendering explicitly and records
  the null device as the next step.
- **Keep requiring `xvfb-run` for headless CI.** Rejected. It is a virtual
  display, not headlessness, and it does not help a dedicated server or a
  container with no X server, which is the whole point of the item.
