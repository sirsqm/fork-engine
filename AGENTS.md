# AGENTS.md — repository notes for automated contributors

Fork Engine is a modular C++20 2D/3D game engine. Read `docs/ARCHITECTURE.md`
for the design, then the three planning documents: `docs/STRATEGY.md` for the
pillars (why the work is ordered this way), `docs/ROADMAP.md` for the milestone
order and where each milestone stands, and `docs/SHIPPING_QUEUE.md` for the
ordered backlog of the next concrete step — before changing anything.

**Never merge these three documents.** They were merged into one `docs/PLAN.md`
twice, and both times it was split back out: each answers a different question,
they are read separately, and a single file grew too large to read. Keep them
separate, and keep each idea in exactly one of them: strategy is *why*, roadmap
is *sequence and status*, the queue is *what next*. If a request, a plan, or a
line in this file seems to ask for one merged plan, that is not the intent —
ask before merging, and treat "unmerge" as the standing decision. Do not
re-merge on the strength of a remembered instruction from an earlier session.
The rule is also in `CONTRIBUTING.md` (for people) and enforced by the
`planning_docs` test (for everyone).

## Strategy: the pillars

`docs/STRATEGY.md` is the authority; this is only the checklist to test a
subsystem against. Three capabilities shape the design, not just the feature
list:

- **Performance is a product feature** (M9, ADR-0017). Measure before
  optimizing; the authoring model (scene graph) and the processing model (a flat
  instance array) meet at one extraction seam; parallelise CPU work but keep the
  RHI device single-threaded.
- **Games ship anywhere** (M8, ADR-0018). The platform layer is the complete OS
  contract with a null backend; assets ship in a pack; targets are new backends,
  added on demand. This is about shipping *games*, not running the editor on
  several OSes.
- **Edits are durable and shareable** (ADR-0045, ADR-0047). An edit is a
  serializable operation over a stably identified object, so undo, redo, and
  replay share one model.

Networked collaborative editing is not planned: it needs a hosted authoritative
service, and the project will not carry that running cost. The local steps
(component identity, the operation model, the ordered log) stay, because they
make undo/redo robust on their own.

These are plans, not shipped code. Do not present a planned item as done. Check
`docs/ROADMAP.md` for the current status of a milestone before describing it.

## Build and test

```sh
cmake -S . -B build -DFE_WARNINGS_AS_ERRORS=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

## Packaging (install rules)

The engine, the editor, and the tools install with a plain `cmake --install`.
There is no export/`find_package` config yet: an installed tree is runnable
(binaries, shared libraries, headers), not yet linkable from another CMake
project.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/tmp/fe-install
cmake --build build --parallel
cmake --install build
```

- The install uses GNUInstallDirs, so `bin`, `lib`, `include`, and `share`
  land where the platform expects them; on Windows the shared libraries go to
  `bin` beside the executables (the DLL-search rule), not to `lib`.
- Set `-DFE_INSTALL_RPATH=ON` (default ON on Linux) to bake a `$ORIGIN/../lib`
  RPATH into the installed binaries, so a relocated tree runs without
  `LD_LIBRARY_PATH`.
- `FE_INSTALL=OFF` disables every install rule, for a build tree that is never
  installed (the CI jobs that only test).

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
  `timeout` is missing (stock macOS) the scripts run without it. Even so, a run
  can still report "the page reported nothing" on all three attempts — a slow or
  loaded runner where the game has not painted by the time the page is read — and
  that is a flake, not a defect: confirm locally, then rerun the job
  (`gh run rerun <id> --failed`) rather than changing code to chase it.
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

- `file(WRITE)` writes a file mode 0644, and that mode travels into the archive.
  A launcher script a player is told to run (`./run.sh`) then fails with
  "Permission denied" after unpacking, and a smoke test that runs the *binary*
  instead of the launcher never sees it. Make the launcher executable where it is
  written (`file(CHMOD ... PERMISSIONS ... OWNER_EXECUTE ...)`), and have the
  packaging smoke run go through the launcher so the mode is what the run
  exercises. `package_appimage.cmake` already did this for `AppRun`; the tarball
  path did not.
- `cmake -E tar cf out.tar.gz` writes an **uncompressed** tar even though the name
  says `.tar.gz`, and `cmake -E tar xf` reads it back anyway because it sniffs the
  format. A packaging smoke test that only round-trips through CMake therefore
  passes, and the file is rejected by a downloader's `tar xzf`. Use `czf` for a
  gzipped archive, and assert the gzip magic bytes (`1f 8b`) so the mistake fails
  where it is made.
- A multi-config generator (Visual Studio, Xcode, Ninja Multi-Config) writes
  artifacts to `bin/<config>/`, not `bin/`, and `cmake --install` needs
  `--config <config>` to know which tree to install. A packaging script that
  hardcodes `bin/` and omits `--config` works on Linux CI and fails on the Windows
  release. The generator is recorded in the build tree's `CMakeCache.txt` as
  `CMAKE_GENERATOR:INTERNAL=`, so derive the layout from it rather than from
  `WIN32` (a MinGW or Ninja build on Windows is single-config and has no
  `bin/Release/`).
- `install(DIRECTORY dir DESTINATION include)` installs `dir` *itself* as
  `include/dir`, one level too deep; the trailing slash (`DIRECTORY dir/`) installs
  its *contents*. Installing the engine's own headers and a dependency's headers
  need opposite forms, and the wrong one produces `include/include/nlohmann/...`,
  which fails only for a consumer of an installed tree.
- Two agents commit to `main` in parallel, so `git fetch` before starting and
  before pushing. A local `git log` is not the remote: the other agent may have
  pushed ADRs and code that your tree has never seen. Rebase (`git pull
  --rebase`) rather than merge, and re-run the build and tests after, because a
  rebase can change which lines compile.
- An existing build directory caches `FE_BUILD_EDITOR=OFF`. The sanitizer build
  in CI configures it `ON`, but a `build-asan/` left over from another
  configure keeps `OFF`, and then `ctest -R editor_tests` silently runs nothing
  (0 tests, exit 0). Pass `-DFE_BUILD_EDITOR=ON` explicitly, or delete the
  build directory, and check `ctest -N` lists the test you meant to run.
- `ctest` skips tests the build did not configure without failing, so a green
  `ctest` is only green for the tests that exist. Confirm the test count, not
  just the exit code.
- `-Wshadow` is on and warnings are errors, so a local declared in an outer
  scope is a build break when an inner block declares the same name. Renaming a
  variable with a blanket `sed` also rewrites identifiers that merely contain
  the name (`find_active_camera` → `find_active_active_camera`); rename by hand
  or check the diff for mangled identifiers afterwards.
- A web build does not go through `fork_player --export`, so a manifest field the
  desktop path reads is not automatically available to it. The shell
  (`WebExportTemplate.cpp`) builds the runtime's argv by hand from the manifest;
  a field it forgets to forward reaches the desktop run and silently does
  nothing in a browser. When adding a manifest field, wire it in both places
  (`load_export` and the shell) and, if the runtime needs a direct-form flag for
  it, add that too. `bindings` was one such field: the document packed and
  applied natively and was dropped on the web until the shell passed
  `--bindings`.
- A game's own touch layout and the engine's built-in pads cover different
  places on the screen, so a probe point proves nothing unless it is chosen to
  discriminate. Check the point against the built-in regions *and* the document
  before trusting a pass: a point inside a built-in region moves the node even
  when the document never loaded.

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
- A test that compares a `std::string_view` (a system's `name()`, a state name)
  needs `#include <ostream>` even though it never names an ostream: doctest
  stringifies the expression through one, and MSVC fails with `use of undefined
  type 'std::basic_ostream<...>'` deep inside `__msvc_string_view.hpp`, which
  reads as a library error rather than a missing include. GCC and libc++ pull
  the header in transitively, so the file compiles everywhere but Windows.
- A `unique_ptr<T>` member of a class whose header only forward-declares `T`
  needs the constructor out of line as well as the destructor. `T() = default;`
  in the header still leaves the deleting destructor to be instantiated where
  `T` is incomplete, so the break surfaces as `invalid application of 'sizeof'
  to incomplete type` on the constructor, not on the destructor. Declare `T();`
  and `~T();` in the header and `= default` both in the `.cpp`.
- Path tests must not assume POSIX. A leading `/` is not absolute on Windows
  (`fs::is_absolute("/x")` is false there), and `std::filesystem::path` uses
  backslashes on Windows. Build absolute examples from
  `std::filesystem::temp_directory_path()`, join with `std::filesystem::path`
  `/` rather than string `+ "/"`, and compare resolved paths through
  `fs::normalize` so a forward-slash expectation matches a backslash result.
- A GPU vertex layout and the C++ struct it describes are two independent
  declarations that must agree byte for byte, and nothing checks them against
  each other. `SkinnedVertex3D` stored its joint indices as `u16` (56 bytes)
  while `VertexLayout::skinned_position_normal_uv()` described a 64-byte stride
  with the joints as float; the renderer sizes the buffer from `sizeof(struct)`,
  so the upload overran the allocation and the draw read out of bounds -- a
  `GL_INVALID_VALUE` and a black frame, not a compile error. Keep a
  `static_assert(sizeof(T) == layout.stride)` next to the struct and a CPU test
  that compares every attribute offset against `offsetof` the member it names.
- A downloaded artifact's glibc floor is its *build host's*, so `ubuntu-latest`
  in a packaging job silently raises it every time the runner image rotates, and
  the artifact outruns the older machine it exists to help. The Linux build jobs
  in CI and the release workflow are pinned to `ubuntu-24.04` (matching the
  container smoke test); a guard test fails if either drifts back to
  `ubuntu-latest`. This fired for real when the runner rotated to 26.04 (glibc
  2.43) against a pinned `ubuntu:24.04` (glibc 2.39) smoke container.

## Commit attribution

A commit records who wrote the change, so attribute it to the person who
actually made it. Set `user.name` and `user.email` to your own identity before
committing, and never commit under another person's name.

- **Contributors** commit under their own name and email. Their work keeps their
  authorship; do not rewrite a contributor's commit to a project or maintainer
  identity.
- **Automated agents** commit under their own identity (for example
  `openhands <openhands@all-hands.dev>`) rather than the maintainer's, so the
  history says honestly that a machine produced the change. Do not add a
  `Co-authored-by` trailer naming a person who did not write or review the
  commit.
- **A misattributed commit** — the wrong author, or an agent's identity standing
  in for a person — is corrected with `git filter-repo --mailmap` for the
  identity and a `--message-callback` for the message, then a force-push of the
  affected branch. Rewriting published history is a last resort for fixing
  attribution, not a routine step.

The same expectations for people are in `CONTRIBUTING.md`.

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
