# ADR-0068: A job system — a thread pool, `parallel_for`, and a task graph

- **Status** — Accepted
- **Supersedes** — none
- **Superseded by** — none

## Context

[ADR-0017](0017-performance-strategy.md) names parallelism as one of the three
things that make performance a product feature, and commits the engine to a
`jobs` module: "a fixed thread pool, `parallel_for`, and a small task graph with
dependencies. `FrameContext` exposes it to systems." It also fixes the boundary
that makes the module safe to add: threads may extract, cull, and compute, but
**the RHI device stays single-threaded**, because that preserves the device
contract and avoids a locking layer inside it.

The module is the last unbuilt row of M9's shape. The extraction seam
([ADR-0052](0052-render-extraction-seam.md)) already produces a flat instance
array, and ADR-0017 says that array "is also the unit that a job system can
chunk"; the follow-on row, parallel extraction and culling, cannot be built
without it. So the job system is a prerequisite, not a parallel workstream.

The forces:

1. **The API must not force callers to branch on threads.** A system that
   parallelises a loop should read the same in a game, a headless tool, and a
   test. If the caller has to ask "are there threads?", every call site grows a
   second path and the two drift.
2. **A nested call must not deadlock.** The natural implementation - submit N
   chunks, block until N are done - hangs if a chunk itself submits work and every
   worker is busy waiting. This is the classic thread-pool deadlock, and it is
   reachable from ordinary code (a parallel extraction that calls a parallel
   cull).
3. **`std::thread`, not a scheduler.** ADR-0017 already decided this: the surface
   needed is small and the standard library is everywhere. A third-party task
   scheduler is deferred until the graph grows past what a few hundred lines
   provide.
4. **A concurrency surface needs a sanitizer.** ADR-0017 states the job system
   "must be tested against the serial path and, once it lands, run under a thread
   sanitizer." Testing against the serial path is only meaningful if a
   single-threaded mode exists to compare against.

## Decision

**Add a `jobs` module with a `JobSystem` that has two execution modes behind one
API - a fixed `std::thread` pool and a single-threaded inline mode - plus
`parallel_for` and a dependency `TaskGraph`; the submitting thread helps run the
queue while it waits; `FrameContext` exposes the system to systems; and CI runs
the pure-logic suite under ThreadSanitizer.**

### Two modes, one API

`JobSystem(worker_count)` with a non-zero count creates that many worker threads.
`JobSystem()` - the default - creates none: every job runs inline on the calling
thread, in order. The two modes are not a convenience wrapper over the threaded
one; they are both first-class, and the API is identical in both. A caller never
branches on which it has, so:

- a system that calls `parallel_for` reads the same everywhere;
- a test can assert the parallel result equals a serial loop by running the same
  call through a single-threaded system and comparing;
- a platform without threads (a browser main thread, a constrained target) selects
  the inline mode by asking for zero workers, not by compiling out the module;
- adding a `JobSystem` to existing code changes no behaviour until a worker count
  is asked for.

### `parallel_for` is serial-equivalent

`parallel_for(begin, end, body)` splits `[begin, end)` into contiguous chunks and
runs them on the pool, blocking until all are done. The guarantee is *serial
equivalence*: the body is invoked once per index, exactly once, each index seeing
the value a serial `for` would give it. Only the **order** of calls is
unspecified, so the body must not assume one chunk ran before another; two indices
that write the same memory are a data race the caller avoids (chunk-local scratch,
or an index-derived output slot). The chunk split is deterministic - the same
range splits the same way every run - so a parallel result is directly comparable
to the serial one. An empty range invokes nothing, and a range shorter than the
worker count uses one chunk per index, so no worker is handed an empty chunk.

### The submitting thread helps

The pool has one mutex, one condition variable, and one queue of callables. A
callable reports completion by decrementing its batch's remaining count; the
submitting thread waits on its batch **and runs queued tasks while it waits**.
Helping is what makes a nested call safe: a worker that calls `parallel_for` has
no other worker guaranteed to pick up its chunks, so it must be willing to run
them itself. The cost is that a nested call does not add parallelism - it runs its
work on the calling thread - which is the correct trade for a module whose job is
to not deadlock.

### The task graph is a builder, and a bad graph runs nothing

`TaskGraph` records tasks and their dependencies; `JobSystem::run` executes it. A
dependency is a hard ordering edge, and a task runs only after every task it
depends on has finished; tasks with no unmet dependency may run in any order,
including in parallel. A graph is the right shape only when that is true - two
tasks that write the same memory must be ordered by a dependency.

The graph is validated **before any body runs**: a dependency on an id that was
never added, or a cycle, returns an `Error` (ADR-0003 - errors are values) and
runs nothing, so a caller never observes a partial run of a bad graph. Validation
is a topological sort (Kahn's algorithm), which doubles as the execution order the
single-threaded mode uses.

A task body must not throw: an exception escaping a worker would terminate the
process. That is the standard library's rule, not a choice made here, and it is
documented on `TaskGraph` so a body that can fail records the failure itself.

### The device stays single-threaded

A job must not touch the RHI device. The module does not enforce this - it cannot
see what a body does - so it is a documented rule on `FrameContext::jobs`: a job
extracts, culls, or computes, and only the thread that owns the frame submits.
This is ADR-0017's boundary, restated where the code will read it.

### ThreadSanitizer in CI

`FE_ENABLE_TSAN` (mutually exclusive with `FE_ENABLE_ASAN`, which is a configure
error) builds with `-fsanitize=thread`, and a CI job runs the pure-logic suite
under it. Only `fork_engine_tests` runs: TSan instruments every thread, so the GL
binaries - Mesa/llvmpipe, GLFW, X11 and their driver threads - report races
entirely inside third-party code, the same way the ASan job excludes their leaks.
The engine tests need no display, so the job installs no X11/Wayland packages and
runs no virtual display.

## Consequences

- The follow-on row, parallel extraction and culling, has the pool and the chunk
  unit it needs; it becomes a change to one step of the extraction seam, not to
  the renderers' structure.
- Every system can parallelise without owning threads, and a test can prove the
  parallel path equals the serial one by running both modes.
- The default is single-threaded, so the module lands without changing any
  existing run: `Application` creates a pool only when a worker count is set.
- The pool is created in `Application::run` and joined when it returns, so no job
  outlives the frame loop; a job must not capture data that dies sooner.
- A nested call runs inline on the calling thread, so a deeply nested job tree
  gains no parallelism. This is documented, not a bug to fix later without a
  scheduler.
- The engine now depends on the platform thread library (`Threads::Threads`),
  linked `PUBLIC` so a game that starts its own threads gets the same flags.
- TSan is a new CI job on the critical path. It is cheap (the pure-logic suite is
  seconds) and it is the only automated guard against a data race in the pool.

## Alternatives considered

- **A third-party job system (Taskflow, enkiTS).** Deferred, as ADR-0017 already
  decided: the surface needed is a pool, `parallel_for`, and dependencies, which
  `std::thread` provides without a new dependency. Revisit if the graph grows.
- **A work-stealing scheduler.** Rejected for the first cut: it is a larger,
  subtler structure for a win that a shared queue with a helping waiter already
  captures at this scale, and it is harder to keep deterministic enough to
  compare against the serial path.
- **A pool-only module, with the single-threaded case left to the caller.** This
  was the tempting simplification and it was rejected: the single-threaded mode is
  what makes the serial-equivalence test possible, what a threadless target needs,
  and what lets the module land without changing behaviour. Making it the default
  is what keeps the API free of "are there threads?" branches.
- **A detached-thread or future-based `parallel_for`.** Rejected: a caller that
  must join the work before the frame ends gains nothing from a future it
  immediately waits on, and blocking submission is the simplest correct contract.
- **Enforcing the single-threaded-device rule in the module.** Rejected as
  unworkable: the module cannot see what a body touches, and a check that cannot
  be made is worse than a documented rule. Review is the guard, as it is for the
  frame arena's lifetime rule (ADR-0017).
- **Running TSan across the whole suite, graphics included.** Rejected: it would
  make the job permanently red over third-party races, which is exactly the
  failure mode the ASan job's `detect_leaks=0` already documents. The jobs tests
  start real threads and need no display, so they are where the guard is
  meaningful.
