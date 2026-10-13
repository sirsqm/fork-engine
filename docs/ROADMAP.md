# Fork Engine Roadmap

The engine is built incrementally. Each milestone is independently testable and
must build and pass its tests before the next one starts. This document is a
plan, not a promise; milestones may be reordered as the design becomes clearer,
and changes are recorded in the ADRs.

The guiding rule: a few genuinely working systems beat a long list of
half-finished features.

## M1 — Foundation (done)

The smallest useful engine: something that builds, runs, renders, and is tested.

- [x] Build system, repository structure, CI
- [x] Application entry point and platform abstraction (window, input, context)
- [x] Logging, assertions, and error handling
- [x] Memory and resource ownership conventions
- [x] RHI abstraction with an OpenGL 3.3 core backend
- [x] Minimal 2D renderer: batched, textured sprites with a camera
- [x] Scene representation with JSON serialization
- [x] A runnable example (the sandbox)
- [x] Unit tests and graphics tests

## M2 — Editor foundation

The editor becomes real before the engine grows much further, so the runtime is
built editor-friendly from the start rather than adapted later.

- [x] A windowing/UI layer for tools (docking, panels) — ADR-0007, `fork_editor`
- [x] Scene hierarchy panel — `HierarchyViewModel` + `HierarchyPanel`
- [x] Property inspector, driven by the reflection system — `PropertyEditor` + `InspectorPanel`
- [ ] 2D viewport rendering into an editor panel
- [ ] Asset browser
- [ ] Undo/redo
- [x] Play-in-editor (embed an `Engine` and drive frames) — `PlaySession`
- [ ] Project settings and a build/export path

### M2 prerequisite — reflection (done)

The inspector is driven by reflection, so the reflection registry landed before
the editor panels. See [ADR-0006](adr/0006-component-reflection.md).

- [x] A reflection registry: types, properties, metadata, editor hints
- [x] Serialization driven by reflection instead of per-component code

### M2 prerequisite — systems and the frame schedule (done)

The hybrid execution model ([ADR-0002](adr/0002-hybrid-scene-ecs-architecture.md))
needed a concrete mechanism before gameplay could grow. See
[ADR-0008](adr/0008-systems-and-frame-schedule.md).

- [x] `System`, `SceneSchedule`, and `FrameContext` (ordered, single-threaded, no job graph)
- [x] The runtime drives the schedule once per frame, after `on_update` and before rendering
- [x] First concrete system: sprite-sheet animation (`SpriteAnimator` + `SpriteAnimationSystem`), reflected so animators serialize and are inspector-editable

## M3 — Reflection and a real asset pipeline

- [ ] Property inspector integration with editor hints (Range, Color, Resource)
- [ ] GUID-based resource references that survive file moves
- [ ] Import settings per asset type, and an import cache
- [ ] Hot reload of resources

## M4 — 3D rendering

- [x] Meshes, materials, cameras, and a 3D renderer — the first lit pass; see
      [ADR-0010](adr/0010-3d-rendering-foundation.md)
- [x] A directional light
- [ ] Directional, point, and spot lights
- [ ] Shadow mapping
- [ ] Spatial hierarchy and frustum culling
- [ ] Environment rendering and a post-processing chain
- [ ] A glTF importer

## M5 — Physics

- [x] 2D physics: box and circle colliders, dynamic and static bodies, a
      sequential-impulse solver (gravity, restitution, friction), as a system
      (`Collider2D`, `RigidBody2D`, `Physics2DSystem`) — see
      [ADR-0009](adr/0009-physics2d-system.md)
- [ ] 2D physics refinements: angular dynamics, collider offsets, joints,
      continuous collision detection, sleeping, a spatial partition
- [ ] Physics queries (raycast, overlap) and contact events
- [ ] 3D physics
- [ ] Integration with the scene and the editor
- [ ] Debug drawing

## M6 — Animation and audio

- [x] 2D sprite-sheet animation (frame playback; the first system) — see M2 prerequisite
- [ ] Tilemaps
- [ ] Skeletal animation and skinning
- [ ] Animation state machines
- [ ] Audio playback, mixing, and 3D spatialisation

## M7 — Scripting

- [ ] Choose and document the language and embedding strategy (ADR first)
- [ ] Reflection-backed bindings
- [ ] Editor integration (attach, inspect, hot reload)
- [ ] Debugging support

## M8 — Cross-platform and shipping

- [ ] macOS and Windows backends brought to parity
- [ ] Packaging and export templates
- [ ] Additional graphics backend (for example Vulkan)
- [ ] Profiling and performance tooling

## Ongoing

- Keep the build reproducible and warning-clean.
- Keep the tests green; every fix gets a test that would have caught it.
- Keep the architecture documentation and ADRs current.
- Review dependency licenses for compatibility with MIT.
