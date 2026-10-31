# ADR-0106: One mark for the engine, the window, and the web page

- **Status:** Accepted
- **Date:** 2026-10
- **Related:** [ADR-0102](0102-downloadable-release-builds.md), [ADR-0103](0103-linux-appimage.md),
  [ADR-0041](0041-platform-contract-and-null-backend.md), [ADR-0071](0071-web-first-2d-shipping-path.md)

## Context

Phase 3 of the roadmap is "a player downloads one file per OS, runs it, and plays
the game." [ADR-0102](0102-downloadable-release-builds.md) and
[ADR-0103](0103-linux-appimage.md) built that download. What they left is
identity: the artifacts exist, but the thing a person actually sees — a window in
a task bar, a tab in a browser, an icon in a file manager — was whatever the OS
or the browser defaulted to. A default icon is a small "unfinished" signal on an
otherwise finished download, and the README had a stale badge pointing at a
repository name that no longer exists.

There are four places the project's mark has to appear, and each has a different
medium:

- the **README**, as a banner that must read on GitHub's light or dark theme;
- the **AppImage icon**, a PNG a desktop's file manager reads;
- the **window icon**, an RGBA buffer handed to GLFW for the title bar and the
  task bar;
- the **web page's favicon**, an inline asset in a single-page export.

Defining the mark four times is the obvious failure: they drift, and the drift is
invisible because nothing compares them. The mark is also the one asset that must
work in two inks, because a white mark on a white tab bar and a black mark on a
dark task bar are both invisible.

## Decision

**1. The mark is defined once, in `branding/`, as SVG.** The logo and the banner
each ship as a black and a white SVG with identical geometry. The SVG is the
source; every PNG and every buffer is a render of it. `tools/brand_raster.py`
renders the PNGs, so a change to the SVG is one command away from every consumer.

**2. The window icon is a bit mask, not an image.** `tools/gen_engine_icon.py`
turns the logo into a 1-bit mask, checked in as `engine/src/platform/
EngineIconMask.h`. `fe::WindowIcon` expands and tints it at construction time, so
the platform layer hands a window backend a plain RGBA buffer without the engine
depending on an image library. White is the default (the dark title bars both
GNOME and Windows use); black is for a light one.

**3. The window icon follows the editor theme.** `EditorHost::sync_window_icon()`
re-sets the icon when the theme changes, so the mark stays legible: white on the
dark chrome, black on the light. The descriptor's default is the engine's mark, so
a window looks like the engine without a caller asking.

**4. The favicon is inline, and theme-aware.** The web shell emits the mark as an
`<svg>` data: URL with a `prefers-color-scheme` rule. Inline because the export is
a single page plus its module and pack — a separate favicon file is one more thing
to serve and one more way for the page to 404. Theme-aware for the same reason the
window icon is: one ink cannot read on both tab bars.

**5. A window backend that cannot set an icon ignores it.** `Window::set_icon()`
defaults to a no-op, the way the rest of the platform contract does
([ADR-0041](0041-platform-contract-and-null-backend.md)): the null backend and the
web backend have no OS title bar, and the call must not require one.

## Consequences

- The mark has one source. A change to `branding/*.svg` regenerates every PNG and
  mask through the checked-in tools, and the four consumers cannot disagree about
  geometry.
- A window and a tab both show the project's mark in the ink their background
  needs, without the user choosing.
- The engine gains a small, dependency-free asset path (`WindowIcon`) rather than a
  dependency on an image decoder.
- The editor gains a second piece of view state (the theme) that the window icon
  reads. It is not serialized: the scheme is not a property of the document.
- The tests pin the properties that matter for a buffer (square, RGBA, both opaque
  and clear pixels, the two inks share coverage) rather than exact pixels, so
  regenerating from the SVG does not fail them.

## Alternatives considered

- **Commit only PNGs and skip the SVG source.** Rejected: the PNG is a render, and
  without the source a resize or a colour change is a manual edit that drifts from
  the other three copies.
- **Have the engine depend on an image library (stb_image, lodepng) to load the
  logo at run time.** Rejected: the mark is a single monochrome bitmap; a 1-bit
  mask expanded in a few lines avoids a dependency and a file the engine has to
  find at run time, which the AppImage and the web export would both have to carry.
- **A separate favicon file beside the export.** Rejected: it is a second thing to
  serve and a 404 waiting to happen, for an asset that is a few hundred bytes.
- **A single-ink mark (always white, or always the brand colour).** Rejected: the
  window chrome and the browser tab bar each come in both light and dark, and one
  ink is invisible on one of them.
