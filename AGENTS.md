# AGENTS.md — repository notes for automated contributors

Fork Engine is a modular C++20 2D/3D game engine. Read `docs/ARCHITECTURE.md`
for the design, `docs/STRATEGY.md` for the three shaping capabilities, and
`docs/ROADMAP.md` for the milestone plan before changing anything.

## Strategy: the three pillars

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
- **Editing is collaborative** (M10, ADR-0019). Edits become serializable
  operations over stably identified objects, replicated through an ordered log
  with an authoritative server. It lands after the editor is mature; its first
  steps (component identity, operations, log) are local and headless and also
  strengthen undo/redo.

These are plans, not shipped code. Do not present a planned item as done.

## Build and test

```sh
cmake -S . -B build -DFE_WARNINGS_AS_ERRORS=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

CI builds every push on Linux/GCC (Debug+Release), macOS/Clang (Debug), and
Windows/MSVC (Debug), with warnings as errors. It also runs a clang-format
check. Keep all five green; a warning on one compiler is a real defect, not
noise.

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

## Portability traps seen before

- Raw-pointer identity checks in doctest (`CHECK(a == b)` on `T*`) fail to
  compile on MSVC because it tries the `shared_ptr` `operator<<`. Wrap the
  expression in an extra pair of parentheses: `CHECK((a == b))`.
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
