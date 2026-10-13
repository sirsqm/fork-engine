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

## Format

Each record has the following sections:

- **Status** — Proposed, Accepted, Superseded by NNNN, or Deprecated.
- **Context** — the forces at play, without a decision.
- **Decision** — what was decided.
- **Consequences** — what becomes easier and harder as a result.
- **Alternatives considered** — the options that were rejected and why.

To propose a new ADR, copy the structure of an existing record, give it the next
number, and set its status to Proposed.
