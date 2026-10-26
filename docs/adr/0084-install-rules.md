# 0084. Install rules and an installed-tree smoke test

Date: 2026-10-25

Status: Accepted

## Context

The shipping path had a hole at its last step. `fork_export` (ADR-0064) turns a
project into a runnable build and copies the player it was built beside, and the
web template (ADR-0071) produces a served directory. Neither gives a
*third party* a usable engine: there was no `cmake --install`, so the only way to
build against Fork Engine or to run its tools was from the source checkout. That
is enough for the engine's own CI and nothing else. A packager assembling a
distribution, or a developer who wants `fork_export` without cloning the engine,
had no supported path.

Two things made this a real gap rather than a missing convenience:

* The tools are part of the shipping path, not the editor — `fork_pack`,
  `fork_player`, and `fork_export` are built against the engine's public headers
  only, exactly as an external tool would be. They are the tools a shipped
  pipeline calls, and none of them could be obtained without the checkout.
* The engine's public headers are a real API (`engine/include/fe/**`, 145 files
  across 25 modules), and `target_include_directories` already carried an
  `$<INSTALL_INTERFACE:include>` generator expression — a promise of installability
  that no install rule honoured. The interface was written as if an installed
  consumer existed, and none could.

There was also no test for the installed tree. A missing library, a wrong RPATH,
or a header that lands at the wrong depth is invisible in a build tree, where
everything is found by absolute path; it fails at a user's first run instead.

## Decision

Add install rules, put them behind one switch, and test the installed tree the
way a consumer sees it.

1. **One place decides the layout.** `cmake/FEInstall.cmake` owns the install
   rules: `fe_install_target()` places a target's runtime, library, and archive
   artifacts via GNUInstallDirs, and `fe_install_header_tree()` installs a header
   tree under the include root. Each subdirectory calls these rather than writing
   its own `install()` calls, so the engine, the editor, and the three tools
   cannot disagree about where a thing goes.

2. **Windows libraries land in `bin`.** A shared library on Windows must sit
   beside the executable that loads it, so `FE_INSTALL_LIBDIR` is `bin` there and
   `lib` (or `lib64`) everywhere else. This is the one platform special case, and
   it is made once.

3. **A relative RPATH on Linux, on by default.** Installed executables get
   `$ORIGIN/../lib`, so a relocated tree runs without `LD_LIBRARY_PATH`. It is
   `FE_INSTALL_RPATH`, on by default, and a no-op on Windows and macOS.

4. **Only our own targets install.** The examples, the unit tests, and the
   benchmark do not: they are development artifacts. The fetched dependencies do
   not either — they are static and already inside the engine library. doctest
   ships its own `install()` rules, which would drop its headers and a CMake
   package into the installed tree; `DOCTEST_NO_INSTALL` is set before the fetch
   so a test framework never reaches a shipped tree.

5. **`FE_INSTALL=OFF` for a build tree that is never installed.** The CI jobs that
   only test can turn the rules off; the default is on, so `cmake --install` works
   out of the box. The Emscripten toolchain sets it off, because a web build ships
   as an export directory (the shell, the pack, the wasm pair), not as an installed
   tree — `cmake --install` would scatter the `.js`/`.wasm` glue into `bin/`, which
   is not how a browser build is served. The export template is the web packaging
   path (ADR-0071).

6. **The installed tree is tested as a consumer sees it.**
   `tests/install/run_install_smoke.sh` installs to a temporary prefix, asserts
   the layout (`bin`, `lib`, `include/fe`, `share/fork-engine`), and runs every
   installed tool with `--help` from that prefix. A missing library or a broken
   RPATH fails there rather than at a user's first run. A Linux CI step runs it
   after the export smoke test.

## Consequences

* A packager, or a developer without the checkout, can `cmake --install` and use
  the engine's tools; the tools' own `--help` is the documented interface.
* The `$<INSTALL_INTERFACE:include>` generator expression is now honoured rather
  than aspirational.
* There is deliberately **no** `find_package(ForkEngine)` config yet. An installed
  tree is runnable, not yet linkable from another CMake project. Adding the export
  set and the config file is a separate change, because it has to name the fetched
  dependencies (GLFW, nlohmann/json, stb, glad, miniaudio), which are not
  installed. A half-working config would mislead a consumer more than its absence;
  the absence is recorded in `AGENTS.md` and here.
* The install smoke test is Linux-only in CI for now. The layout rules are
  platform-aware, so a Windows or macOS step can be added when one is wanted; the
  script itself is portable and takes a build directory.

## See also

* ADR-0064 — export runtime and player (the tools this makes installable).
* ADR-0071 — the web-first 2D shipping path.
* `AGENTS.md` — the install command and the layout.
