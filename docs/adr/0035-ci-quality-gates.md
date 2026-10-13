# ADR-0035: CI quality gates - sanitizers and an optimised warnings build

- **Status:** Accepted
- **Date:** 2026-10
- **Related:** [ADR-0003](0003-error-handling.md),
  [ADR-0017](0017-performance-strategy.md)

## Context

The CI workflow (`.github/workflows/ci.yml`) grew with the engine and now does
three things: a build-and-test matrix (Linux/GCC in Debug and Release,
macOS/Clang Debug, Windows/MSVC Debug), a `clang-format` check, and a Linux
headless smoke test of the sandbox and the editor. Every job configures with
`-DFE_WARNINGS_AS_ERRORS=ON` and runs `ctest`.

Two gaps remained, both of them the kind that only bite after a release:

- **No sanitizers in CI.** `CMakeLists.txt` already offers `-DFE_ENABLE_ASAN=ON`
  (AddressSanitizer + UndefinedBehaviorSanitizer, Debug only) as a local
  convenience, but nothing in CI runs it. Memory errors, use-after-free, and
  undefined behaviour are exactly the defects a normal Debug or Release build
  does not reliably surface, and the codebase is still small enough that keeping
  a sanitizer run green is cheap. The roadmap's near-term priority #4 names
  sanitizer builds alongside the benchmark gate for this reason.
- **Warnings were only checked on the unoptimised path.** The matrix builds
  Debug on Linux and both Debug and Release on the release-critical path only
  through the existing matrix entry; optimisation-sensitive warnings (aliasing,
  unused results that the optimiser changes, `-Wmaybe-uninitialized` fired only
  with inlining) are a Release-class concern that a Debug build can miss. A
  dedicated optimised warnings build closes that gap.

The forces:

- **A gate that is not run is not a gate.** The `-DFE_ENABLE_ASAN` option and
  `-DFE_WARNINGS_AS_ERRORS` already exist; the missing piece is running them per
  push so a regression is caught by the machine, not by a maintainer's memory.
- **UBSan does not fail a run by default.** It prints and continues. A sanitizer
  job that lets UB scroll past in the log is worse than none: it looks green
  while a defect ships.
- **Sanitizer builds are slow.** An instrumented Debug build of the whole engine
  plus its fetched dependencies takes minutes; re-doing it from scratch on every
  push is where the cost is, and where caching pays.
- **Do not over-engineer.** A small number of reliable jobs beats a large matrix
  of flaky ones. The existing matrix already covers three OSes and two build
  types; the new work is two Linux jobs and sharing the existing dependency
  setup, not a new axis of combinations.

## Decision

**Add two Linux jobs to `ci.yml` - a sanitizer build-and-test and an optimised
warnings-only build - and give both a ccache-backed build cache. Keep the
existing `build-and-test` and `format-check` jobs unchanged.**

### The sanitizer job

- `ubuntu-latest`, GCC, `-DCMAKE_BUILD_TYPE=Debug -DFE_ENABLE_ASAN=ON
  -DFE_WARNINGS_AS_ERRORS=ON`, then `ctest` under `xvfb-run` (the graphics tests
  need a GL context, so it installs the same Linux dependencies and the virtual
  display the main matrix does).
- Warnings-as-errors is on, so the instrumented build is held to the same warning
  bar as every other job.
- `UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`, so a finding aborts the job
  rather than printing past it. This is the single most important detail of the
  job: without it UBSan would not fail the build.
- **Leak detection is on for the engine and editor tests and off for the run
  that includes the GL binaries.** LeakSanitizer's exit-time report is clean for
  the pure-logic tests (`engine_tests` is 390 cases / 7163 assertions with no
  leak), but the tests that create a GL context report leaks whose stacks are
  entirely third-party - Mesa/llvmpipe, GLFW and X11, symbolized only as
  `<unknown module>` - with no engine frame in any stack. The untouched
  `fork_sandbox` example reports the same leaks under the same build, which
  confirms the source is the platform stack, not the engine. The job therefore
  runs `ctest -R 'engine_tests|editor_tests'` with `detect_leaks=1` and the full
  suite with `detect_leaks=0`, reusing the one build. Every other sanitizer check
  (buffer overflow, use-after-free, double-free, and all UBSan checks) still runs
  across the whole suite, graphics included.

### The warnings job

- `ubuntu-latest`, GCC, `-DCMAKE_BUILD_TYPE=RelWithDebInfo
  -DFE_WARNINGS_AS_ERRORS=ON`, build only. RelWithDebInfo is chosen over Release
  so the optimiser runs (which is the point) while a developer can still read a
  stack trace if one is ever needed. It does not run `ctest`: the existing
  matrix already runs the suite in both Debug and Release, so repeating it here
  would spend minutes to learn nothing new.

### Caching

- Each new job sets `CCACHE_DIR` to a workspace-local directory and configures
  with `-DCMAKE_C{XX,}_COMPILER_LAUNCHER=ccache`, then caches that directory with
  `actions/cache@v4`. The cache key is `hashFiles('**/CMakeLists.txt', 'cmake/**')`,
  so the cache is invalidated when the build configuration changes and reused
  across pushes otherwise (with a `restore-keys` prefix so a partial hit still
  helps). ccache is asked for in the same `apt-get install` as the other
  dependencies, so there is no separate setup step.
- The plain matrix jobs are left uncached on purpose. They build a small project
  on hosted runners whose ephemeral state is already cold; adding a cache there
  would spend more time restoring than it saves and would add a moving part.

### What was deliberately not done

- **A nightly scheduled sanitizer run.** The task allowed one only where a
  benchmark target exists to gate on. No benchmark target exists on `main`, and
  creating one would require editing `CMakeLists.txt`, which this change is
  scoped out of. The sanitizer job runs per push instead, which is strictly
  stronger while the suite is cheap; a nightly sweep becomes worthwhile only
  once the sanitizer run can no longer afford to be per-push.
- **Sanitizers on macOS/Windows.** Apple Clang and MSVC sanitizer support is
  narrower and the Windows path cannot use the CMake option as written
  (`FE_ENABLE_ASAN` is a no-op under MSVC by design in `CMakeLists.txt`). Linux
  GCC is where the option is proven and where the graphics tests can run under a
  virtual display, so that is where the job lives.

## Consequences

**Easier:**

- Memory and undefined-behaviour regressions are caught per push, with the exact
  finding and a stack trace, instead of surviving until someone runs a sanitizer
  build by hand.
- Warning regressions on the optimised path have a job to fail, so the
  `-DFE_WARNINGS_AS_ERRORS=ON` bar is enforced everywhere it is meaningful, not
  only where a Debug build happens to prove it.
- ccache keeps the two new jobs' incremental cost low: after the first run the
  unchanged translation units are replayed from the cache, so day-to-day pushes
  pay mostly for what actually changed.

**Harder:**

- Two more jobs run on every push to `main` and every PR. The sanitizer job is
  the slow one (instrumented Debug across the whole engine); if the matrix ever
  needs trimming to fit a runner budget, this is the job to make nightly, and
  the ADR records that choice as available rather than taken.
- The ccache keys hash `CMakeLists.txt` and `cmake/**` but not compiler version;
  a runner image bump that changes the toolchain will produce cache misses until
  the caches age out. The `restore-keys` prefix keeps a partial hit useful in
  the meantime, and a stale cache never causes a wrong result - ccache validates
  the compiler and flags it was built with.
- A new sanitizer failure blocks the push. That is the intent, but it means a
  pre-existing latent defect surfaced by enabling the job must be fixed rather
  than silenced; the job is not configured to tolerate findings.

## Alternatives considered

- **Enable sanitizers inside the existing `build-and-test` matrix.** Rejected:
  it would multiply the matrix (a sanitizer entry per OS/build type) or silently
  apply sanitizers to jobs that do not need them, and it would lose the chance
  to give the sanitizer build its own cache key and its own failure signal. A
  separate job keeps the matrix readable.
- **Rely on `-DFE_WARNINGS_AS_ERRORS=ON` in the existing jobs and add no
  warnings job.** Rejected: the matrix's optimised (Release) entry is the only
  place optimiser-sensitive warnings fire, and coupling the warnings bar to a
  job whose purpose is the test matrix makes a warning regression look like a
  test failure. A dedicated build says what it is for.
- **Let UBSan default to print-and-continue.** Rejected outright: it would make
  the job green on undetected UB. `halt_on_error=1` is the difference between a
  gate and a log line.
- **Gate on LeakSanitizer for the whole suite, GL binaries included.** Rejected
  after measuring: the leaks those binaries report are entirely in the
  Mesa/llvmpipe/GLFW/X11 stack (`<unknown module>`), reproduce on the untouched
  `fork_sandbox`, and involve no engine frame, so a hard gate would be
  permanently red for allocations the project cannot free. Leak detection is kept
  where it is clean and meaningful (the engine and editor tests) and disabled
  only for the run that includes the GL binaries.
- **A nightly scheduled sanitizer job now.** Deferred as above: no benchmark
  target exists to gate on and creating one is outside this change's scope. Per
  push is stronger and still affordable.
- **A matrix of sanitizers (ASan, TSan, MSan) and build types.** Rejected as
  over-engineering for the current size: TSan and MSan would need whole-build
  and dependency support the project does not have, and the single ASan+UBSan
  Debug job is where the value is first.
- **Cache the CMake build directory instead of ccache.** Rejected: a restored
  build directory with stale generated files and absolute paths from another
  runner is a classic source of confusing incremental-build failures. ccache
  caches compiled objects keyed on the exact inputs and is safe to share.
