# ADR-0098: Persisted settings and a settings screen

## Status

Accepted.

## Context

The breakout slice could be played, paused, and scored, and it saved a best
score through the writable-user-data seam ([ADR-0096](0096-writable-user-data-storage.md)).
It had no settings: the volume was a compile-time constant and the paddle speed
was a field of the `Controls` struct baked into the systems. A player on a phone
could not turn the sound down, and a player who found the paddle too fast had no
recourse but a rebuild. The shipping queue's `slice-settings` item asks for
"volume and control settings, persisted, with a touch-friendly menu", done when
"a test changes a setting, reloads, and observes it retained".

The forces at play:

- The best-score save already established the pattern: a system reads the store
  from the `FrameContext`, which is nullable, so a headless run does not touch a
  real save directory. Settings are the second consumer of that seam, and they
  must not fight the first over one key.
- The engine's `AudioSystem` is created *after* the schedule is built (it needs
  the device), so a system cannot be handed it at construction. The volume has
  to reach the mixer anyway, or the setting is cosmetic.
- The paddle speed is already read every frame by `PaddleControlSystem`, so a
  setting should change what that system reads, not introduce a second copy of
  the value that can drift.
- The menu UI is the screen-space `UiCanvas`/`UiElement` layout
  ([ADR-0086](0086-in-game-ui-layout-and-extraction.md)), whose whole point is
  that a click lands where the button draws. A settings menu should be built
  from the same elements as the other menus, with no bespoke input path.
- The state machine's screens are a closed set, and a new screen has to be
  reachable, escapable, and unable to corrupt the screen it was opened from.

## Decision

**A `Settings` value type in the pure rules.** Volume (0..1) and paddle speed
(world units per second) live on `BreakoutState` as a `Settings` struct with
clamped setters and step adjusters (`adjust_volume`, `adjust_paddle_speed`). The
type is pure and engine-free, like the rest of `BreakoutState`, so the clamping
and the text form are unit-testable with no scene. Defaults reproduce the game
as it was authored before the setting existed, so a first run is unchanged.

**Settings are serialized as the same `key=value` text the best score uses, under
their own key.** `config::kSettingsKey` is distinct from `config::kBestScoreKey`,
so the two saves never overwrite each other. An unreadable or unknown line is
skipped rather than fatal: a save from a newer build must not stop the game.

**A `Screen::Settings` screen with a recorded return.** `open_settings()` is
allowed only from the title and a pause -- the two screens that carry a settings
button, and neither of which is a live rally -- and records which one it was.
`close_settings()` returns there, so opening settings from a pause does not
silently abandon the paused game. Opening from the game-over screen is refused:
its result would immediately re-assert itself over the settings screen, since the
rules own the game-over screen and `sync_screen()` reconciles it every frame.

**A `SettingsSystem` that loads once, applies every frame, and saves on the edge
of a change.** It takes the store from the `FrameContext` (nullable, like
`PersistSystem`), reads the saved settings on the first frame a store is
available, mirrors the volume onto the audio system's master gain every frame,
and writes only when the settings differ from the last frame's -- so a held
button does not write a file sixty times a second. The paddle speed needs no
application step at all: `PaddleControlSystem` reads `state.settings().paddle_speed()`
directly, so there is one copy of the value.

**The application hands the audio system to the settings system after adopting
it**, through a borrowed pointer set in `main.cpp`. The system tolerates a null
audio pointer, so a headless test loads and saves settings with nothing to apply
the volume to.

**The menu is authored with the existing UI components.** A settings canvas holds
a `Column` of rows, each row a `Row` of a "less" button, a value readout, and a
"more" button, all sized and placed by the one layout solver the renderer and the
input system already share. `MenuSystem` reads a click on a button through the
same `UiElement::interaction.clicked` path the other menus use, so a tap and a
click are the same event. The readouts are written by `MenuSystem`, keeping the
drawing of a value and the value itself in one system.

## Consequences

- A setting changed on the menu is in force the same frame (the volume reaches the
  mixer, the paddle reads the new speed) and is saved once, then read back by a
  fresh state on the same store -- the acceptance test.
- A run with no store still applies a setting but writes nothing, so a headless
  autoplay run leaves a real save directory untouched.
- The settings screen is reachable by a tap (the title and pause buttons) and by
  the existing menu navigation, and it cannot be entered mid-rally or from the
  game-over screen, so it cannot disturb a live game or a finished one.
- Adding a third setting is one row in the builder, one field on `Settings`, and
  one line in `parse_settings`; nothing else in the screen changes.
- `PaddleControlSystem` now reads the speed from the state rather than from the
  `Controls` struct, so the struct's `paddle_speed` is only the default the
  settings start from. That is the one place the old field and the new value
  could disagree, and the default is set from the struct to keep them equal.

## Alternatives considered

- **Store the volume on the `AudioSystem` and read it back.** Rejected: the
  setting is a property of the player's preferences, not of the mixer, and a run
  with no audio would lose it. The mixer is the thing the setting is *applied*
  to, not the thing that owns it.
- **Give the paddle system a `set_speed` the menu calls.** Rejected: it is a
  second copy of the value that can drift from the saved one, and it makes the
  menu know about a gameplay system. Reading the state keeps one copy and one
  direction of dependency.
- **Save settings every frame.** Rejected: a menu button held down would write
  the file on every frame. Saving on the change edge writes once per decision.
- **A separate settings file, or a new storage format.** Rejected: the store is
  keyed, so one more key is free, and the `key=value` text is already proven by
  the best score. A second format is a second thing to version.
- **Allow opening settings from the game-over screen.** Rejected: the rules
  reconcile the screen to game-over every frame, so the settings screen would be
  replaced on the next frame. Making it work would mean teaching the rules about
  a screen they do not own, for no player benefit.
