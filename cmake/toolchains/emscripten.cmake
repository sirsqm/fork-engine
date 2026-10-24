# Emscripten toolchain for the Fork Engine web target.
#
# Configure the web build with:
#
#   cmake -S . -B build-web -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/emscripten.cmake
#   cmake --build build-web --parallel
#
# `emcmake cmake ...` is the alternative and is what CI uses
# (docs/adr/0073-emscripten-toolchain.md). This file exists because it is checked
# in and reviewable, and because the web build needs decisions that `emcmake`
# alone does not make (see below).
#
# What it does:
#   * selects the emscripten toolchain that ships inside emsdk, via the
#     EMSCRIPTEN_ROOT cache variable (defaults to $EMSDK/upstream/emscripten,
#     which emsdk's environment provides);
#   * makes the configure a *cross* configure to wasm32, which is what tells
#     CMake that the host's system libraries are not the target's;
#   * marks the target platform as web so cmake/FEPlatform.cmake defines
#     FE_PLATFORM_WEB (and not FE_PLATFORM_DESKTOP);
#   * turns the desktop-only build options off by default: the editor is a
#     desktop authoring tool with no business in a shipped game, the unit tests
#     exercise the desktop paths, and the benchmark gate is a CI tool. A web
#     configure may still override any of them.
#
# What it deliberately does not do: it does not fetch or install emsdk. The ADR
# records that an automation run does not download emsdk (it is a large download
# and would consume a whole run's budget); the CI job installs and caches it.
# A configure without emsdk fails here, at the toolchain check, with a message
# that says so.

if(NOT DEFINED EMSCRIPTEN_ROOT)
  if(DEFINED ENV{EMSDK})
    set(EMSCRIPTEN_ROOT "$ENV{EMSDK}/upstream/emscripten")
  else()
    set(EMSCRIPTEN_ROOT "")
  endif()
endif()

if(NOT EXISTS "${EMSCRIPTEN_ROOT}/cmake/Modules/Platform/Emscripten.cmake")
  message(
    FATAL_ERROR
      "Emscripten not found (looked for \"${EMSCRIPTEN_ROOT}/cmake/Modules/Platform/Emscripten.cmake\").\n"
      "Install and activate emsdk, then configure again:\n"
      "  git clone https://github.com/emscripten-core/emsdk.git && cd emsdk\n"
      "  ./emsdk install latest && ./emsdk activate latest && source ./emsdk_env.sh\n"
      "Alternatively pass -DEMSCRIPTEN_ROOT=/path/to/emscripten."
  )
endif()

# Emscripten's platform module does the toolchain, sysroot, and ABI setup. It
# also sets CMAKE_SYSTEM_NAME to Emscripten, which is what fe_detect_platform()
# reads to classify the target as web.
include("${EMSCRIPTEN_ROOT}/cmake/Modules/Platform/Emscripten.cmake")

# The engine's assets and generated pack are read at run time and served from a
# single virtual filesystem, so a web build produces a JS/HTML pair rather than a
# native executable.
set(CMAKE_EXECUTABLE_SUFFIX ".js")

# Web defaults. These are normal cache variables, so a caller can override them
# on the command line; the editor has no web path yet, and the desktop-only
# quality jobs (tests, benchmarks) are not meaningful for a wasm configure.
set(FE_BUILD_EDITOR OFF CACHE BOOL "Build the Fork Engine editor" FORCE)
set(FE_BUILD_TESTS OFF CACHE BOOL "Build the Fork Engine unit tests" FORCE)
set(FE_BUILD_BENCHMARKS OFF CACHE BOOL "Build the Fork Engine benchmarks" FORCE)

# FE_USE_SYSTEM_DEPS=ON would look for host GLFW/nlohmann/doctest, none of which
# are linkable into wasm; the fetches are pinned and cross-compile cleanly.
set(FE_USE_SYSTEM_DEPS OFF CACHE BOOL "Prefer system-installed dependencies over FetchContent" FORCE)
