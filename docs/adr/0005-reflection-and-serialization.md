# ADR-0005: Reflection and serialization strategy

- **Status:** Accepted (implementation in [ADR-0006](0006-component-reflection.md))
- **Date:** 2026-01
- **Related:** [ADR-0002](0002-hybrid-scene-ecs-architecture.md)

## Context

The editor's property inspector, the scene serializer, and any future scripting
bindings all need the same thing: a machine-readable description of a type's
properties — their names, types, and editor hints. If each of those three
consumers hard-codes its own knowledge of every component, adding a property
means editing three places, and they will drift.

At the same time, building a full reflection system before there is an editor to
consume it is speculative work. The current component set is small (a sprite and
a camera).

## Decision

Serialization is driven by **type information**, and the serializer is written so
that this information can come from reflection later without changing the file
format.

- Today, `serialization::SceneSerializer` uses a small per-component switch: one
  pair of `to_json`/`from_json` functions per component type. This is explicit,
  fast, and easy to audit. It is deliberately a temporary shape, and it is
  documented as such at the call site.
- The **file format** is defined independently of that switch: a stable
  `format`/`format_version` header, components keyed by `type_name()`, and node
  identity persisted as a `Uuid`. The format does not depend on how properties
  are discovered.
- When the component set grows, a **registry** mapping `type_name()` to
  read/write callbacks replaces the switch. The registry is the first step
  toward reflection: property name, type, default, and editor hints.
- The editor's inspector will be driven by that same registry, so a property is
  described in exactly one place.

## Consequences

**Easier:**

- The file format is stable while the discovery mechanism changes underneath it.
- Adding a component today is a local, obvious change.
- The path to reflection, scripting, and an auto-generated inspector is clear
  and incremental.

**Harder:**

- Until the registry exists, each component needs its own serialization code, and
  an unknown component type is only recorded by name (the reader warns rather
  than silently dropping it).
- A full reflection system (arbitrary types, metadata, editor hints) is
  substantial; deferring it means the inspector will be built against the
  registry, not against hand-written per-component UI.

## Alternatives considered

- **Build full reflection now.** Rejected as premature: no consumer exists yet,
  and the shape of the metadata is easier to get right once the inspector's needs
  are concrete.
- **A schema/IDL compiler that generates serialization code.** Powerful, but a
  large piece of build tooling to maintain before the payoff is clear. Deferred.
- **Serializing with a third-party reflection library.** Would add a dependency
  and constrain the metadata model; not justified at this size. Revisit if the
  registry proves too limited.
