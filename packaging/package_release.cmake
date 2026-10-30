# Fork Engine release packaging.
#
# Turns one configured build tree into the per-OS archives a release serves, so
# the download step is a script the CI job and a maintainer run the same way
# rather than a sequence of commands typed into a workflow. Run it in CMake
# script mode:
#
#   cmake -DFE_BUILD_DIR=build -DFE_OUT_DIR=dist -DFE_PLATFORM=linux \
#         -DFE_ARCH=x86_64 -P packaging/package_release.cmake
#
# It produces two archives plus a checksum file, all named after the engine
# version, the platform, and the architecture:
#
#   fork-engine-<ver>-<platform>-<arch>-game.<ext>   the playable breakout build
#   fork-engine-<ver>-<platform>-<arch>-sdk.<ext>    the installed engine tree
#   SHA256SUMS                                       one line per archive
#
# The `game` archive is what a player downloads: the breakout binary, the assets
# and scene it loads, and a launcher that runs it from the archive root, which is
# the working directory the game resolves its paths against. The `sdk` archive is
# the `cmake --install` tree (ADR-0084): the library, the public headers, and the
# shipping tools, for a developer who wants the engine without the checkout.
#
# A `.tar.gz` is produced everywhere but Windows, where a `.zip` is what a
# downloader expects to be able to open without extra tooling. The extension is
# chosen once, here, so the workflow and the checksum file cannot disagree.
#
# After writing the archives it unpacks the game archive into a scratch
# directory and runs it, so "the artifact exists" is not mistaken for "the
# artifact works". The run needs a display on Linux (the game opens a GL
# context), which the caller supplies through FE_SMOKE_PREFIX (e.g. xvfb-run);
# on Windows and macOS the runner has a desktop and the prefix is empty.
#
# Inputs (all via -D):
#   FE_BUILD_DIR    required  a configured build tree with install rules on
#   FE_OUT_DIR      required  where the archives are written
#   FE_PLATFORM     required  linux | windows | macos
#   FE_ARCH         required  x86_64 | aarch64 | ...
#   FE_CONFIG       optional  build configuration for multi-config generators
#                             (default Release; ignored by single-config ones)
#   FE_VERSION      optional  defaults to the build tree's project version
#   FE_SMOKE        optional  ON (default) unpacks the game archive and checks it
#   FE_SMOKE_GAME   optional  OFF (default) also plays the unpacked game, which
#                             needs a GL context
#   FE_SMOKE_PREFIX optional  a command list prepended to the run (xvfb-run ...)
#   FE_SMOKE_FRAMES optional  frames for the smoke run (default 60)

cmake_minimum_required(VERSION 3.20)

foreach(_required FE_BUILD_DIR FE_OUT_DIR FE_PLATFORM FE_ARCH)
  if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
    message(FATAL_ERROR "${_required} must be set (see the header of this file)")
  endif()
endforeach()

get_filename_component(FE_BUILD_DIR "${FE_BUILD_DIR}" ABSOLUTE)
get_filename_component(FE_OUT_DIR "${FE_OUT_DIR}" ABSOLUTE)

if(NOT EXISTS "${FE_BUILD_DIR}/CMakeCache.txt")
  message(FATAL_ERROR "no configured build tree at ${FE_BUILD_DIR}")
endif()

# The version has one source of truth (the root project() call), and the build
# tree records it, so a release cannot be stamped with a version the binary does
# not carry. A caller may still pass one explicitly.
if(NOT DEFINED FE_VERSION OR "${FE_VERSION}" STREQUAL "")
  file(STRINGS "${FE_BUILD_DIR}/CMakeCache.txt" _version_line
       REGEX "^CMAKE_PROJECT_VERSION:STATIC=")
  if(NOT _version_line)
    message(FATAL_ERROR "cannot read the project version from ${FE_BUILD_DIR}/CMakeCache.txt")
  endif()
  string(REPLACE "CMAKE_PROJECT_VERSION:STATIC=" "" FE_VERSION "${_version_line}")
endif()

set(FE_SMOKE "ON" CACHE STRING "")
# Playing the packaged game needs a GL context; the structural and --help checks
# do not. Off by default so a caller without a display still exercises the
# artifact, and turned on where a context exists (Linux CI via Xvfb).
set(FE_SMOKE_GAME "OFF" CACHE STRING "")
set(FE_SMOKE_FRAMES "60" CACHE STRING "")

# Multi-config generators (Visual Studio, Xcode, Ninja Multi-Config) place
# artifacts under a per-configuration subdirectory of the output directory, and
# `cmake --install` needs to be told which one. The build tree records its
# generator, so the layout is derived from it rather than guessed from the
# platform: a MinGW or Ninja build on Windows is single-config and has no
# `bin/Release/`, while a Visual Studio build does.
set(FE_CONFIG "Release" CACHE STRING "")
file(STRINGS "${FE_BUILD_DIR}/CMakeCache.txt" _generator_line
     REGEX "^CMAKE_GENERATOR:INTERNAL=")
if(_generator_line MATCHES "Visual Studio" OR _generator_line MATCHES "Xcode"
   OR _generator_line MATCHES "Ninja Multi-Config")
  set(FE_MULTI_CONFIG TRUE)
else()
  set(FE_MULTI_CONFIG FALSE)
endif()

# The directory a runtime artifact lands in, which differs between generator
# kinds: `bin` for single-config, `bin/<config>` for multi-config. Everything
# that looks for a binary uses this, so the two cases are decided once.
if(FE_MULTI_CONFIG)
  set(FE_BIN_DIR "${FE_BUILD_DIR}/bin/${FE_CONFIG}")
  set(FE_INSTALL_CONFIG_ARG "--config" "${FE_CONFIG}")
else()
  set(FE_BIN_DIR "${FE_BUILD_DIR}/bin")
  set(FE_INSTALL_CONFIG_ARG "")
endif()

# Windows gets a zip; everything else a gzipped tarball. The extension, the
# CMake archive format, and the tar operation are decided together so they cannot
# drift: `cf` writes an *uncompressed* tar, so a `.tar.gz` name without the `z`
# would be a plain tar that only `tar xf` (or `cmake -E tar xf`, which sniffs the
# format) can read, and a player's `tar xzf` would fail. zip is already
# compressed, so it takes `cf`.
if(FE_PLATFORM STREQUAL "windows")
  set(FE_ARCHIVE_EXT "zip")
  set(FE_ARCHIVE_FORMAT "zip")
  set(FE_TAR_OP "cf")
else()
  set(FE_ARCHIVE_EXT "tar.gz")
  set(FE_ARCHIVE_FORMAT "gnutar")
  set(FE_TAR_OP "czf")
endif()

set(FE_STEM "fork-engine-${FE_VERSION}-${FE_PLATFORM}-${FE_ARCH}")
set(FE_GAME_ARCHIVE "${FE_OUT_DIR}/${FE_STEM}-game.${FE_ARCHIVE_EXT}")
set(FE_SDK_ARCHIVE "${FE_OUT_DIR}/${FE_STEM}-sdk.${FE_ARCHIVE_EXT}")

# The executable suffix comes from the target platform, not from the host: this
# script may run on Linux while packaging a Windows build, and `cmake -P` script
# mode does not set CMAKE_EXECUTABLE_SUFFIX to the *target's* value. Deriving it
# from FE_PLATFORM keeps the name a caller looks for and the name in the archive
# the same on every host.
if(FE_PLATFORM STREQUAL "windows")
  set(FE_EXE_SUFFIX ".exe")
else()
  set(FE_EXE_SUFFIX "")
endif()

function(fe_run)
  execute_process(COMMAND ${ARGN} RESULT_VARIABLE _rc)
  if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "command failed (${_rc}): ${ARGN}")
  endif()
endfunction()

file(MAKE_DIRECTORY "${FE_OUT_DIR}")
file(REMOVE "${FE_GAME_ARCHIVE}" "${FE_SDK_ARCHIVE}")

# The archive is compressed, not merely named as if it were. `cmake -E tar cf`
# writes an uncompressed tar that `cmake -E tar xf` still reads by sniffing the
# format, so a wrong tar operation would pass the smoke test below and fail only
# for a player running `tar xzf`. Asserting the gzip magic bytes makes that
# mistake fail here instead. zip is checked by its own magic.
function(fe_assert_archive_format archive)
  if(NOT EXISTS "${archive}")
    message(FATAL_ERROR "expected an archive at ${archive}")
  endif()
  file(READ "${archive}" _magic OFFSET 0 LIMIT 2 HEX)
  if(FE_ARCHIVE_FORMAT STREQUAL "zip")
    if(NOT _magic STREQUAL "504b")
      message(FATAL_ERROR "${archive} is not a zip (magic ${_magic})")
    endif()
  else()
    if(NOT _magic STREQUAL "1f8b")
      message(FATAL_ERROR "${archive} is not gzip-compressed (magic ${_magic})")
    endif()
  endif()
endfunction()

# ---------------------------------------------------------------------------
# The SDK archive: exactly the installed tree.
#
# `cmake --install` is the packaging primitive (ADR-0084), so the archive is a
# relocation of it rather than a second layout that could drift from what an
# installed consumer sees. The stage directory is the archive's top-level
# directory, so unpacking one archive next to another does not interleave them.
# ---------------------------------------------------------------------------
set(FE_SDK_STAGE "${FE_OUT_DIR}/stage-sdk/${FE_STEM}")
file(REMOVE_RECURSE "${FE_OUT_DIR}/stage-sdk")
file(MAKE_DIRECTORY "${FE_SDK_STAGE}")
fe_run("${CMAKE_COMMAND}" --install "${FE_BUILD_DIR}" ${FE_INSTALL_CONFIG_ARG} --prefix "${FE_SDK_STAGE}")
fe_run("${CMAKE_COMMAND}" -E tar "${FE_TAR_OP}" "${FE_SDK_ARCHIVE}" "--format=${FE_ARCHIVE_FORMAT}" "${FE_STEM}"
       WORKING_DIRECTORY "${FE_OUT_DIR}/stage-sdk")
file(REMOVE_RECURSE "${FE_OUT_DIR}/stage-sdk")
fe_assert_archive_format("${FE_SDK_ARCHIVE}")

# ---------------------------------------------------------------------------
# The game archive: the playable slice and the content it loads.
#
# The game resolves its scene and assets by paths relative to the working
# directory (`examples/breakout/scenes/...`), so the archive root must reproduce
# that shape and the launcher must run the binary from there. Reproducing the
# paths rather than teaching the game a packaged layout keeps the shipped binary
# the same one CI tests.
# ---------------------------------------------------------------------------
set(FE_GAME_STAGE "${FE_OUT_DIR}/stage-game/${FE_STEM}")
file(REMOVE_RECURSE "${FE_OUT_DIR}/stage-game")
file(MAKE_DIRECTORY "${FE_GAME_STAGE}/examples/breakout")

set(FE_GAME_BINARY "fork_breakout${FE_EXE_SUFFIX}")
if(NOT EXISTS "${FE_BIN_DIR}/${FE_GAME_BINARY}")
  message(FATAL_ERROR "no ${FE_GAME_BINARY} in ${FE_BIN_DIR}; build with FE_BUILD_EXAMPLES=ON")
endif()
file(COPY "${FE_BIN_DIR}/${FE_GAME_BINARY}" DESTINATION "${FE_GAME_STAGE}")
foreach(_dir assets scenes)
  file(COPY "${CMAKE_CURRENT_LIST_DIR}/../examples/breakout/${_dir}"
       DESTINATION "${FE_GAME_STAGE}/examples/breakout")
endforeach()
file(COPY "${CMAKE_CURRENT_LIST_DIR}/../LICENSE" DESTINATION "${FE_GAME_STAGE}")

# A launcher, so a player runs the game from the directory it expects without
# knowing that. Both are written; only the one for the platform is meaningful,
# and having the other costs a few bytes.
file(WRITE "${FE_GAME_STAGE}/run.sh"
"#!/usr/bin/env sh
# Run Fork Engine's breakout from the directory it resolves its content against.
cd \"$(dirname \"$0\")\" || exit 1
exec ./${FE_GAME_BINARY} \"$@\"
")
# `file(WRITE)` creates the file 0644, and the mode travels into the archive: a
# player who unpacks the tarball and runs the documented `./run.sh` gets
# "Permission denied". The smoke test below runs the binary directly, so it never
# saw this; the launcher has to be executable where it is written, not where it
# is run.
file(CHMOD "${FE_GAME_STAGE}/run.sh"
     PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
file(WRITE "${FE_GAME_STAGE}/run.bat"
"@echo off
rem Run Fork Engine's breakout from the directory it resolves its content against.
cd /d \"%~dp0\"
\"${FE_GAME_BINARY}\" %*
")
file(WRITE "${FE_GAME_STAGE}/README.txt"
"Fork Engine ${FE_VERSION} - breakout

A small, complete game built on Fork Engine. Run it with:

  Linux/macOS:  ./run.sh
  Windows:      run.bat

Add --headless 300 to run without a window, which is what a smoke test does.

Fork Engine is MIT licensed; see LICENSE.
")

fe_run("${CMAKE_COMMAND}" -E tar "${FE_TAR_OP}" "${FE_GAME_ARCHIVE}" "--format=${FE_ARCHIVE_FORMAT}" "${FE_STEM}"
       WORKING_DIRECTORY "${FE_OUT_DIR}/stage-game")
file(REMOVE_RECURSE "${FE_OUT_DIR}/stage-game")
fe_assert_archive_format("${FE_GAME_ARCHIVE}")

# ---------------------------------------------------------------------------
# Checksums.
#
# Written from the archives that were just produced, so a download can be
# verified against the release rather than against a remembered hash.
# ---------------------------------------------------------------------------
set(FE_SUMS "")
foreach(_archive "${FE_GAME_ARCHIVE}" "${FE_SDK_ARCHIVE}")
  execute_process(COMMAND "${CMAKE_COMMAND}" -E sha256sum "${_archive}"
                  OUTPUT_VARIABLE _line OUTPUT_STRIP_TRAILING_WHITESPACE)
  string(REGEX REPLACE "^([0-9a-f]+) .*" "\\1" _hash "${_line}")
  get_filename_component(_name "${_archive}" NAME)
  string(APPEND FE_SUMS "${_hash}  ${_name}\n")
endforeach()
file(WRITE "${FE_OUT_DIR}/SHA256SUMS" "${FE_SUMS}")

# ---------------------------------------------------------------------------
# Smoke: unpack the game archive and exercise it.
#
# The point is to exercise the artifact a player receives, not the build tree it
# came from: unpacking reproduces a download, and running from there catches a
# missing asset, a launcher that does not resolve, or a binary that only worked
# beside its build directory.
#
# Two levels, because they fail for different reasons and only one of them is
# available everywhere:
#
#   * the structural check and `--help` need nothing but the artifact. `--help`
#     prints usage and exits before the engine initialises, so it proves the
#     binary runs, its loader resolves, and its RPATH is right without opening a
#     GL context. This runs on every platform.
#   * playing the game needs a GL 3.3 context, which a headless CI runner does
#     not have off Linux (there it comes from Xvfb plus Mesa's software driver,
#     passed in through FE_SMOKE_PREFIX). FE_SMOKE_GAME turns this level on, and
#     it is what proves the content actually loads and the slice plays.
# ---------------------------------------------------------------------------
if(FE_SMOKE)
  set(FE_SMOKE_DIR "${FE_OUT_DIR}/smoke")
  file(REMOVE_RECURSE "${FE_SMOKE_DIR}")
  file(MAKE_DIRECTORY "${FE_SMOKE_DIR}")
  fe_run("${CMAKE_COMMAND}" -E tar "xf" "${FE_GAME_ARCHIVE}" WORKING_DIRECTORY "${FE_SMOKE_DIR}")
  set(FE_SMOKE_ROOT "${FE_SMOKE_DIR}/${FE_STEM}")

  # What a player must receive for the game to start at all. A missing scene or
  # asset is invisible in the build tree, where the paths resolve to the
  # checkout, and only shows up when the archive is unpacked somewhere else.
  set(FE_SMOKE_CONTENT
      "${FE_GAME_BINARY}"
      "run.sh"
      "run.bat"
      "README.txt"
      "LICENSE"
      "examples/breakout/scenes/breakout.fescene"
      "examples/breakout/assets/fonts/hud.fefont"
      "examples/breakout/assets/textures/brick.png"
      "examples/breakout/assets/audio/beep.wav"
  )
  foreach(_entry IN LISTS FE_SMOKE_CONTENT)
    if(NOT EXISTS "${FE_SMOKE_ROOT}/${_entry}")
      message(FATAL_ERROR "the game archive is missing ${_entry}")
    endif()
  endforeach()
  message(STATUS "release: the game archive carries its scene, assets, and launcher")

  # Run the launcher a player runs, not the binary beside it, so the launcher's
  # `cd`, its executable bit (which travels in the archive), and the game's
  # relative paths are all exercised. Running `${FE_GAME_BINARY}` directly would
  # pass even when `run.sh` shipped mode 0644 and failed for a player.
  #
  # On Windows the launcher is a `.bat`, which `execute_process` does not launch
  # reliably without a shell, and there is no executable bit to lose in a zip; so
  # Windows runs the binary directly. The failure this guards against is
  # POSIX-only.
  set(FE_SMOKE_RUNNER "${FE_SMOKE_ROOT}/run.sh")
  if(FE_PLATFORM STREQUAL "windows")
    set(FE_SMOKE_RUNNER "${FE_SMOKE_ROOT}/${FE_GAME_BINARY}")
  endif()
  execute_process(COMMAND ${FE_SMOKE_PREFIX} "${FE_SMOKE_RUNNER}" --help
                  WORKING_DIRECTORY "${FE_SMOKE_ROOT}"
                  RESULT_VARIABLE _help_rc)
  if(NOT _help_rc EQUAL 0)
    message(FATAL_ERROR "the packaged game did not start (--help exited ${_help_rc})")
  endif()
  message(STATUS "release: the packaged game runs from the unpacked archive")

  # Level two: actually play it. Needs a GL context, so the caller turns it on
  # where one exists.
  if(FE_SMOKE_GAME)
    execute_process(COMMAND ${FE_SMOKE_PREFIX} "${FE_SMOKE_RUNNER}" --headless "${FE_SMOKE_FRAMES}"
                    WORKING_DIRECTORY "${FE_SMOKE_ROOT}"
                    RESULT_VARIABLE _smoke_rc)
    if(NOT _smoke_rc EQUAL 0)
      message(FATAL_ERROR "the packaged game did not run (${_smoke_rc})")
    endif()
    message(STATUS "release: the packaged game played ${FE_SMOKE_FRAMES} headless frames")
  endif()

  file(REMOVE_RECURSE "${FE_SMOKE_DIR}")
endif()

message(STATUS "release: ${FE_GAME_ARCHIVE}")
message(STATUS "release: ${FE_SDK_ARCHIVE}")
message(STATUS "release: ${FE_OUT_DIR}/SHA256SUMS")
