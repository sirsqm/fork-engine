# ADR-0069: Parallel extraction and culling — measure first, chunk the instance array

- **Status** — Accepted
- **Supersedes** — none
- **Superseded by** — none

## Context

[ADR-0017](0017-performance-strategy.md) commits the engine to parallel CPU work
and names the extraction seam as the place to do it: the flat instance array
produced by [ADR-0052](0052-render-extraction-seam.md) "is also the unit that a
job system can chunk". [ADR-0068](0068-job-system.md) landed the `jobs` module
(thread pool, `parallel_for`, a dependency graph, and a single-threaded mode that
is serial-equivalent) and left the follow-on row open: **parallel extraction and
culling, with output identical to the serial path.**

Three forces shape how that row should be built:

1. **Performance is a product feature, and it is measured** (ADR-0017). The
   strategy is explicit that a change which cannot show a win does not land, and
   that measurement comes before optimisation. So the first question is not "how
   do we parallelise this?" but "which step, and from what size, actually pays
   for a fan-out?" — and the answer has to come from numbers on the reference
   machine, not from intuition.
2. **The draw step reads the instance array, so a different array is a different
   image.** Any parallel path must reproduce the serial array *exactly* — the same
   instances, in the same order. This is a correctness property, not a
   performance one, and it is what the tests have to pin.
3. **Culling is cheap per instance; extraction is not.** The two steps have
   opposite cost profiles, so a single "parallelise the render step" decision
   would be wrong for one of them.

## Measurement

The reference machine is 4 workers, the null device, timing only the step with
the input copy excluded (so the numbers are the work, not the plumbing). The
sweep:

| instances | extract serial | extract parallel | extract speed-up | cull serial | cull parallel | cull speed-up |
| --------: | -------------: | ---------------: | ---------------: | ----------: | ------------: | ------------: |
|      1200 |        ~0.10ms |           ~0.10ms |            1.01x |     0.016ms |       0.062ms |         0.26x |
|      2000 |        0.147ms |           0.118ms |            1.25x |     0.029ms |       0.080ms |         0.36x |
|      8000 |        0.598ms |           0.309ms |            1.93x |     0.131ms |       ~0.20ms |         0.66x |
|     20000 |        ~1.4ms  |           ~0.6ms |            2.3x  |     ~0.30ms |       ~0.27ms |         0.93x |
|     40000 |            —   |                — |               —  |     ~0.60ms |       ~0.40ms |         1.48x |

Two conclusions fall out, and they are the whole decision:

- **Extraction parallelises cleanly and crosses over early.** Break-even is around
  1200 nodes; from 2000 it is a real win that grows with size. Extraction is the
  larger part of the render step at these scales (at 2000 sprites, 0.147ms of a
  ~0.18ms render, roughly 80%).
- **Culling does not pay to parallelise until it is large.** The per-instance test
  is a handful of floats; the fan-out (queue the chunks, wake the workers, join)
  is a fixed cost a small array never repays. It is a *loss* until roughly 16000
  instances. The bottleneck is not the visibility test — that is trivially
  parallel — it is the compaction that has to follow it.

The honest reading: parallelise extraction for real, and keep culling serial
below a high threshold rather than parallelising it because it looks parallel.

## Decision

**Chunk the instance array across the job pool for extraction above a measured
threshold, and for culling above a much higher one; keep both serial below their
thresholds and when no pool is supplied; guarantee byte-identical output to the
serial path by construction, and assert it with exact-equality tests.**

### Two thresholds, an order of magnitude apart

`render::kParallelExtractThreshold` (1024) and `render::kParallelCullThreshold`
(16384) encode the table above. Below its threshold a step runs the serial loop
even when a `JobSystem` is supplied, because the fan-out costs more than the work
it saves. The thresholds are deliberately conservative — the cost of
parallelising too eagerly is a slower frame — and they are documented as a
starting point to re-measure, not a law.

### Equivalence by construction, not by luck

Both steps are pure functions of an array, so both are chunkable, and both keep
output identical to serial by a construction that does not depend on the
scheduler:

- **Extraction** flattens the scene's nodes into a list first (a pointer tree is
  not indexable, a flat list is), splits the list into contiguous chunks, and has
  each chunk append to its **own** vector. The vectors are then concatenated in
  chunk order. The fan-out order is unspecified; the concatenation order is not,
  and no two chunks write the same slot. The result is the serial array exactly.
- **Culling** tests visibility in parallel into a `u8` mask indexed by position,
  then compacts with a single serial pass. The mask makes the survivors keep their
  original relative order. This is also the shape the measurement supports: the
  visibility test parallelises, the compaction — the actual bottleneck — stays one
  sequential pass over the array rather than N chunk-local lists to re-join.

### The parallel walks share the serial resolution

Each step's per-node work lives in one helper (`resolve_sprite`, `resolve_mesh`,
`append_tile`) that both the serial and the parallel walk call. The two paths
cannot diverge on *which* nodes count or on *what* a node resolves to, because
there is only one place that decides.

### The pool reaches the seam through the renderers, and only when asked

`render::extract_*` and `render::cull_*` take a trailing `JobSystem* jobs = nullptr`.
`Renderer2D::render`/`extract`/`cull` and `Renderer3D::render`/`extract`/`cull_flat`
pass it through, defaulted to null. `Application::render_frame` supplies its own
pool, so an interactive run gets the win; every other caller (the editor's
viewport, play-in-editor, the sandbox, the tests) keeps the serial path with no
source change. `Renderer3D::cull` keeps its BVH, which is already sub-linear, so a
pool does not change it.

## Consequences

- An interactive run with a worker count set extracts large scenes in parallel and
  draws the same image. A run with no pool — the default — is byte-for-byte the
  old behaviour.
- The win is reproducible: `fork_bench --textured --workers N` exercises the path.
  At 60000 sprites under the software rasteriser, `frame_render` p50 fell from
  10.22ms to 7.98ms (workers 0 → 4).
- The tests pin the property that matters: a new `test_render_parallel.cpp` builds
  scenes past each threshold, runs the step serially and in parallel, and compares
  with `SpriteInstance`/`MeshInstance` `operator==` — exact, not approximate — so a
  lost, duplicated, or reordered instance fails the test. It also covers the
  below-threshold path, the `JobSystem(0)` path, `meshes_seen` order, a null
  frustum, and equal draw/cull counts through `render()`.
- `render` now depends on `jobs` (a forward declaration in the header; the
  `.cpp` includes the pool). `jobs` depends only on `core`, so this adds no cycle.
- The thresholds are a maintenance point: a machine with a different core count or
  a much cheaper per-instance test would want different numbers, and the comment
  says so.
- Culling is left serial at the target scales on purpose. A future change that
  makes the per-instance test much more expensive (a real box-box test, a
  per-instance LOD decision) would move its crossover down, and the threshold
  should move with it.

## Alternatives considered

- **Parallelise culling everywhere, because it looks parallel.** Rejected on the
  measurement: it is a 2.8x *loss* at 2000 instances, and it only wins past
  ~16000. Parallelising a step that is already cheap is how a "performance" change
  makes frames slower.
- **Per-chunk output vectors for culling, like extraction.** Measured and
  rejected: the fan-out and the re-join of N vectors cost more than the mask plus
  one serial compaction. The mask is the cheaper compaction.
- **A single shared threshold for both steps.** Rejected: the crossovers differ by
  an order of magnitude (1200 vs 16000), so one number would be wrong for one of
  them — too eager for culling, or too lazy for extraction.
- **Parallelise only extraction and never culling.** Tempting, and almost what
  landed — but a threshold that is never crossed is not the same as a step that
  cannot be parallelised. The mask-based cull is there for the scale where it wins,
  behind a number that keeps it off below that scale.
- **Approximate equality in the equivalence tests.** Rejected: the draw step reads
  the array, so a reordered or lost instance is a different image; an approximate
  compare would pass while the image changed. Exact equality is the property, and
  `Mat4`/`SpriteInstance`/`MeshInstance` gained a defaulted `operator==` to make it
  a one-line assertion.
- **Change the benchmark's default workload to textured sprites.** Rejected: it
  would invalidate the recorded baseline and change what the CI gate measures. The
  textured workload is opt-in (`--textured`), so the gate is untouched and the win
  is still reproducible by hand.
