# 0108 — The AppImage ships the editor, and defaults to it

## Status

Accepted.

## Context

[ADR-0103](0103-linux-appimage.md) made the Linux download one file: an AppImage
carrying the breakout game, its content, and the C++ runtime. It answered "a
player downloads one file, runs it, and plays the game."

The editor is the other half of the product, and it was not in that file. The
editor shell installs into the SDK archive's `bin/`, so a developer who downloads
the SDK and has a display can run `fork_editor_shell` — but the README describes
the SDK as "the installed engine for building against," and the single-file
download, the AppImage, contained only the game. The editor was also the
subsystem just given a real typeface, a role-named palette, and a density toggle
([ADR-0107](0107-editor-typeface-and-palette.md)), so the thing worth shipping was
the thing the download did not contain.

There is a second, sharper problem that shipping the editor exposed. The demo
project's startup scene stores its sprite texture as a path-keyed reference,
`tex:assets/textures/checker.png`. A project's `asset_root` points the *asset
database* at the project directory, but a `tex:<path>` reference is read from the
**process working directory** — the asset root is consulted to resolve a `guid:`
identity, not to prefix a path key. From the repository root the reference
resolves by coincidence: a sample `assets/` tree sits there for the editor's
built-in example scene. Anywhere else — including a freshly unpacked AppImage —
the texture does not load, and the sprite renders untextured:

```
warn: serialization: could not load resource 'tex:assets/textures/checker.png'
      for property 'texture'
```

## Decision

**The AppImage carries both binaries and both content sets, and the editor is the
default entry point.**

`packaging/package_appimage.cmake` copies `fork_breakout` and `fork_editor_shell`
into `usr/bin`, the game's content (`examples/breakout`) and the editor's demo
project (`examples/demo_project`) into `examples/`, and the editor's built-in
sample assets (`assets/`) at the AppDir root. `AppRun` dispatches:

- no argument, or `--editor` — the editor, opened on the bundled demo project
  (`--project "$HERE/examples/demo_project"`, an absolute path so the launch
  directory does not matter);
- `--game` — the breakout slice;
- any other argument — the game, so the file stays a drop-in for the
  `--headless N` and `--save-scene` flags the container smoke test and the README
  already use.

The desktop entry names the editor as the default and offers "Play Breakout" as a
second action; both `Exec` lines go through `AppRun`, because `AppRun` is what
sets the working directory and the library path and a desktop launcher inherits
neither.

**Libraries are bundled from both binaries.** `ldd` is run over each and the union
is copied, minus glibc and the graphics stack as before. Today the two link the
same runtime, so the union is no larger; bundling from only one would leave the
other needing a library the host may not have, which is exactly the failure the
bundle exists to prevent.

**The sample `assets/` tree is shipped so the path-keyed reference resolves.** The
AppDir root is the working directory `AppRun` sets, so placing `assets/` there
makes the download match the layout CI runs the editor against from the repository
root. This is a packaging fix, not a fix for the underlying path semantics: a
`tex:<path>` reference is still resolved against the working directory rather than
the project's asset root.

**The smoke test exercises both entry points.** After `--help`, the script plays
the game headless and *also* runs the editor headless (`--editor --headless
--frames N`) on the bundled project, so a bundle that starts the editor but cannot
draw it is caught where it is made. `tests/appimage/run_appimage_container_smoke.sh`
runs the editor in the clean container for the same reason.

## Consequences

**Easier:**

- One download is the whole product: the editor to look at and the game to play,
  with no artifact to choose between and no README paragraph to read first.
- A person trying the engine meets the editor with a project already open, which
  is the better first impression than a game window.
- The container smoke test now proves the editor's GL path in a clean
  environment, not just on the build host.

**Harder:**

- The AppImage is larger: the editor binary, its demo project, and the sample
  assets are added to a file that carried only the game.
- The editor needs a display and a GL 3.3 context to do anything, so the default
  entry point fails on a machine with no display where the game's `--headless`
  mode would have worked. The headless flags are documented in the README for
  that case.
- Shipping `assets/` treats a symptom. The path-keyed reference's
  working-directory dependence is real and remains; a project opened from
  somewhere without an `assets/` beside it still renders the demo scene
  untextured. A follow-up should resolve a path-keyed `tex:` reference against
  the project's asset root, the way a `guid:` identity already is, and this ADR
  does not do that.

## Alternatives considered

**Ship a second AppImage for the editor.** Rejected: two files to choose between
is the decision the single-file download exists to remove, and a person who does
not know the engine cannot pick correctly.

**Put the editor only in the SDK archive and document it there.** Rejected: the
SDK is framed as a build-against tree, and its editor needs a display and a
project path to be useful. It is the wrong artifact for "try it."

**Default to the game and require `--editor`.** Rejected: the editor is the
subsystem that was just polished and the thing with the most to show; opening it
first, on a project that is already loaded, is the stronger first run.

**Fix the `tex:` path semantics in this change instead of shipping `assets/`.**
Considered and deferred. The correct fix is to resolve a path-keyed reference
against the project's asset root, which touches `ResourceManager`'s resolution
order and needs its own tests; it is a resource-system change, not a packaging
one, and bundling it here would mix two concerns in one reviewable step. The
symptom is removed for the download now; the follow-up is named above.
