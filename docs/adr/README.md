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
| [0019](0019-collaborative-editing.md) | Collaborative editing: operations, a replicated log, an authority | Proposed |
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

## Format

Each record has the following sections:

- **Status** — Proposed, Accepted, Superseded by NNNN, or Deprecated.
- **Context** — the forces at play, without a decision.
- **Decision** — what was decided.
- **Consequences** — what becomes easier and harder as a result.
- **Alternatives considered** — the options that were rejected and why.

To propose a new ADR, copy the structure of an existing record, give it the next
number, and set its status to Proposed.
