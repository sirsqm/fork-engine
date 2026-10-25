# 0078 — Engine-shipped gameplay behaviours

Date: 2026-10-25

Status: Accepted

## Context

Gameplay logic has lived in C++ gameplay modules: shared libraries the engine
loads at run time (ADR-0044, ADR-0061). That seam is right for a desktop game —
it is how a game ships its own logic without a rebuild of the engine — but a
browser has no dynamic loader. `fork_export --target web` therefore refuses to
record a module at all, and a web build of a scene could render but had no logic
to consume input. The renderer, the input backend and the action seam (ADR-0075)
all worked in a browser; the thing that *reads* the actions did not exist there.

This surfaced when a 3D scene was exported for the web: the geometry drew, the
frame loop ran, and pressing a key did nothing, because there was no behaviour
compiled into the wasm build to respond to it.

The fix cannot be "make `dlopen` work in a browser" — it cannot. The question is
where a small, useful set of behaviours lives so that *every* target, including
wasm, has gameplay to run.

## Decision

Add an engine module, `fe/player/` (`engine/include/fe/player/BuiltinBehaviours.h`,
`engine/src/player/BuiltinBehaviours.cpp`), that ships a small set of behaviours
**compiled into the engine** and registered in-process:

* `FlyCamera3D` — a first-person camera: move with the movement actions, look
  with the look actions or a mouse drag.
* `MoveNode3D` — translates its node with the movement actions, in the node's own
  frame, with an optional vertical lock.
* `SpinNode3D` — rotates its node about an axis at a constant rate; no input, just
  time, which is the simplest possible proof that a behaviour runs on a target.

They are ordinary `Script`s: they register into a `ScriptRegistry` like any
module's would, a scene binds them by name like any other behaviour, and a host
drives them through the ordinary `ScriptSystem`. No new execution model is
introduced, and nothing in the module is web-specific.

The module also declares the **standard action set** (`declare_builtin_actions`),
binding each action to a keyboard and to a touch region in one place, so the
built-in behaviours are playable with a keyboard and on a phone without the game
author writing any input code. The behaviours read actions through `FrameContext`
(the action map and the input snapshot), never a platform API, so the identical
code drives a keyboard on desktop and a touchscreen in a browser (ADR-0075).

The player calls `register_builtin_behaviours` and `declare_builtin_actions` for
every target; the editor calls the former so a designer can attach these in the
inspector. A game may still add its own gameplay module on the platforms that
support one, and a module may override a built-in by registering the same name.

## Consequences

* A web build has gameplay to run, so a browser game responds to input. This is
  what makes an exported game *playable* on the web rather than merely visible.
* One place writes down the standard bindings, so a keyboard and a touchscreen
  cannot drift apart, and a game gets both for free.
* A game that needs logic beyond these still ships a module on desktop; on the
  web it is limited to the built-ins (or to logic compiled into a custom build).
  That is a real limitation of the target, stated rather than hidden.
* The behaviours are deliberately a separate module from `script`: `script` is
  the language-agnostic seam (what a `Script` *is*), and `player` is a library of
  behaviours built on it (what a `Script` can *do*). A game that wants none of
  these pays nothing for them.

## Alternatives considered

* **A scripting language (Lua, JS, Wasm) for gameplay.** The most flexible answer,
  and the one a mature engine would eventually take, but it is a much larger
  change than "a browser game needs to respond to a key", and it would need its
  own ADR. The built-ins unblock the web target now without foreclosing it.
* **Keep gameplay in modules only.** Rejected: it leaves the web target with no
  gameplay at all, which is the defect this records.
* **Put the behaviours in the `script` module.** Rejected: it would make the
  language-agnostic seam carry a specific behaviour library, and a game that
  wants a different set would still compile these in.
