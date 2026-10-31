# Fork Engine Linux AppImage packaging.
#
# Turns a configured build tree into a single-file Linux artifact a player can
# download, make executable, and run on a machine that has never seen the
# engine. Run it in CMake script mode:
#
#   cmake -DFE_BUILD_DIR=build -DFE_OUT_DIR=dist -DFE_ARCH=x86_64 \
#         -DFE_APPIMAGETOOL=/path/to/appimagetool -P packaging/package_appimage.cmake
#
# It produces one file, named after the engine version and the architecture:
#
#   fork-engine-<ver>-<arch>.AppImage
#
# Why an AppImage on top of the tarball `package_release.cmake` already writes:
# the tarball is a directory a downloader unpacks and then has to run from the
# right place, and it assumes the C++ runtime the build linked against is present.
# An AppImage is one file, carries the C++ runtime with it, and runs from anywhere
# - which is what "download it, try it" means for a friend who is not a developer.
#
# What goes in the AppDir:
#
#   AppRun                        the entry point; sets the library path, cd's to
#                                 the content root, and execs the game (default)
#                                 or the editor (`--editor`)
#   usr/bin/fork_breakout         the game
#   usr/bin/fork_editor_shell     the editor
#   usr/lib/<bundled libs>        the C++ runtime, so an older host still runs it
#   examples/breakout/{assets,scenes}
#                                 the game's content, at the paths it loads them
#   examples/demo_project/        a small project the editor opens, so the
#                                 editor has something to show on first run
#   assets/                       the editor's built-in sample assets, at the
#                                 path the binaries resolve them from (see below)
#   fork-engine.desktop, fork-engine.png, LICENSE
#
# One file carries both the game and the editor. They are separate entry points
# but one download: a friend who wants to "try it" should not have to know which
# artifact is which, and the editor is the thing that was just polished. The
# default entry point is the editor, because opening a tool with something in it
# is the better first impression; `--game` (or the desktop entry's game action)
# runs the slice.
#
# The libraries are the interesting decision. `ldd` names what the binary needs;
# everything is bundled *except* glibc and the graphics driver stack. Bundling
# glibc is the classic AppImage bug - the host's loader cannot mix a bundled
# libc with its own ld.so - and bundling libGL/libEGL means shipping a driver
# that does not match the user's GPU. So the C++ runtime travels with the app
# and everything that must match the host stays with the host.
#
# Reproducibility: SOURCE_DATE_EPOCH is pinned, which makes mksquashfs record a
# fixed timestamp for every file and the AppImage runtime skip the wall-clock
# bits it would otherwise embed. Two runs over the same build tree then produce
# byte-identical artifacts, which is what lets a download be checked against a
# recorded hash. See docs/adr/0103-linux-appimage.md.
#
# Inputs (all via -D):
#   FE_BUILD_DIR      required  a configured build tree with the game built
#   FE_OUT_DIR        required  where the AppImage is written
#   FE_APPIMAGETOOL   required  path to appimagetool (see packaging/fetch_appimagetool.sh)
#   FE_ARCH           optional  x86_64 (default) | aarch64 | ...
#   FE_CONFIG         optional  build configuration for multi-config generators
#                               (default Release; ignored by single-config ones)
#   FE_VERSION        optional  defaults to the build tree's project version
#   FE_SMOKE          optional  ON (default) runs the built AppImage
#   FE_SMOKE_GAME     optional  OFF (default) also plays it, which needs a GL
#                               context
#   FE_SMOKE_PREFIX   optional  a command list prepended to the run (xvfb-run ...)
#   FE_SMOKE_FRAMES   optional  frames for the play run (default 60)

cmake_minimum_required(VERSION 3.20)

foreach(_required FE_BUILD_DIR FE_OUT_DIR)
  if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
    message(FATAL_ERROR "${_required} must be set (see the header of this file)")
  endif()
endforeach()

get_filename_component(FE_BUILD_DIR "${FE_BUILD_DIR}" ABSOLUTE)
get_filename_component(FE_OUT_DIR "${FE_OUT_DIR}" ABSOLUTE)
get_filename_component(FE_SCRIPT_DIR "${CMAKE_CURRENT_LIST_FILE}" DIRECTORY)

if(NOT EXISTS "${FE_BUILD_DIR}/CMakeCache.txt")
  message(FATAL_ERROR "no configured build tree at ${FE_BUILD_DIR}")
endif()

if(NOT DEFINED FE_ARCH OR "${FE_ARCH}" STREQUAL "")
  set(FE_ARCH "x86_64")
endif()

# The version has one source of truth (the root project() call) and the build
# tree records it, so the artifact name cannot disagree with the binary inside.
if(NOT DEFINED FE_VERSION OR "${FE_VERSION}" STREQUAL "")
  file(STRINGS "${FE_BUILD_DIR}/CMakeCache.txt" _version_line REGEX "^CMAKE_PROJECT_VERSION:STATIC=")
  if(NOT _version_line)
    message(FATAL_ERROR "cannot read the project version from ${FE_BUILD_DIR}/CMakeCache.txt")
  endif()
  string(REPLACE "CMAKE_PROJECT_VERSION:STATIC=" "" FE_VERSION "${_version_line}")
endif()

set(FE_SMOKE "ON" CACHE STRING "")
# Playing the game needs a GL context, which a headless runner has only under a
# virtual display. The structural check and `--help` need nothing, so they run
# everywhere and this is turned on where a context exists.
set(FE_SMOKE_GAME "OFF" CACHE STRING "")
set(FE_SMOKE_FRAMES "60" CACHE STRING "")

# The tool is fetched by packaging/fetch_appimagetool.sh, which pins and checksums
# it. The script does not download it itself: a packaging step that reaches the
# network should say so where a caller can see it, and CI fetches once and passes
# the path in.
if(NOT DEFINED FE_APPIMAGETOOL OR "${FE_APPIMAGETOOL}" STREQUAL "")
  message(FATAL_ERROR
    "FE_APPIMAGETOOL must name appimagetool. Fetch it with "
    "packaging/fetch_appimagetool.sh and pass the printed path.")
endif()
if(NOT EXISTS "${FE_APPIMAGETOOL}")
  message(FATAL_ERROR "no appimagetool at ${FE_APPIMAGETOOL}")
endif()

# Multi-config generators put artifacts under a per-configuration subdirectory,
# and the build tree records which kind it is, so the layout is derived rather
# than guessed from the platform.
set(FE_CONFIG "Release" CACHE STRING "")
file(STRINGS "${FE_BUILD_DIR}/CMakeCache.txt" _generator_line REGEX "^CMAKE_GENERATOR:INTERNAL=")
if(_generator_line MATCHES "Visual Studio" OR _generator_line MATCHES "Xcode"
   OR _generator_line MATCHES "Ninja Multi-Config")
  set(FE_BIN_DIR "${FE_BUILD_DIR}/bin/${FE_CONFIG}")
else()
  set(FE_BIN_DIR "${FE_BUILD_DIR}/bin")
endif()

function(fe_run)
  execute_process(COMMAND ${ARGN} RESULT_VARIABLE _rc)
  if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "command failed (${_rc}): ${ARGN}")
  endif()
endfunction()

set(FE_APPDIR "${FE_OUT_DIR}/AppDir")
set(FE_STEM "fork-engine-${FE_VERSION}-${FE_ARCH}")
set(FE_APPIMAGE "${FE_OUT_DIR}/${FE_STEM}.AppImage")

file(MAKE_DIRECTORY "${FE_OUT_DIR}")
file(REMOVE_RECURSE "${FE_APPDIR}")
file(REMOVE "${FE_APPIMAGE}")

# ---------------------------------------------------------------------------
# The binaries: the game and the editor.
#
# Both ship in the one AppImage. The editor is the default entry point (see
# AppRun below), so the download opens a tool with a project in it rather than a
# game with no obvious way to change it.
# ---------------------------------------------------------------------------
set(FE_GAME_BINARY "fork_breakout")
set(FE_EDITOR_BINARY "fork_editor_shell")
file(MAKE_DIRECTORY "${FE_APPDIR}/usr/bin")
foreach(_binary IN ITEMS "${FE_GAME_BINARY}" "${FE_EDITOR_BINARY}")
  if(NOT EXISTS "${FE_BIN_DIR}/${_binary}")
    message(FATAL_ERROR "no ${_binary} in ${FE_BIN_DIR}; build with FE_BUILD_EXAMPLES=ON and FE_BUILD_EDITOR=ON")
  endif()
  file(COPY "${FE_BIN_DIR}/${_binary}" DESTINATION "${FE_APPDIR}/usr/bin")
  file(CHMOD "${FE_APPDIR}/usr/bin/${_binary}"
       PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
endforeach()

# The content, at the paths each binary resolves by relative path, so the shipped
# binaries are the ones CI tests and no path is rewritten for the package.
#   * the game reads examples/breakout/{assets,scenes}
#   * the editor opens examples/demo_project, which is small and gives the panels
#     something to draw on the first run
file(MAKE_DIRECTORY "${FE_APPDIR}/examples/breakout")
foreach(_dir assets scenes)
  file(COPY "${FE_SCRIPT_DIR}/../examples/breakout/${_dir}"
       DESTINATION "${FE_APPDIR}/examples/breakout")
endforeach()
# The demo project's import cache is build output (it is regenerated on open), so
# it is excluded: shipping it would put a stale cache in the download.
file(COPY "${FE_SCRIPT_DIR}/../examples/demo_project"
     DESTINATION "${FE_APPDIR}/examples"
     PATTERN ".fork" EXCLUDE)
# The editor's built-in sample assets. The editor and the demo project both
# reference `tex:assets/textures/checker.png`, which the resource manager resolves
# against the process working directory (a project's asset root points the asset
# *database* at the project, but a path-keyed `tex:` reference is still read from
# cwd). The binaries run with the AppDir root as their cwd, so the sample tree
# must be there or the texture fails to load and the sprite renders untextured.
# Shipping it makes the download match the layout CI runs the editor against.
file(COPY "${FE_SCRIPT_DIR}/../assets" DESTINATION "${FE_APPDIR}")
file(COPY "${FE_SCRIPT_DIR}/../LICENSE" DESTINATION "${FE_APPDIR}")

# ---------------------------------------------------------------------------
# The bundled libraries.
#
# `ldd` names every shared object the game resolves. Everything it names is
# copied into usr/lib except what must come from the host:
#
#   * glibc (libc, ld.so, libm, libpthread, ...): the AppImage runtime runs the
#     host's loader, so a bundled libc would be loaded *beside* the host's and
#     fail on the first versioned symbol. This is the mistake that makes an
#     AppImage that works only on the machine that built it.
#   * the graphics driver stack (libGL, libEGL, libX11, ...): the game must use
#     the user's driver, not the one from the build host's Mesa.
#
# What is left is the C++ runtime (libstdc++, libgcc_s), which is exactly the
# part a machine with an older distribution does not have.
# ---------------------------------------------------------------------------
set(FE_HOST_ONLY
    # glibc family
    libc libm libdl libpthread librt libresolv libutil libnsl libcrypt
    # the loader and the kernel-provided shim
    ld-linux linux-vdso
    # graphics and windowing drivers: must match the user's system
    libGL libGLX libGLdispatch libOpenGL libEGL libGLES libvulkan
    libX11 libXext libXcursor libXrandr libXi libXinerama libXxf86vm libXfixes
    libxcb libwayland libxkbcommon libdrm libgbm
    libnvidia libcuda libEGL_nvidia
)

file(MAKE_DIRECTORY "${FE_APPDIR}/usr/lib")
# Both binaries are scanned: the game and the editor link the same runtime today,
# but bundling from one and not the other would leave whichever is not scanned
# needing a library the host may not have. The union is what must travel.
set(FE_BUNDLED "")
foreach(_binary IN ITEMS "${FE_GAME_BINARY}" "${FE_EDITOR_BINARY}")
  execute_process(COMMAND ldd "${FE_APPDIR}/usr/bin/${_binary}"
                  OUTPUT_VARIABLE _ldd OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE _ldd_rc)
  if(NOT _ldd_rc EQUAL 0)
    message(FATAL_ERROR "ldd failed on ${_binary} (${_ldd_rc})")
  endif()
  string(REPLACE "\n" ";" _ldd_lines "${_ldd}")

  foreach(_line IN LISTS _ldd_lines)
    # Only resolved entries name a file: "libfoo.so => /path (0x..)". An entry
    # with "not found" means the build host cannot run its own binary, which is a
    # broken build rather than something to package.
    if(_line MATCHES "=> not found")
      message(FATAL_ERROR "${_binary} needs a library that is not present: ${_line}")
    endif()
    if(NOT _line MATCHES "=>[ \t]+(/[^ \t]+)")
      continue()
    endif()
    set(_path "${CMAKE_MATCH_1}")
    get_filename_component(_name "${_path}" NAME)
    # Compare on the soname's stem so libstdc++.so.6 matches the "libstdc++"
    # family entry, and libm.so.6 matches "libm".
    string(REGEX REPLACE "\\.[0-9].*$" "" _stem "${_name}")
    string(REGEX REPLACE "\\.so$" "" _stem "${_stem}")

    set(_host_only FALSE)
    foreach(_prefix IN LISTS FE_HOST_ONLY)
      if(_stem STREQUAL "${_prefix}")
        set(_host_only TRUE)
        break()
      endif()
    endforeach()
    if(_host_only)
      continue()
    endif()
    # A library both binaries need is copied once; the second sighting is a
    # no-op, but appending only on the first keeps the status message honest.
    if(_name IN_LIST FE_BUNDLED)
      continue()
    endif()

    # The name under usr/lib is the soname the loader asks for, but the bytes must
    # be the real file: on a normal host libstdc++.so.6 is a symlink to
    # libstdc++.so.6.0.33, and copying the symlink leaves a dangling link in the
    # AppImage, so the loader silently falls back to the host's library and the
    # bundle does nothing on the older machine it was meant to help.
    # configure_file reads the contents (following the symlink) and writes them
    # under the soname.
    configure_file("${_path}" "${FE_APPDIR}/usr/lib/${_name}" COPYONLY)
    list(APPEND FE_BUNDLED "${_name}")
  endforeach()
endforeach()

if(FE_BUNDLED STREQUAL "")
  message(WARNING
    "no libraries were bundled; the game will need the C++ runtime the host "
    "provides. A statically linked binary is fine; otherwise check ldd output.")
endif()
message(STATUS "appimage: bundling ${FE_BUNDLED}")

# ---------------------------------------------------------------------------
# The desktop entry, the icon, and the entry point.
#
# The desktop entry's Icon= name must match the image at the AppDir root, and
# appimagetool refuses to build without both - so they are written here rather
# than left to a caller.
# ---------------------------------------------------------------------------
file(COPY "${FE_SCRIPT_DIR}/fork-engine.png" DESTINATION "${FE_APPDIR}")
# Two desktop actions: the editor is the default, the game is one click away in
# the same menu. Exec goes through AppRun rather than naming a binary directly,
# because AppRun is what sets the working directory and the library path; a
# desktop launcher does not inherit either. The action carries the same icon so
# the choice is legible in a menu.
file(WRITE "${FE_APPDIR}/fork-engine.desktop"
"[Desktop Entry]
Type=Application
Name=Fork Engine Editor
Comment=The Fork Engine editor: open the demo project, or play the breakout slice
Exec=AppRun --editor
Icon=fork-engine
Categories=Development;Game;
Terminal=false
Actions=PlayBreakout;

[Desktop Action PlayBreakout]
Name=Play Breakout
Exec=AppRun --game
Icon=fork-engine
")

# AppRun is what the AppImage executes. It cd's to the content root (both binaries
# resolve examples/... relatively), points the loader at the bundled libraries,
# and dispatches:
#
#   no argument / --editor   the editor, opened on the bundled demo project
#   --game                   the breakout slice
#   anything else            the game, so the AppImage stays a drop-in for the
#                            `--headless N` / `--save-scene` flags the container
#                            smoke test and the README already use
#
# The demo project is passed as an absolute path under $HERE, so the editor finds
# it whatever directory the AppImage was launched from.
file(WRITE "${FE_APPDIR}/AppRun"
"#!/bin/sh
# Fork Engine AppImage entry point.
HERE=\"\$(dirname \"\$(readlink -f \"\$0\")\")\"
cd \"\$HERE\" || exit 1
export LD_LIBRARY_PATH=\"\$HERE/usr/lib\${LD_LIBRARY_PATH:+:\$LD_LIBRARY_PATH}\"
case \"\${1:-}\" in
  --game)
    shift
    exec \"\$HERE/usr/bin/${FE_GAME_BINARY}\" \"\$@\"
    ;;
  --editor)
    shift
    exec \"\$HERE/usr/bin/${FE_EDITOR_BINARY}\" --project \"\$HERE/examples/demo_project\" \"\$@\"
    ;;
  '')
    exec \"\$HERE/usr/bin/${FE_EDITOR_BINARY}\" --project \"\$HERE/examples/demo_project\"
    ;;
  *)
    exec \"\$HERE/usr/bin/${FE_GAME_BINARY}\" \"\$@\"
    ;;
esac
")
file(CHMOD "${FE_APPDIR}/AppRun"
     PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)

# Belt and braces: the copy loop above should have excluded glibc, but a bug
# there produces an artifact that works on the build host and nowhere else, so
# the absence is asserted where the mistake would be made.
file(GLOB _bundled_libs "${FE_APPDIR}/usr/lib/*")
foreach(_lib IN LISTS _bundled_libs)
  get_filename_component(_name "${_lib}" NAME)
  if(_name MATCHES "^libc\\.so" OR _name MATCHES "^ld-linux" OR _name MATCHES "^libm\\.so")
    message(FATAL_ERROR
      "${_name} was bundled into the AppDir. glibc must come from the host: the "
      "AppImage runtime runs the host's loader, and a bundled libc fails on the "
      "first versioned symbol.")
  endif()
  # Every bundled library must be a real file with content. A dangling symlink -
  # the shape a symlinked soname takes if it is copied rather than followed -
  # leaves the loader quietly using the host's copy, which is the failure the
  # bundle exists to prevent, and it is invisible without this check.
  if(IS_SYMLINK "${_lib}")
    message(FATAL_ERROR
      "${_name} was copied as a symlink, not as the file it points at. The "
      "loader would fall back to the host's copy; copy the resolved contents.")
  endif()
  file(SIZE "${_lib}" _lib_size)
  if(_lib_size LESS 1)
    message(FATAL_ERROR "${_name} was bundled but is empty")
  endif()
endforeach()

# ---------------------------------------------------------------------------
# Build the AppImage.
#
# APPIMAGE_EXTRACT_AND_RUN=1 makes the tool (which is itself an AppImage) unpack
# instead of mounting, so this works with no FUSE - on a bare host and in a CI
# container alike. SOURCE_DATE_EPOCH is pinned for the reason in the header:
# reproducibility is what makes the checksum meaningful.
# ---------------------------------------------------------------------------
fe_run("${CMAKE_COMMAND}" -E env
       "APPIMAGE_EXTRACT_AND_RUN=1"
       "ARCH=${FE_ARCH}"
       "SOURCE_DATE_EPOCH=0"
       "${FE_APPIMAGETOOL}" "${FE_APPDIR}" "${FE_APPIMAGE}")

if(NOT EXISTS "${FE_APPIMAGE}")
  message(FATAL_ERROR "appimagetool reported success but wrote no ${FE_APPIMAGE}")
endif()

# The artifact is an ELF with the AppImage magic in its header: bytes 0..3 are
# `\x7fELF` and bytes 8..10 are `AI\x02`. Asserting both catches a truncated or
# mislabelled file at the point it is made rather than at a downloader's first
# run - a plain ELF here would mean the runtime was not embedded and the file is
# not an AppImage at all.
file(READ "${FE_APPIMAGE}" _elf_magic OFFSET 0 LIMIT 4 HEX)
file(READ "${FE_APPIMAGE}" _appimage_magic OFFSET 8 LIMIT 3 HEX)
string(REPLACE " " "" _elf_magic "${_elf_magic}")
string(REPLACE " " "" _appimage_magic "${_appimage_magic}")
if(NOT _elf_magic STREQUAL "7f454c46")
  message(FATAL_ERROR "${FE_APPIMAGE} is not an ELF executable (header ${_elf_magic})")
endif()
if(NOT _appimage_magic STREQUAL "414902")
  message(FATAL_ERROR
    "${FE_APPIMAGE} has no AppImage magic (bytes 8..10 are ${_appimage_magic}, "
    "expected 414902). The runtime was not embedded, so this is an ELF, not an "
    "AppImage.")
endif()

# ---------------------------------------------------------------------------
# Smoke: run the artifact that was just built, not the build tree.
#
# `--help` prints usage and exits before the engine initialises, so it proves
# the AppImage mounts, AppRun runs, the loader resolves the bundled libraries,
# and the binary starts - with no display and no GPU. Playing it needs a GL 3.3
# context, so FE_SMOKE_GAME turns that half on where one exists.
#
# Both entry points are checked, because both are shipped: the editor is the
# default, so its failure would be the one a downloader hits first. The editor
# runs headless frames (`--headless`) so a bundle that starts the editor but
# cannot draw is caught here rather than by the friend.
# ---------------------------------------------------------------------------
if(FE_SMOKE)
  execute_process(COMMAND ${CMAKE_COMMAND} -E env "APPIMAGE_EXTRACT_AND_RUN=1"
                          ${FE_SMOKE_PREFIX} "${FE_APPIMAGE}" --help
                  RESULT_VARIABLE _help_rc)
  if(NOT _help_rc EQUAL 0)
    message(FATAL_ERROR "the AppImage did not run (--help exited ${_help_rc})")
  endif()
  message(STATUS "appimage: the AppImage runs (--help)")

  if(FE_SMOKE_GAME)
    execute_process(COMMAND ${CMAKE_COMMAND} -E env "APPIMAGE_EXTRACT_AND_RUN=1"
                            ${FE_SMOKE_PREFIX} "${FE_APPIMAGE}" --headless "${FE_SMOKE_FRAMES}"
                    RESULT_VARIABLE _play_rc)
    if(NOT _play_rc EQUAL 0)
      message(FATAL_ERROR "the AppImage did not play (${_play_rc})")
    endif()
    message(STATUS "appimage: the AppImage played ${FE_SMOKE_FRAMES} headless frames")

    # The default entry point: the editor, opened on the bundled demo project,
    # drawing a few frames and exiting. This is the path a downloader takes when
    # they double-click the file, so it is the one worth exercising.
    execute_process(COMMAND ${CMAKE_COMMAND} -E env "APPIMAGE_EXTRACT_AND_RUN=1"
                            ${FE_SMOKE_PREFIX} "${FE_APPIMAGE}" --editor --headless --frames "${FE_SMOKE_FRAMES}"
                    RESULT_VARIABLE _editor_rc)
    if(NOT _editor_rc EQUAL 0)
      message(FATAL_ERROR "the AppImage editor did not run (${_editor_rc})")
    endif()
    message(STATUS "appimage: the AppImage editor ran ${FE_SMOKE_FRAMES} headless frames")
  endif()
endif()

file(REMOVE_RECURSE "${FE_APPDIR}")

message(STATUS "appimage: ${FE_APPIMAGE}")
