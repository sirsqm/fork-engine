# ADR-0059: Animation state machines

- Status: Accepted
- Date: 2026-10-15
- Deciders: sqm

## Context

M6's sprite-sheet animation landed early, as the first concrete system
(`SpriteAnimator` + `SpriteAnimationSystem`, ADR-0008). It plays one loop: a
grid of cells advanced at an `fps`. That is the right primitive, and it is
deliberately the only one — it holds *state* (which cell is showing, whether
time is running) and a system holds the *behaviour* that steps it.

What it cannot express is the next thing every game needs: "idle plays until the
player moves, then run plays; attack interrupts either and returns to idle when
it finishes." That is a state machine, and M6 lists it as an open row. The
question this ADR answers is where the machine lives and what shape it has, so
that it is authored, serialized, inspected, and stepped like the rest of the
engine rather than bolted on as ad-hoc gameplay code.

The forces:

- **It is authored data, not code.** A designer wants to add a state or change a
  transition without recompiling. That argues for a component, which the
  serializer and the inspector already understand, over a `Script` (ADR-0044),
  which is arbitrary game code with no structure the editor can present.
- **It must reuse the animation we have.** A state selects a clip; the clip is
  a range of the sprite sheet the existing animator already knows how to show.
  Inventing a second playback path would duplicate `SpriteAnimationSystem`.
- **The module graph is acyclic and reviewed.** A new module edge is an
  architectural change. The machine needs `scene` (components), `systems` (the
  frame step), and `reflect` (serialization), all of which are below it.
- **Reflection is a closed, scalar model.** `reflect::Value` (ADR-0005,
  ADR-0006) is a fixed set of scalar leaves plus fixed-size groups. It has no
  "list of structs" kind, and a state graph is exactly a variable-length list of
  states and transitions.

## Decision

Add an `anim` module (`engine/include/fe/anim/`, `fe::AnimationStateMachine`,
`fe::AnimationStateMachineSystem`, `fe::reflect::register_anim_types`) that
depends only on `core`, `scene`, `systems`, and `reflect`.

**The machine is a component; the step is a system.** This mirrors the existing
split exactly. `AnimationStateMachine` holds the authored graph — a list of
`AnimationState` (name, first frame, frame count, fps, loop) and a list of
`AnimationTransition` (from, to, require-finished, trigger conditions).
`AnimationStateMachineSystem` gathers the machine-bearing nodes into a flat list
and advances each once per frame, the same shape every system follows
(ADR-0002).

**A state is a clip over the existing animator's sheet.** A state names an
absolute first cell and a frame count, so several states share one sheet the way
a character sheet is normally authored (one clip per row). When
`drive_animator` is set, the system writes the active state's cell onto the
node's `SpriteAnimator::current_frame`; the existing `SpriteAnimationSystem`
keeps stepping the sheet unchanged, and the renderer is untouched. The machine
chooses *which* cell; it does not take over UV generation.

**Ordering is explicit, as everywhere.** Add `AnimationStateMachineSystem`
after `SpriteAnimationSystem`, so the machine's cell is the last writer in the
frame and the rendered UVs are the machine's. A machine with
`drive_animator = false` writes nothing and the animator plays its own loop.

**Transitions are gated by triggers and clip completion.** A transition is
eligible when its source is the active state, or when its source is empty
("any state", the interrupt edge). Conditions are trigger names combined with
AND; a transition fires when all its triggers are set (and, if
`require_finished`, the source clip has ended). Triggers are edge events: the
firing transition consumes them, so a trigger drives at most one edge per frame.
A triggerless self-edge is ignored, which is what stops an unconditional
`idle -> idle` from re-entering its own state forever.

**The graph serializes as a compact JSON string on one reflection property.**
Rather than widen `reflect::Value` with a list-of-struct kind for a single
component, the machine exposes one `String` property (`graph`) whose getter
calls `to_json()` and whose setter calls `from_json()`. The scene serializer
round-trips it like any other string, so a machine persists, loads, and can be
hand-edited in a scene file today. The property carries the `Multiline` hint, so
the inspector shows it as a text box. This is a deliberate, documented
limitation: a dedicated graph editor is a follow-up, and when it lands it can
either keep the string form (and render it) or promote the graph to a real
reflection kind.

**Playback is pure and headless.** `advance(delta_time)` owns the clip clock
(frame count derived from `fps`, exactly as `SpriteAnimationSystem` derives it)
and the transition evaluation; it takes no clock, no window, and no RHI. It
advances on the same frame a state is entered, matching the existing animator,
so the two never disagree about when a step happens. Tests drive it with a known
delta and assert on frames and state names, never on wall-clock time.

## Consequences

- The M6 "animation state machines" row is met at the frame-playback cut:
  states, transitions, triggers, entry/exit, one-shot and looping clips, and
  driving the animator.
- A game or the editor gets a state machine by adding the component and the
  system to a schedule; nothing else changes, and the renderer and animation
  system are untouched.
- The graph persists through the scene serializer with no new reflection kind,
  at the cost of a stringly-typed property. The trade is recorded here so the
  follow-up (a graph editor, or a richer reflection model) starts from a
  decision rather than a surprise.
- Not in this cut, and left for later M6 work: blend trees and cross-fades,
  parameters with continuous ranges (a float comparison, not just a trigger),
  sub-state machines, and skeletal states. The component's shape — named states
  over clips, transitions over triggers — is what those build on.

## Alternatives considered

- **A `Script` per behaviour.** Rejected: a script is arbitrary code the editor
  cannot present as structure, and it would move authored data out of the scene.
- **A new `reflect` list kind.** Rejected for now: it is a change to a shared,
  deliberately small model to serve one component. The string property achieves
  persistence today; a list kind can still be added when a second consumer needs
  it, which is the right time to widen a shared model.
- **Folding the machine into `SpriteAnimationSystem`.** Rejected: it would put
  two responsibilities (step one loop; choose among many) in one system and
  break the "one system, one traversal" shape. Two systems, ordered, keep each
  testable on its own.
- **A per-node `update()` on the component.** Rejected: the engine has no
  per-node update callback by design (ADR-0002); the machine is stepped by a
  system like everything else.
