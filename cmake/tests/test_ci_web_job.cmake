# Test for the web CI job in .github/workflows/ci.yml, run by ctest in script
# mode.
#
# The web job is the only thing that stops the wasm build from rotting between
# runs (docs/PLAN.md, `web-smoke-test-ci`), and nothing in the desktop
# suite can observe it: a green desktop build says nothing about whether the job
# still configures the web target, still builds the player, or still runs the
# browser smoke test. A workflow is YAML, not code, so it is checked here the
# same way cmake/FEPlatform.cmake is: by reading it as text and asserting the
# decisions it has to make.
#
# The failure this guards against is a silent one. The job originally configured
# with -DFE_BUILD_EXAMPLES=OFF, which still compiles the engine library, so the
# job stayed green while fork_player -- the runtime an exported web build
# actually runs -- was never built for wasm at all.
#
# Run by tests/CMakeLists.txt:
#   cmake -DCI_WORKFLOW=<path> -P cmake/tests/test_ci_web_job.cmake

if(NOT DEFINED CI_WORKFLOW)
  message(FATAL_ERROR "CI_WORKFLOW must point at .github/workflows/ci.yml")
endif()

if(NOT EXISTS "${CI_WORKFLOW}")
  message(FATAL_ERROR "CI_WORKFLOW does not exist: ${CI_WORKFLOW}")
endif()

file(READ "${CI_WORKFLOW}" _workflow)
string(REPLACE "\r\n" "\n" _workflow "${_workflow}")

# The jobs are the keys indented by exactly two spaces. Take the `web:` block as
# everything from its key up to the next such key, so a check cannot accidentally
# be satisfied by a different job.
string(REPLACE "\n" ";" _lines "${_workflow}")
set(_web_job "")
set(_in_web FALSE)
set(_found_web FALSE)
foreach(_line IN LISTS _lines)
  if(_line MATCHES "^  ([A-Za-z0-9_-]+):[ \t]*$")
    if(_in_web)
      break()
    endif()
    if(CMAKE_MATCH_1 STREQUAL "web")
      set(_in_web TRUE)
      set(_found_web TRUE)
    endif()
  endif()
  if(_in_web)
    string(APPEND _web_job "${_line}\n")
  endif()
endforeach()

if(NOT _found_web)
  message(FATAL_ERROR "ci.yml has no `web:` job; the wasm build is unguarded")
endif()

# check: <label>|<literal substring the web job must contain>
# `|` rather than `;` because a `;` inside a set() argument is a list separator,
# and not `=` because several of the needles contain one.
set(_checks
    "installs emsdk|emsdk.git"
    "pins the emsdk version|EMSDK_VERSION:"
    "installs the pinned version|install \"\$EMSDK_VERSION\""
    "caches emsdk|actions/cache@v4"
    "cache key carries the version|emsdk-\${{ env.EMSDK_VERSION }}"
    "uses the checked-in toolchain|-DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/emscripten.cmake"
    "builds the shipping tools|-DFE_BUILD_EXAMPLES=ON"
    "asserts the player js exists|test -f build-web/bin/fork_player.js"
    "asserts the player wasm exists|test -f build-web/bin/fork_player.wasm"
    "runs the browser smoke test|tests/web/run_web_smoke.sh"
    "runs the exported game in a browser|tests/web/run_web_game.sh"
    "publishes the export for the upload|FE_WEB_GAME_KEEP_DIR:"
    "uploads the exported game|actions/upload-artifact@v4"
    "names the artifact|name: web-game"
    "uploads the published export|path: \${{ runner.temp }}/web-game-export"
)

foreach(_check IN LISTS _checks)
  string(REPLACE "|" ";" _parts "${_check}")
  list(GET _parts 0 _label)
  list(GET _parts 1 _needle)
  string(FIND "${_web_job}" "${_needle}" _at)
  if(_at EQUAL -1)
    message(FATAL_ERROR "web job is missing '${_label}' (looked for: ${_needle})")
  endif()
  message(STATUS "  ${_label}: ok")
endforeach()

# The browser checks are the only steps that prove the web path runs rather than
# merely compiling, so they must not be allowed to disappear behind a
# `continue-on-error` or an `if: false`.
if(_web_job MATCHES "continue-on-error")
  message(FATAL_ERROR "web job sets continue-on-error; a web failure must fail the build")
endif()

message(STATUS "CI web job: builds the player for wasm and runs the browser checks")
