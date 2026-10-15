# ADR-0055: An in-editor profiler panel fed by the core profiler

- **Status:** Accepted
- **Date:** 2026-10
- **Related:** [ADR-0022](0022-profiler-and-frame-timing.md),
  [ADR-0024](0024-profiler-instrumentation.md),
  [ADR-0033](0033-performance-regression-gate.md),
  [ADR-0007](0007-editor-ui-toolkit.md),
  [ADR-0001](0001-engine-architecture-overview.md)

## Context

The M9 roadmap closes with *"an in-editor profiler panel fed by the profiler
data."* Everything it stands on already exists: `core::Profiler` with
`ProfileScope` (ADR-0022), the `FE_PROFILE_SCOPE`/`FE_PROFILE_FUNCTION` macros
and the runtime's frame/update/render bracketing (ADR-0024), a by-value
`FrameReport` that is the documented stats surface, and a benchmark regression
gate that reads the same numbers in CI (ADR-0033). What is missing is the
in-editor read-out.

The forces are specific to the editor. The profiler in a shipped game belongs to
`Application`, which owns the frame loop and installs it as the thread's current
profiler for each frame. **The editor is not an `Application`.** ADR-0001 and
docs/ARCHITECTURE.md section 8 make the editor a client of the runtime that
embeds an `Engine` and drives frames itself, so that play-in-editor shares the
exact runtime a game uses. The editor therefore has no profiler, and its frame
is not bracketed by anything. Adding a panel means deciding where the editor's
profiler comes from and who brackets the editor's frame.

Two constraints shape the decision. First, the panel must show **the same
numbers** the profiler records and CI gates on — a second measurement would be a
second source of truth, which is exactly what ADR-0024 works to avoid. Second,
the panel's content must be **testable without a window or a GPU**, because the
editor's headless tests never create a GL context (ADR-0007 keeps the toolkit in
one place; the panels are thin projections of headless view models).

## Decision

**The editor owns an optional `Profiler` in `EditorContext`, installs it as the
thread's current profiler for the duration of each frame, and exposes a headless
view model that reshapes the profiler's latest `FrameReport` for a thin ImGui
panel to draw.**

### The profiler lives in the document, not the engine

`EditorContext` gains `std::optional<Profiler> profiler_`, created lazily by
`ensure_profiler()` and reached through `profiler()`. It is a `std::optional`
rather than a by-value member because `Profiler` is non-movable and large (the
frame ring is inline storage, roughly a quarter of a megabyte); a host that
never shows the panel should not pay for it, and an optional holds the object in
place while still allowing "no profiler yet".

It is *not* added to `Engine`. `Engine` owns long-lived subsystems in a fixed
creation order and deliberately does not run a frame loop (docs/ARCHITECTURE.md
section 6); a profiler is a frame-loop concern, which is why `Application` owns
one. The editor drives its own loop, so the editor owns its own profiler, by the
same rule.

### The host brackets the frame; the panel never starts recording

`EditorHost::step()` installs the document's profiler as the thread's current
one, calls `begin_frame()`, runs the frame (UI + panels + scene), then
`end_frame()`, and restores the previous current profiler. The panel is a pure
reader: it calls `capture` on the view model and never begins or ends a frame.
This keeps the lifecycle in the one place that owns the loop, the same
arrangement ADR-0022 uses for `Application`.

While the panel is drawn, it reads the *previous* committed frame — the frame
being recorded is not finished yet. That is the honest read-out, and it is the
same offset `Application::last_frame_report` defaults to.

### The view model is the headless half

`ProfilerPanelViewModel` turns a `FrameReport` into ordered scope rows and
counter rows, plus the frame totals and the truncation flag. It reads the
profiler only through the public `frame_report()` surface. `capture(const
FrameReport&)` is the pure reshaping step, split out from `capture(const
Profiler&)` so a test can drive it with a hand-built report and assert on the
mapping without a profiler, a clock, or a machine-dependent duration. The ImGui
panel (`ProfilerPanel`) only emits widgets over the view model, which is what
lets the content be tested headlessly.

### The panel profiles itself, honestly

`capture(const Profiler&)` brackets the read with `FE_PROFILE_SCOPE
("editor/profiler_panel/capture")`, so the cost of taking a read-out appears in
the profiler's own per-name breakdown. The panel is a consumer of the profiler
it displays, and the macro is a no-op when profiling is compiled out or no
profiler is current, so a release build without instrumentation pays nothing.

## Consequences

The editor gains a live frame read-out fed by the same `Profiler` and
`FrameReport` the runtime and CI use, with no second timing layer: the numbers a
developer sees in the editor are the numbers CI gates on. The panel shows the
frame total, update and render regions, the per-name scope breakdown with each
scope's share of the frame, the counters, and a warning when a frame overflowed
the profiler's fixed capacity.

Because the profiler is optional and created lazily, a headless editor test that
never calls `ensure_profiler()` allocates none of it. The view model's pure
`capture(const FrameReport&)` overload means the panel's content is covered by
ordinary headless tests that assert on structure, never on absolute timings.

The costs are small and stated: the editor's frame is now bracketed by the
profiler (cheap and allocation-free by ADR-0022), the panel reads one frame
behind (it shows the last completed frame, not the one in flight), and the
profiler is editor-local — it does not observe the embedded engine's internal
scopes, only the scopes the editor's own loop and panels record. Showing the
engine's internal work would mean instrumenting `Engine`/`Application`'s regions
inside the editor, which is out of scope here and belongs with the parallel
extraction and job-system work that will add those scopes.

## Alternatives considered

**Add a second timing layer in the editor.** Rejected: it would duplicate
`Profiler`, and two measurements drift. ADR-0024 exists precisely so the stats
surface is one thing.

**Have the editor reuse `Application`'s profiler.** Rejected: the editor does
not use `Application`; it drives `Engine` frames directly (ADR-0001). Reaching
into an `Application` the editor does not own would invert the dependency.

**Put the profiler in `Engine` so both loops share one.** Rejected here as
scope: `Engine` has no frame loop and no notion of a frame, so a profiler there
would sit unused until something brackets it, and it changes a core runtime
class shared with the standalone game path. The editor-local profiler gets the
feature landed without that churn; unifying the two can be a later ADR if it
earns its keep.

**Draw straight from a `FrameReport` member with no view model.** Rejected: the
mapping (which scopes, what order, how a truncated frame reads) would then live
in ImGui code and could not be tested without a GL context, which the editor's
headless tests deliberately avoid.
