# Demo 3D project

A minimal Fork Engine 3D project, the 3D counterpart to `demo_project`. It
exists so the 3D path has something real to build, load, export, and render —
including in a browser, which is what `tests/web/run_web_game.sh` checks.

```
fork.project.json      name, asset root, and startup scene
assets/meshes/         the cube mesh the scene references
assets/textures/       the checker texture the floor references
scenes/main3d.fescene  the startup scene (a camera, a light, a floor, two cubes)
```

Open it with the editor shell, from the repository root:

```bash
./build/bin/fork_editor_shell --project examples/demo3d_project
```

On a machine without a display, `--headless --frames N` opens the project, loads
the startup scene, and exits after N frames, which is enough to check that the
project loads:

```bash
./build/bin/fork_editor_shell --project examples/demo3d_project --headless
```

The scene references its mesh as `obj:` and its texture as `tex:`; a pruned
export carries exactly the assets the scene names. That pruning traces the scene
through reflection, so a tool that has not registered the 3D component types
sees no references at all and ships an empty pack — the failure ADR-0076 records.
