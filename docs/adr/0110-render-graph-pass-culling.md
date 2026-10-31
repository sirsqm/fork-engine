# ADR-0110: Render-graph pass culling

- **Status:** Accepted
- **Date:** 2026-10
- **Related:** [ADR-0100](0100-render-graph-seam.md),
  [ADR-0017](0017-performance-strategy.md),
  [ADR-0052](0052-render-extraction-seam.md),
  [ADR-0057](0057-environment-and-post-processing.md),
  [ADR-0042](0042-null-rhi-device.md)

## Context

[ADR-0100](0100-render-graph-seam.md) added the render-graph seam and named, as
the explicit follow-on, the two things it deliberately did not do:

> The graph does not yet cull passes whose output is unread, nor alias render
> targets. Those are the follow-on that turns the scheduler into an optimizer;
> they need passes to describe their resources, which is a larger change.

The first half of that follow-on is culling. It is the cheaper half: it needs a
resource *name* on a pass, not a resource *object* the graph allocates and
aliases. A pass that writes a target nobody reads is wasted work, and the
renderers are about to grow chains of intermediate targets (the post-processing
chain already runs over an off-screen colour target,
[ADR-0057](0057-environment-and-post-processing.md)), so a way to drop a dead
pass is worth having before the chains get longer.

Constraints that shaped the decision:

- **Do not break what exists.** `Renderer2D::render_frame` builds a two-node
  graph (`ui` after `world`) with no resource information. Whatever rule is
  chosen must leave that frame byte for byte as it was.
- **A name, not an allocator.** The graph must stay a pure value
  ([ADR-0042](0042-null-rhi-device.md)): it reasons about resources, it does not
  own them. That keeps it testable headlessly and keeps the resource-owning
  change separate.
- **A pass that presents has no declared output.** The final pass of a frame
  writes the swapchain, but many passes simply present without naming a target.
  Culling must not drop those.

## Decision

A pass may declare the resources it **reads** and **writes**, by name
(`add_read`, `add_write`), and a resource may be marked as an **output**
(`mark_output`) when something outside the graph observes it -- the swapchain, a
readback. Culling is a liveness rule over that data:

- A pass that declares **no writes** is never culled. It is assumed to have an
  external effect (it presents), so it is always live. This is what leaves the
  existing resource-free graphs unchanged.
- A pass that declares writes is **live** when at least one of them is read by
  another live pass, or is marked as an output. Otherwise it is culled.
- Culling is **iterative**: dropping a consumer makes its producer's write dead,
  so the rule is applied to a fixed point.
- A dependency on a culled pass is **dropped, not an error**: the dependent was
  written against a producer that turned out to have no live output.

`order()` and `run()` operate on the live passes only; `culled()` exposes the
dropped names so a test can assert on the decision directly. `Renderer2D`
declares its flow (`world` writes `backbuffer`, `ui` reads it and writes the
marked `swapchain`), which keeps both passes live and produces the same order the
dependency already expressed -- the declarations cost nothing at run time and
make the seam able to drop a pass as the renderer grows intermediate targets.

## Consequences

**Easier:**

- A pass whose output nobody reads is dropped, so a dead branch of the frame
  costs nothing. As the renderers grow target chains, a chain that feeds nothing
  visible is culled end to end.
- The rule is provable headlessly, like the scheduler: liveness, iteration,
  output marks, dangling dependencies, and the no-declaration regression are all
  asserted with no window and no context.
- A pass with no resource declarations is unchanged, so every existing graph --
  `Renderer2D`'s pair, the profiler-scope test -- runs exactly as before.

**Harder / not yet done:**

- The graph still does not own, allocate, or alias render targets. Culling is
  the liveness half of the optimizer; memory aliasing is the other half and needs
  a resource *object*, not a name.
- `Renderer3D`'s shadow/main/post sequence is still written as calls, so its
  passes are not yet declared to the graph and cannot yet be culled. Converting
  it is the follow-on that makes the culling pay off on the 3D path.

## Alternatives considered

- **Track resource objects and lifetimes now.** The full optimizer. Rejected for
  the same reason [ADR-0100](0100-render-graph-seam.md) and
  [ADR-0057](0057-environment-and-post-processing.md) refused the full graph: it
  needs an allocation and aliasing model the renderers do not have, and a liveness
  rule that is used is worth more than a resource system that is not.
- **Cull a pass that writes nothing.** Rejected: the presenting pass writes
  nothing the graph knows about, so this would drop the final pass of every
  frame. Treating "no declared writes" as "has an external effect" is what makes
  the rule safe by default.
- **Treat a dependency on a culled pass as an error.** Rejected: it would make
  culling unusable, because the dependent is written against a producer whose
  output turned out to be dead, and that is a normal outcome of the rule, not a
  caller mistake.
- **Make output marks a per-pass flag instead of a per-resource mark.** Simpler,
  but it conflates "this pass writes the swapchain" with "this resource is the
  swapchain"; a resource read by several passes and written once is marked once,
  which is the property the graph reasons about.
