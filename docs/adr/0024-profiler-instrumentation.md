# ADR-0024: Profiler instrumentation, runtime integration, and the frame report

- **Status:** Proposed
- **Date:** 2026-10
- **Related:** [ADR-0017](0017-performance-strategy.md),
  [ADR-0022](0022-profiler-and-frame-timing.md),
  [ADR-0008](0008-systems-and-frame-schedule.md),
  [ADR-0003](0003-error-handling.md)

## Context

ADR-0022 built the `Profiler` storage: a fixed ring of frame samples, interned
names, and an RAII `ProfileScope` that records against a thread-local current
profiler. That gives the engine a way to measure, but a profiler nothing feeds
is not a feature. ADR-0017 ("measure before optimizing") makes the measurement
foundation the first M9 deliverable, and the value only appears once the engine
instrument itself and something can read the numbers.

This ADR records three decisions that ADR-0022 deliberately left open:

1. How code opens a sample cheaply and safely, including the compile-time
   disabled build.
2. How the runtime (the frame loop and the system schedule) is instrumented so a
   frame's breakdown exists without a caller adding a macro by hand.
3. What a frame's report contains and how a caller reads it without holding the
   profiler's moving ring.

The forces:

- **The macros run in hot code.** A sample must cost a thread-local read and a
  branch when idle, and one or two `now()` calls when a frame is in progress.
  Nothing may allocate, lock, or grow.
- **The disabled build must be free.** A shipping build that turns profiling off
  should pay nothing: not a branch, not an unused variable, not a string.
- **A scope must be correct across every exit path**, including early returns,
  and must not be able to write into the wrong frame.
- **The report must be a value, not a view.** A caller (a test, a log, CI, or the
  editor's future panel) reads it while the frame loop keeps running, so it
  cannot be a pointer into a ring that advances underneath it.
- **A worker thread must not corrupt the main thread's frame.** The guarantee
  must be stated, not assumed.

## Decision

### Instrumentation macros

`FE_PROFILE_SCOPE(name)` declares a `ProfileScope` in the enclosing block;
`FE_PROFILE_FUNCTION()` is the same named after `__func__`. Both are RAII, so
the destructor closes the sample on every exit path, including early returns.

`FE_PROFILING` (default 1) gates them. When it is 0 the macros expand to
`((void)0)`: no object is constructed, no name string exists, and no unused
variable is left behind. The define is per-translation-unit, so a single target
can mix instrumented and uninstrumented files (the test suite does exactly this
to prove the disabled path).

The variable name uses a two-level token concat over `__COUNTER__`
(`FE_PROFILE_CONCAT(fe_profile_scope_, __COUNTER__)`), so two scopes on one line
get distinct names and the pasted token is the expanded counter, not the literal
`__COUNTER__`.

### Cost when enabled

- With no current profiler, or no frame in progress: one `thread_local` read and
  one branch.
- With a frame in progress: one `now()` on open and one on close, one interned
  name lookup, and two array writes. No allocation, no lock, no growth.

`ProfileScope` captures the profiler and the frame generation, then opens the
scope only if `scope_active(generation)` is still true. `begin_scope` re-checks
the generation, so a scope that races a `begin_frame`/`end_frame` on the same
thread is simply not recorded rather than landing in the wrong frame.

### Thread-safety guarantee

The current profiler is **thread-local**, and a profiler is **not internally
synchronized**. The guarantee is: a profiler is recorded from exactly one thread
(the thread that called `set_current` for it and the one driving its
`begin_frame`/`end_frame`). A worker thread that wants to profile sets its own
profiler as *its* current and records there; it never touches the main thread's
ring, so it cannot corrupt a frame. There is no cross-thread aggregation -- that
is deferred to the `jobs` module (M9), at which point each thread's profiler
merges into a main-thread frame.

### Runtime integration

- `Application` owns a `Profiler` and installs it as the thread's current
  profiler for the duration of `run()`. It brackets each frame with `"frame"`,
  the update phase (the `on_update` hook plus the system run) with
  `"frame/update"`, and the render phase with `"frame/render"`. No macro is
  needed at the call site; a subclass gets the breakdown for free.
- `SceneSchedule::update` wraps each system in `"system/<name>"`, so a frame's
  cost breaks down by system. The label is built into a fixed stack buffer with
  `snprintf` and passed as a `string_view` into `ProfileScope`, which interns it;
  no heap string is created. Names are expected to be stable, which is the
  contract `System::name()` already states.
- `Application::set_profiling(false)` skips `begin_frame`/`end_frame` and the
  loop pays only the idle scope checks; `profiling_enabled()` reports it. This
  lets a host that wants zero profiler activity turn it off without a rebuild.

### The frame report

`Profiler::frame_report(offset)` returns a `FrameReport` **by value**: total
frame time, the `update` and `render` region times, the per-name totals
(`ScopeAggregate`, sorted by descending cost), the counters, and the drop
counts. It owns its `std::string` names, so it is independent of the ring and
safe to keep, copy, and pass across threads. Aggregation is a short O(n^2) over
at most `kMaxScopesPerFrame` samples, done on request -- not on the recording
path.

`FrameReport::summary()` is a stable single line for logs and CI
(`"frame 1.23ms | update 0.90ms | render 0.30ms | 12 scopes | 2 counters"`);
`to_string()` adds the per-scope and per-counter lines. `Application::last_frame_report()`
is the runtime's read-only accessor for the most recent frame.

## Consequences

**Easier:**

- The engine is instrumented where it matters (frame, update, render, per
  system) with no per-call-site effort, so a frame's breakdown is always
  available in a build with profiling on.
- A headless caller -- a test, CI, the editor -- reads a `FrameReport` value and
  asserts on structure (counts, ordering, region presence) without wall-clock
  thresholds, so it cannot flake.
- The disabled build is genuinely free: the macros vanish, verified by compiling
  one test translation unit with `FE_PROFILING=0` and confirming its object file
  references no profiler symbol.
- The report is the interface the editor's profiler panel (a later M9 item) will
  consume; the editor lane renders it and this lane never edits editor files.

**Harder:**

- `SceneSchedule::update` now formats a label per system per frame (a bounded
  `snprintf` into a stack buffer). It is small but not zero, and it happens even
  when no profiler is current; a future optimization could skip the format when
  `Profiler::current()` is null. Recorded here as a known, measured-cheap cost.
- `FrameReport` is a large value (arrays sized to the per-frame capacities); it
  is returned by value and meant to be taken on request, not every frame.
- `FE_PROFILING` is per translation unit, so a build can be partly instrumented.
  That is intended (it is how the disabled path is tested) but it means "is this
  build profiling?" is a per-file question, not a global one.

## Alternatives considered

- **A function-like `FE_PROFILE_SCOPE(name)` that expands to a statement plus a
  hidden variable.** That is what it is; the alternative is a `do {} while(0)`
  block, which would not let the scope close at the *enclosing* block's exit --
  it would close at the end of the macro's own block, timing nothing. The bare
  declaration is the only form that times the enclosing scope.
- **A runtime-only switch (no compile-time `FE_PROFILING`).** Simpler to reason
  about, but it cannot make the disabled build free, and it leaves the name
  string in the binary. ADR-0017 asks for a build that pays nothing when off.
- **A global singleton profiler instead of thread-local current.** Breaks two
  profilers in one process (tests, and the editor embedding the runtime) and
  turns thread safety into a global problem. Thread-local current is the smaller
  commitment and gives the stated per-thread guarantee.
- **Instrumenting only via explicit macros in each system.** Puts the burden on
  every caller and leaves the frame loop itself unmeasured. The runtime brackets
  the frame and the schedule times systems, so the baseline breakdown needs no
  cooperation.
- **A live view into the ring (a pointer to the newest `FrameSample`) instead of
  a snapshot.** Cheaper, but the ring advances under the reader and the sample's
  names are interned handles, not strings. A by-value snapshot is what a UI or a
  log can safely hold.
- **A rolling average maintained during recording.** Adds work to the hot path
  and a second source of truth. `scope_stats` already aggregates over the ring on
  request; the report reuses the same idea per frame.
