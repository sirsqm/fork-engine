# ADR-0022: A frame profiler in `core`

- **Status:** Proposed
- **Date:** 2026-10
- **Related:** [ADR-0017](0017-performance-strategy.md),
  [ADR-0003](0003-error-handling.md), [ADR-0008](0008-systems-and-frame-schedule.md)

## Context

ADR-0017 set the performance strategy: "measure before optimizing". It named a
`Profiler` in `core` with `ProfileScope` RAII scopes recording into a fixed ring
buffer, and made that the first M9 deliverable, because every later M9 item
(render extraction, the job system, the per-frame arena) needs a way to show a
number going down and to catch a regression. This ADR records the concrete
interface and storage decisions that ADR-0017 left open, now that the profiler
is being built.

The forces:

1. **The hot path must not allocate.** A scope is opened and closed around work
   that runs every frame, often thousands of times. A `std::string` name, a
   `std::vector` push, or a hash-map insertion per scope is itself a source of
   frame-time noise — measuring would perturb what is measured.
2. **The number of frames and scopes is bounded, but the bound must fail
   visibly.** A frame that records more scopes than the buffer holds is a signal
   about the program, not something to hide by growing.
3. **Measurement must be correct across frames.** A scope that outlives its
   frame (an exception path, a mis-scoped object) must not corrupt a later
   frame's sample.
4. **Headless testability.** ADR-0008 makes systems testable with no window and
   no GPU; the profiler that times them must be equally testable, with
   assertions that do not depend on wall-clock magnitude (they would flake on
   CI).

## Decision

Build `fe::Profiler` in `core` as a fixed-capacity frame ring with interned
names, a thread-local "current" profiler, and an RAII `ProfileScope`.

### Storage: a fixed ring of frame samples

- The profiler owns `std::array<FrameSample, kMaxFrames>` (64 frames) inline.
  Each `FrameSample` holds fixed arrays of `ScopeSample` (128) and
  `CounterSample` (32). Nothing on the recording path allocates or grows.
- Names are interned once into a fixed `std::array<std::array<char, 64>, 128>`
  table. A scope stores a `u32` handle, not a string. Interning scans the table
  (at most 128 entries) — acceptable because names are interned rarely, and it
  keeps the table allocation-free and deterministic.
- The ring keeps the most recent `kMaxFrames` frames; `frame_from_latest(0)` is
  the newest. Overwrites are silent by design (it is a ring), but a frame that
  exceeds the per-frame scope or counter capacity increments `dropped_scopes` /
  `dropped_counters`, so overflow is observable rather than lost.
- The object is large (roughly a quarter of a megabyte). It is documented as a
  long-lived member or heap allocation; it is never created per frame.

### Recording: RAII scope over a thread-local current profiler

- `ProfileScope` captures `Profiler::current()` (a `thread_local` pointer) on
  construction, opens a scope in the current frame, and records the duration on
  destruction. With no current profiler, or no frame in progress, it is a no-op.
  There is no `begin`/`end` pair for callers to mismatch.
- A frame is bracketed by `begin_frame()` / `end_frame()`. Both return
  `Result<void>` and fail on misuse (double begin, end without begin).
- Each `begin_frame()` bumps a `frame_generation`. `ProfileScope` stores the
  generation it opened under and passes it back on destruction; `end_scope`
  ignores a scope whose generation no longer matches. A scope that outlives its
  frame therefore cannot write into a later frame's sample.

### Timing and units

- `std::chrono::steady_clock` only: monotonic, so a wall-clock adjustment cannot
  make a duration negative. Durations are exposed as `f64` milliseconds.
- Queries: `frame_from_latest(offset)`, `scope_stats(name)` (last/min/max/avg/
  count), and `counter_value(name, frame_offset)`. `scope_stats` and
  `counter_value` return `fe::Result<T>` for expected failures (unknown name,
  absent from the frame, offset out of range), matching ADR-0003.

## Consequences

**Easier:**

- Recording is allocation-free and O(1) amortized, so the profiler can be left on
  in a debug build without changing the shape of the frame.
- `scope_stats` is exactly the data a future in-editor profiler panel and the M9
  benchmark target need; they read the same accessors the tests do.
- The thread-local current profiler means a worker thread (once the job system
  lands) records on its own profiler and cannot corrupt the main thread's frame.
- Everything is deterministic in structure: a test asserts on scope counts,
  nesting depths, and counter values, never on wall-clock magnitude.

**Harder:**

- The capacities are compile-time constants. A scene with more than 128 scopes in
  a frame must raise the constant (and accept the larger object) — deliberately,
  rather than growing at runtime.
- `Profiler` is not copyable or movable, and it is large; it must be placed
  carefully. The header says so.
- Interned names are bounded at 64 characters and 128 distinct names per
  profiler. Longer names are truncated; a program that exceeds 128 names counts
  the drops.
- The current design is single-thread-per-profiler. Aggregating across threads is
  out of scope until the job system exists.

## Alternatives considered

- **An unbounded `std::vector<FrameSample>` with a configured history length.**
  Simplest to write, but it allocates on growth and its footprint is a runtime
  surprise. A fixed ring matches ADR-0017 and makes the memory cost explicit.
- **`std::string` names per scope.** Obvious, but every scope construction would
  allocate or copy. Interning a `u32` handle is the cheap, deterministic choice.
- **Manual `begin(name)` / `end()` for callers.** No RAII wrapper. It is easy to
  forget an `end`, and a missed `end` silently mis-nests everything after it.
  `ProfileScope` makes the common case impossible to get wrong.
- **A global singleton profiler instead of thread-local.** Easier to reach, but
  it breaks multiple profilers in one process (tests, and later the editor's
  play-in-editor embedding the runtime) and makes thread safety a global problem.
  Thread-local current is the smaller commitment.
- **A `ScopeSample` holding a `std::string_view` into a caller-owned literal.**
  Avoids the name table, but the lifetime of the view is the caller's problem and
  a temporary name would dangle. The interned table owns its bytes.
- **Sampling profiler (stack sampling on a timer) instead of explicit scopes.**
  It finds hot code without instrumentation, but it needs a signal handler or a
  helper thread, is platform-specific, and cannot report the engine's counters.
  Explicit scopes are the portable first step; a sampling profiler is a separate
  tool, not a replacement.
