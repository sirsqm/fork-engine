# ADR-0073: The Emscripten toolchain and the web platform macro

- **Status:** Accepted
- **Date:** 2026-10
- **Related:** [ADR-0071](0071-web-first-2d-shipping-path.md), [ADR-0072](0072-glsl-dialect-seam.md),
  [ADR-0032](0032-near-term-platform-scope.md), [ADR-0041](0041-platform-contract-and-null-backend.md)

## Context

[ADR-0071](0071-web-first-2d-shipping-path.md) made web the first shipping target.
[ADR-0072](0072-glsl-dialect-seam.md) gave the renderer a dialect seam so the same
shader sources can be emitted as GLSL ES 3.00. Neither is reachable without a way
to *build* the engine for WebAssembly, and the build had no such path: the only
platform decision in the tree was `FE_PLATFORM_DESKTOP=1` (plus a per-OS macro)
defined in `cmake/FESettings.cmake` from `UNIX`/`APPLE`/`WIN32`.

That decision is wrong for a cross build. Those are *host* variables: a configure
running on Linux with an Emscripten toolchain sets `UNIX` for the host while
targeting wasm32, so the old code would have claimed the target was desktop
Linux. The web target needs a classification that reads the *target* platform,
and it needs the classification to be observable — a desktop-only test suite
cannot notice that the web branch is broken, because it never takes it.

Two dependencies also have to stop being unconditional. GLFW cannot be compiled
to wasm at all, and glad's loader resolves GL entry points against a driver that
a browser does not expose (WebGL2 supplies its own). Both are desktop-only, and
the build must say so rather than the sources.

## Decision

**1. One platform classifier, in `cmake/FEPlatform.cmake`, keyed on
`CMAKE_SYSTEM_NAME` first.** `fe_detect_platform()` classifies the target as
`web | windows | macos | linux | android | ios | unix | unknown`, reading
`CMAKE_SYSTEM_NAME` before falling back to `WIN32`/`APPLE`/`UNIX`. A toolchain
file sets `CMAKE_SYSTEM_NAME` before `project()`, which is exactly what makes it
correct for a cross build.

**2. Exactly one of `FE_PLATFORM_DESKTOP` / `FE_PLATFORM_WEB`.**
`fe_apply_platform_definitions()` defines `FE_PLATFORM_WEB=1` for the web target
and `FE_PLATFORM_DESKTOP=1` plus the per-OS macro otherwise. A source that asks
"is there a desktop OS?" cannot accidentally take both paths. The per-OS macro
(`FE_PLATFORM_LINUX` and friends) is deliberately *not* defined for web: web is
not Linux.

**3. Detection is a pure function so it can be tested without the toolchain.**
The classifier creates no target and calls no `add_*`, so
`cmake/tests/test_fe_platform.cmake` drives it in `cmake -P` script mode and
asserts every classification, including `Emscripten -> web`. That test is the
acceptance evidence for this ADR in a desktop-only CI: it fails if anyone makes
the web target look like desktop Linux.

**4. A checked-in toolchain file, `cmake/toolchains/emscripten.cmake`.** It
includes emsdk's own `Platform/Emscripten.cmake` from `EMSCRIPTEN_ROOT`
(defaulting to `$EMSDK/upstream/emscripten`) and adds what `emcmake` alone does
not: web defaults for the build options (`FE_BUILD_EDITOR`, `FE_BUILD_TESTS`,
`FE_BUILD_BENCHMARKS`, `FE_USE_SYSTEM_DEPS` all `OFF`, each overridable) and a
`.js` executable suffix. It fails loudly, at configure time, with installation
instructions, when emsdk is absent.

**5. glad and GLFW are skipped for the web target, in
`cmake/FEDependencies.cmake`.** The web configure neither creates `FE::glad` nor
resolves GLFW, so nothing can accidentally link either. `FE_GLFW_TARGET` is
empty for web; `fe_mark_system()` is guarded accordingly.

## The documented, reproducible commands

With emsdk installed and active (`source ./emsdk_env.sh`), the web build is:

```sh
cmake -S . -B build-web -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/emscripten.cmake
cmake --build build-web --parallel
```

The equivalent via emsdk's wrapper, which is what the CI job (a later queue item)
uses, and which also works without activating the environment:

```sh
emcmake cmake -S . -B build-web
cmake --build build-web --parallel
```

Both classify the target as `web`, define `FE_PLATFORM_WEB=1`, skip the editor,
tests, benchmarks, glad, and GLFW, and produce `build-web/.../*.js`.

## Consequences

- The desktop build is unchanged: the same definitions at the same scope, and
  the new test is the only addition to the suite.
- The web build configures and compiles the non-windowing platform modules;
  GLFW's replacement (a canvas-backed window and WebGL2 context) is
  `web-window-backend`, a later queue item, so a web *link* is expected to be
  incomplete until then.
- `emcmake` remains a supported entry point, so the toolchain file is a
  convenience over the standard path rather than a requirement. CI can use
  whichever it likes; the classification is the same because both set
  `CMAKE_SYSTEM_NAME=Emscripten`.
- Automation runs do not install emsdk (it is a large download); the classifier
  test is what keeps this step honest until CI runs the real thing.
