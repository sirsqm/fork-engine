# ADR-0014: Directional shadow mapping

- **Status** — Accepted
- **Supersedes** — none
- **Superseded by** — none

## Context

ADR-0012 gave the renderer a directional light plus bounded arrays of point and
spot lights. Lighting is correct but casts nothing: a mesh between the sun and
the floor does not darken the floor. Shadow mapping is the next item in M4, and
it is the first feature that needs the renderer to render the scene **twice**
into different targets, so it forces two questions the RHI could not previously
answer.

1. **How does a depth-only pass get its depth into the main pass?** The main
   pass must sample the depth the light saw. That requires a render target whose
   depth attachment is a *texture*, not a renderbuffer. Until now every depth
   attachment was a renderbuffer, which cannot be sampled.
2. **How is the depth pass bounded and delivered?** A directional light has no
   position, so the shadow map's coverage must be derived from the scene. The
   map is a fixed-size texture, so the projection that fills it has to be chosen
   deliberately rather than derived from the camera.

The engine's rules apply: no speculative frameworks, one working feature over
several half-features, and nothing is done until it is tested.

## Decision

**A single-cascade directional shadow map rendered in a depth-only pass, sampled
with a 3×3 PCF filter in the main pass.**

### RHI

- `RenderTargetDesc` gains `sample_depth`. When true, the depth attachment is a
  texture (created through the existing `create_texture` path, which already
  supports the `Depth24` format) instead of a renderbuffer.
- `RenderTarget::depth_texture()` returns that texture, or `nullptr` when the
  target has no sampleable depth. This keeps the default behaviour unchanged:
  the editor viewport and the 2D tests use a plain renderbuffer and see
  `nullptr`.
- The depth texture is sampled with compare mode `NONE`, so the comparison is
  done in the shader rather than through a hardware shadow sampler. This keeps
  the shader backend-agnostic; a `sampler2DShadow` path is an optimization, not
  a correctness requirement.

### Shadow pass

- `Renderer3D` renders the visible instances once more into a 1024×1024
  sample-depth target with a depth-only shader (position in, no colour out).
- The light-space matrix is an orthographic projection fitted to the world AABB
  of the instances that survived camera culling. Those are the only casters whose
  shadows can land on something visible, and fitting to them keeps the map from
  being spread thin by geometry the camera cannot see.
- The orthographic box is sized from the scene's bounding **sphere**, not its
  AABB. The sphere radius is view-independent, so coverage does not shrink when
  the scene is viewed edge-on the way a per-axis AABB fit would.
- The light "eye" is placed opposite the light direction, one scene diameter
  back, with a margin for geometry above the scene box. The up vector falls back
  to +Z when the light is near-vertical, so `look_at` never degenerates.
- The map is cleared to far depth before the pass, and the previously bound
  render target is restored afterwards, so a caller that had bound its own target
  (an editor panel) still gets the main pass where it expected.

### Sampling

- The main pass converts the world position into light space, remaps clip space
  to `[0, 1]`, and compares against the stored depth. Fragments outside the map
  are treated as lit — there is no occlusion information there.
- A slope-scaled bias removes self-shadowing acne on surfaces nearly edge-on to
  the light.
- A 3×3 neighbourhood is averaged to soften the texel edge enough to read as a
  shadow rather than a staircase.
- Only the directional light casts a shadow. Ambient stays unshadowed so
  occluded areas keep some fill, and point/spot shadows are left for later.

## Consequences

- A scene with a directional light and any geometry casts and receives shadows
  with no new components and no per-object flags: every rendered mesh is a
  caster, every lit fragment is a receiver.
- The shadow map is a **fixed 1024² over the whole culled scene**. A large scene
  shares one map, so shadows lose crispness as the scene grows. Cascaded shadow
  maps fix this without changing the call site — the light-space matrix is
  already computed in one place.
- `last_shadow_casters()` reports how many instances the depth pass drew, so the
  pass is observable and testable without inspecting GPU state.
- The 1024² target is created lazily on the first frame with a directional
  light, so a 2D-only or unlit scene never pays for the allocation.
- `sample_depth` is a general RHI capability, not a shadow special case; a
  reflection pass or a depth-of-field effect can use it unchanged.
- The extra pass roughly doubles the vertex cost of a frame. For the scene sizes
  this engine targets today that is acceptable; a depth prepass or caster
  culling is the natural next step if it stops being.

## Alternatives considered

- **Hardware shadow samplers (`sampler2DShadow` + `GL_TEXTURE_COMPARE_MODE`).**
  Deferred: it would let the hardware do the comparison and gain free PCF on
  some drivers, but it needs a separate sampler type and a compare-mode texture
  parameter in the RHI. Manual comparison keeps the RHI smaller and the shader
  portable, and the cost is a few extra texture reads.
- **Cascaded shadow maps (CSM) now.** Rejected for this step: CSM is several
  passes with frustum fitting per cascade. One cascade is correct for the common
  "sun over a scene" case and is the foundation CSM would extend.
- **Fitting the ortho box to the camera frustum rather than the scene AABB.**
  Rejected: it needs the frustum corners and a stable fit, and it makes the map
  contents depend on where the camera looks, which causes shimmering as the
  camera moves. The scene AABB is stable and simpler.
- **A per-object `casts_shadow` / `receives_shadow` flag.** Deferred: it is the
  first thing a user will want for a large ground plane, but it adds component
  surface and serialization for a knob that has no second use yet. The renderer
  treats every mesh uniformly until a concrete need appears.
- **Point and spot shadow cubes/cones in the same change.** Rejected: each needs
  its own map layout (a cube map, a single frustum) and its own fitting. Doing
  directional shadows first, well, is worth more than three partial shadow types.
- **A renderbuffer depth attachment plus a depth copy to a texture.** Rejected:
  an extra full-resolution copy every frame for no benefit over allocating the
  depth attachment as a texture directly.
