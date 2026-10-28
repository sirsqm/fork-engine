# ADR-0070: Instancing, an arena-backed extraction seam, and a parallel broad phase

- **Status** — Accepted
- **Supersedes** — none
- **Superseded by** — none

## Context

[ADR-0017](0017-performance-strategy.md) makes performance a product feature and
requires a measurement before an optimisation lands. Three M9 rows were open and
each one is the same kind of decision — move work off a path that does not need
it — so they are recorded together:

1. **GPU efficiency: instancing** (docs/ROADMAP.md, M9). `Renderer2D` built one quad
   per sprite as four vertices and six indices in a CPU buffer, then uploaded the
   whole thing every frame. The vertex data for two sprites at different
   positions is identical except for the transform, so the stream was mostly
   duplicated constants.
2. **The extraction seam onto the frame arena** (ADR-0051/0054 follow-on). The
   arena existed and `Application` reset it once per frame, but nothing consumed
   it: `render::extract_sprites`/`extract_tiles` still returned `std::vector`, so
   a frame's extraction was a heap allocation, and the parallel path allocated one
   chunk list per worker.
3. **A parallel physics broad phase** (ADR-0023/0068 follow-on). The job system
   landed and the extraction seam used it, but `BroadPhase::build` — the
   O(n * cells-per-body) insert that runs once per step — was still serial.

Two forces bound all three:

- **The parallel path must produce output byte-identical to the serial path.**
  This is the recurring constraint from ADR-0069, and it is why every parallel
  step here is a *count-then-write* over a read-only input rather than a
  "each worker builds a list and we join them" shape: the join order is the
  correctness argument, and an array written in place has no join to get wrong.
- **A change that cannot show a win does not land, and a fallback must be
  invisible.** The arena does not grow, so the arena-backed seam must degrade to
  the heap path on overflow rather than fail; the broad phase must stay serial
  below a threshold where the fan-out does not pay.

## Decision

**Instanced rendering in `Renderer2D`.** `rhi::VertexAttribute` gained a
per-instance advance flag and `rhi::Device` gained `bind_instance_buffer` and
`draw_indexed_instanced`. `Renderer2D` now uploads the unit-square corners once
(shared by every sprite) and a per-instance record of world transform, UV rect,
and colour, then issues one indexed instanced draw per texture batch. The
per-vertex UV convention is preserved exactly (bottom-left corner maps to `v = 1`)
so the instanced image matches the pre-instancing one pixel for pixel.

**An arena-backed extraction seam.** `render::extract_sprites_into`,
`extract_tiles_into`, and `cull_sprites_in_place` take a caller-sized
`std::span` backed by the frame arena; `render::sprite_instance_bound` gives an
upper bound to size it. The parallel paths take their scratch (the flat node list,
the per-chunk counts and offsets, the cull mask) from the arena too, so a frame
whose extraction comes from the arena performs no heap allocation in the 2D pass.
`Renderer2D::render(..., FrameArena&, JobSystem*)` and `Application::render_frame`
use it; a null arena or an exhausted one falls back to the existing heap path.

**A parallel broad-phase build.** `BroadPhase::build` takes an optional
`JobSystem*`. Above `kParallelBuildThreshold` (1024 bounds) the insert is chunked:
each chunk counts its entries, the counts become offsets, and each chunk writes
straight into its slice of the shared entry buffer. The sort, the pair emission,
and the dedup are unchanged and serial, so the pair array is identical to the
serial build.

## Consequences

Easier:

- A frame's 2D pass can be heap-free end to end, which is what the arena was
  built for. The win is tail latency (no `malloc` in the frame), not throughput.
- The broad phase scales with bodies without changing a single candidate pair,
  so a physics step stays deterministic and replayable.
- The instanced draw path is the one a render-graph seam would build on: a batch
  is already "one draw over N instances".

Harder, and the trade-offs accepted:

- The arena path requires the caller to size the destination, and a size that is
  too small silently truncates the frame rather than growing. That is why
  `sprite_instance_bound` exists and why the renderer sizes from it; a caller that
  passes a hand-picked smaller span gets a smaller draw, by design.
- Instancing assumes every 2D sprite is the same unit quad. A future sprite shape
  that is not a quad needs a second vertex stream or a vertex-shader variant.
- The broad-phase threshold is a tuned constant (1024, matching the extraction
  threshold) and must be re-measured if the insert cost changes.

## Alternatives considered

- **Per-chunk vectors joined in order for the arena extraction.** Rejected: each
  chunk list is a heap allocation, which defeats the point of the arena. The
  count-then-write shape needs no join and no per-chunk allocation.
- **A growing arena.** Rejected: growth is a heap allocation, which is the thing
  being removed. Errors are values (ADR-0003), so overflow is a fallback, not an
  exception.
- **Parallelising the broad phase's pair emission or its sort.** Rejected for
  now: the insert dominates and the sort is `std::sort` over an already-grouped
  array, so a parallel sort would add complexity for little gain and a second
  place for the output to diverge.
- **Batching by material without instancing.** Rejected as a substitute: it
  reduces draw calls but not the duplicated per-vertex upload, which is the cost
  instancing removes.
