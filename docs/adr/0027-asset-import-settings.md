# ADR-0027: Per-asset import settings stored in the sidecar

- **Status:** Accepted
- **Date:** 2026-10
- **Related:** [ADR-0004](0004-resource-ownership-weak-cache.md),
  [ADR-0005](0005-reflection-and-serialization.md),
  [ADR-0006](0006-component-reflection.md),
  [ADR-0020](0020-resource-identity-guid-sidecar.md),
  [ADR-0025](0025-project-file.md)

## Context

ADR-0020 gave every asset a stable identity in a per-asset sidecar
(`hero.png.meta`) and made the sidecar's `scheme` the importer selector: several
loaders can share a file extension (a `.png` is both a CPU `Image` and a GPU
`Texture2D`), so the scheme says which importer owns the file. The sidecar
carries the GUID and the scheme, and nothing else.

That is enough to *find* and *load* an asset, but not enough to *configure* it.
How a texture is sampled (nearest or linear, repeat or clamp, mipmaps or not) is
a property of the asset, not of every `Sprite2D` that references it. Today those
choices are hard-coded in `Texture2DLoader`: a texture is always linear-filtered,
clamped, and uploaded without mipmaps. A pixel-art project cannot ask for nearest
sampling; a tiling background cannot ask for repeat; neither can be expressed,
so neither can be honoured.

The same gap is coming for every other asset type. A mesh will want a scale and
an axis convention, a sprite sheet will want a grid, an audio clip will want a
loop point. Without a place to put these, each loader grows its own ad-hoc
configuration, or the settings leak into the components that use the asset,
which is exactly the duplication ADR-0020 was written to avoid.

The question is where per-asset settings live, how they are typed, and how they
reach a loader that today takes only a path.

## Decision

Add **per-asset import settings** to the resource system, stored in the asset's
existing sidecar and applied when the asset loads. The first kind is the texture
settings the pipeline already hard-codes; the mechanism is general so later
kinds add fields, not a second system.

### The sidecar's `scheme` stays the selector; `settings` is its payload

The sidecar already has a top-level `scheme`. Import settings are a sibling
`settings` object whose `kind` matches the scheme's importer. Keeping them
separate preserves ADR-0020's meaning of `scheme` (which loader owns the file)
and keeps a sidecar readable when the two differ, e.g. while a project is being
migrated between importers. A reader that finds no `settings` object takes the
defaults, so **every sidecar written before this ADR still loads unchanged**.

### Settings are a tagged value type, not a class hierarchy

`ImportSettings` is a single struct with a `Kind` discriminator
(`Generic`, `Texture`) and the fields each kind needs:

| Field | Meaning |
| --- | --- |
| `kind` | which importer's settings these are |
| `filter` | texture min/mag sampling (`Nearest` / `Linear`) |
| `wrap` | texture addressing (`Repeat` / `ClampToEdge`) |
| `mipmaps` | generate a mip chain on upload |

A tagged value rather than a `TextureSettings`/`MeshSettings` hierarchy for three
reasons. The number of kinds is small and each has few fields, so a hierarchy
buys nothing but virtual dispatch and allocation. The sidecar is JSON, where a
tag is the natural representation. And the editor draws settings by kind, so a
flat value with a `kind` is exactly the shape the inspector wants. Adding a kind
is a new enumerator and its fields; it does not change how any other kind is
stored or drawn.

`ImportSettings` is a pure value: it serializes itself and describes itself, but
it never touches the filesystem or a graphics device. **Applying** a setting is
the loader's job, because only the loader knows what the setting means for its
resource.

### The manager scopes settings for the loader call

`ResourceLoader::load(path)` takes a path and nothing else. Threading a settings
parameter through it would touch every loader, including the three that have no
settings today. Instead the manager scopes the settings for the duration of the
load:

```
load(reference)
  -> resolve the asset (path or guid)
  -> load_file(scheme, path, &asset->settings)
       -> current_settings_ = settings
          loader->load(path)          // loader may call current_import_settings()
       -> current_settings_ = previous
```

A loader that cares asks the manager for `current_import_settings()`; a loader
that does not is untouched. `Texture2DLoader` already holds a
`ResourceManager&` (it loads the CPU `Image` through it), so it gains the ability
to honour settings with no constructor change and no new dependency.

Two details make this correct rather than merely convenient:

- **The texture loader captures the settings before decoding.** Decoding loads
  the CPU `Image` through the manager under the default scheme, which re-enters
  `load_file` and temporarily overwrites `current_settings_` with the image's
  (generic) settings. Reading the settings *before* that call, as the loader
  does, is what stops the texture from picking up the image's defaults.
- **The scope is restored on every exit.** `load_file` saves and restores the
  previous pointer around the loader call, so a failed load or a nested load
  leaves no settings in scope. The engine is single-threaded per ADR-0004, so one
  pointer on the manager is sufficient; a threading model change would make this
  per-thread.

### A path load honours settings too

A `"guid:"` reference and a `"tex:assets/hero.png"` path can name the same asset.
Both now resolve the asset's settings: the GUID branch from the resolved asset,
the path branch from `AssetDatabase::find_by_path`. A path reference to an
unindexed file (a quick test asset, an external file) simply has no settings and
loads with defaults, exactly as before.

### Defaults preserve the old behaviour exactly

`ImportSettings::texture_defaults()` is linear / clamp / no mipmaps, which is
what the loader did before this ADR. A texture imported with no explicit settings
therefore behaves identically to before. The `is_default()` predicate reports
whether settings equal their kind's defaults, which the writer uses to keep a
sidecar for a settings-less asset byte-identical to one written before this ADR.

The one deliberate exception: an asset whose `kind` is not `Generic` always
records its `kind`, even when every value is a default. The kind is real
information -- it is what the editor uses to draw the right settings -- and it
must survive a reload. A `Generic` asset omits the block entirely, so a sidecar
for an asset with no typed settings is unchanged.

### Malformed settings are an error, not a silent default

A settings block that does not parse, names an unknown kind, or sets an unknown
filter or wrap is rejected by `read_asset_meta`, and the scan drops that asset
rather than indexing it with wrong settings. A sidecar written by a *newer* engine
therefore fails clearly instead of silently importing an asset differently than
its author intended. An *older* sidecar, with no block, is not an error: absence
means "defaults".

### The editor reads the kind, and never writes settings itself

`AssetBrowserViewModel::Entry` gains `settings_kind`, populated from the database,
so the browser can show what an asset will be imported as (the tooltip now reads
`scheme: tex` / `import: texture`). The editor is a client of the resource
system: it displays the kind. A settings *editor* -- a panel that writes the
block back through `AssetDatabase::import(path, scheme, settings)` -- is a
follow-up, and the round-trip it needs is already tested.

## Consequences

**Easier:**

- A texture can be sampled as its project needs: nearest for pixel art, repeat
  for a tiling background, mipmaps for a distant surface. The setting lives with
  the asset and is honoured by every reference to it.
- The mechanism is general. A mesh, sprite-sheet, or audio importer adds a kind
  and its fields; it does not add a second configuration path or a new file.
- `AssetDatabase::import(path, scheme, settings)` is the single write point, so
  re-importing with new settings keeps the GUID (ADR-0020) and rewrites the
  sidecar. Changing how an asset is imported never breaks references to it.
- Settings are exhaustively unit-testable: the value type round-trips, the
  database persists them, the manager scopes them, and the graphics test proves
  the filter actually changes a read-back pixel.

**Harder / accepted costs:**

- **A side channel between the manager and its loaders.** `current_import_settings()`
  is state on the manager read during a load. It is narrower than a settings
  parameter on every loader and needs no loader to change, but it is implicit: a
  loader must know to ask. The alternative -- a parameter -- is explicit but
  touches every loader for the benefit of one. The manager-scoped pointer is the
  smaller change, and it is documented where it is set and read.
- **A tagged union grows with each kind.** The struct carries the fields of every
  kind, not only the active one. With a handful of small kinds that is a few
  bytes of unused storage, not a design problem; a variant would trade the
  flatness the editor wants for the savings.
- **Unknown values are fatal to the asset.** A typo in a hand-edited sidecar
  drops the asset from the index rather than falling back to defaults. This is
  the intended strictness -- a wrong filter is worse than a missing asset -- but
  it means the error must be legible, which it is (it names the value and the
  file).
- **No settings UI yet.** The kind is shown; the values are not editable in the
  editor. Authoring a nearest-filtered texture today means editing the sidecar or
  calling `import` with settings from a tool. The data model and round-trip are
  ready for the panel.

## Alternatives considered

- **Settings in the asset file's bytes.** Rejected in ADR-0020 already: it
  rewrites the asset, breaks content hashing, and cannot describe an asset whose
  bytes are produced by a tool.
- **A parallel settings manifest for the whole project.** Rejected: it
  re-introduces the merge-conflict and stale-reference problems ADR-0020 removed
  by putting identity beside the asset. Settings belong beside the asset for the
  same reason the GUID does.
- **A `std::map<std::string, std::string>` of free-form settings.** Rejected: it
  is flexible but unsafe. Nothing validates a value, a typo is a silent wrong
  filter, and the editor has no type to draw a widget from. Typed fields with a
  kind discriminator make a malformed value a clear error and give the inspector
  something concrete.
- **A per-scheme C++ settings class hierarchy.** Rejected: virtual dispatch and
  allocation for a handful of small value types, and a shape the JSON sidecar and
  the inspector both have to unpick. The tagged value is flatter and matches both.
- **A settings parameter on `ResourceLoader::load`.** Rejected as the primary
  mechanism: it changes the contract and every loader (four today, more later) to
  serve the ones that have settings. The manager-scoped pointer keeps the
  contract and the loaders that do not care unchanged. A parameter remains the
  fallback if a loader ever needs settings that are not on the manager's current
  asset.
- **Applying settings in the editor only.** Rejected: play-in-editor and headless
  runs must import the same way the editor shows, or a game's appearance depends
  on how it was launched. Import is a runtime concern; the editor only displays
  its result.
- **Deriving the kind from the scheme on read instead of storing it.** Rejected:
  it would make the sidecar's meaning depend on a table in the engine, so a
  scheme rename would silently reinterpret every asset. The kind is stored, which
  is what makes an older sidecar load unchanged and a newer one fail loudly.
