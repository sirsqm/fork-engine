# Game 3D project

A playable 3D game that runs in a browser. Where `demo3d_project` exists to
prove the 3D renderer draws, this one proves a *player* can drive it: a
third-person character with a follow camera, a course to walk around, and
physics the character and the props share. It is the acceptance target for 3D
gameplay on the web, and for input reaching gameplay at all in a browser build.

```
fork.project.json      name, asset root, startup scene, and input bindings
assets/meshes/         the cube and sphere every object uses, plus the imported crate
assets/textures/       the checker texture the floor uses
input/bindings.json    the game's own keys and touch regions (fork_input document)
scenes/main3d.fescene  the scene: a character, a follow camera, a course, props
```

## What is in the scene

- **Player** — a `CharacterController3D` on a box with a `Collider3D` and a
  `RigidBody3D`. It walks, runs, turns and jumps, and it collides with the
  world through the physics step.
- **Camera** — a `FollowCamera3D` that trails the Player from behind and above,
  swinging round as the character turns.
- **Floor, Ramp, Platform, four walls** — static colliders that bound the course.
- **Four crates and a ball** — dynamic bodies with gravity, so the character can
  shove the crates and the ball bounces. The ball is a sphere collider drawn as
  a sphere mesh.
- **Spinner** — a `SpinNode3D`, showing a non-physics behaviour running alongside
  the physics ones. It draws the *imported* crate: `assets/meshes/crate.gltf`, a
  multi-file glTF whose vertex buffer (`crate.bin`) and base-colour map
  (`crate_albedo.png`) are named inside the glTF rather than by the scene. That
  makes it the asset that proves a pruned pack follows a container's own
  references, which is why the web acceptance test exports this project
  (`tests/web/run_web_game.sh`, [ADR-0088](../../docs/adr/0088-transitive-pack-references.md)).

The Player's body has `affected_by_gravity` off and `fixed_rotation` on: the
controller owns the vertical axis and the facing, and the solver owns everything
else. That split is what makes "grounded" and a jump feel deterministic rather
than emerging from impulses.

## Playing it

Run it natively, straight from the project (no export step):

```bash
./build/bin/fork_player --pack /dev/null --scene scenes/main3d.fescene   # direct form
# or, the shipped shape:
./build/bin/fork_export --project examples/game3d_project --out /tmp/game3d --target desktop
/tmp/game3d/fork_player --export /tmp/game3d
```

On a machine without a display, `--headless N` loads the scene, runs N frames and
exits, which is enough to check that the scene, its behaviours and the physics
schedule all load:

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

The game declares its own layout in `input/bindings.json`, a `fork_input`
document ([ADR-0090](../../docs/adr/0090-declarative-input-bindings.md)). It is
packed with the game and applied by the player in place of the engine's built-in
layout, so the layout is data a designer can change without a rebuild.

| Action | Keyboard | Touch region (normalised, origin top-left) |
| --- | --- | --- |
| Walk forward | `W` / `↑` | the middle column of the lower half |
| Walk back | `S` / `↓` | the middle column of the upper half |
| Turn left | `A` / `←` | the left column of the lower half |
| Turn right | `D` / `→` | the right column of the lower half |
| Jump | `Space` | the right half of the upper band |

Every entry carries `"replace": true`, so the document's regions replace the
engine's built-in ones for the kinds it binds. Without that the built-in pads and
the document's pads would overlap, and one touch would fire two actions.

The character walks along its own facing and turns with `A`/`D`, so the whole
game is playable with a keyboard alone and with a touchscreen alone — no mouse
capture, which a browser only grants after a user gesture and a phone does not
have at all. The camera follows automatically.

## How the logic works, and why it is in the engine

The behaviours this scene binds — `CharacterController3D`, `FollowCamera3D`,
`SpinNode3D` — are shipped in the engine (`fe/player/`, ADR-0078), not in a game
module. That is deliberate: a browser build has no dynamic loader, so a gameplay
module cannot be loaded there, and a web export that relied on one would ship
static geometry with no logic to consume input. Compiling a small, useful set of
behaviours into the engine gives every target gameplay to run, and a game can
still add its own module on the platforms that support one.

The scene binds them by name like any behaviour, so nothing about the scene says
which side of the process boundary the logic came from.

## Physics is scheduled by the player, not assumed

The scene has colliders, so the runtime schedules a `Physics3DSystem` for it,
after the `ScriptSystem` and before the frame's render. That order is what lets a
controller write a velocity and the solver resolve it in the same frame: a
character that walks into a wall is stopped by the collision response rather than
passing through it. A 2D scene schedules the 2D system instead, and a scene with
neither pays for neither. Before this the player scheduled no physics at all, so
an exported 3D game could draw but nothing could move or collide.

## Verifying input in a browser

A browser harness can dispatch events but cannot press a key, so the player can
do it and report the result. `?probe=1` on the page makes the runtime press the
movement keys *and* the jump key with real synthetic DOM events and report how
far the watched node moved and how high it rose:

```
http://localhost:8000/index.html?frames=2&probe=1&probeNode=Player
```

The page title then reads
`FE_PLAYER_RESULT canvas=1 frames=2 distinct=… moved=… probe=1 jumped=… glerr=0`. A
non-zero `moved` is proof that a synthetic key reached the action layer and drove
a behaviour, and a non-zero `jumped` is proof that the jump half of the
controller fired too — the movement check alone would pass on a controller whose
jump never worked. `glerr` is the backend's error count after the run: a WebGL2
context rejects calls a desktop GL 3.3 context tolerates, and a rejected pass can
still leave a non-blank frame behind, so the check fails on a non-zero count
rather than trusting the frame's colours alone
([ADR-0095](../../docs/adr/0095-web-gl-error-verification.md)). `tests/web/run_web_game.sh`
runs this project with the probe on and fails the run when `moved` or `jumped` is
zero or `glerr` is non-zero, so "input works in the browser, and the driver
accepted every call" is checked on every push rather than asserted in prose.

The probe pins a fixed simulation step (`--fixed-step`, defaulting to 1/60 s when
the probe is on; ADR-0079), so the distance it reports is a property of the input
and the behaviour rather than of how fast the browser happened to run the frames.
Two frames are therefore enough, and the same run reports the same `moved` every
time. A plain run steps by the measured frame time as a game does.
