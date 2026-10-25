# 0080 — Required references and candidate references

Date: 2026-10-25

Status: Accepted

## Context

ADR-0065 makes a pruned pack ship only the assets the startup scene names. The
tracer, `serialization::scene::collect_resource_references`, finds two kinds of
reference:

1. A component property whose reflected kind is `ResourceRef`. The reflection
   registry says the field *is* a resource, so the reference is a certainty.
2. Every string inside a script's `"properties"` object. A behaviour's property
   kinds are unknown until its module is loaded, which a packer does not do, so a
   script string is only a *candidate*. ADR-0065 is explicit that a false
   candidate "costs nothing" and is resolved away by the caller.

The collector returned both kinds in one flat `std::vector<std::string>`, and
both callers — `fork_pack` and `fork_export` — treated every entry as required:
an entry that resolved to neither an indexed asset nor a file under the pack root
was a hard error.

That is wrong for the second kind, and it broke the moment a scene carried a
behaviour with any string property. `FollowCamera3D` records its followed node's
name (`target = "Player"`) in its script properties, so the tracer read `"Player"`
as a reference, the caller could not resolve it to a file, and the export failed:

```
fork_export: scene reference 'Player' is not an asset under 'examples/game3d_project'
```

A scene whose every asset was present refused to pack because a *label* was not a
file. The bug is not in the tracer, which does what ADR-0065 says; it is that the
caller had no way to tell a required reference from a candidate, because the
collector had thrown the distinction away.

## Decision

Keep the two kinds apart in the type, and apply the policy in one place.

`collect_resource_references` returns a `SceneReferences { std::vector<std::string>
required; std::vector<std::string> candidates; }`. A reflected `ResourceRef`
property is `required`; a script property string is a `candidate`.

A new `serialization::scene::resolve_scene_asset_paths(references, pack_root,
assets)` resolves both lists against the asset database and the pack root and
returns the paths for `PackOptions::include`:

* Every `required` reference must resolve, or the call fails with the same message
  the tools used to print. A scene that names an asset it does not carry is still
  a hard error, because a build that draws nothing is worse than a failed pack.
* A `candidate` is kept only when it resolves; the rest are discarded. This is
  what makes tracing script strings safe, and it is what ADR-0065 already said
  the caller would do.

Both tools call the one resolver, so the policy cannot drift between them again —
it already had, in the sense that the same wrong assumption lived in two copies.

## Consequences

* A scene that binds a behaviour with a string property — a follow target, a door
  that names a key, a spawner that names a prefab — packs and exports normally.
  Before this, any such scene failed to export, which is a large class of games.
* The required/candidate distinction is now in the type, so a future caller
  cannot accidentally treat a label as an asset: it has to choose a list.
* The shared resolver removes the duplicated policy from `fork_pack` and
  `fork_export`; a change to it is made once.
* Duplicate references (many nodes naming the same texture) are collapsed, which
  the previous per-tool loops did not do. This only shrinks the include list; the
  writer's own set-difference was already idempotent.

## Alternatives considered

* **Drop script strings from the tracer.** Rejected: ADR-0065's rule exists so a
  script that references an asset through a property whose module is not loaded
  is still packed. Removing it would silently drop such an asset, which is the
  failure mode ADR-0065 set out to prevent.
* **Resolve candidates by file extension** (treat only `*.png`, `*.obj` as
  resources). Rejected: it hard-codes a format list in the tracer, which is the
  per-component knowledge ADR-0065 deliberately avoids, and it would miss a
  custom scheme.
* **Warn instead of error on an unresolved required reference.** Rejected: it
  ships a build that breaks when it draws, which ADR-0065 already rejected.
* **Have each tool keep its own loop and just skip candidates.** Rejected: it
  leaves the policy duplicated, which is how the two callers drifted in the first
  place.
