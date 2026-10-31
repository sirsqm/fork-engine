# ADR-0107: A real typeface and a role-named palette for the editor UI

- **Status:** Accepted
- **Date:** 2026-10
- **Related:** [ADR-0007](0007-editor-ui-toolkit.md), [ADR-0106](0106-one-mark-for-the-engine.md)

## Context

The editor drew with Dear ImGui's built-in bitmap font and a stock
`StyleColorsDark()` theme. That is the correct starting point — it is one call
and it needs no assets — but it is also the reason the editor read as a debug
overlay rather than a tool: the bitmap font is a fixed 13px pixel face that
cannot be scaled, and a stock ImGui theme names its colours by widget
(`Button`, `FrameBg`, `Header`) rather than by role, so there was nowhere to
say "this is the raised surface" and have every widget agree.

Three concrete problems followed from that:

- **No typographic hierarchy.** A panel title, a section heading and a row label
  were all the same face at the same size, so nothing guided the eye.
- **No scaling.** The bitmap font is baked at one size. A larger UI or a denser
  UI (a big monitor, a small laptop) was not expressible.
- **Colour by widget, not by role.** Two panels that both wanted "a muted hint"
  reached for two different ImGui slots and drifted apart; a theme change meant
  editing each site.

The last commit before this one ([ADR-0106](0106-one-mark-for-the-engine.md))
gave the project a mark and a light/dark pair, which made the remaining gap
obvious: the *chrome* still looked like a framework default.

## Decision

The editor ships its own typeface and its own role-named palette, and both are
values that can be tested without a toolkit.

**Fonts.** Inter (Regular and SemiBold) and JetBrains Mono (Regular), all under
the SIL Open Font License, are committed under `editor/assets/fonts/` with their
licences beside them, and their bytes are **embedded in the binary** by
`cmake/EmbedFonts.cmake` at configure time. Embedding rather than loading from a
data directory is what makes the font findable from a build tree, an install
tree, and an AppImage alike: there is no directory to locate relative to the
executable, and a user cannot break the editor by moving a file. The committed
contract is `EditorFontData.h`; the generated unit is never committed.

**Palette.** `EditorPalette` names colours by **role**, not by widget:
`window_bg`, `surface`, `surface_raised`, `surface_sunken`, `border`,
`text`/`text_muted`/`text_disabled`, `accent`, `selection_bg`, `danger`,
`success`. `EditorStyle.cpp` is the one place that maps a role onto ImGui's
colour slots, so a panel never names an ImGui slot and re-theming is a change to
the palette and nowhere else. Because the palette is a plain value it is checked
without a window: the tests hold body text to the WCAG AA contrast ratio (4.5:1),
require the three text tiers to be strictly ordered, and require the surfaces to
be ordered from sunken to raised.

**Metrics and density.** `EditorMetrics` carries the measurements (paddings,
radii, indent, font sizes) and derives two densities: Comfortable (the default)
and Compact, which tightens the vertical rhythm and steps the type down a little
without going below a legible floor. The scheme — palette plus metrics — is one
value, `EditorScheme`, selected by theme and density together.

**Shared widgets.** `EditorWidgets.h` holds the pieces every panel repeats: a
label/value row, a section heading, a status badge, an empty-state hint. A panel
that reaches for these instead of assembling its own spacing is what makes the
editor look like one program rather than a set of panels.

**Chrome.** The menu bar keeps only menus; a second **transport bar** (a
viewport side bar on the `surface` colour) carries the real controls — Play/Stop,
Undo/Redo, a segmented 2D/Auto/3D viewport-mode control — and a right-aligned
status readout with `modified` and `PLAYING` badges.

**One subtlety worth recording.** A density change moves the font size, so the
atlas must be re-baked. Clearing the atlas *inside* a frame removes the font that
frame has bound, which ImGui documents as illegal and which aborts with an
assertion inside `ImTextureData::GetPixels()`. The rebuild is therefore deferred:
`apply_theme` sets a flag and `begin_frame` performs the rebuild between frames,
where nothing is bound. The host test drives this exact sequence (Comfortable →
Compact → Comfortable across frames) so the abort cannot return unnoticed.

## Consequences

- The editor has a typographic hierarchy (Regular body, SemiBold headings,
  Mono for paths and numbers) and can be re-scaled by adding a density, because
  the sizes are one place.
- A theme or density change is immediate and total: one function re-applies every
  colour and measurement, and the shared widgets read the same palette.
- The look is testable. The palette's contrast and ordering and the metrics'
  internal consistency are unit tests (`test_editor_style.cpp`), so an unreadable
  scheme fails a test rather than shipping.
- The binary grows by the embedded font bytes (~1.2 MB). That is the price of not
  having a data directory to find; it is small next to the engine and it buys a
  single self-contained executable.
- Fonts are a new kind of committed asset, so they carry their licences and a
  reader can see why they are safe to redistribute.

## Alternatives considered

- **Load the fonts from disk at run time.** Rejected: the path differs between a
  build tree, an install tree, and an AppImage, and the failure mode (a missing
  font) is a wrong-looking UI with no build error. Embedding removes the search.
- **Keep the bitmap font and only restyle the colours.** Rejected: it fixes the
  palette but not the hierarchy or the scaling, which are two of the three
  problems.
- **Name colours by ImGui widget and theme per widget.** Rejected: it is what the
  stock theme does, and it is why panels drifted. A role name is what lets a
  panel ask for "a muted hint" and get the same colour everywhere.
- **Re-bake the atlas inside `apply_theme`.** Rejected: it aborts, as described
  above. Deferring to the frame boundary is the only correct place.
