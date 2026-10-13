# ADR-0028: Editor hint metadata — typed hints on a reflected property

- **Status:** Accepted
- **Date:** 2026-10
- **Related:** [ADR-0005](0005-reflection-and-serialization.md),
  [ADR-0006](0006-component-reflection.md),
  [ADR-0007](0007-editor-ui-toolkit.md),
  [ADR-0015](0015-editor-undo-redo.md)

## Context

The inspector draws one widget per reflected property (ADR-0006). A property
already carries enough to pick a widget for a plain scalar: its `ValueKind`
says Bool, Int, Float, String, Uuid, Group, or ResourceRef, and the inspector
maps each to a checkbox, a drag, a text box, a read-only label, a row of drags,
or a resource field.

That mapping is not enough. Several properties of the same `ValueKind` want
different widgets:

- `Camera2D::zoom` is a Float that should be a bounded slider, not an unbounded
  drag. `SpriteAnimator::fps` likewise.
- `Sprite2D::color` is a four-float Group that should be a colour picker;
  `Sprite2D::uv` is also a four-float Group but should be four plain drags.
- A resource reference should be an asset drop target, and the panel should
  know the expected type ("Texture2D") to label it and to check a drop.

So the property description needs to carry *editor intent* alongside its kind.
The question is what that metadata looks like.

The first cut, landed with M2, was an enum `Hint` (`Range`, `Color`, `Resource`,
`Multiline`, `FilePath`) plus a free-form `std::string hint_text` and a
`std::string resource_type`. A `Range` property encoded its bounds as the
string `"0,1"`, and the editor parsed it (`parse_range_hint`). It worked, but it
had the smell of a stringly-typed interface: the bounds are two numbers and the
resource type is a name, yet both were carried as text that every consumer had
to parse, and a typo was only detectable at the moment the inspector read it.
It also split the description of one property across three members
(`hint`, `hint_text`, `resource_type`) with no type-level guarantee that they
agreed.

## Decision

**Editor hints are typed data attached to a `Property`, not strings.** A
property gains three optional hint members, each present only when set:

```cpp
struct RangeHint { f64 minimum; f64 maximum; f64 step; };
struct ColorHint {};                       // channels come from the member count
struct ResourceHint { std::string type_name; };

struct Property {
    // ... kind, accessors ...
    std::optional<RangeHint> range;
    std::optional<ColorHint> color;
    std::optional<ResourceHint> resource;
    Hint hint = Hint::None;                 // Multiline, FilePath, None
};
```

`Hint` keeps only the kinds that are genuinely a single label with no data
("this string is multi-line", "this string is a path"). Everything with data
becomes a struct, so the compiler carries the shape and there is no parser.

Registration gains small builder helpers so the intent reads at the call site:

```cpp
desc.properties.push_back(
    with_range(make_scalar<Camera2D, f32>("zoom", &Camera2D::zoom, ValueKind::Float, &defaults),
               0.01, 16.0, 0.01));
desc.properties.push_back(
    with_color(make_vector_group<Sprite2D, Vec4>("color", &Sprite2D::color, "Vec4", {"r","g","b","a"})));
```

Specifics:

- **`hint_text` and `resource_type` are removed.** Their data moves into
  `RangeHint` and `ResourceHint`. Nothing else in the engine read them: the
  serializer keys off `ValueKind` and the `resource_ref`/`resolve_resource`
  callbacks, not off the hint, so the change is editor-local in effect.
- **`ColorHint` carries no data.** Whether a colour is RGB or RGBA is the
  group's member count, which the inspector already has. A tag plus a count is
  less to keep consistent than a tag plus a channel field.
- **The headless helpers become value transforms, not parsers.**
  `resolve_range_hint` validates an `optional<RangeHint>` into widget bounds
  (`valid` is false when the hint is absent or the interval is degenerate), and
  `clamp_to_range` bounds an edited value. Both live in `fork_editor_core`, so
  they are covered by the headless editor tests. `parse_range_hint` is deleted.
- **A degenerate range degrades to unbounded**, exactly as a malformed hint
  string did before: a mis-registration yields a usable drag, not a broken
  slider.

### Resource edits become undoable

Making the resource hint typed exposed a second gap. The M2 inspector edited a
resource reference by calling `resolve_resource` directly, which meant a
resource edit was **not** a `Command` and therefore not undoable - the one
editable property class outside ADR-0015's model. The reason was that resolving
a reference needs a `ResourceManager`, which `SetPropertyCommand` does not
carry.

This ADR closes that gap with a `SetResourceCommand` that carries a borrowed
`ResourceManager*` and resolves through the property's `resolve_resource`
callback. It captures the previous reference on first execute and restores it on
undo, and it coalesces on the same (node, component, property) key
`SetPropertyCommand` uses, so a drag from the asset browser is one undo step.
The inspector now edits a resource reference three ways - a text field for the
manager key, a drop target for an asset dragged out of the browser, and a clear
button - and all three go through the command. An empty reference clears the
slot, which `make_resource_ref`'s resolver now honours explicitly.

The borrowed `ResourceManager*` is safe by the editor's destruction order: the
document (and the command stack with it) is released before the embedded engine,
so the manager outlives every command. A command built with a null manager is a
no-op, which is the headless case where there is nothing to resolve against.

## Consequences

**Easier:**

- The inspector picks widgets from data the compiler checks: a bounded slider,
  a colour picker, an asset drop target. There is no string to parse and no
  format to document beyond the struct.
- A mis-registration is visible at the call site (`with_range(x, 1.0, 1.0)`)
  rather than discovered when the panel reads a string.
- Adding a hint means adding a struct and a builder, not extending a parser.
- Every editable property is now undoable: resource references are no longer
  the exception.

**Harder:**

- The `Property` struct grows three optionals. It is a description type built
  once per type at registration, not a per-frame hot path, so the cost is a few
  bytes of metadata and nothing in the frame loop.
- Two spellings of "resource": the `ValueKind::ResourceRef` (which says the
  value is a resource) and `ResourceHint` (which says the expected type). They
  always co-occur today; the kind is the structural fact and the hint is the
  editor's label, and keeping them separate is what lets a `ResourceRef` exist
  without a type hint.

## Alternatives considered

- **Keep the stringly-typed `hint_text` and add more parsers.** Rejected: it
  scales by adding a grammar per hint, and it loses the type information the
  registry could otherwise carry. A number that is two numbers should be two
  numbers.
- **One `variant<RangeHint, ColorHint, ResourceHint, ...>` on `Property`.**
  Closer to the "one hint at a time" mental model, but the hints are genuinely
  independent (a resource could later be bounded, a colour could later gain a
  space tag), and a `variant` would force a single-hint rule that has no reason
  to exist. Three optionals express the independence directly.
- **Put the widget choice in the editor, keyed off `ValueKind` and the group
  type name.** Rejected: it moves component knowledge into the editor (a
  `"Vec4"` group is a colour, a `"Vec2"` is not), which is exactly the
  per-type knowledge reflection exists to remove (ADR-0006). The type that
  knows a field is a colour is the type that registered it.
- **A generic key/value metadata map (`map<string, string>`).** Flexible and
  untyped, so it reintroduces the parsing problem under a different name and
  gives no help to a reader. Rejected for the same reason `hint_text` was.
- **Make resource edits a special case of `SetPropertyCommand` by threading a
  `ResourceManager` into `execute()`.** Would change the `Command` interface
  (and every existing command) to serve one command. A sibling command that
  carries the manager it needs is smaller and keeps the `Command` contract
  (`execute(Scene&)`) unchanged.
