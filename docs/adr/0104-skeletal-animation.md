# ADR-0104: Skeletal animation and skinning

- **Status:** Accepted
- **Date:** 2026-10
- **Related:** [ADR-0010](0010-mesh-representation.md),
  [ADR-0040](0040-gltf-importer.md),
  [ADR-0052](0052-render-extraction-seam.md),
  [ADR-0059](0059-animation-state-machines.md),
  [ADR-0017](0017-performance-strategy.md),
  [ADR-0002](0002-scene-graph-and-systems.md)

## Context

M6 is "animation and audio", and the one item still open in it is skeletal
animation and skinning — the last thing a 3D game with animated characters needs.
Everything the milestone shipped before it animates a *sprite*: a frame index into
a sheet, advanced by `SpriteAnimationSystem` and gated by an
`AnimationStateMachine`. A character made of a skinned mesh is a different shape
of problem. Its vertices are shared between joints, each vertex is pulled by up
to four of them, and the pose that drives it is a tree of transforms sampled from
one or two clips that blend as the character changes state.

Two earlier decisions made this a design rather than an extension.
[ADR-0010](0010-mesh-representation.md) fixed the engine's `Mesh` as one indexed
triangle list with a position/normal/uv vertex, and [ADR-0040](0040-gltf-importer.md)
had `GltfLoader` read neither a skin nor an animation, deliberately: a skinned
vertex needs two more attributes, and the file's `skins` and `animations` are
top-level arrays with their own identity. [ADR-0052](0052-render-extraction-seam.md)
then split both renderers at an extraction seam, so the *set* of drawables is a
flat array that culling and batching operate on — and a skinned drawable is a
different record from a static one.

Constraints that shaped the decision:

- **A rig is shared; a mesh is shared; a clip is shared.** One `character.glb`
  mesh is drawn by a crowd, one rig drives two meshes, and one walk clip plays on
  several rigs. Whatever holds the skeleton cannot be welded into the mesh.
- **The palette is derived, not authored.** It is recomputed every frame from the
  pose and the inverse bind matrices. Persisting it would let a saved scene carry
  a pose that disagrees with the clip that produced it.
- **Skinning must be testable with no GPU.** A pose, a palette, and a vertex
  buffer are values. An assertion that "rotating this joint moves this vertex
  here" is precise; a read-back of a rendered frame is not.
- **A rig has more joints than a vertex shader has attributes.** A humanoid is
  forty to eighty joints. Per-instance attributes cannot carry a per-joint
  palette.
- **The static path must not pay.** Every existing scene, test, and primitive
  generator is a `Vertex3D` and a `Mesh`. Widening either would make every static
  vertex and every static shader carry skinning data they never use.

## Decision

Skeletal animation is a set of CPU-side resources and components, plus one RHI
addition, in four parts.

### 1. The rig, the clip, and the pose are three separate types

- `anim::Skeleton` is a `Resource`: a flat array of joints, each with a name, a
  parent index, its rest transform, and its inverse bind matrix. A mesh names its
  skeleton; a skeleton does not name a mesh, so one rig serves several meshes. The
  constructor computes `order()`, the joint indices sorted so every parent
  precedes its child, because glTF does not promise that `nodes` is topologically
  ordered and the palette walk needs one order.
- `anim::AnimationClip` is a `Resource`: a name, a duration derived from its
  keys, and one `JointTrack` per animated joint. A track carries up to three
  optional channels — translation, rotation, scale — and a channel the clip does
  not carry is *not* written, so a clip that animates one arm leaves the rest of
  the body at rest. Keys interpolate as step, linear, or cubic (glTF's three
  modes); a rotation track chooses slerp or nlerp. The clip binds to a rig by
  name, once, and a track whose joint is missing is skipped rather than written to
  joint zero.
- `anim::SkeletonPose` is a plain value type: one `JointPose` (translation,
  quaternion, scale) per joint, with no reference to a skeleton. It computes world
  matrices and the skinning palette `world * inverse_bind` when handed a rig. A
  pose can be sampled before the rig that will drive it is known, and a test
  asserts on one with no scene, no resource, and no GPU. In the bind pose the
  palette is the identity, which is the property that proves the walk and the
  inverse bind matrices agree.

### 2. Behaviour is a system; state is a component

`anim::Animator` is a `Component`: which skeleton, which clip, the playback clock
and speed, the loop mode, and whether to apply root motion — the authored,
serialized half. Its pose and palette are derived state, deliberately not
serialized and not reflected. `anim::SkeletonAnimationSystem` is the behaviour
half: it advances every animator once per frame and writes the palette in place.

In place is the point. The system samples into the animator's own `SkeletonPose`
and fills the animator's own `std::vector<Mat4>` palette through
`mutable_pose()` / `mutable_palette()`, so the per-joint buffers are allocated
once and reused. A `set_derived(pose)` taking the pose by value would copy or move
a vector per joint per character per frame. This is the same reasoning the
renderer's per-frame instance buffer uses, and the same split
`SpriteAnimationSystem` already has (ADR-0002).

Cross-fading is two clips, not one: `Animator` holds a `blend_clip` and a
`blend_weight`, the system samples both and blends the poses (translations lerp,
rotations slerp), and the outgoing clip keeps its *own* clock so the two are
sampled at the positions they actually reached. A single-clip animator would force
every transition to be authored as a clip of its own. Root motion is opt-in and
removes the root's delta from the pose after adding it to the node, so a game
driving its own controller is not fighting the clip for the same transform.

### 3. Skinning is CPU-computable, and the CPU form is the reference

`anim::SkinnedMesh` is a `Resource` distinct from `Mesh`: interleaved
`SkinnedVertex3D` (position, normal, uv, four joint indices, four weights) plus
its skeleton. `skin_vertices` / `skin_vertex` apply a palette on the CPU by linear
blend skinning — exactly what the vertex shader will do — so a pose and a palette
can be asserted on with no device. Weights are normalised by their sum, a vertex
with no usable influence stays at its bind position rather than collapsing to the
origin, and a joint index past the end of the palette is skipped. `skinned_bounds`
expands the AABB over the *posed* positions, because a raised arm reaches outside
the bind-pose box and a cull that used the bind box would drop the character.

`anim::SkinnedMeshRenderer3D` is a `Component` that holds a `MeshRenderer3D` and
delegates `resolve_surface()` to it, so the material precedence rule has one
definition and the skinned shader cannot drift from the static one. A skinned
renderer with no animator draws its bind pose — its static shape — rather than
vanishing.

### 4. The palette reaches the shader as a uniform array

`rhi::Shader` gains `set_vec4_array(name, const Vec4*, count)`, implemented in the
OpenGL backend (`name`/`name[0]` uniform resolution, `glUniform4fv`) and the null
device. The palette is uploaded as `mat4 u_palette[N]` and indexed by the vertex's
joint indices in the shader.

A uniform array rather than per-instance attributes, which is the decision that
matters most here. A vertex attribute stream is sized by the mesh's vertex count,
but a palette is sized by the rig's *joint* count and is the same for every vertex
of one instance. A forty-joint rig would need forty attribute slots, past every
backend's limit, and would multiply the instance record by the joint count. A
uniform array is bounded by the uniform budget, is uploaded once per instance, and
keeps `SkinnedVertex3D` to eight attributes.

Extraction gains `render::SkinnedInstance` (the world matrix, the normal matrix,
and a pointer to the animator's palette), `extract_skinned_meshes`, and
`cull_skinned_meshes`. A skinned drawable is a separate record and a separate cull
from a static one, so the static path is byte-identical.

## Consequences

- A character animates with no GPU in the loop for anything but the final draw:
  the rig, the clip, the pose, the palette, and the CPU skin are all values a test
  asserts on. The new tests cover the bind-pose identity, parent-first ordering,
  key interpolation, channel-preserving sampling, cross-fade, root motion, weight
  normalisation, posed bounds, extraction, and reflection.
- The static 3D path is untouched: `Vertex3D`, `Mesh`, `MeshRenderer3D`, and the
  static vertex shader are unchanged, and the skinned path is additive.
- The palette is bounded by the uniform budget, not the attribute limit, so a rig
  of any practical joint count fits. A future very-large rig would chunk the
  palette into several draws; that is a shader change, not a data-model one.
- Root motion is a per-animator flag rather than a global, so one character can be
  animation-driven and another controller-driven in the same scene.
- **The asset and draw halves are not in this change.** `GltfLoader` does not yet
  read `skins`, `JOINTS_0`/`WEIGHTS_0`, or `animations`, and `Renderer3D` does not
  yet bind the skinned vertex layout or the palette uniform. The runtime is
  complete and tested; the importer and the skinned draw call are the remaining
  work, recorded in the roadmap and the shipping queue.

## Alternatives considered

- **Fold the skeleton into the mesh.** Rejected: a mesh is shared by instances and
  a rig drives several meshes, so welding them forbids both, and it would put
  animation state inside a resource the manager treats as immutable. glTF models
  `skins` as a top-level array beside `meshes` for the same reason.
- **One track per joint holding a full transform, all three channels always.**
  Rejected: a look-at rig animates only rotation and a bounce only translation;
  forcing all three would overwrite an untouched channel with a default and make
  every clip author all three.
- **Euler angles in the pose.** Rejected: interpolating Euler angles takes a path
  the author did not intend and sweeps the long way across a ±π wrap. The pose
  stores a quaternion; `Transform` keeps its Euler API for authoring and the
  conversion happens at the boundary, as the physics solver already does.
- **Per-instance attributes for the palette.** Rejected: a forty-joint rig exceeds
  the attribute limit and multiplies the instance record by the joint count. A
  uniform array is bounded by the uniform budget and uploaded once per instance.
- **Compute the palette in the renderer.** Rejected: a game asks "where is the hand
  joint" for a hitbox, an editor draws the rig, and a tool bakes a pose — none of
  which is a draw. One computation in the frame step means all of them read one
  answer.
- **A `skinned` flag on `Mesh` and `MeshRenderer3D`.** Rejected: it makes every
  consumer of `Mesh` ask which kind it holds and puts a branch on the static path
  that it never takes. Two types keep the static path exactly as it was.
