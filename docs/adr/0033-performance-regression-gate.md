# ADR-0033: A performance regression gate

- **Status:** Accepted
- **Date:** 2026-10
- **Related:** [ADR-0017](0017-performance-strategy.md),
  [ADR-0022](0022-profiler-and-frame-timing.md),
  [ADR-0024](0024-profiler-instrumentation.md),
  [ADR-0035](0035-ci-quality-gates.md)

## Context

Performance is a product feature for this engine
([ADR-0017](0017-performance-strategy.md)), and the near-term priorities put
"guard the performance claims" fourth: turn the benchmark into a CI regression
gate. The sanitizer half of that priority is a separate record
([ADR-0035](0035-ci-quality-gates.md)); this one is the benchmark.

Two pieces already exist. The `Profiler` records named scopes and counters per
frame into a fixed ring, and `Application` brackets each frame with
`frame`/`frame/update`/`frame/render` scopes automatically
([ADR-0022](0022-profiler-and-frame-timing.md),
[ADR-0024](0024-profiler-instrumentation.md)). What is missing is the thing that
*uses* that data: a benchmark that runs real scenes and reports frame-time
percentiles, and CI that compares those numbers against a stored baseline and
fails on a regression.

Constraints that shape the design:

- **CI runners are shared and noisy.** Wall-clock frame times vary by tens of
  percent between runs on the same commit. A gate that fails on ordinary noise
  will be disabled, which is worse than no gate.
- **The benchmark must exercise the real renderer.** A stub would measure the
  stub. That means it needs a GL context, which on a headless runner means Xvfb
  plus Mesa's software driver (llvmpipe).
- **The existing tools should be reused.** The engine already has a profiler and
  a frame report; the benchmark reads them rather than adding a second
  measurement model.

## Decision

Add a headless benchmark, a coarse regression gate, and a CI job that runs it.

### The benchmark

`benchmarks/scene_bench.cpp` is a small `fe::Application` that builds a synthetic
2D scene and runs a fixed number of headless frames through the real frame loop,
so the measurement is of the engine's own `renderer2d` and `physics2d` systems
rather than a synthetic loop. Three named scenes (`sprite_heavy`,
`physics_heavy`, `mixed`) and per-count flags let a caller shape the load.

It reports **percentiles, not an average**: a frame-time average hides the tail
that stutters. p50 describes a typical frame; p95 and p99 describe the worst
frames. It drops a warm-up prefix, because the first frames pay one-off costs
(shader compile, buffer growth, allocator warm-up) that a steady-state gate
should not track.

Output is machine-readable — CSV by default, JSON with `--json` — and a
`--out PATH` option writes the same block to a file for CI to keep as an
artifact. `--help` documents the surface. The benchmark skips cleanly (exit 0)
when there is no display or GL backend, so its `ctest` smoke run stays green on a
machine without one; a failure *after* the context exists still propagates.

### The regression gate

`benchmarks/check_regression.py` reads a recorded baseline
(`benchmarks/baseline.json`) and the run from the current build and exits
non-zero when a metric regressed beyond a tolerance. The design is deliberately
coarse so the gate survives CI noise:

- **Only the p50 is gated.** p95/p99 are reported but not gated, because the
  worst frames on a shared runner are dominated by noise the engine does not
  control.
- **The tolerance defaults to 25%.** A real regression is larger; runner noise
  is not.
- **A floor (0.5 ms) skips the check when the baseline is tiny.** Below a
  fraction of a millisecond the measurement is dominated by timer resolution, so
  a percentage comparison is meaningless.
- **`--update` re-records the baseline.** An intentional change is recorded, not
  worked around.

The baseline is committed. It is a number from one machine, not a promise about
all machines; it is meaningful because CI compares like against like (the same
runner image, the same Release build), which is what a regression gate needs. It
is therefore recorded *on the CI runner* — from the gate job's
`benchmark-result` artifact — not locally: a developer machine is faster than a
shared runner, and a locally recorded baseline made the gate fail on the very
first pull request. See `benchmarks/README.md` for the re-recording procedure.

## Consequences

- **Easier:** a 2x slowdown in the frame loop is caught before it lands; the
  benchmark doubles as a dev tool (`fork_bench --scene physics_heavy`); the
  numbers are recorded per run as a CI artifact for later inspection.
- **Harder:** the baseline needs re-recording when a change intentionally moves
  the numbers, and the gate's coarse tolerance will not catch a small regression
  (that is the intended trade-off, not an oversight).
- **Known limitation:** the gate runs on Linux only, because it needs a GL
  context and the Linux runner supplies one via Xvfb. The other platforms keep
  their build-and-test coverage; only the timing gate is Linux-only.

## Alternatives considered

- **Gate the mean instead of percentiles.** Rejected: the mean is pulled around
  by the same tail that makes frames stutter, and it hides whether typical
  frames got worse.
- **Gate p99.** Rejected as flaky on shared runners; p99 is reported for a human
  but not gated.
- **A tighter tolerance (5–10%).** Rejected: it would fail on CI noise and be
  disabled. A gate that is off protects nothing.
- **A dedicated benchmark framework (Google Benchmark).** Rejected for now: it
  adds a dependency and a second measurement model. The engine already has a
  profiler; the benchmark uses it. A framework can replace this later behind the
  same output format if the need appears.
- **A stub renderer, to keep the benchmark headless without a GL context.**
  Rejected: it would measure the stub, not the engine, and the real render path
  is the thing worth guarding.
