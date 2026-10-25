# Architecture Decision Records

An Architecture Decision Record (ADR) captures a decision that has real
trade-offs, the context in which it was made, and the consequences. ADRs are
immutable once accepted: if a decision changes, a new ADR supersedes the old one
rather than editing it.

## Index

| ID | Title | Status |
| --- | --- | --- |
| [0001](0001-engine-architecture-overview.md) | Engine architecture overview | Accepted |
| [0002](0002-hybrid-scene-ecs-architecture.md) | Hybrid scene-graph / entity-component architecture | Accepted |
| [0003](0003-error-handling.md) | Errors as values, not exceptions | Accepted |
| [0004](0004-resource-ownership-weak-cache.md) | Resource ownership with a weak cache | Accepted |
| [0005](0005-reflection-and-serialization.md) | Reflection and serialization strategy | Accepted |
| [0006](0006-component-reflection.md) | Component reflection for serialization and the editor | Accepted |
| [0007](0007-editor-ui-toolkit.md) | Editor UI toolkit: Dear ImGui with a docking host | Accepted |
| [0008](0008-systems-and-frame-schedule.md) | Systems and the frame schedule | Accepted |
| [0009](0009-physics2d-system.md) | 2D physics as the first gameplay system | Accepted |
| [0010](0010-3d-rendering-foundation.md) | A 3D rendering foundation on the shared RHI | Accepted |
| [0011](0011-frustum-culling.md) | Conservative frustum culling in the 3D renderer | Accepted |
| [0012](0012-forward-lighting-model.md) | A forward lighting model with a bounded light budget | Accepted |
| [0013](0013-physics2d-queries-events-angular.md) | Physics queries, contact events, and angular dynamics | Accepted |
| [0014](0014-directional-shadow-mapping.md) | Directional shadow mapping | Accepted |
| [0015](0015-editor-undo-redo.md) | Editor undo/redo: a command stack with coalescing | Accepted |
| [0016](0016-audio-module.md) | Audio as a module with a device seam and a pure mixer | Accepted |
| [0017](0017-performance-strategy.md) | Performance strategy: measurement first, a data-oriented seam | Proposed |
| [0018](0018-multi-platform-shipping.md) | Multi-platform shipping: one runtime, many targets | Proposed |
| [0019](0019-collaborative-editing.md) | Collaborative editing: operations, a replicated log, an authority | Deprecated |
| [0020](0020-resource-identity-guid-sidecar.md) | Stable resource identity via a per-asset GUID sidecar | Accepted |
| [0021](0021-physics2d-joints.md) | 2D joints (distance and revolute constraints) | Accepted |
| [0022](0022-profiler-and-frame-timing.md) | A frame profiler in `core` | Proposed |
| [0023](0023-physics2d-broadphase.md) | A uniform-grid broad phase for 2D physics | Proposed |
| [0024](0024-profiler-instrumentation.md) | Profiler instrumentation, runtime integration, and the frame report | Proposed |
| [0025](0025-project-file.md) | The project file: settings as a JSON document the editor opens | Accepted |
| [0026](0026-physics2d-islands-sleeping.md) | 2D island detection and body sleeping | Accepted |
| [0027](0027-asset-import-settings.md) | Per-asset import settings stored in the sidecar | Accepted |
| [0028](0028-editor-hint-metadata.md) | Editor hint metadata: typed hints on a reflected property | Accepted |
| [0029](0029-import-cache.md) | A content-addressed import cache | Accepted |
| [0030](0030-asset-pack-format.md) | Asset packs: one container the runtime mounts like a directory | Accepted |
| [0031](0031-physics3d-and-backend-seam.md) | 3D physics, and a backend seam so the solver can be replaced | Accepted |
| [0032](0032-near-term-platform-scope.md) | Near-term platform scope: desktop is the target, shipping is the plan | Accepted |
| [0033](0033-performance-regression-gate.md) | A performance regression gate | Accepted |
| [0034](0034-audio-attenuation-buses.md) | Audio attenuation and mixing buses | Accepted |
| [0035](0035-ci-quality-gates.md) | CI quality gates: sanitizers and an optimised warnings build | Accepted |
| [0036](0036-breakout-vertical-slice.md) | The breakout vertical slice, and the integration test it is | Accepted |
| [0037](0037-physics2d-oriented-boxes-offsets.md) | Oriented-box colliders and collider offsets | Accepted |
| [0040](0040-gltf-importer.md) | A glTF 2.0 importer for 3D content | Accepted |
| [0041](0041-platform-contract-and-null-backend.md) | The platform contract and a null/headless backend | Accepted |
| [0042](0042-null-rhi-device.md) | A null RHI device for headless rendering | Accepted |
| [0043](0043-physics3d-angular-dynamics.md) | 3D angular dynamics, and a quaternion orientation | Accepted |
| [0044](0044-scripting-seam.md) | The scripting seam, and C++ as the first language behind it | Accepted |
| [0045](0045-component-identity.md) | Stable component identity | Accepted |
| [0046](0046-resource-change-detection-and-reload.md) | Resource change detection and reload | Accepted |
| [0047](0047-local-operation-model.md) | The local operation model | Accepted |
| [0048](0048-replicated-operation-log.md) | The replicated operation log, local only | Accepted |
| [0049](0049-net-transport-and-session-server.md) | The transport and the authoritative session server | Accepted |
| [0051](0051-frame-arena.md) | A per-frame arena in `core` | Accepted |
| [0052](0052-render-extraction-seam.md) | The render extraction seam | Accepted |
| [0053](0053-spatial-bvh.md) | A spatial BVH over static geometry in `core` | Accepted |
| [0054](0054-frame-loop-arena.md) | The frame arena in the frame loop | Accepted |
| [0055](0055-editor-profiler-panel.md) | An in-editor profiler panel fed by the core profiler | Accepted |
| [0056](0056-renderer3d-spatial-cull.md) | A spatial-hierarchy cull in the 3D renderer | Accepted |
| [0057](0057-environment-and-post-processing.md) | Environment rendering and a post-processing chain | Accepted |
| [0058](0058-editor-physics3d-and-debug-draw.md) | 3D physics in the editor, and debug drawing | Accepted |
| [0059](0059-animation-state-machines.md) | Animation state machines | Accepted |
| [0060](0060-compressed-audio-and-streaming.md) | Compressed audio codecs and streaming playback | Accepted |
| [0061](0061-cpp-gameplay-modules-hot-reload.md) | C++ gameplay modules and hot reload | Accepted |
| [0062](0062-tilemaps.md) | Tilemaps | Accepted |
| [0063](0063-reflection-backed-script-properties.md) | Reflection-backed script properties and script persistence | Accepted |
| [0064](0064-export-runtime-and-player.md) | Export: a runtime that plays a pack | Accepted |
| [0065](0065-pack-reachability-pruning.md) | Pack reachability pruning | Accepted |
| [0066](0066-editor-driven-module-hot-reload.md) | Editor-driven gameplay-module hot reload | Accepted |
| [0067](0067-debouncing-a-module-build.md) | Debouncing a gameplay-module build | Accepted |
| [0068](0068-job-system.md) | A job system: thread pool, `parallel_for`, and a task graph | Accepted |
| [0069](0069-parallel-extraction-and-culling.md) | Parallel extraction and culling | Accepted |
| [0070](0070-instancing-arena-seam-and-parallel-broadphase.md) | Instancing, an arena-backed extraction seam, and a parallel broad phase | Accepted |
| [0071](0071-web-first-2d-shipping-path.md) | Shipping a 2D game to every platform — the web-first critical path | Accepted |
| [0072](0072-glsl-dialect-seam.md) | A GLSL dialect translation seam in the RHI | Accepted |
| [0073](0073-emscripten-toolchain.md) | The Emscripten toolchain and the web platform macro | Accepted |
| [0074](0074-web-window-backend.md) | The web window backend (canvas + WebGL2) | Accepted |
| [0075](0075-gameplay-actions-input-seam.md) | Gameplay actions as the input seam | Accepted |
| [0076](0076-3d-in-the-browser.md) | 3D in the browser | Accepted |
| [0077](0077-web-canvas-resize-and-dpr.md) | The web canvas size and the input coordinate space | Accepted |
| [0078](0078-engine-shipped-gameplay-behaviours.md) | Engine-shipped gameplay behaviours | Accepted |
| [0079](0079-fixed-simulation-step.md) | A fixed simulation step | Accepted |
| [0080](0080-required-and-candidate-references.md) | Required references and candidate references | Accepted |

## Format

Each record has the following sections:

- **Status** — Proposed, Accepted, Superseded by NNNN, or Deprecated.
- **Context** — the forces at play, without a decision.
- **Decision** — what was decided.
- **Consequences** — what becomes easier and harder as a result.
- **Alternatives considered** — the options that were rejected and why.

To propose a new ADR, copy the structure of an existing record, give it the next
number, and set its status to Proposed.
