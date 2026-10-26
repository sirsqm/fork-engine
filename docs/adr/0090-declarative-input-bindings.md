# 0090 — Declarative input bindings

Date: 2026-10-25

Status: Accepted

Related: [ADR-0075](0075-gameplay-actions-input-seam.md),
[ADR-0077](0077-touch-region-layout.md),
[ADR-0025](0025-project-file.md), [ADR-0064](0064-export-runtime-and-player.md),
[ADR-0076](0076-3d-in-the-browser.md)

## Context

[ADR-0075](0075-gameplay-actions-input-seam.md) made input abstract: gameplay
reads named actions, and `ActionMap` resolves an action from whatever produced
it — a key, a mouse button, or a touch in a screen region. That is the seam a
phone and a keyboard share, and the `touch-input-actions` queue item shipped it.

It left one thing open. Regions were bindable, but only from C++:
`ActionMap::bind_touch_region` is a function call, so a game that wants "the
lower-left eighth steers" says so in a source file. The
`touch-gesture-regions` queue item is exactly that gap — "a game declares the
left third steers once instead of reading raw touch points."

The shape of the problem is the one every shipped game hits. A layout is not a
property of the engine, it is a property of the *game*: which region jumps,
which steers, how wide the thumb pads are. Written in C++ it cannot change
without a rebuild, cannot be authored in an editor, and cannot be overridden per
device. The engine already made the same move for its own content — a scene is
a document, a project is a document — and a game's input layout belongs with
them.

There is also a measured hazard. The engine-shipped behaviours
([ADR-0078](0078-engine-shipped-gameplay-behaviours.md)) declare a conventional
touch layout in `declare_builtin_actions`. A game that wants a *different* layout
must be able to replace it. If a document could only add regions, a game's
"steer" region would sit on top of the built-in one and a single touch would
fire two actions — a bug that is invisible in code review and obvious to a
player.

## Decision

Add a declarative binding document, `fe::player::ActionBindings`, and the seam
that carries it to a shipped build.

**The document** is `fork_input` version 1: an array of actions, each naming
itself and optionally its keys (by UI Events `code`), its mouse buttons, and its
normalised touch regions. `ActionBindings::from_json` parses it into plain data;
`apply` declares its actions on an `ActionMap` and installs the bindings.

Four decisions in the format are load-bearing:

1. **Keys are UI Events codes** (`"KeyA"`, `"ArrowLeft"`, `"Space"`), the
   physical naming `key_from_code` already uses. It is what a browser reports
   and what a rebinding screen would write back, so one document is portable
   across the web and desktop backends. An unknown code is an error, not a
   dropped binding: a typo must fail the load rather than produce a control that
   silently does nothing.

2. **Touch regions are normalised** (0..1, origin top-left). This is what makes
   one declaration serve portrait and landscape and survive a resize, the same
   reason the built-ins are normalised
   ([ADR-0077](0077-touch-region-layout.md)). A region written in pixels is
   rejected, not clamped, because the mistake is a unit error and the failure
   mode of guessing is a rectangle that never matches.

3. **`"replace": true`** on an action clears that action's bindings *of the
   kinds the document then binds*, before installing its own. It clears per
   kind, not wholesale, so a game that redefines its touch layout does not also
   lose the action's keys. Without `replace` a document can only add to the
   built-in layout, which is the two-actions-on-one-touch bug above.

4. **A document is self-contained.** `apply` declares every action it names
   (reusing an existing handle of the same name), so it does not have to assume
   the game declared the names first, and a name bound to nothing is a reserved
   action a later document fills in.

**The seam.** `fork.project.json` gains a `bindings` path. `fork_export` packs
the document and records its pack key in `fork.export.json`; the player reads it
from the pack after `declare_builtin_actions` and applies it. Two details are
not incidental: the document is added to a pruned pack's include list
explicitly, because nothing in the scene references it and pruning would drop
it; and its key is a *pack* key, not a path beside the manifest, so the player
takes it verbatim rather than resolving it against the export directory.

**The proof** is a browser run. The 3D game declares its regions in
`input/bindings.json`, and the web check dispatches a synthetic touch at
(0.5, 0.75) — a point no built-in region covers but the document's MoveForward
region does. The player's touch probe presses *no* keys, so the watched node
moving at all means the document was packed, fetched, parsed and applied: the
whole path, in a browser, with no way to pass by accident.

## Consequences

Easier:

- A game's input layout is data. It can change without a rebuild, be authored by
  a tool, and be overridden per device — the three things a shipped game needs
  and C++ cannot give.
- The engine's own layout is now just a default a game can replace, which is the
  right relationship: the built-ins are a convention, not a policy.
- A malformed document is a load failure with a sentence, not a game whose
  controls half-work.

Harder:

- A binding is now potentially in two places — the C++ built-ins and a document
  — so "what fires this action" is answered by `replace` semantics rather than
  by reading one function. The `replace` flag exists to keep that answer
  comprehensible.
- The document is a second input format to keep stable. It is versioned and
  rejects a version it does not know, so an old build refuses a new document
  rather than misreading it.

## Alternatives considered

**A `.fescene` field.** Rejected: bindings are per game, not per scene, and a
game with several scenes would have to repeat them. The project file already
owns per-game settings.

**Bind regions in C++ with a builder.** Rejected: it is the status quo with nicer
syntax. It still cannot be authored, overridden, or changed without a rebuild.

**Let a document only add, never replace.** Rejected: it makes the built-in
layout impossible to override, and a touch in an overlapping region fires two
actions. `replace` is the minimum that makes the built-ins a default.

**Pixel coordinates, converted at load.** Rejected: the conversion needs a
viewport the loader does not have, and a document that names pixels cannot
follow a rotation. Normalised is the only form that survives both.
