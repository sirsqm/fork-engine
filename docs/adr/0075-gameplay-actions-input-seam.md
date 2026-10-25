# 0075. Gameplay actions as the input seam

Date: 2026-10-25

Status: Accepted

## Context

`InputState` reports devices, not intentions. It knows which *keys* are down,
which *mouse buttons* are held, and where each *touch contact* is; it does not
know that a key, a button and a finger can all mean "move left". Gameplay code
therefore had to name devices: the breakout paddle asked
`is_key_down(Key::Left) || is_key_down(Key::A)`.

That is a real problem for the shipping path, not a stylistic one. ADR-0071 makes
a 2D game in a browser the first target, and a phone has no keyboard. Touch was
already reaching `InputState` through `on_touch` — but a touch present in the
input snapshot that no gameplay code can ask about is not input; it is a
partially wired subsystem. The gap was visible in the roadmap's own wording:
touch was "reported through `InputState`", with "no backend feeding it yet" and
nothing consuming it even once a backend did.

Three further facts shaped the decision:

* The web backend delivered no input at all. `WebWindow::poll_events()` was an
  intentional no-op, because a browser delivers input as DOM events rather than
  as a poll, and nothing had registered listeners for them. A web build therefore
  rendered a game nobody could play.
* The same gameplay code has to run on desktop and on a phone. Branching inside
  each system on "is this a touch build?" would spread device knowledge through
  gameplay and make a third device (a gamepad, ADR-0041's remaining row) another
  edit in every system.
* An emsdk build resolves an Emscripten event target through
  `document.querySelector()`, so a listener on the canvas receives key events
  only while the canvas holds focus. Where an event binds is a behavioural
  decision that has to be measured, not assumed.

## Decision

Introduce a gameplay **action** layer, and wire the web backend's DOM events
into `InputState` underneath it.

1. **`ActionMap` (platform) is the seam.** An action is a named intention
   ("MoveLeft") with a handle; a binding connects a device to it — a `Key`, a
   `MouseButton`, or a normalised touch region. `update(input, viewport)`
   recomputes every action from the frame's input snapshot, so actions inherit
   `InputState`'s per-frame edge semantics exactly and cannot drift from them.
   Gameplay asks `is_down(MoveLeft)` and never learns which device fired.

2. **Touch regions are normalised (0..1).** A binding survives a resize and a
   change of orientation, which is what a phone needs. The region is matched
   against the contact position divided by the viewport, so the same binding
   works in portrait and landscape.

3. **`FrameContext::actions` carries the map to systems.** The `Application`
   owns one `ActionMap`, re-evaluates it once per frame after the backend has
   delivered events and before any hook or system runs, and hands it to every
   system through the frame context. Nullable, like `input`, so a headless tool
   or a test still runs a schedule with no actions.

4. **The web backend feeds `InputState` from DOM events.** Keyboard binds to the
   *window*, because a canvas listener misses keystrokes while the canvas is
   unfocused. Pointer, wheel and touch bind to the *canvas*, because their
   coordinates are reported relative to the element the listener is on and
   gameplay wants canvas space. Touch maps the browser's stable contact
   `identifier` onto `InputState`'s touch id, so per-contact queries work
   unchanged. Touch, wheel and key handlers return `true` where appropriate to
   suppress the browser's default behaviour (scrolling, zoom, a long-press menu),
   which is what makes a phone playable rather than merely event-receiving.

5. **Physical keys, not produced characters.** The keyboard table maps the UI
   Events `code` ("KeyA", "ArrowLeft") rather than `key` ("a"), so WASD stays put
   on a non-QWERTY layout — the same choice the GLFW backend makes by mapping
   physical key codes. `KeyCode` holds the table as a pure function, so the
   mapping is testable with no browser and is the stable vocabulary a rebinding
   file would use.

6. **Touch is enumerable.** `InputState::touch_count()` / `touch_id_at()` let a
   caller find contacts it does not know the ids of, which is exactly what an
   action map is: a consumer that must discover the contacts itself.

## Consequences

* One gameplay system serves a keyboard, a mouse and a touchscreen. Breakout's
  paddle and ball launcher now read actions, with the raw-key reads kept only as
  a fallback for a schedule built with no action map, so no existing caller
  breaks.
* A phone can play the exported web build: the outer thirds of the screen steer
  and the middle third serves. That is the roadmap's Phase 2 goal, and it is
  verified in a real browser, not only in a unit test.
* The device set is extensible in one place. Adding a gamepad is a new binding
  kind and a row in the backend, not an edit to every system.
* `ActionMap` is derived state, not accumulating state. It holds no per-frame
  history of its own, so it cannot disagree with `InputState` about an edge.
* The web backend now depends on where a listener is bound, and that differs
  between keyboard and pointer. The choice is documented at the binding site and
  guarded by the browser smoke test, which dispatches real DOM events and asserts
  they arrive — a wrong target fails the test rather than shipping a game that
  ignores input.
* Action handles are 1-based positions, so a game declares its actions once at
  startup and holds the handles; removing an action would renumber later ones.
  Actions are only ever added, which is how a game declares them.

## Alternatives considered

* **Branch on the device in each system** (`#ifdef FE_PLATFORM_WEB`, or a runtime
  "is touch" flag). Rejected: it puts device knowledge in gameplay, multiplies
  with every device, and cannot express "this finger means jump" at all.
* **Map touch onto synthetic keys** (a touch region writes `Key::Left` into
  `InputState`). Rejected: it would make the input snapshot lie about a keyboard
  that is not there, break per-contact queries, and collapse two fingers onto one
  key, so simultaneous steering and acting would be impossible.
* **Bind every event to the window.** Rejected on the measurement above: pointer
  and touch coordinates would then be page-relative, silently offset by the
  canvas's place in the page.
* **Bind keyboard to the canvas and require a click first.** Rejected: a game
  that ignores input until the player clicks the canvas is a bug, and the window
  receives the bubbled events anyway.
* **Use the layout-dependent `key` value.** Rejected: it breaks WASD on any
  non-QWERTY keyboard, and the desktop backend already binds physically.
* **A third-party input library.** Rejected: the engine's platform layer is
  meant to be the complete OS contract (ADR-0041), and the mapping is small
  enough to own and test.
