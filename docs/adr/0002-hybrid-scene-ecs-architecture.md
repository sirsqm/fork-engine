# ADR-0002: Hybrid scene-graph / entity-component architecture

- **Status:** Accepted
- **Date:** 2026-01
- **Related:** [ADR-0001](0001-engine-architecture-overview.md)

## Context

The engine needs an object model that satisfies two audiences at once:

1. **The editor and the user**, who think in terms of a hierarchy they can see
   and manipulate: a parent transform, children that inherit it, components
   attached to a thing.
2. **Performance-critical systems**, which want data laid out for the way they
   read it — contiguous transforms, colliders, animation channels — rather than
   scattered across a pointer graph.

Two textbook answers exist. A **scene graph** gives an intuitive hierarchy but
encourages pointer-chasing traversal and object-per-node design. A pure
**entity-component-system (ECS)** gives excellent data locality and system
composition but is a poor fit for direct visual authoring and makes hierarchical
transforms a special case that has to be bolted on.

Forcing one model on every subsystem would either cripple authoring (pure ECS) or
cap performance (pure scene graph).

## Decision

Use a **hybrid** model, and choose the model per subsystem rather than globally.

- A `Node` provides identity (a stable `Uuid`), a name, one transform, and an
  ordered list of children. It is the unit the editor manipulates and the
  serializer persists.
- A `Component` provides a capability (sprite, camera, collider) as data plus,
  where appropriate, logic. It does not own hierarchy. Components are composed
  onto nodes and are move-only.
- The hierarchy organises **data**; it does not dictate **execution**. A system
  that needs a flat, contiguous layout reads the component data it cares about
  into its own structure each frame. Rendering already does this: it walks the
  graph once to produce a flat, sorted batch list.

There is exactly one transform per node, so "where does the transform live?" has
a single answer, and hierarchical transforms compose naturally.

## Consequences

**Easier:**

- The editor model maps directly onto the runtime model; there is no translation
  layer between what the user sees and what the engine stores.
- Hierarchical transforms are trivial (`world = parent_world * local`).
- Components can be added and removed without disturbing identity or hierarchy.
- A future system can adopt a data-oriented layout internally without changing
  the authoring model or the serialized format.

**Harder:**

- The scene graph is a pointer graph, so a system that naively walks it every
  frame pays for cache misses. Mitigated by extracting flat arrays per system
  when the cost is measured, not assumed.
- There is a conceptual split to teach: "the graph is for authoring and
  organisation; systems choose their own layout." Documented here and in the
  architecture guide.

## Alternatives considered

- **Pure scene graph with everything as objects and virtual update methods.**
  Intuitive but caps performance and spreads behaviour across many virtual
  calls. Rejected.
- **Pure ECS as the primary model.** Excellent for systems, poor for direct
  authoring, and awkward for hierarchical transforms and the inspector.
  Rejected as the primary model; its good ideas are available to individual
  systems.
- **No hierarchy (flat entity list).** Loses parent/child composition, which 2D
  and 3D authoring rely on heavily. Rejected.
