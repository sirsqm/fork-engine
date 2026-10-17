# ADR-0063: Reflection-backed script properties and script persistence

- **Status** — Accepted
- **Date** — 2026-10-17
- **Supersedes** — none
- **Superseded by** — none

## Context

M7 landed C++ gameplay modules and hot reload
([ADR-0061](0061-cpp-gameplay-modules-hot-reload.md)) on top of the scripting
seam ([ADR-0044](0044-scripting-seam.md)). A game can now ship behaviours as a
shared library, load them by name, and reload them without a restart. What a
script still cannot do is *carry data*.

A `Script` is deliberately not a component: components are engine data
([ADR-0002](0002-component-storage.md), [ADR-0005](0005-reflection.md)), scripts
are game code. That split is right, and it is why scripts were never
reflection-serialized. But the split left three gaps that all trace to the same
missing piece:

1. **A script's fields are invisible.** A designer sets a patrol speed by
   editing code and rebuilding. The reflection registry already makes a
   component's fields visible to the inspector and the serializer with one
   registration; a script has no equivalent, so its fields appear nowhere.

2. **A binding does not survive a save.** The scene serializer writes nodes and
   components. It does not write "this node runs `PatrolScript`", so a saved
   scene comes back unscripted. The authoring loop — place a node, attach a
   behaviour, tune it, save — cannot close.

3. **The editor cannot show or edit a script.** Without a description to read,
   the inspector has nothing to draw for a scripted node.

The scene serializer is in the `serialization` module and the script types are
in the `script` module, which sits above it. The serializer therefore cannot name
`ScriptHost` or `ScriptRegistry` without inverting the layering
(`core <- reflect <- scene, resources`; `core <- serialization <- ...`;
`core, scene, systems, platform <- script`). The gap is not "the serializer
should know about scripts"; it is "the serializer should expose a seam a script
binding can implement".

## Decision

Scripts gain reflected properties and the scene serializer gains a script
binding seam.

### Scripts are described, not just constructed

`ScriptRegistry` now stores a `ScriptDesc` per behaviour — a name, a factory, and
a vector of `reflect::Property` — instead of a bare name/factory pair. This
mirrors `reflect::TypeDesc` for gameplay code. A script author registers its
properties with the same `make_scalar` / `make_vector_group` helpers a component
uses, naming the concrete script type:

```cpp
ScriptDesc desc;
desc.name = "PatrolScript";
desc.factory = [] { return std::unique_ptr<Script>(new PatrolScript()); };
desc.properties.push_back(
    reflect::make_scalar<PatrolScript, f32>("speed", &PatrolScript::speed_,
                                            reflect::ValueKind::Float));
registry.register_desc(std::move(desc));
```

The property accessors take the owning object pointer, which here is the
`Script`. Nothing about `reflect::Property` changes: it already works on a
`void*` and a type-erased getter/setter, so a `Script*` and a `Component*` are
the same to it. A behaviour with no properties is still valid and still
round-trips; only its name is recorded.

### The serializer exposes a script seam

The `serialization` module declares an abstract `serialization::ScriptBinding`
with four calls:

```cpp
void write(const Node& node, json& node_json) const;  // per node, while writing
void reset();                                         // once, before reading
void read(const Node& node, const json& node_json);   // per node, in order
Error finish(Scene& scene);                           // once, after all nodes
```

`to_json`/`from_json`/`save`/`load` take an optional `ScriptBinding*`, defaulting
to null. With a null binding the document is byte-for-byte what it was before
this record: no node carries a `"scripts"` array. This is deliberate — the
feature must not rewrite every existing scene.

The `script` module implements the interface (`fe::script::ScriptBinding`),
wrapping the `ScriptHost` that owns the live instances and the `ScriptRegistry`
that describes them. The reader is three-phase because a binding cannot be made
until every node exists: `read` records `(node id, behaviour, properties)` per
node, and `finish` — after the scene's id index is rebuilt — constructs each
behaviour from the registry and attaches it. A binding names a node by its
stable id, not by index or name, so it lands on the right node and survives a
rename.

### The shared property<->JSON mapping moves to a module of its own

The serializer's property<->JSON mapping (Bool->boolean, Int/Float->number,
String->string, Uuid->string, ResourceRef->string, Group->array or object) is
extracted into `serialization::properties_to_json` / `apply_properties` in a new
`PropertyJson` unit. Both the scene serializer and the script binding use it, so
a script property and a component property have exactly one document shape.
Putting it in `serialization` (which already owns the file format) rather than in
`reflect` keeps the format out of the reflection layer.

### Errors match the component policy

An unknown behaviour, a binding that names a missing node, and a malformed
property value are all non-fatal: the reader logs a warning and skips, exactly as
it does for an unknown component type. A scene authored against a richer build is
not silently corrupted, and a hand-edited typo costs a default, not a failed
load.

## Consequences

- A script's fields persist and appear in the inspector with one registration
  edit, the same as a component's. The authoring loop closes: attach, tune, save,
  reload.
- A scene document now has a fourth format version. Version 4 adds the per-node
  `"scripts"` array; older documents load with every node unscripted, and a
  document written without a binding present is unchanged.
- `ScriptRegistry` grows `find`, `create`, `remove_script`, `contains`,
  `script_count`, and `names`. The old `register_script(name, factory)` remains
  as a shim over `register_desc`.
- `ScriptHost` gains `bindings()`, so a consumer can enumerate what is scripted
  without reaching into the host's internals.
- `serialization` gains a dependency on nothing new; `script` gains a dependency
  on `serialization`. The layering stays acyclic.
- The seam is language-agnostic in the same sense the scripting seam is: a second
  language would register `ScriptDesc`s and implement `ScriptBinding`, not change
  these interfaces.
- The editor gains a `ScriptCatalog` (`editor/ScriptCatalog.h`): a `ScriptHost`
  and a `ScriptRegistry` held as a pair, so the inspector can attach, replace,
  and detach a behaviour and the document can persist its bindings. It is
  headless and lives in `fork_editor_core`, so script editing is covered by the
  ordinary editor tests. `EditorContext` owns one, clears it when a different
  scene is loaded, and threads a `script::ScriptBinding` over it through save and
  load.

### What this does not do

Editing a script property from the inspector is not undoable yet. The panel now
lists a scripted node's behaviour and draws its reflected property values, and
the document persists and reloads the bindings through a `ScriptCatalog` that
pairs a `ScriptHost` with the `ScriptRegistry` — but a property row is shown
read-only, because routing an edit through the undo stack needs an operation that
names a *script* rather than a component id, and the operation model
([ADR-0047](0047-local-operation-model.md)) has no such field. Making a script
property an editable, undoable operation is the next editor cut; showing a row
that looks editable but is not would be worse than showing it plainly.

File-watching hot reload is likewise still editor work: `ScriptLibrary` reloads
on demand (ADR-0061), but nothing in the editor watches the module file yet.

## Alternatives considered

**Make a script a component.** Rejected: it collapses the engine-data /
game-code split that ADR-0044 drew, and it would put game behaviour into the
engine's component registry, which a hot-reloaded module owns.

**Let the serializer depend on the script module.** Rejected: it inverts the
layering (`script` sits above `serialization`) and would drag `ScriptHost` and
the module loader into every consumer of the serializer, including headless
tools.

**Serialize scripts by node index.** Rejected: index is not identity. A binding
names a node by the stable id the document already writes, for the same reason a
component is written by id.

**Give each script its own JSON writer.** Rejected: it would fork the document
shape, so a script property and a component property could drift. The shared
`PropertyJson` mapping keeps one shape.
