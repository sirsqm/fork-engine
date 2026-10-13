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
| [0011](0011-editor-undo-redo.md) | Editor undo/redo: a command stack with coalescing | Accepted |

## Format

Each record has the following sections:

- **Status** — Proposed, Accepted, Superseded by NNNN, or Deprecated.
- **Context** — the forces at play, without a decision.
- **Decision** — what was decided.
- **Consequences** — what becomes easier and harder as a result.
- **Alternatives considered** — the options that were rejected and why.

To propose a new ADR, copy the structure of an existing record, give it the next
number, and set its status to Proposed.
