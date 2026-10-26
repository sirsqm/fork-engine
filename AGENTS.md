# AGENTS.md — repository notes for automated contributors

Fork Engine is a modular C++20 2D/3D game engine. Read `docs/ARCHITECTURE.md`
for the design and `docs/PLAN.md` for the pillars, the milestone order, the
shipping queue, and the next concrete step, before changing anything.

## Strategy: the pillars

Three capabilities shape the design, not just the feature list, and every
subsystem should be checked against them:

- **Performance is a product feature** (M9, ADR-0017). Measure before
  optimizing; the authoring model (scene graph) and the processing model (a flat
  instance array) meet at one extraction seam; parallelise CPU work but keep the
  RHI device single-threaded.
- **Games ship anywhere** (M8, ADR-0018). The platform layer is the complete OS
  contract with a null backend; assets ship in a pack; targets are new backends,
  added on demand. This is about shipping *games*, not running the editor on
  several OSes.
- **Edits are durable and shareable** (M10). An edit is a serializable operation
  over a stably identified object, so undo, redo, and replay share one model;
  component identity is [ADR-0045](docs/adr/0045-component-identity.md), the
  local operation model [ADR-0047](docs/adr/0047-local-operation-model.md).

The local steps toward collaborative editing — stable component identity, a
serializable operation model, and an ordered log — are shipped and kept, because
they make undo/redo robust on their own. Networked collaborative editing is not
planned: it needs a hosted authoritative service, and the project will not carry
that running cost.

These are plans, not shipped code. Do not present a planned item as done.

## Build and test

```sh
cmake -S . -B build -DFE_WARNINGS_AS_ERRORS=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

## Building for the web (Emscripten)

Much of the shipping queue is web work, so most runs need this build as well as
the desktop one. The web toolchain is Emscripten; it is **not** installed by
default, and configuring without it fails at the toolchain check with a message
that says so.

```sh
# Once per run, if emsdk is not already present and activated:
git clone --depth 1 https://github.com/emscripten-core/emsdk.git /tmp/emsdk
/tmp/emsdk/emsdk install 6.0.13
/tmp/emsdk/emsdk activate 6.0.13
source /tmp/emsdk/emsdk_env.sh

# Then configure and build the engine for wasm:
cmake -S . -B build-web \
  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/emscripten.cmake \
  -DFE_BUILD_EXAMPLES=OFF
cmake --build build-web --parallel
```

- Use the **checked-in toolchain file**, not `emcmake`; it makes the decisions
  the web build needs (see its comments and ADR-0073).
- `source /tmp/emsdk/emsdk_env.sh` must be re-run in every new shell, or
  `EMSDK` is unset and the configure fails.
- The web build defaults the editor, unit tests and benchmarks off; they are
  desktop-only. Do not try to build the test suite for wasm.
- The engine library building for wasm is not proof a browser will run it. Two
  browser checks are the acceptance tests and each needs a headless browser:

```sh
source /tmp/emsdk/emsdk_env.sh
tests/web/run_web_smoke.sh      # compiles the backend to wasm, runs headless Chromium
tests/web/run_web_game.sh       # exports the 2D demo and the playable 3D game, runs them in headless Chromium
```

- The smoke script proves the window backend can get a WebGL2 context; the game
  script proves a whole exported game runs on it (export template, pack fetched
  over HTTP, ES shaders, both renderers). Run both when touching the web path.
  The game script runs `examples/demo_project` and the playable
  `examples/game3d_project`; set `FE_WEB_GAME_PROJECT=DIR` for a single focused
  run. The 3D run turns on the player's input probe (`?probe=1&probeNode=Player`)
  and fails when the watched node does not move, so "a synthetic key reaches
  gameplay in a browser" is checked, not assumed. The probe pins a fixed
  simulation step (ADR-0079), so its reported distance is reproducible.
- The game script exports with the native `fork_export` when one exists and
  otherwise with the wasm one under node, so it works in the web CI job, which
  has no desktop build. The player reports what it drew into the page title
  (`FE_PLAYER_RESULT canvas=… frames=… distinct=…`); `distinct` is a blankness
  check — one colour means nothing rendered — so a broken renderer fails the
  check with no golden image. `?frames=N` on the page bounds a run, which is how
  the check observes a game that would otherwise run until its tab closes.
- Both scripts need `em++` on `PATH` and a Chromium/Chrome binary; they honour
  `CHROME_BIN` and retry a slow browser start, so they are safe to run in CI and
  in a run. Each attempt gets its own port and its own `--user-data-dir`, because
  two headless Chromes that share the default profile race on its singleton lock
  and the loser exits at once without loading the page — the retry would then
  fail the same way three times. `timeout` bounds a browser that hangs; where
  `timeout` is missing (stock macOS) the scripts run without it.
- `clang-format` must never be allowed to see an `EM_ASM` body containing `===`:
  it parses the body as C++, rewrites `a === b` to `a == = b`, and the result is
  a JavaScript syntax error that breaks the module at run time with no build
  error. Write such bodies without strict equality (use `startsWith`, `==`).

CI builds every push on Linux/GCC (Debug+Release), macOS/Clang (Debug), and
Windows/MSVC (Debug), with warnings as errors. It also runs a clang-format
check. Two Linux-only jobs guard quality beyond a build: a sanitizer run
(AddressSanitizer + UndefinedBehaviorSanitizer, with LeakSanitizer on for the
pure-logic tests and off for the GL tests; ADR-0035) and a benchmark regression
gate (ADR-0033). Keep every job green; a warning on one compiler is a real
defect, not noise.

## Conventions that CI enforces

- Run `clang-format -i` on every file you touch. CI pins clang-format 23.1.3;
  other versions may format differently.
- Format with the same file list CI uses, not `git status`:
  `clang-format -i $(git ls-files '*.h' '*.hpp' '*.cpp' '*.cc' | grep -v '^third_party/')`.
  `git status --porcelain` lists untracked *directories* (`?? engine/include/fe/physics2d/`),
  not the files inside them, so a new module can pass a `git status`-based
  format step and still fail the CI check.
- Warnings are errors. The warning flags live on the `fe_warnings` interface
  target (`cmake/FEWarnings.cmake`); do not call `add_compile_options()` for
  warnings, because that is directory-scoped and leaks into dependency builds.
- Dependencies are pulled in as external headers. On MSVC this needs
  `/external:W0` (see `FEWarnings.cmake`); marking includes `SYSTEM` alone is
  not enough.
- Include the standard header for every std type you name. GCC and libc++
  provide many headers transitively; MSVC does not, so a missing
  `#include <string_view>` compiles everywhere except Windows.
- The benchmark regression baseline (`benchmarks/baseline.json`) is recorded on
  the CI runner, not locally. The runner is ~40% slower than a dev machine, so a
  locally recorded baseline fails the gate on its own reference code. Re-record
  from the gate job's `benchmark-result` artifact (see `benchmarks/README.md`).

## Portability traps seen before

- A construct that is legal on a desktop GL 3.3 core context can be illegal in
  WebGL2 (GLSL ES 3.00), and the browser reports it only at run time. Two seen so
  far: a depth texture's format/type pair must be legal in ES (`GL_DEPTH_COMPONENT`
  with `GL_UNSIGNED_INT`, not `GL_FLOAT`), and a draw whose fragment shader has no
  output needs a framebuffer with no colour attachment (`glDrawBuffers(0, nullptr)`,
  `RenderTargetDesc::color = false`). See ADR-0076. When touching GL, run
  `tests/web/run_web_game.sh`, not just the desktop build.
- Raw-pointer identity checks in doctest (`CHECK(a == b)` on `T*`) fail to
  compile on MSVC because it tries the `shared_ptr` `operator<<`. Wrap the
  expression in an extra pair of parentheses: `CHECK((a == b))`.
- The same trap fires on a `shared_ptr<T>` when `T` has no `operator<<`, and it
  is not limited to identity checks: `REQUIRE(font != nullptr)` on a
  `std::shared_ptr<Font>` fails to compile because stringifying the expression
  reaches for `std::operator<<(ostream&, const shared_ptr<T>&)`, which needs
  `T` to be streamable. Parenthesise it too: `REQUIRE((font != nullptr))`. The
  type's own streamability is irrelevant to the check; the parens keep doctest
  from stringifying it at all.
- longjmp-based test harnesses trip MSVC C4611; suppress it at file scope with
  a comment, not globally.
- Path tests must not assume POSIX. A leading `/` is not absolute on Windows
  (`fs::is_absolute("/x")` is false there), and `std::filesystem::path` uses
  backslashes on Windows. Build absolute examples from
  `std::filesystem::temp_directory_path()`, join with `std::filesystem::path`
  `/` rather than string `+ "/"`, and compare resolved paths through
  `fs::normalize` so a forward-slash expectation matches a backslash result.

## Commit identity

Every commit in this repository is authored and committed by
`sqm <sirsamyoudev@gmail.com>`. Do not add a `Co-authored-by` trailer, and do
not let an automated agent's identity (`openhands <openhands@all-hands.dev>`,
`GitHub <noreply@github.com>`, or any other) appear in the author, committer, or
message. If such a commit is found, rewrite it with `git filter-repo --mailmap`
(identity) and a `--message-callback` (trailer/body), then force-push `main`.

## ADR numbering

ADR numbers are claimed by hand and branches are developed in parallel, so two
records can claim the same number before either merges. Before writing a new
ADR, list `docs/adr/` on `main` **and** on every open branch/PR and pick a number
none of them use. Update the index table in `docs/adr/README.md` in the same
change. A gap in the numbering is fine; a collision is a merge-time rename.

## Versioning

The engine version has one source of truth: the `project(... VERSION ...)`
call in the root `CMakeLists.txt`. `cmake/FEVersion.cmake` generates
`fe/version/Version.h` from it, exposing `fe::version::kEngineVersion`,
`kEngineVersionString`, and the `FE_VERSION_*` macros. The scene file-format
version is independent (`fe::serialization::scene::kFormatVersion`). Do not
hardcode a version string anywhere else.
