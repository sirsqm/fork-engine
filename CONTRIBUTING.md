# Contributing to Fork Engine

Thanks for your interest in Fork Engine. This document explains how to build the
project, the conventions we follow, and what a good contribution looks like.

The project is early. The most valuable contributions right now are things that
make the foundation solid: bug fixes, tests, documentation, and small, complete
improvements to existing systems. Please open an issue before starting a large
feature so we can agree on the design first.

Development happens in the open: work lands on `main` through reviewable pull
requests, architectural decisions are recorded as ADRs in `docs/adr/`, and the
roadmap's [near-term priorities](docs/ROADMAP.md#near-term-priorities) say what
is being worked on next. The maintainer decides direction, ADRs record the
reasoning, and anyone can propose a change.

## Ways to contribute

- **Report a bug.** Open an issue with a minimal reproduction, what you expected,
  and what happened.
- **Fix a bug or add a small feature.** Keep the change focused; one concern per
  pull request.
- **Improve the docs.** Architecture notes, examples, and clarifications are all
  welcome.
- **Add tests.** Coverage of existing behaviour is always useful.
- **Build a game in the engine.** The single most useful contribution right now
  is a small, complete game built end to end (roadmap M2.5) — it proves the
  systems work together and exposes the gaps that matter.

Two capabilities shape where the project is going — performance, and shipping to
every platform players use — and are planned in
[`docs/ROADMAP.md`](docs/ROADMAP.md). Work that advances either is especially
welcome; check [`docs/SHIPPING_QUEUE.md`](docs/SHIPPING_QUEUE.md) first so we do
not duplicate effort.

## Getting started

### Prerequisites

- A C++20 compiler (GCC 12+, Clang 15+, or MSVC 19.3+)
- CMake 3.24+
- On Linux, the X11 or Wayland development headers required by GLFW
- Optional: `clang-format` and `clang-tidy` for style and static analysis

### Build and test

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
cd build && ctest --output-on-failure
```

Dependencies are fetched and pinned automatically. Useful options:

| Option | Effect |
| --- | --- |
| `-DFE_BUILD_TESTS=OFF` | Skip the tests (faster build) |
| `-DFE_BUILD_EXAMPLES=OFF` | Skip the example programs |
| `-DFE_USE_SYSTEM_DEPS=ON` | Use system packages instead of fetching |
| `-DFE_WARNINGS_AS_ERRORS=ON` | Fail the build on any warning (what CI uses) |
| `-DFE_ENABLE_ASAN=ON` | Enable AddressSanitizer + UBSan in Debug |

On a machine without a display, run the graphics tests under a virtual one:

```bash
xvfb-run -a -s "-screen 0 640x480x24" ./build/bin/fork_graphics_tests
```

## Coding conventions

The code is C++20 and formatted with `clang-format` (see `.clang-format`). Run
`clang-format -i` on the files you touch before committing.

- **Naming.** `PascalCase` for types, `snake_case` for functions, variables, and
  namespaces, `kPascalCase` for constants, trailing underscore for private
  members (`vertices_`).
- **Types.** Use the engine's fixed-width aliases (`u32`, `i32`, `f32`, `f64`,
  `usize`) rather than the raw built-in types.
- **Headers.** Public headers live under `engine/include/fe/<module>/` and use
  `#pragma once`. A public header must not leak a third-party dependency (for
  example, GLFW appears only as a forward-declared handle).
- **Errors.** Return `fe::Error` or `fe::Result<T>` for expected failures. Do not
  throw. Reserve `FE_ASSERT` (debug) and `FE_CHECK` (always-on) for programming
  errors, never for recoverable ones.
- **Ownership.** Express ownership in types: `std::unique_ptr` for exclusive
  ownership, `std::shared_ptr` for shared resources, references and spans for
  borrows. Prefer value types for small data.
- **Comments.** Comment the *why*, not the *what*. Explain invariants, ordering
  requirements, and deliberate trade-offs. Do not narrate the diff.
- **Warnings.** The build is expected to be warning-clean with `-Wall -Wextra
  -Wpedantic`. CI builds with `-Werror`.

## Module boundaries

Modules may depend only "downwards" (see `docs/ARCHITECTURE.md`). When you add a
dependency edge, it must point down this graph; a new edge is an architectural
change and should be called out in the pull request.

```
core  <-  platform  <-  rhi  <-  resources  <-  scene  <-  renderer2d
core  <-  reflect  <-  scene, resources
core  <-  serialization  <-  scene, resources, reflect
core, scene, resources  <-  systems
core, scene, systems, reflect  <-  physics2d
core, scene, resources  <-  mesh  <-  renderer3d
core, scene, resources  <-  scene3d
core, scene, systems, resources  <-  audio
everything  <-  runtime
```

The editor (`editor/`) sits outside the engine library and may depend on it, never
the reverse.

If you need an architectural change, write a short ADR in `docs/adr/` (copy the
template used by the existing records) and reference it from the pull request.

## Tests

- Add tests for new behaviour and for bug fixes. A bug fix without a test that
  would have caught it is incomplete.
- Put pure-logic tests in `tests/src/`. Graphics tests that need a GL context go
  in `tests/src/test_renderer2d.cpp` and must skip cleanly when no context is
  available. Editor tests that need only editor logic go in the editor test
  target and link `fork_editor_core`, never the UI toolkit.
- Test real code paths. Avoid mocks; if you must use one, justify it in the pull
  request.
- Write tests that are platform-independent. Paths, line endings, and locale are
  common sources of Windows-only failures; compare `std::filesystem::path` values
  rather than raw strings, and wrap raw-pointer identity checks in an extra pair
  of parentheses for doctest (`CHECK((a == b))`).
- Run the whole suite before opening a pull request:

  ```bash
  cd build && ctest --output-on-failure
  ```

## Pull request process

1. Fork the repository and create a branch from `main` with a descriptive name.
2. Make focused changes. Keep the diff small enough to review carefully.
3. Ensure the build is warning-clean and all tests pass.
4. Update documentation when you change an interface or add a subsystem.
5. Open the pull request with a clear description: what changed, why, how it was
   tested, and any trade-offs or follow-ups.
6. Be responsive to review. A reviewer may ask for a change in approach; that is
   a design discussion, not a rejection.

**Do not claim that code compiles, tests pass, or a feature works unless you have
actually verified it.** If you could not run the build or the tests (no display,
no compiler, and so on), say so explicitly in the pull request.

## Commit messages

Write a short imperative subject line ("Add 2D sprite batching"), and a body that
explains the motivation and any non-obvious decisions when the change is not
self-evident. Reference issues where relevant (`Fixes #12`).

## Reporting bugs and requesting features

Use the GitHub issue tracker. For bugs, include:

- what you did, what you expected, and what happened;
- the platform, compiler, and CMake options you used;
- a minimal reproduction if you can produce one.

For features, describe the problem you are trying to solve before proposing a
specific implementation. That keeps the conversation about the right thing.

## License

By contributing, you agree that your contributions are licensed under the
project's [MIT License](LICENSE).
