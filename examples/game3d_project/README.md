# Game 3D project

A playable 3D game that runs in a browser. Where `demo3d_project` exists to
prove the 3D renderer draws, this one proves a *player* can drive it: it is the
acceptance target for 3D gameplay on the web, and for input reaching gameplay at
all in a browser build.

```
fork.project.json      name, asset root, and startup scene
assets/meshes/         the cube mesh every object uses
assets/textures/       the checker texture the floor uses
scenes/main3d.fescene  the scene: a fly camera, a floor, a movable player, spinners
```

## Playing it

Run it natively, straight from the project (no export step):

```bash
./build/bin/fork_player --pack /dev/null --scene scenes/main3d.fescene   # direct form
# or, the shipped shape:
./build/bin/fork_export --project examples/game3d_project --out /tmp/game3d --target desktop
/tmp/game3d/fork_player --export /tmp/game3d
```

On a machine without a display, `--headless N` loads the scene, runs N frames and
exits, which is enough to check that the scene and its behaviours load:

```bash
/tmp/game3d/fork_player --export /tmp/game3d --headless 10
```

In a browser, export for the web target and serve the result:

```bash
source /path/to/emsdk_env.sh
cmake -S . -B build-web -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/emscripten.cmake -DFE_BUILD_EXAMPLES=ON
cmake --build build-web --parallel
./build/bin/fork_export --project examples/game3d_project --out /tmp/game3d-web \
  --target web --player build-web/bin/fork_player.js
python3 -m http.server 8000 --directory /tmp/game3d-web
# open http://localhost:8000/index.html
```

## Controls

| Action | Keyboard | Touch |
| --- | --- | --- |
| Move (player, and the fly camera) | `W` `A` `S` `D` or the arrow keys | lower-left regions |
| Move up / down | `Space` / `Left Shift` | — |
| Run | `Left Ctrl` | — |
| Look | `I` `J` `K` `L`, or drag with the left mouse button | lower-right regions |

Movement is relative to where you are looking, so `W` always goes into the
screen. The player walks on the floor plane; the camera is free to fly.

## How the logic works, and why it is in the engine

The behaviours this scene binds — `FlyCamera3D`, `MoveNode3D`, `SpinNode3D` — are
shipped in the engine (`fe/player/`, ADR-0078), not in a game module. That is
deliberate: a browser build has no dynamic loader, so a gameplay module cannot be
loaded there, and a web export that relied on one would ship static geometry with
no logic to consume input. Compiling a small, useful set of behaviours into the
engine gives every target gameplay to run, and a game can still add its own
module on the platforms that support one.

The scene binds them by name like any behaviour, so nothing about the scene says
which side of the process boundary the logic came from.

## Verifying input in a browser

A browser harness can dispatch events but cannot press a key, so the player can
do it and report the result. `?probe=1` on the page makes the runtime press the
movement keys with real synthetic DOM events and report how far the watched node
moved:

```
http://localhost:8000/index.html?frames=2&probe=1
```

The page title then reads
`FE_PLAYER_RESULT canvas=1 frames=2 distinct=… moved=… probe=1`; a non-zero
`moved` is proof that a synthetic key reached the action layer and drove a
behaviour. `tests/web/run_web_game.sh` runs this project with the probe on and
fails the run when `moved` is zero, so "input works in the browser" is checked on
every push rather than asserted in prose.
