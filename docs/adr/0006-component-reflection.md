# ADR-0006: Component reflection for serialization and the editor

- **Status:** Accepted
- **Date:** 2026-10
- **Related:** [ADR-0005](0005-reflection-and-serialization.md), [ADR-0002](0002-hybrid-scene-ecs-architecture.md)

## Context

ADR-0005 deferred reflection because no consumer existed yet, and predicted the
shape of the first step: "when the component set grows, a registry mapping
`type_name()` to read/write callbacks replaces the switch." The editor's property
inspector is that consumer, and it is the next milestone. Building the inspector
against hand-written per-component UI would mean describing every property twice
(once to serialize it, once to show it), and the two descriptions would drift.

The serializer also carried a real cost by this point: adding a single property
meant editing the component header *and* a branch of a `if (type == ...)` chain in
`SceneSerializer.cpp`, with the JSON keys duplicated between read and write.

## Decision

Introduce a small reflection registry that describes a component's properties
once, and drive serialization from it. The scope is deliberately narrow:

- **Properties are described as data.** A `Property` is either a scalar leaf
  (`Bool`, `Int`, `Float`, `String`, `Uuid`, `ResourceRef`) or a `Group` (a
  struct such as `Vec2`, `Vec4`, a colour) whose members are nested properties.
  Accessors are `std::function` getters/setters over the owning component.
- **Editor hints are part of the description** (`Hint::Range`, `Resource`,
  `Color`, ...). They do not affect the file format; they are what the inspector
  will read to choose a widget.
- **A `Registry` maps a type name to a `TypeDesc`** (a factory plus its
  properties). The serializer looks up a type by the name recorded in the
  document, constructs it through the factory, and applies each property.
- **The registry lives in a new `reflect` module** that depends on `core`,
  `scene`, and `resources`. Factories are typed `std::unique_ptr<Component>()`,
  so the serializer can attach a runtime-known component without naming any
  component type. This is what removes the per-type switch.

Registrations are grouped in `ComponentTypes.cpp`, one block per component. Each
block is the single place a property is described.

**The on-disk format is unchanged.** `format`/`format_version`, components keyed
by `type_name()`, node identity as a `Uuid`, and the existing JSON shape for each
value are all the same as before. This is a change to how properties are
*discovered*, not to what is written, so existing scene files keep loading.

## Consequences

**Easier:**

- Adding a property is one edit in `ComponentTypes.cpp`; it then persists and,
  next milestone, appears in the inspector with no further work.
- The serializer no longer names `Sprite2D` or `Camera2D`; unknown component
  types are a clean lookup miss that warns, which is the behaviour ADR-0005
  wanted.
- The inspector can be built against a real description instead of invented
  metadata.

**Harder:**

- The property set is a closed enum, not arbitrary types. A property whose type
  is not one of the supported kinds needs a new kind; that is intentional and
  keeps both consumers a single obvious switch.
- `reflect` depends on `scene` (for the `Component` base), so it is not a
  general-purpose reflection library. Widening it later is a new decision.
- Struct-valued properties are modelled as groups rather than reflected
  recursively. Nested structs beyond one level are not supported yet; no current
  component needs them.

## Alternatives considered

- **Keep the per-component switch.** Fine for two components, but it forces the
  inspector to duplicate every property and guarantees drift. Rejected as the
  component set is about to grow.
- **A full general reflection system now** (arbitrary types, recursive metadata,
  attributes). More power than the inspector needs and a large surface to get
  wrong before the inspector's needs are concrete. Deferred; this ADR is the
  incremental step ADR-0005 anticipated.
- **A schema/IDL compiler generating serialization code.** Powerful, but new
  build tooling to maintain before the payoff is clear. Deferred.
- **Reflection as a `core` module.** It would need the `Component` base and the
  resource manager, which would pull `scene`/`resources` into `core` and break
  the "core depends on nothing" rule. Rejected.
