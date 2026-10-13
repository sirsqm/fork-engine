# ADR-0012: A forward lighting model with a bounded light budget

- **Status** — Accepted
- **Supersedes** — none
- **Superseded by** — none

## Context

ADR-0010 gave the 3D renderer exactly one light: a single `DirectionalLight3D`
looked up per frame and uploaded as a handful of scalar uniforms. That was the
right first cut, and it is enough to light a scene with a sun, but M4 lists
"directional, point, and spot lights" next, and a scene with a lamp or a
flashlight needs local emitters.

Three questions had to be settled before writing any shading code:

1. **What light types does the renderer take?** Directional, point, and spot are
   the standard three. Point and spot both need a position, so they must read
   the owning node's world transform rather than storing one, the way the camera
   already does.
2. **How are many lights delivered to a shader?** The engine targets OpenGL 3.3
   core. Uniform arrays are the portable answer; a uniform buffer or a
   storage buffer would be less portable and more machinery than a first pass
   needs.
3. **What happens when a scene has more lights than the shader can hold?** An
   unbounded loop is not an option, and neither is silently dropping the lights
   a player is most likely to see.

The engine's rules apply: no speculative frameworks, no premature optimization,
and a system is only done when it is tested.

## Decision

**A forward, single-pass lighting model with per-light-type uniform arrays and a
fixed per-frame budget of eight point and eight spot lights.**

### Components

- `PointLight3D` carries `color`, `intensity`, and `range`. It reads its world
  position from the owning node, exactly as `Camera3D` reads its eye.
- `SpotLight3D` adds `inner_cone_degrees` and `outer_cone_degrees`. The cone
  axis is the node's local -Z, the same convention the camera uses, so rotating
  a node aims a spot light the way it aims a camera.
- Both register with reflection like every other component, so they serialize
  and appear in the inspector without any per-type serialization code.

### Shading

- Blinn-Phong, matching the existing directional path: diffuse plus a specular
  term, computed per fragment in world space.
- Point and spot lights use a squared windowed falloff that reaches zero at
  `range`: `window = max(1 - distance/range, 0)`, `attenuation = window^2`.
  A bounded range keeps a light's influence finite, which matters once light
  culling is added, and avoids the never-quite-zero of raw inverse-square.
- A spot light multiplies its point-light contribution by a cone term that
  interpolates between `cos(outer)` and `cos(inner)`, giving a soft edge. The
  renderer keeps `outer > inner` so the interpolation has a non-zero width.

### Delivery and the budget

- Lights are gathered once per frame in `Renderer3D`, where the owning node is
  still in hand, into two small vectors. Each light's node world position (and
  the spot's forward axis) is resolved there.
- When a scene exceeds the budget, the renderer keeps the brightest by
  `intensity * range` using `std::nth_element`. That proxy is cheap and
  reasonable: a bright light with a large range is the one most likely to
  dominate the frame.
- The packed data is uploaded through a new `rhi::Shader::set_vec4_array`. The
  shader's loops are bounded by `u_point_light_count` / `u_spot_light_count`
  uniforms, so the compiler can unroll them and no branch depends on scene
  contents at draw time.
- `last_point_lights()` / `last_spot_lights()` report how many lights were
  forwarded, so the cap is observable and testable.

## Consequences

- A scene with a handful of local lights renders correctly with no new
  machinery: no light nodes, no per-light shader, no multi-pass.
- The budget is a **hard, visible limit**. More than eight point lights in a
  scene means the dimmest are dropped, and the drop is deterministic (brightest
  first), not arbitrary. This is documented on `kMaxPointLights` /
  `kMaxSpotLights`.
- The uniform arrays have a fixed size, so the shader's cost is bounded and
  predictable regardless of scene size. The cost of *gathering* lights is linear
  in the number of light components, which is fine for the scene sizes this
  engine targets today.
- `set_vec4_array` is a general RHI capability, not a lighting special case; a
  future shadow pass or a bone-matrix array can use it unchanged.
- The `vec3`/`float` light uniforms keep their old names and defaults, so a
  scene with only a directional light shades exactly as it did before. The
  directional path is now expressed through the same `shade` helper as the local
  lights, which removes a duplicated lighting expression.

## Alternatives considered

- **Deferred or clustered shading.** Rejected for this step: they scale to
  thousands of lights but bring multiple render targets, a light-volume
  structure, and a much larger shader. Forward with a bounded array is the right
  size for the current target and keeps the code legible.
- **A uniform buffer object (UBO) for lights.** Deferred: a UBO would let the
  budget grow without touching the shader's uniform declarations, but it adds a
  buffer layout, alignment rules, and a binding point for a first pass that fits
  comfortably in plain uniform arrays.
- **Uploading every light with no cap.** Rejected: an unbounded array is not
  valid GLSL and an unbounded loop is a performance and portability hazard.
- **Choosing the kept lights by distance to the camera.** Rejected: brightness
  scaled by range is cheaper (no per-light camera-distance sort beyond what the
  gather already computes) and does not need the camera position at selection
  time. It can be revisited if a scene shows the proxy choosing badly.
- **A single generic light component with a type enum.** Rejected: point and
  spot lights expose different properties, and three small typed components read
  better in the inspector and in code than one component with fields that are
  meaningless for some types.
- **Raw inverse-square falloff.** Rejected: it never reaches zero, so a light's
  influence is unbounded and every fragment pays for every light. The windowed
  form is cheaper and gives the artist a real `range` knob.
