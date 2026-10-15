# ADR-0057: Environment rendering and a post-processing chain

- Status: Accepted
- Date: 2026-10-14
- Deciders: sqm

## Context

M4's last open row is "Environment rendering and a post-processing chain". The
3D renderer (ADR-0010) clears to the camera's flat clear colour and draws
straight to whatever framebuffer the caller bound. Two things are missing that
every 3D engine eventually needs:

1. A background that reads as an environment rather than a flat colour, so an
   untextured scene does not look like it is floating in a void.
2. A place to grade the finished frame - exposure, tonemap, vignette - which is
   also the seam every later screen-space effect (bloom, FXAA, colour grading)
   will use.

The RHI already anticipated this. `RenderTarget`'s own comment calls itself "the
seam a future post-processing chain and a 3D shadow pass will use". The shadow
pass now exists (ADR-0014). This record is the other half.

There is one hard constraint that shapes the design: the window's default
framebuffer cannot be sampled as a texture. A post-processing pass has to read
its source, so the 3D pass must render into an off-screen colour target before
the chain can run over it.

## Decision

Add two things to `Renderer3D`, both opt-in and both inert by default.

### Environment background

`Camera3D` gains a `background_enabled` flag and a `background_top` /
`background_bottom` colour pair. When enabled, `Renderer3D` draws a full-screen
quad, in clip space, behind the geometry. The fragment shader mixes the two
colours on the vertical axis.

This is a screen-space vertical gradient, not a skybox. It is the cheapest
environment that reads as a sky and it needs no cubemap, no new asset type, and
no texture sampling. It is deliberately the first cut: a cubemap or an
atmospheric-scattering sky is a later, larger step that would keep this exact
call site (`draw_background`) and swap only the shader.

The gradient follows the screen, not the camera's pitch. That is the behaviour
that is always correct for a first cut - there is no horizon line to get wrong -
and it costs nothing. A world-space sky is a property of a future cubemap.

The background is drawn with depth testing off and no blend, so it cannot
occlude or tint the geometry that follows. It is drawn even when the cull left
nothing, because an empty scene under a sky is a valid frame.

### Post-processing chain

A new `renderer3d::PostProcess` owns one shader and one full-screen quad.
`PostProcessSettings` carries `enabled`, `exposure`, `tonemap`, and `vignette`,
with defaults that are the identity (exposure 1, no vignette). `apply` samples a
source texture and writes the graded image into whatever target is bound.

`Renderer3D::render` takes an optional `PostProcessSettings`. When
`settings.enabled` is false, nothing changes: the pass draws straight to the
caller's target, byte-for-byte the pre-chain behaviour, and no off-screen target
is allocated. When it is true, `draw_chain`:

1. binds a lazily created off-screen colour target, sized to the viewport,
2. draws the background (if the camera enables it) and then the geometry into
   that target,
3. rebinds the caller's target and runs `PostProcess::apply`, sampling the
   off-screen colour texture.

The background is drawn *inside* `draw_chain`, after the destination target is
bound, so it lands in whichever image the pass draws into and is graded along
with the geometry. Drawing it earlier would put it in the wrong image.

If the off-screen target or the post-process shader cannot be created, the chain
degrades to a direct draw for that frame rather than dropping the frame. A
missing effect must never cost the caller their image.

## Consequences

- A scene gets an environment and a grading chain with no change to the runtime
  or the editor: both call the existing `render` entry point, and the default
  settings are inert.
- The off-screen target is allocated only when the chain is first enabled, so a
  2D-only or un-post-processed scene never pays for it. It is reused across
  frames and recreated only on a viewport-size change.
- The chain is a single pass today. Its shape - "sample a colour image, write a
  graded one" - is the seam bloom, FXAA, and a user-authored effect stack will
  grow behind, without changing `Renderer3D`'s call site.
- `Renderer3D::post_process_frames()` and `background_draws()` report that each
  pass ran, so tests can prove submission without reading back pixels.
- A future cubemap sky and a multi-pass effect stack are additive: they replace
  `draw_background`'s shader and extend `PostProcess`'s chain respectively.
- The window cannot be sampled, so any pass that grades the window must first
  render off-screen. That is inherent to the graphics API, not to this design,
  and it is why the off-screen target exists at all.

## Alternatives considered

- **Render the gradient as a world-space skybox.** Rejected for the first cut:
  it needs a cubemap and a projection-aware shader, and it gets the horizon
  wrong for a camera that does not look at it. A screen gradient is always
  correct and costs one draw.
- **Post-process directly into the window.** Not possible: the default
  framebuffer is not a texture. The off-screen target is forced by the API.
- **A general render-graph / effect-stack abstraction now.** Rejected as
  speculative. One pass with four uniforms is enough to prove the seam; the
  graph can be built when a second and third pass exist to justify it.
- **Fold the background into the clear colour.** A clear is one colour; the
  point of the row is a gradient environment.
