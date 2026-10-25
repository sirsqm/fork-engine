# 0076. 3D in the browser

Date: 2026-10-25

Status: Accepted

## Context

ADR-0074 gave the engine a WebGL2 window backend, and ADR-0071's shipping path
proved a *2D* game runs in a browser end to end. The 3D renderer had never been
asked to run there, and when it was, three defects appeared in a row — each one
invisible on the desktop, each one a hard failure in the browser. They are worth
recording together because they are the same kind of defect: desktop OpenGL 3.3
is permissive where WebGL2 (GLSL ES 3.00) is strict, so a construct that works on
one context fails on the other with no build-time signal.

1. **The shadow map's depth texture was allocated illegally.** The shadow pass
   renders depth into a texture and samples it in the main pass, so
   `OpenGLRenderTarget` allocates it with `glTexImage2D(..., GL_DEPTH_COMPONENT24,
   ..., GL_DEPTH_COMPONENT, GL_FLOAT, nullptr)`. The pair
   `(GL_DEPTH_COMPONENT, GL_FLOAT)` is a valid combination on a desktop GL 3.3
   core context. In GLSL ES 3.00 it is *not* a valid combination, so the browser
   rejected the call with `GL_INVALID_OPERATION`, the texture kept no storage, and
   the shadow framebuffer came back incomplete (`GL status 36054`).

2. **The shadow target had a colour attachment its pass never wrote.** The render
   target always attached a colour texture. The shadow fragment shader has no
   output at all — it is a depth-only pass. A desktop context tolerates a draw
   whose program writes no colour into a framebuffer with a colour attachment;
   WebGL2 rejects it with "Active draw buffers with missing fragment shader
   outputs", and every shadow draw is dropped.

3. **The pruning tracer only knew the 2D types.** `fork_export` and `fork_pack`
   trace a startup scene's resource references so a pruned pack ships exactly the
   assets the scene names. The tracer called `reflect::register_builtin_types()`,
   which registers the 2D components (`Sprite2D`, `Camera2D`, `SpriteAnimator`)
   and nothing else; the 3D types are registered separately by
   `reflect::register_scene3d_types()`. A tool that links the engine library and
   traces a scene without an `Engine` therefore saw a `MeshRenderer3D` as an
   unregistered type, collected **zero** references, and shipped a pack with no
   mesh and no texture in it. The desktop player hit the same path, so a pruned
   3D export was empty on both targets — this one is not web-specific, it was
   simply never exercised because no 3D project was ever exported.

There was a fourth, smaller issue in the verification itself, covered below.

## Decision

1. **Allocate the depth texture with a combination both contexts accept.** Use
   `GL_UNSIGNED_INT` as the pixel type with `GL_DEPTH_COMPONENT24`: valid on
   desktop GL 3.3 *and* in GLSL ES 3.00. The engine already authors one shader
   source and translates the dialect in the RHI (ADR-0072); the same principle
   applies to texture storage, and one allocation now serves both targets.

2. **Make the colour attachment optional, and make the shadow map depth-only.**
   `RenderTargetDesc` grows a `color` flag (default `true`, so every existing
   caller is unchanged). The OpenGL backend skips the colour texture when it is
   false and calls `glDrawBuffers(0, nullptr)` to tell the framebuffer so
   explicitly — that call is the actual fix for the missing-output rejection. The
   null backend mirrors it so the two agree. The shadow target sets `color =
   false`, and `clear()` only clears the buffers it actually has.

3. **Register every component type through one aggregate, and call it from the
   tools.** A new `fe::runtime::register_all_component_types()` registers the 2D
   builtins, tilemaps, animation, 2D and 3D physics, the 3D scene components, and
   audio. It lives in `runtime` because that is the one module allowed to depend
   on all of them; `reflect` cannot host it, because `scene3d` and the other
   component modules already depend on `reflect`, so the edge would be a cycle.
   `Engine::initialize` now calls the aggregate instead of listing the seven
   registrations itself, and `fork_export` and `fork_pack` call it before tracing
   a scene. The scene-reference collector keeps its 2D floor — a caller that
   traces a 2D scene still needs no setup — and its comment now points at the
   aggregate for a caller that needs more.

4. **Verify 3D by rendering a whole frame, not a centred patch.** The player's
   web verdict reports how many distinct colours the last frame contains, which
   is a blankness check that needs no golden image. It read a 64x64 patch at the
   centre of the frame. That is only sound when the geometry covers the middle of
   the screen: the 3D demo scene framed its camera such that the centre patch
   landed entirely on the background gradient, so a *correctly rendered* frame
   reported one colour and would have failed the check. The whole frame is read
   instead. The cost is one readback at the end of a bounded run, which is
   negligible next to the frames, and an unbounded run never reads back at all.

## Consequences

* The 3D renderer runs in a browser: the demo scene draws its three meshes
  (`drawn=3`), the shadow pass runs (`shadow=3`), the frame contains 44 distinct
  colours, and no GL error is reported. `tests/web/run_web_game.sh` gains a 3D
  project and asserts the same blankness property it asserts for 2D.
* The three browser-only constraints are now stated where the next person will
  look: a depth texture's format/type pair must be legal in GLSL ES 3.00, a
  framebuffer must declare its draw buffers, and a draw whose shader writes no
  colour needs a framebuffer with no colour attachment. The first is the kind of
  thing that will recur; the comment on `allocate_depth` is the guard.
* Pruning is correct for 3D projects, which it never was. The bug was latent
  rather than web-specific, and the fix is a real behaviour change: a pruned 3D
  export now carries its assets instead of an empty pack. `fork_export` reports
  `pruning to 4 scene-referenced asset(s)` for the demo project where it reported
  0 before.
* `register_all_component_types()` is now the single list of shipped component
  modules. Adding a module means adding one line there rather than hunting for
  every place that enumerated the set — the bug in (3) was exactly that hunt
  being missed.
* The `color` flag is a small widening of the RHI's render-target descriptor.
  A future depth-prepass or shadow-cascade pass gets the same benefit for free; a
  target that needs both colour *and* a depth texture (deferred shading) already
  works, since the flag defaults to true.

## References

* ADR-0071 — the web-first 2D shipping path (which this extends to 3D).
* ADR-0072 — the GLSL dialect seam (one source, translated per context).
* ADR-0073 — the Emscripten toolchain and the `FE_PLATFORM_WEB` macro.
* ADR-0074 — the web window backend (the WebGL2 context this renders into).
* ADR-0065 — pack reachability pruning (the behaviour fixed in point 3).
* `tests/web/run_web_game.sh` — the acceptance test for this decision.
