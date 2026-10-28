# ADR-0100: A render-graph seam

- **Status:** Accepted
- **Date:** 2026-10
- **Related:** [ADR-0017](0017-performance-strategy.md),
  [ADR-0052](0052-render-extraction-seam.md),
  [ADR-0057](0057-environment-and-post-processing.md),
  [ADR-0022](0022-profiler-and-frame-timing.md),
  [ADR-0042](0042-null-rhi-device.md)

## Context

[ADR-0017](0017-performance-strategy.md) names performance as a product feature
and lists the work that follows the extraction seam. The extraction seam
([ADR-0052](0052-render-extraction-seam.md)) made the *set* of drawables data:
one walk emits a flat instance array, and culling, sorting, and batching became
operations on that array. The next item on the same list is a **render graph**:

> A **render graph** for passes and post-processing ... it is what lets passes be
> reordered, culled, and measured as a unit.

[ADR-0057](0057-environment-and-post-processing.md) deferred a graph as
speculative when only one post-processing pass existed, on the rule that a graph
should be built when a second and third pass justify it. That condition is now
met: a frame already runs a world pass and a UI pass in `Renderer2D`
([ADR-0084](0084-in-engine-text-rendering.md) and the in-game-UI work), and
`Renderer3D` runs a shadow pass, a main pass, and a post-processing chain
([ADR-0057](0057-environment-and-post-processing.md)). The order in every case is
correct but *implicit*: it lives in the sequence of calls in a `render()` body.

That shape blocks the work ADR-0017 lists as following the seam. A pass cannot be
reordered, dropped, or timed as a unit when its position is a line of code rather
than data. `Renderer2D::render_frame` calls the world pass and then the UI pass;
that the UI must come second is a fact about the frame, but it is written as an
ordering of statements, so nothing can express it, check it, or change it without
editing the renderer.

Constraints that shaped the decision:

- **The seam is a value, not a device.** The extraction seam is testable with no
  GPU because it is a pure function of the scene. A pass scheduler can be the same
  kind of thing -- names and dependencies in, an order out -- so it should be, and
  its correctness should be provable headlessly, as with
  [ADR-0042](0042-null-rhi-device.md)'s null device.
- **Do not over-reach.** A full render graph owns render targets, tracks resource
  lifetimes, aliases memory, and *culls* a pass whose output nobody reads. That
  needs a resource description the passes do not carry yet. Building it now would
  be the speculative abstraction ADR-0057 already refused once.
- **Stability matters more than cleverness.** Two frames of the same scene must
  order their passes identically, or a benchmark and a visual test become flaky.
  A declaration-order tie-break is enough to guarantee that.

## Decision

Add a **render-graph seam**: a `render::RenderGraph` that makes pass order data.

A caller declares passes by name and the dependencies between them. The graph
returns a run order that respects every dependency, breaking ties by declaration
order so the result is stable and an unconstrained graph comes out exactly as
declared. `run()` invokes each pass in that order and brackets each in a profiler
scope named `pass/<name>`, so a `FrameReport` ([ADR-0022](0022-profiler-and-frame-timing.md))
attributes time per pass with no per-call instrumentation.

Scope is deliberately the *scheduler*, not the optimizer:

- It orders passes and runs them. It does not own render targets, manage resource
  lifetimes, or drop passes.
- It is a pure value. It holds no GPU state and touches no device, so it is fully
  testable headlessly -- the same split the extraction seam uses, for the same
  reason.
- A dependency cycle is refused with the passes involved named, rather than
  guessed at or silently broken.

`Renderer2D` gains `render_frame`, which declares the world pass and the UI pass
as a two-node graph (`ui` after `world`) and runs it. The ordering that was
previously the sequence of two statements is now data the renderer hands to the
graph; the drawn output is identical.

## Consequences

**Easier:**

- Pass order is data. Reordering a frame, inserting a pass, or making one
  conditional is an edit to a declaration rather than to a renderer's control
  flow.
- Every pass is timed for free and shows up in the frame report as `pass/<name>`,
  so the cost of the world pass and the UI pass can be compared without adding
  instrumentation.
- The scheduler is testable with no window and no context: order, stability,
  cycle detection, and the profiler scope are all asserted headlessly.
- A dependency is checked, not assumed. A cycle fails loudly with the passes
  named, instead of producing an order that is subtly wrong.

**Harder / not yet done:**

- The graph does not yet cull passes whose output is unread, nor alias render
  targets. Those are the follow-on that turns the scheduler into an optimizer;
  they need passes to describe their resources, which is a larger change.
- `Renderer3D` is not yet driven by the graph. Its shadow/main/post sequence is
  still written as calls; converting it is a follow-on, and the graph is shaped so
  that conversion is a declaration change.

## Alternatives considered

- **Keep ordering as a sequence of calls.** The current shape. Rejected because
  it is what blocks reordering, culling, and per-pass timing -- the three things
  ADR-0017 names as the point of the seam.
- **A full render graph with automatic resource tracking and pass culling now.**
  Rejected as the same speculative step ADR-0057 already refused: it needs a
  resource model the passes do not carry, and one pass scheduler that is used is
  worth more than a resource system that is not.
- **A fixed enum of known passes instead of names.** Simpler, but it hardcodes the
  pass set into the engine and makes a caller's own pass (a debug overlay, a
  custom effect) a change to the engine. Names keep the graph open while still
  catching duplicates and cycles.
- **Sort by dependency depth.** Deterministic, but it moves an unconstrained pass
  to a position the caller did not choose. Declaration-order tie-breaking keeps
  the caller's intent visible in the order while still respecting every
  dependency.
