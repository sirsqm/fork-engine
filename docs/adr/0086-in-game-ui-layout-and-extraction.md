# 0086 — In-game UI: screen-space layout through the 2D extraction seam

Date: 2026-10-25

Status: Accepted

## Context

[ADR-0083](0083-in-engine-text-rendering.md) gave the engine text: a `TextLabel`
that emits glyph quads. That is enough for a score readout, and the breakout
slice uses exactly that — but a label is a *world-space* object. Its glyphs are
placed by the node's transform and drawn through the same camera as the field,
so its on-screen position moves with the camera and its size scales with the
camera's fit. A HUD built from labels therefore drifts: change the window's
aspect ratio and the score slides out of its corner, and a "centre the message"
screen is centred on the *field*, not on the window.

Phase 4 ("the game is worth shipping") needs more than a drifting readout. The
queue's `slice-title-and-menus` is a title screen, a pause overlay, and a
game-over screen, and `slice-settings` is a panel of controls. Every one of them
is *screen-space*: a menu is pinned to the window, not to the world, and a
button is a rectangle the player points at, not a sprite the player collides
with. Text alone cannot express any of that.

The two constraints are the ones the engine already set. First, the render seam
([ADR-0052](0052-render-extraction-seam.md)): the authoring model (a scene graph
of components) and the processing model (a flat array of draw instances) meet at
one seam, and a new drawable should join that seam rather than add a second draw
path — glyph quads and tiles already prove this works. Second, the authoring
model itself: UI should be *components in the scene*, so it is saved by the same
serializer, undone by the same operation log, and edited by the same inspector
as everything else. A parallel widget tree with its own lifetime and its own
save format is a second source of truth, which the durable-edit model
([ADR-0045](0045-component-identity.md),
[ADR-0047](0047-local-operation-model.md)) is built to avoid.

There is no in-game UI today. The editor has Dear ImGui, but ImGui is an editor
dependency the game must not link ([ADR-0007](0007-editor-ui-toolkit.md)), and
the web shell's UI is HTML that lives outside the engine.

## Decision

In-game UI is a set of scene components whose rectangles are computed by one
pure layout solver, drawn as quads through the existing 2D extraction seam, and
hit tested against that same solver's output.

**`UiCanvas` and `UiElement` are components, and they are authored in the scene
graph.** A `UiCanvas` names a design resolution, a scale mode, a font, and a
base layer; a `UiElement` is a rectangle under a canvas with a size, margins,
padding, an anchor, a flow direction, a fill, an optional texture and text, and
interaction flags. An element's children are its node's children, so the tree
the scene graph already has *is* the layout tree. This is what makes UI save,
undo, and (later) replicate for free.

**One pure layout solver is the single source of truth.** `fe::ui::layout(scene,
viewport_width, viewport_height, out)` walks the canvas and its elements and
produces a flat `UiLayoutResult` of resolved rectangles in *viewport pixels*,
each carrying its source component, its canvas, its depth, and its layer.
Drawing and hit testing both consume this one result. They cannot disagree about
where a button is, because they never compute it separately — the property the
tests exist to pin down.

**Layout is a small, named subset.** Sizing is `Fixed`, `Percent`, `Fill`, or
`Content` (the element's text measured with the font). Placement is an anchor
(nine positions), a screen-space offset, and margins. A container lays its
children out in a `Row` or `Column` with a gap, and distributes free space with
`Start`/`Center`/`End`/`SpaceBetween` on the main axis and `Start`/`Center`/
`End`/`Stretch` on the cross axis. There is no wrapping, no flex-grow, no grid,
and no scroll view; those are named as not-done so the solver is not mistaken
for a browser engine.

**Drawing joins the existing seam.** `render::extract_ui` emits `SpriteInstance`
records exactly as sprites, tiles, and glyphs do — a fill is a white-textured
quad, a UI texture is a textured quad, and text is the same glyph quads ADR-0083
already produces, now placed in screen space. The `SpriteInstance` gains a
`const ui::UiElement* ui` provenance field so a caller can attribute a quad to
its element, and the extraction is offered in both the vector and the
arena-backed form with the same byte-identical contract as the other walks.

**The UI is a second render pass, not part of the world pass.** It draws with a
screen-space projection (origin top-left, y down) and it is not culled by the
world camera. It batches and instances through the same submit helper as the
world pass, so it costs no new shader, no new pipeline, and no new batching
code; only the matrix and the ordering differ, and the ordering is insertion
order so a parent paints before its children. A scene with no canvas pays only
the extraction walk.

**Interaction is a component state, written by one system.** `UiInputSystem`
runs first in the schedule. It lays out the scene once, hit tests the current
pointer against that result (topmost first, skipping non-interactable
elements), and writes `hovered` / `pressed` / `released` / `clicked` /
`pressed_this_frame` onto each element's `UiInteraction`. Press and release are
matched by component identity, so dragging off a button and releasing is not a
click. The pointer is the mouse if there is one and the first touch contact if
there is not, so one UI serves a desktop and a phone.

**A canvas scales its design to the viewport.** `Fit` (uniform, centred with
letterbox margins), `Stretch` (independent axes), or `None` (1:1), so a HUD
authored against a reference resolution stays inside the field at any window
shape — which is the concrete defect the world-space labels had.

## Consequences

* Phase 4's `slice-title-and-menus` and `slice-settings` are unblocked: a menu
  is a canvas of elements, a button is an element with a fill that the input
  system reports a click on, and no new plumbing is needed to author either.
* The breakout HUD is migrated to the system as the acceptance proof, and its
  score and lives now stay in the window's corners at any aspect ratio. The
  authored `breakout.fescene` is regenerated to match.
* UI is testable with no GPU and no browser, like every other extraction
  concern: layout and hit testing are pure functions of a scene and a viewport,
  and the pass runs against the null RHI device. The load-bearing test asserts
  that the rectangle the renderer draws a button in equals the rectangle the
  input system hit tests.
* The draw path is unchanged. UI quads batch and instance with the world's, so
  the existing performance work covers UI by construction; a UI that shares no
  texture with the world costs one extra draw call, which the tests count.
* Because UI is components, it inherits serialization, reflection, and undo
  without new code, and it is positioned to join the operation log
  ([ADR-0048](0048-replicated-operation-log.md)) as an ordinary component rather
  than a special case.
* The solver is intentionally small. Centring, pinning, stacks, and text
  measurement cover menus and HUDs; a layout that needs wrapping or flex-grow is
  a reason to extend the solver behind the same interface, not to reach for a
  second one.
* A `UiElement` measures its text with the canvas's font, so an element's size
  can depend on a resource. A missing font yields a zero-size text box rather
  than a crash, which is the same "draw blank, do not fail" rule the textures
  follow.

## Alternatives considered

* **Keep using world-space `TextLabel`s and reposition them each frame.** What
  the slice did before this. Rejected: the position must be recomputed from the
  window size every frame in every game, the size still scales with the camera,
  and there is no way to hit test a label, so a menu could not be built at all.
  It is the defect, not the fix.
* **Draw the game's UI with Dear ImGui.** Rejected: ImGui is the editor's
  toolkit, the game must not link it, and it would put an immediate-mode UI in
  the runtime's draw path with its own input handling and its own font atlas,
  duplicating both the text path and the input path that already exist.
* **A separate retained UI tree with its own root, lifetime, and save format.**
  Rejected: it is a second source of truth beside the scene graph, it would need
  its own serialization and its own undo integration, and it is exactly the
  duplication the durable-edit model exists to avoid. Components in the scene get
  all of that for free.
* **A third draw pass with its own shader and its own batching.** Rejected: UI
  is textured and coloured quads, which is precisely what the 2D path already
  draws. A second path would duplicate the batching, the instancing, and the
  arena handling, and would need its own culling story. The UI pass differs from
  the world pass only in its matrix and its ordering.
* **Hit test by walking the scene graph directly.** Rejected: it would compute
  the rectangles a second time, and the second computation is the one that
  drifts from what is drawn. Hit testing the layout result keeps one source of
  truth, which is why a test can assert the two agree.
* **A full flexbox/CSS layout engine.** Rejected for now as a large, separate
  problem with a large surface. The small solver covers the slice's menus and
  HUDs and leaves the door open: the interface is one function over a scene.
