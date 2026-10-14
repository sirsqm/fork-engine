# ADR-0043: The scripting seam, and C++ as the first language behind it

- **Status:** Accepted
- **Date:** 2026-10
- **Related:** [ADR-0001](0001-engine-architecture-overview.md),
  [ADR-0002](0002-hybrid-scene-ecs-architecture.md),
  [ADR-0003](0003-error-handling.md),
  [ADR-0005](0005-reflection-and-serialization.md),
  [ADR-0006](0006-component-reflection.md),
  [ADR-0008](0008-systems-and-frame-schedule.md),
  [ADR-0017](0017-performance-strategy.md),
  [ADR-0018](0018-multi-platform-shipping.md),
  [ADR-0019](0019-collaborative-editing.md)

## Context

The engine has no scripting story. Gameplay today is written as a `System`
([ADR-0008](0008-systems-and-frame-schedule.md)): a unit of per-frame behaviour
that owns its own traversal of the scene and runs in a `SceneSchedule`. That
model is correct for engine-shipped behaviour — animation, physics, audio — and
it is deliberately *not* a per-node callback. But it is a poor fit for
game-specific logic. A level designer cannot add "this door opens when the
player has the key" without a programmer writing a system and recompiling the
engine.

Three audiences want to attach behaviour to a node, and they want different
things from it:

1. **The engine and its own systems** want data-oriented execution and no
   per-object virtual dispatch.
2. **A gameplay programmer** wants to write a small piece of behaviour, attach
   it to a node, and have it run once per frame with the frame's context.
3. **A designer, later** wants to set that behaviour up in the editor and tune
   its properties without a rebuild.

[`STRATEGY.md`](../STRATEGY.md) orders the work and names scripting as **M7**,
after the vertical slice (M2.5): "nothing makes the engine usable to anyone but
its author until scripting exists." It also records the direction already
chosen, and this ADR is where that direction is decided properly:

> C++ is the primary language for now, via native gameplay modules that build
> against the engine and hot-reload, and the scripting layer is built as a
> language-agnostic seam so another language can be added later without the
> engine core naming it.

The expensive decision is the *language*. The cheap, reversible one is the
*seam*. This ADR commits to the seam and to C++ behind it, and states the
criteria under which the language choice would be revisited.

## Decision

**Attach gameplay behaviour to nodes as `Script` objects owned by a `ScriptHost`,
behind a seam that never names a concrete language; C++ is the first language,
built directly against the engine.**

The module is `script`, header-only for now
(`engine/include/fe/script/`), and depends only on `core`, `scene`, and
`systems` — the same downward edges `systems` already has, plus nothing.

### The seam

- **`Script`** is a per-node unit of behaviour: `attach(Node&)`,
  `detach(Node&)`, `update(const FrameContext&)`, a `name()`, and an
  `enabled` flag. It reads the frame's context (delta time, resources, input)
  the way a `System` does, so it is testable headlessly.
- **`ScriptHost`** owns the live `Script` instances, keyed by the node's stable
  `Uuid` — the same identity the serializer persists and the collaboration work
  ([ADR-0019](0019-collaborative-editing.md), M10) is built on. One script per
  node, one node per script. The host is the only thing that calls a script's
  lifecycle and update methods, and it is the boundary where a failing script is
  contained.
- **`ScriptRegistry`** maps a behaviour's name to a factory that constructs it,
  so a script can be created by name — which is what the editor and a future
  loader need. It mirrors the shape of `reflect::Registry`
  ([ADR-0006](0006-component-reflection.md)) deliberately: register a
  descriptor once, look it up by name everywhere after.
- **`ScriptSystem`** is an ordinary `System` that drives a `ScriptHost` from the
  existing `SceneSchedule`. It is the bridge that makes scripts run in the
  frame loop the engine already has, without a new execution path.

### Why behaviour lives in a `Script` and not a `Component`

A component is data plus, "where appropriate, logic"
([ADR-0002](0002-hybrid-scene-ecs-architecture.md)) — but component logic is
engine-owned, reflection-driven, and serialized as *data*
([ADR-0005](0005-reflection-and-serialization.md)). Gameplay behaviour is a
different kind of thing: it is code, it is game-specific, and it may come from a
module the engine did not compile. Putting it in a component would force every
gameplay script to be a reflected, serializable engine type, and would drag the
per-node-`update()` model in through the back door that ADR-0002 closed.

`Script` is therefore a **sibling of `System`, not a component**. It is per-node
(so a designer can attach it in the editor), but it is driven by a system (so
execution stays owned by the schedule), and it is not reflection-serialized
*code*. The two concepts stay separate: components are engine data, scripts are
gameplay behaviour, and the host is the only thing that binds a script to a
node.

### The failures are contained and reported

Following [ADR-0003](0003-error-handling.md), a `Script` reports failure as a
value: `update()` returns an `Error`. A script that returns an error, or (in a
build with exceptions, as the C++ path has) throws, must not take the frame loop
down with it. `ScriptHost` catches both at the boundary, reports through the
logger with the node's id and the script's name, disables that one script, and
keeps running the others. One bad script is one bad script.

## Consequences

**Easier:**

- Gameplay code has a place to live that is not "edit the engine." A programmer
  writes a `Script`, registers it, attaches it to a node by id, and it runs in
  the existing frame loop through `ScriptSystem`.
- The seam is language-agnostic by construction: `Script` is an interface, and
  nothing in `core`, `scene`, `systems`, or the runtime names C++, Lua, or
  anything else. A second language is a second `ScriptRegistry` population and a
  second way to make `Script` instances — not a change to the engine.
- Scripts key on the stable node `Uuid`, so the identity the serializer and M10
  already need is the same identity the host uses. There is no parallel id
  scheme.
- Because `ScriptSystem` is a plain `System`, scripts run headlessly (the null
  backend, `fork_headless`) and in play-in-editor through the identical path —
  no window, no RHI, no editor dependency.
- Failures are observable (logged, and reported by `update`), so a broken script
  is a bug you can see rather than a crash you have to debug.

**Harder:**

- Two ways to add behaviour now exist — a `System` and a `Script` — and the
  boundary must be explained or it will drift. The rule is stated above and in
  `ARCHITECTURE.md`: engine-shipped, data-oriented, reflection-serialized
  behaviour is a `System`; game-specific, per-node, editor-attached behaviour is
  a `Script`. A `Script` that wants to be a hot loop should become a `System`.
- A per-node virtual call per frame is exactly the shape ADR-0002 rejected for
  the engine's *own* hot paths. It is accepted *here*, for gameplay, because
  gameplay scripts are few and their cost is the game author's to manage; the
  engine's own systems keep the data-oriented path. This is a deliberate,
  bounded exception, not a reversal.
- `ScriptHost` owns live instances and holds a `Node*`; a script must not
  outlive its node. Detaching is explicit and the host clears a node's entry on
  detach, but a caller that destroys a node without detaching its script is a
  use-after-free the host cannot prevent — it can only make the correct path
  (detach before destroy) the easy one.

**Deliberately not in this cut (named so they are planned, not forgotten):**

- **Hot reload.** The roadmap row is *not* ticked by this ADR. Loading a C++
  gameplay module as a dynamic library, reloading it while the editor runs, and
  rebinding live instances is the next cut; it needs the platform module's
  dynamic-library part (deferred in
  [ADR-0041](0041-platform-contract-and-null-backend.md)) and is its own ADR
  when it lands.
- **Reflection-backed bindings.** A script exposing properties to the inspector
  and the serializer is the cut after hot reload; it reuses
  [ADR-0006](0006-component-reflection.md) and does not change this seam.
- **Editor integration and debugging.** Attach/inspect UI, and a debugger, come
  with the editor and hot-reload cuts.

## When a second language would be added

The seam exists so this is a cheap decision later. It should be revisited when
**all** of the following hold:

1. **A concrete audience needs it.** Modders or designers who cannot build C++,
   or a platform where shipping a compiler toolchain is not viable.
2. **The C++ path is proven.** Hot reload and reflection-backed bindings work,
   so the cost of a second language is measured against a working baseline, not
   a hoped-for one.
3. **The seam is unchanged by it.** Adding the language touches only the
   registry population and the way `Script` instances are made — if it needs a
   change to `Script`, the seam was wrong and that is the real work.

If those hold, the next ADR chooses between embedding a proven language (Lua,
Wren) and a purpose-built one. A custom engine language (a GDScript-style
option) remains a *possible* future and is **not started**: it is a multi-year
commitment and only worth it if it is dramatically better for the engine's use
case than embedding a proven language — not merely different.

## Alternatives considered

- **An embedded VM now (Lua or Wren).** Rejected for this cut. It is a real,
  irreversible dependency (a third-party runtime, its build, its licenses, its
  sandbox and performance profile) chosen before a single gameplay script exists
  to justify it, and before the seam it would sit behind is proven. The seam is
  cheap and reversible; the language is expensive. Do the cheap one first, and
  decide the expensive one with evidence.
- **A data-driven behaviour graph now** (nodes and wires describing behaviour).
  Rejected as the first cut. A graph is a good *authoring* surface for
  non-programmers, but building one before there is a runtime behaviour to
  author is a UI ahead of its model. A `Script` is the model; a graph can be a
  front end for it later without changing the seam.
- **Per-node `update()` on `Node`/`Component`** (the Godot/Unity-instance
  model). Rejected. It is precisely what ADR-0002 and ADR-0008 rejected for the
  engine's execution model, and adopting it would undo both. The bounded,
  opt-in `Script` path keeps the fast path intact.
- **Make scripts a reflected `Component` type.** Rejected. It conflates engine
  data with game code, forces every script to be a serializable engine type, and
  pulls the per-node callback model back in. Components stay data; scripts stay
  behaviour; the host binds them.
- **A full scripting abstraction now** (virtual machines, handles, bytecode,
  language negotiation). Rejected as speculative. The interface is `Script` plus
  a name-to-factory registry; anything larger is cost paid before a second
  implementation exists.
