# Breakout — the M2.5 vertical slice

This is the engine's first complete game: a small, finished breakout built
entirely against Fork Engine's public API, exactly as an external project would
build one. It exists to answer a single question the unit tests cannot: *do the
subsystems work together?* Infrastructure that has only ever passed its own
tests has not been proven to work in one artifact.

The game is deliberately not clever. It is the smallest thing that exercises the
whole engine at once, and every gap it exposed was fixed in the engine rather than
papered over in the game.

## What it exercises

| Subsystem | How the game uses it |
| --- | --- |
| Scene graph and transforms | Every wall, brick, paddle, ball, camera, and audio node is a `Node` |
| `Camera2D` | One camera frames the 1536x960 field |
| Resource manager + texture loader | Four textures loaded by path |
| `Sprite2D` | Every visible entity |
| `Physics2D` | Static brick/wall colliders, a kinematic paddle, a dynamic ball, and contact events |
| Audio | `WavLoader`, `AudioClip`, the pure mixer, `AudioSystem`, and the real miniaudio backend |
| Serialization | The field round-trips through the JSON scene format, audio node included |
| Input | Paddle control and launch |
| Systems + `SceneSchedule` | The whole game is six systems run in a fixed order |

## How it is built

The code is split three ways on purpose.

- **`breakout_state.h/.cpp` — the rules.** Pure C++ with no engine dependency:
  score, lives, bricks, level, win/lose. Because it has no dependency it is
  unit-tested directly, and the numbers are asserted by hand.
- **`breakout_scene.h/.cpp` — the field and the wiring.** Builds the canonical
  layout and installs the systems. This is the single source of truth: the
  serialized scene file is a dump of exactly this, so the file path, the
  code-built fallback, and the tests cannot drift apart.
- **`main.cpp` — the application shell.** Loads or builds the scene, opens the
  audio device, drives the loop, rebuilds the field when the rules ask (a
  restart or a level clear), and logs a machine-checkable result.

The systems, in order (`ADR-0008`):

1. `PaddleControlSystem` — reads input; in autoplay it tracks the ball.
2. `BallLaunchSystem` — launches on the launch key, or after a short autoplay
   delay.
3. `Physics2DSystem` — steps the world (gravity off; breakout is gravity-free).
4. `BallContactSystem` — turns a ball-vs-brick contact into a score change, a
   broken brick, and a cue; turns a paddle contact into the classic *aimed*
   bounce, where the hit offset sets the outgoing angle. That aim is what makes
   it a game rather than a vertical loop.
5. `GameStateSystem` — a missed ball costs a life; the field freezes on win/lose.
6. `AudioSystem` — added last by `main.cpp` (the engine owns it and the
   application adopts it), so a cue fired this frame is mixed this frame.

The one non-obvious piece of glue the slice needed was making the input state
reach systems at all: systems receive a `FrameContext`, so `InputState` now
travels on it (`Engine::input()` -> `FrameContext::input`) instead of systems
reaching back into the `Engine`. That is a small, general addition any interactive
game needs.

## Running it

```sh
cmake -S . -B build -DFE_BUILD_EXAMPLES=ON
cmake --build build --parallel
./build/bin/fork_breakout
```

Controls: Left/Right or A/D move the paddle, Space/Enter starts a game and
launches the ball, P pauses and resumes, R restarts, Escape quits. The game opens
on a title screen; the same keys (or a tap in the middle of the screen) leave it
and continue after a game over.

Clear a board and the level advances: the bricks come back and the ball is
launched a little faster, so each level is harder than the last. There are three
levels; clearing the third wins. The HUD shows the level between the score and
the lives.

Headless (no window, autoplay drives the ball, exits on win/lose or after N
frames):

```sh
xvfb-run -a ./build/bin/fork_breakout --headless 60000
```

A run prints two lines a script can check:

```
info: breakout: audio mixed 4989091 frames, peak 0.4200
info: breakout: score=120 lives=3 result=playing
```

`peak` is the largest absolute sample the mixer ever produced, so it is a real
"audio data was produced" signal even on a machine with no sound card (and even
though the final frame of a run is usually silent).

Regenerate the authored scene from the code-built one:

```sh
./build/bin/fork_breakout --save-scene examples/breakout/scenes/breakout.fescene
```

## Assets

The textures and the WAV cue are generated, not checked in as binaries someone
hand-drew. `tools/gen_breakout_assets.py` writes them from the standard library
only (no Pillow), so the assets are reproducible and reviewable. The WAV is 16-bit
mono PCM at 44.1 kHz — a real file the `WavLoader` decodes, not a fixture the
tests fabricate.

## What is deferred

This slice is finished as a game, but these are explicitly later work, and the
game does not pretend to use them:

- **3D positional audio.** The `AudioSource` spatial fields exist, but the game
  plays every cue non-positionally; wiring attenuation to world transforms is
  separate work.
- **Streaming / large-file playback.** Clips are fully decoded into memory.
- **Compression codecs.** Only PCM WAV is decoded.
- **Audio buses / mixing groups.** The mixer supports them; the game uses the
  master bus only.
