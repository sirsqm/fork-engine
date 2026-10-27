# 0082 — The page visibility event is what the clock is told about

Date: 2026-10-25

Status: Accepted

## Context

A backgrounded browser tab stops receiving animation frames. The page is hidden,
so the browser stops calling `requestAnimationFrame`, and the wall clock keeps
running. When the player returns, the frame callback fires again and the delta
the loop measures spans the whole time the tab was hidden.

ADR-0079's clamp (`kMaxDeltaTime`, 0.25 s) bounds how far one step can move the
simulation, but it does not fix the case: the player was not playing, so *no*
game time passed, and a bounded step is still a lurch. `web-visibility-pause`
(docs/PLAN.md) asks for the honest answer — a resume advances the simulation by
nothing. The engine half of that already exists and is recorded here: `Engine`
owns the clock policy, `notify_page_visibility(visible)` rebaselines it so the
first frame after a hidden span steps by zero and the next frame measures
normally, and `set_time_source()` lets a host supply the frame time rather than
have the engine measure a second clock. (That half landed first; the code cites
this record.)

What was missing was the wiring. The engine can only rebaseline if the host tells
it the page changed visibility, and the host learns that from the platform. The
platform is the only layer that sees the browser's `visibilitychange` event, and
no part of the web backend listened for it. So an exported game ran the engine's
rebaseline code path zero times: the mechanism was tested headless and inert in
the shipped build. The transition also cannot be polled. A host that asked "is
the page hidden now?" each frame would only ever see "visible": the frames that
would have observed the hidden state never run, because the platform stopped
delivering them. The change has to be recorded by the event, when it happens, and
held until a frame comes back to read it.

## Decision

The platform records the page-visibility transition on the window; the frame loop
forwards it to the engine before the engine reads the clock.

* `Window` gains `note_page_visibility(hidden)` and
  `take_page_visibility_change(hidden)`. They live on the base `Window`, beside
  `InputState`, for the same reason the input event handlers do: the platform
  reports events through the object they belong to, and a test can inject one to
  exercise the path without a browser.
* The transitions are **queued**, not collapsed to the latest state. This is the
  subtle part. On the web *no frame runs while the page is hidden*, so a tab that
  is backgrounded and restored between two frames records two transitions —
  hidden, then visible — with no frame in between to observe the first. A single
  "is it hidden now?" flag would report only "visible" on the next frame, the
  engine would never learn a hidden span occurred, and the resume would not be
  rebaselined. The queue is a small fixed ring (a host drains it in a loop), so
  the hidden span is reported in full and in order, and the mechanism allocates
  nothing.
* The web backend registers a `visibilitychange` listener
  (`emscripten_set_visibilitychange_callback`) and records `document.hidden` in
  it. This is the only layer that can: the event is a DOM event and the frames
  that would observe the hidden state never run.
* `Application`, the frame loop, drains the queue at the frame boundary and calls
  `Engine::notify_page_visibility(!hidden)` for each transition, before
  `engine_.tick()`. The engine owns the clock policy; the host owns the wiring. A
  desktop window never records a transition, so the drain does nothing and its
  clock is unchanged.
* The null and desktop backends are untouched: they never call
  `note_page_visibility()`, and the base-class queue stays empty.

## Consequences

* A backgrounded browser tab resumes without a jump: the player is where they
  left off. The engine's rebaseline, which was already written and tested, is now
  actually reached by the shipped build.
* The path is tested at both ends: the engine test proves the clock policy
  headless, a new frame-loop test drives the window's one-shot through
  `Application` and proves it reaches the engine, and the web smoke test drives a
  real `visibilitychange` event and checks the backend records it. No layer
  assumes another's half.
* The window carries two more bools and the backend one more listener. The cost
  is paid once per window, not per frame.
* A host that drives `step_frame()` gets the wiring for free. A host that calls
  `Engine::tick()` directly must read the window's one-shot and report it itself,
  the same two lines `Application` runs.
* The desktop is deliberately unchanged. A minimised desktop window keeps its
  loop running and its clock advancing, which is the existing behaviour and what
  a desktop game expects; only a browser stops delivering frames.

## Alternatives considered

* **Detect the resume in the engine by comparing visibility across frames.**
  Rejected: the engine has no visibility of its own, and the frames that would
  observe the hidden state never run, so there is no edge to compare.
* **Poll `document.hidden` from the frame loop.** Rejected for the same reason:
  by the time a frame runs again the page is visible, so a poll sees no change.
  The event is the only place the hidden state is observable.
* **Have the web backend call `Engine::notify_page_visibility()` directly.**
  Rejected: the platform layer does not know the engine, and a window is created
  and destroyed around the engine. Keeping the event on the window and the policy
  on the engine preserves the layering the platform contract depends on
  (ADR-0041).
* **Make `take_page_visibility_change()` a virtual with a per-backend override.**
  Rejected as unnecessary: the state is the same for every backend (there is no
  transition until one is recorded), so one base implementation serves them all,
  and only the recording call is web-specific.
* **A general "time jumped" heuristic instead of a visibility event.** Rejected:
  the platform event distinguishes a hidden tab from a genuine stall for free,
  and the two want different behaviour — a resume steps by nothing, a stall steps
  by the clamp.
