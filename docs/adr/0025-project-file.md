# ADR-0025: The project file - settings as a JSON document the editor opens

- **Status:** Accepted
- **Date:** 2026-10
- **Related:** [ADR-0005](0005-reflection-and-serialization.md), [ADR-0007](0007-editor-ui-toolkit.md), [ADR-0018](0018-multi-platform-shipping.md), [ADR-0020](0020-resource-identity-guid-sidecar.md)

## Context

The editor grew up without a project. It opened a scene file, or built an
example scene, and had no notion of "the project this scene belongs to". The
asset browser work exposed the cost of that: the browser reads the engine's
`AssetDatabase`, but nothing ever told the database where the assets live, so it
was always empty. The fix at the time was a single field,
`EditorContext::set_project_root`, set from a `--project DIR` flag.

One field is not a project. The editor now needs a thing the user *opens* and
*saves* that carries more than one setting, and that has somewhere to put the
settings still to come (default scene, renderer choice, per-asset import
defaults). The questions are what a project is on disk, where its settings live,
how paths in it survive being moved between machines, and how far the engine
should know about any of it.

There is a strong precedent in the engine already: scenes are UTF-8 JSON, chosen
for being diffable and inspectable while the format is still moving, versioned
so a future reader does not silently misread an old document, and identified by
a `format` marker ([ADR-0005](0005-reflection-and-serialization.md)). A project
file has the same needs, so it should not invent a different answer.

## Decision

**A project is a directory containing a `fork.project.json` file. The file is
what the editor opens and saves; it holds the project's settings as versioned
UTF-8 JSON. The engine stays unaware of projects - the editor reads the settings
and applies them through the engine's existing public APIs.**

### The project file

```json
{
  "format": "fork_project",
  "format_version": 1,
  "name": "Demo",
  "asset_root": "assets",
  "startup_scene": "scenes/main.json"
}
```

- **`format` / `format_version`** are the same guard scenes use: the marker
  catches a JSON file that is not a project, the version catches one from the
  future. A reader rejects both rather than misinterpreting them.
- **`asset_root`** is the directory the asset database indexes and that GUID
  references resolve against. It defaults to `assets`; an empty value means
  "this project has no asset root", which leaves the browser empty.
- **`startup_scene`** is the scene the editor opens at launch. Empty means the
  built-in example scene, the pre-project behaviour.

The set is deliberately small. It is the settings that change behaviour today;
the schema is versioned and additive, so `renderer`, per-asset import defaults,
and the rest of M3/M4 land here as fields rather than as a second mechanism.

### Paths are stored relative to the project

`asset_root` and `startup_scene` are written **relative to the project
directory** when they point inside it, and absolute when they do not (a shared
asset tree elsewhere on the machine). The resolved path is computed on load by
joining the stored value onto the project directory. This is what makes a
project portable: a checked-out project refers to its own files, not to
`/home/alice/...`. It is the same instinct as [ADR-0020](0020-resource-identity-guid-sidecar.md)
- an identity that travels - applied one level up.

### Where the code lives, and what the engine knows

`ProjectSettings` is a headless value type in `fork_editor_core`: it parses,
writes, and resolves paths, and owns no window, so it is tested without a
display. The engine does **not** gain a project type. `EditorContext::load_project`
reads the settings and then calls the engine's existing public API -
`ResourceManager::assets().set_root` - to point the database at the resolved
asset root. The host does this **before the first scene load**, so GUID
references inside the startup scene resolve during deserialization rather than
after it.

This keeps the engine reusable and project-agnostic: a tool or a test can use
`ResourceManager` with an asset root it sets itself, and nothing in the engine
has to know what a project is. The editor is the owner of "the project" because
the editor is the thing that opens one.

### Build and export are not here

"Project settings and a build/export path" was one milestone line. They are two
different sizes of work: settings are a small, editor-local format, while
assembling a runnable build from a project is the packaging and export-template
machinery already planned for [ADR-0018](0018-multi-platform-shipping.md)
(M8). The line is split: settings are done here; the build/export path is M8's
`fork export` work, where the runtime, pack format, and per-platform templates
live.

## Consequences

**Easier:**

- The editor has a thing to open and save. `fork_editor_shell --project DIR`
  (or a file, or `--project-file`) opens it; `EditorContext::save_project`
  writes it.
- Future settings have a home: a new field is one key in the document and one
  accessor on `ProjectSettings`, not a new file or a new mechanism.
- A project is portable, because its paths are relative.
- The engine stays free of project concepts, so it remains reusable by tools and
  tests.

**Harder:**

- A second versioned format to maintain and migrate. It is small and shares the
  scene format's shape, so the cost is low, but it is real.
- `--project` has to decide whether its argument is a directory or a file. The
  rule is narrow (a `.json` argument is a file; anything else is a directory
  where the default file name is looked for), but it is a heuristic, and a
  project file with an unusual name must be given to `--project-file`.
- The project directory is currently the directory the file lives in; a project
  that wants to keep its file elsewhere is not expressible yet. No use case has
  needed it.

## Alternatives considered

- **No project file; keep CLI flags and environment variables.** Rejected: there
  is nothing to open, save, or version-control as the project, and every setting
  becomes a flag a user must remember. Flags are a fine override for a run, not a
  home for a project.
- **Store settings in the scene file.** Rejected: settings are properties of the
  project, not of one scene, and a project has many scenes. Duplicating the
  asset root into every scene guarantees they drift.
- **Put the project concept in the engine** (an `fe::Project` in `runtime`).
  Rejected for now: the engine's resource system is deliberately reusable and
  project-agnostic, and the editor is the component that opens a project. If a
  shipped game needs project settings at runtime (packaging, default scene), M8's
  export work can promote a subset of this into the engine then, with a concrete
  consumer to design against rather than a guess.
- **A binary or CMake-based project file.** Rejected: JSON matches the scene
  format, diffs and merges cleanly, and needs no new tooling. A CMake project
  file would couple the data format to the build system, which is the wrong
  axis - the build system may change; the settings should not.
- **Absolute paths in the file.** Rejected: not portable, and a project moved
  between machines (or checked out fresh) would break. Relative paths with an
  absolute escape hatch are strictly more useful.
- **A central manifest or global user config for "the open project".** Rejected:
  it puts machine state in a shared place and makes the project's settings
  invisible to the project's own version control. The settings belong in the
  project directory.
