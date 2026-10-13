# Demo project

A minimal Fork Engine project, used to try the editor without authoring
anything first. It has a project file, an asset root, and one scene with two
textured sprites.

```
fork.project.json      name, asset root, and startup scene
assets/textures/       the texture the scene references
scenes/main.fescene    the startup scene (a background and a hero sprite)
```

Open it with the editor shell, from the repository root:

```bash
./build/bin/fork_editor_shell --project examples/demo_project
```

`--project` accepts either a project directory (the file `fork.project.json`
inside it is loaded) or a project file directly. On a machine without a
display, `--headless --frames N` opens the project, loads the startup scene,
and exits after N frames, which is enough to check that the project loads:

```bash
./build/bin/fork_editor_shell --project examples/demo_project --headless
```

The scene stores its texture as a `tex:` reference. Once the asset is imported
into the project database it can be referenced by a stable `guid:` id instead,
so the reference survives the file being moved or renamed (see ADR-0020).
