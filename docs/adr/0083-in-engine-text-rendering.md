# 0083 — In-engine text rendering

Date: 2026-10-25

Status: Accepted

## Context

The engine can draw a textured quad and a grid of them, but it cannot draw a
single character. Text exists in exactly two places today, and neither is
reachable from a game:

* the editor's panels, drawn with Dear ImGui, which only `fork_editor` links
  ([ADR-0007](0007-editor-ui-toolkit.md)); and
* the web export shell's loading message, which is HTML the page shows before
  the wasm module starts.

A game built in the engine therefore cannot show a score, a title, a menu, or
any other string. This is not a missing nicety: the shipping queue's Phase 4
("the game is worth shipping") is `slice-title-and-menus`,
`slice-scoring-and-progression`, and `slice-settings`, and every one of them
needs text. The slice is playable and its score is only visible in the log line
(`examples/breakout/src/main.cpp`), which is not a game a stranger can play.

The constraint that shapes the decision is the one the render seam already
established ([ADR-0052](0052-render-extraction-seam.md)): the authoring model (a
scene graph of components) and the processing model (a flat array of draw
instances) meet at one seam, and a new drawable should join that seam rather than
add a second draw path. `Tilemap` is the precedent — a grid of tiles is many
quads from one component, and it reuses the sprite path exactly.

The second constraint is dependencies. The engine vendors only `glad` and
`stb_image`; it has no font rasterizer, no TTF/OTF parser, and no text-shaping
library. Adding one is a real dependency with a real quality question behind it
(hinting, subpixel positioning, kerning, shaping), and it is not needed to make
a game's HUD legible.

## Decision

Text is a component that emits glyph quads into the existing 2D extraction
seam, and the glyphs come from a generated bitmap-font atlas. No new draw path,
no new dependency.

**A `TextLabel` component emits one `SpriteInstance` per glyph.** It lives in
the `scene` module beside `Sprite2D` and `Tilemap`, and it is resolved by a
`resolve_label` in the same style as `resolve_sprite`/`resolve_tile`. Extraction
turns the string into glyph quads with the label's world matrix composed with
each glyph's local offset, so the renderer never learns that a character exists —
it sees textured quads, batches them with the sprites and tiles, and instances
them the same way. The parallel and arena extraction paths
([ADR-0069](0069-parallel-extraction-and-culling.md),
[ADR-0070](0070-instancing-arena-seam-and-parallel-broadphase.md)) gain a text
walk with the same byte-identical-to-serial contract.

**The font is a bitmap atlas plus a metrics sidecar.** A `Font` resource holds
the atlas as a `Texture2D` and a per-glyph record — the atlas cell's UVs, the
advance, and the bearing. The metrics are a small JSON document beside the PNG,
so the atlas is a normal texture the existing `Texture2DLoader` already
understands and only the metrics need a new loader. A glyph is looked up by its
byte value; an unknown byte draws nothing rather than a wrong glyph.

**The atlas is generated, deterministically, and committed.** A
`tools/gen_font_atlas.py` writes the PNG and its JSON in the style of
`tools/gen_breakout_assets.py`: `struct` + `zlib`, no network, no image library,
regenerable byte-for-byte. The output is committed so the build stays hermetic
and a test needs no generator run.

**Layout is deliberately simple and honest.** A single line, left-to-right,
with an integer advance per glyph; the label offers left/centre/right alignment
about its origin. There is no wrapping, no shaping, no kerning, and no
bidirectional text. Those are named as not-done so the seam is not mistaken for
a text engine.

## Consequences

* Phase 4 is unblocked. A game can draw its score, a title, and a menu, which is
  what "a game a stranger can play" requires.
* The draw path is unchanged. Glyph quads batch and instance exactly as sprites
  do, so text costs no new shader, no new pipeline state, and no renderer
  special-casing — the existing performance work covers it by construction.
* Text is testable with no GPU and no browser, like every other extraction
  concern: a test builds a `TextLabel`, extracts, and asserts on the instance
  array. The acceptance check for the feature runs in the ordinary unit suite.
* The atlas is deterministic and committed, so a test that asserts a glyph's UV
  is asserting a real, reproducible value rather than whatever a font file
  happened to rasterize.
* Scaling text scales the quads, so a large size shows the atlas's pixels. That
  is honest for a bitmap font and acceptable for a HUD; a crisp large-size
  result is a reason to add a rasterizer later, behind the same component.
* Only the glyphs baked into the atlas can be drawn. The generator includes the
  printable ASCII range, which covers the slice and most HUDs; a string with
  other bytes draws blanks rather than failing.

## Alternatives considered

* **Vendor `stb_truetype` and rasterize at load.** The most capable next step,
  and the seam is shaped so it can replace the bitmap atlas without changing
  `TextLabel`. Rejected *for now*: it adds a dependency and immediately raises
  hinting, subpixel positioning, and font-file loading, none of which is needed
  to put a legible score on screen, and all of which would be rushed. The
  bitmap atlas gets the slice shippable today and leaves this as a clean
  follow-on.
* **Draw text with ImGui in the game.** Rejected: ImGui is an editor
  dependency, the game must not link it, and it would put a UI toolkit in the
  runtime's draw path.
* **A separate text renderer and shader.** Rejected: it duplicates the quad
  pipeline, the batching, and the instancing that already exist, and it would
  need its own culling and its own arena handling. Text is textured quads; it
  should ride the path that already draws them.
* **Store the font as a code-defined bitmap in the engine.** Rejected: it puts
  content in the binary, cannot be regenerated or replaced by a game, and gives
  the atlas no path through the resource system, so a game could not ship a
  different font.
* **A full text-shaping layer (wrapping, bidi, kerning).** Rejected for now as
  a large, separate problem. Naming it as out of scope keeps the seam honest:
  the component's contract is one line of left-to-right glyphs.
