# Test for the Linux AppImage packaging path, run by ctest in script mode.
#
# The AppImage is built only on a release tag (and in the Linux CI job), so a
# mistake in it is invisible to the desktop suite -- a green build says nothing
# about whether the artifact still carries the content the game loads, still
# bundles the C++ runtime, or still refuses to bundle glibc. Each of those
# failures produces an AppImage that works on the build machine and fails on a
# downloader's, which is the worst kind of packaging bug: it is found by the
# person who downloaded it.
#
# Like the other workflow guards (test_release_workflow.cmake, test_ci_web_job.cmake)
# this reads the script as text and asserts the decisions it has to make, because
# the alternative is running a network download and a 10 MiB build on every push.
#
# Run by tests/CMakeLists.txt:
#   cmake -DAPPIMAGE_SCRIPT=<path> -DFETCH_SCRIPT=<path> -DRELEASE_WORKFLOW=<path>
#         -DCI_WORKFLOW=<path> -DICON=<path> -P cmake/tests/test_appimage_packaging.cmake

foreach(_required APPIMAGE_SCRIPT FETCH_SCRIPT RELEASE_WORKFLOW CI_WORKFLOW ICON)
  if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
    message(FATAL_ERROR "${_required} must be set (see the header of this file)")
  endif()
  if(NOT EXISTS "${${_required}}")
    message(FATAL_ERROR "${_required} does not exist: ${${_required}}")
  endif()
endforeach()

file(READ "${APPIMAGE_SCRIPT}" _script)
string(REPLACE "\r\n" "\n" _script "${_script}")

# check: <label>|<literal substring the script must contain>
set(_checks
    # The artifact is named from the version and the architecture, so a download
    # says what it is without being unpacked.
    "names the artifact from version and arch|fork-engine-\${FE_VERSION}-\${FE_ARCH}"
    # The game and the content it loads by relative path.
    "packages the game|fork_breakout"
    "stages the example content|examples/breakout/\${_dir}"
    "stages the assets and the scene|foreach(_dir assets scenes)"
    "carries the license|LICENSE"
    # The desktop entry and the icon are what make it installable on a desktop,
    # and appimagetool refuses to build without either.
    "writes a desktop entry|fork-engine.desktop"
    "carries the icon|fork-engine.png"
    # AppRun is the entry point: it must cd to the content root and put the
    # bundled libraries on the search path, or the game loads nothing and finds
    # no runtime.
    "writes AppRun|AppRun"
    "cds to the content root|cd \\\"\\\$HERE\\\""
    "sets the library path|LD_LIBRARY_PATH"
    # The library decision: bundle the C++ runtime, never glibc or a driver.
    "excludes glibc from the bundle|libc libm libdl libpthread"
    "excludes the graphics drivers|libGL libGLX libGLdispatch"
    "asserts glibc was not bundled|glibc must come from the host"
    "asserts a bundled library is a real file|was copied as a symlink"
    # Reproducibility is what makes a published checksum meaningful.
    "pins the timestamp|SOURCE_DATE_EPOCH=0"
    # The tool is an AppImage itself, so it is unpacked rather than mounted.
    "runs the tool without FUSE|APPIMAGE_EXTRACT_AND_RUN=1"
    # The artifact is checked to be an AppImage, not a bare ELF.
    "asserts the ELF magic|7f454c46"
    "asserts the AppImage magic|414902"
    # The smoke runs the artifact, not the build tree.
    "smoke-runs the artifact|the AppImage did not run"
    "can play the artifact|the AppImage did not play"
)

foreach(_check IN LISTS _checks)
  string(REPLACE "|" ";" _parts "${_check}")
  list(GET _parts 0 _label)
  list(GET _parts 1 _needle)
  string(FIND "${_script}" "${_needle}" _at)
  if(_at EQUAL -1)
    message(FATAL_ERROR "the AppImage script is missing '${_label}' (looked for: ${_needle})")
  endif()
  message(STATUS "  ${_label}: ok")
endforeach()

# The bundle must be the *resolved contents* of each library, not the symlink:
# copying libstdc++.so.6 as a symlink leaves a dangling link and the loader
# silently uses the host's copy, so the bundle does nothing on the older machine
# it exists to help. configure_file follows the symlink; file(COPY) does not.
set(_needle "configure_file(\"\${_path}\"")
string(FIND "${_script}" "${_needle}" _at)
if(_at EQUAL -1)
  message(FATAL_ERROR
    "the AppImage script no longer copies a bundled library with configure_file. "
    "file(COPY) preserves a symlinked soname as a dangling symlink, so the loader "
    "falls back to the host's copy and the bundle is a no-op.")
endif()

# ---------------------------------------------------------------------------
# The fetch script: the tool is downloaded, so the pin and its checksum are the
# security boundary and both have to be present and checked.
# ---------------------------------------------------------------------------
file(READ "${FETCH_SCRIPT}" _fetch)
string(REPLACE "\r\n" "\n" _fetch "${_fetch}")

foreach(_needle "FE_APPIMAGETOOL_VERSION=" "FE_APPIMAGETOOL_SHA256=" "sha256sum")
  string(FIND "${_fetch}" "${_needle}" _at)
  if(_at EQUAL -1)
    message(FATAL_ERROR "the fetch script no longer pins/checks the tool (missing ${_needle})")
  endif()
endforeach()
if(NOT _fetch MATCHES "checksum mismatch")
  message(FATAL_ERROR
    "the fetch script downloads the tool without failing on a checksum mismatch; "
    "a pin that is not enforced is not a pin.")
endif()
# The version in the URL and the checksum are updated together, so the URL must
# be built from the version variable rather than repeating the string.
set(_needle "releases/download/\${FE_APPIMAGETOOL_VERSION}/")
string(FIND "${_fetch}" "${_needle}" _at)
if(_at EQUAL -1)
  message(FATAL_ERROR
    "the fetch script no longer builds the download URL from the pinned version; "
    "a hand-typed URL can drift from the checksum it is checked against.")
endif()

# ---------------------------------------------------------------------------
# The icon is a generated binary, so it has to actually be a PNG of a size a
# desktop accepts, not an empty file committed by mistake. Bytes 16..19 are the
# width and 20..23 the height, big-endian; 256x256 is what the generator writes.
# ---------------------------------------------------------------------------
file(READ "${ICON}" _png_magic OFFSET 0 LIMIT 8 HEX)
string(REPLACE " " "" _png_magic "${_png_magic}")
file(READ "${ICON}" _png_width OFFSET 16 LIMIT 4 HEX)
file(READ "${ICON}" _png_height OFFSET 20 LIMIT 4 HEX)
string(REPLACE " " "" _png_width "${_png_width}")
string(REPLACE " " "" _png_height "${_png_height}")
if(NOT _png_magic STREQUAL "89504e470d0a1a0a")
  message(FATAL_ERROR "the AppImage icon is not a PNG (header ${_png_magic})")
endif()
if(NOT _png_width STREQUAL "00000100" OR NOT _png_height STREQUAL "00000100")
  message(FATAL_ERROR
    "the AppImage icon is not 256x256 (got ${_png_width}x${_png_height}); regenerate "
    "it with tools/gen_appimage_icon.py.")
endif()

# ---------------------------------------------------------------------------
# The wiring: the release must attach the AppImage, and CI must build it so a
# change to the script is caught on a push rather than at the next tag.
# ---------------------------------------------------------------------------
file(READ "${RELEASE_WORKFLOW}" _release)
string(REPLACE "\r\n" "\n" _release "${_release}")

if(NOT _release MATCHES "package_appimage.cmake")
  message(FATAL_ERROR "the release workflow no longer builds the AppImage")
endif()
if(NOT _release MATCHES "fetch_appimagetool.sh")
  message(FATAL_ERROR "the release workflow no longer fetches the pinned appimagetool")
endif()
# The published assets are collected by extension, so the AppImage has to be in
# that set or it is built, uploaded as an artifact, and then never published.
if(NOT _release MATCHES "name '\\*\\.AppImage'")
  message(FATAL_ERROR
    "the release no longer collects '*.AppImage' when it assembles the published "
    "assets, so the AppImage would be built but never downloadable.")
endif()

file(READ "${CI_WORKFLOW}" _ci)
string(REPLACE "\r\n" "\n" _ci "${_ci}")
if(NOT _ci MATCHES "package_appimage.cmake")
  message(FATAL_ERROR
    "CI no longer builds the AppImage, so a break in the packaging script would "
    "not be caught until a release was cut.")
endif()

# The Linux build jobs must not track `ubuntu-latest`. The AppImage is built in
# this job and checked in a container pinned to `ubuntu:24.04`; the artifact's
# glibc floor is the build host's, so building on a newer runner makes the pinned
# container -- and every machine older than the runner -- unable to run it. That
# is exactly the failure this smoke test exists to catch, and it fired for real
# when the runner image rotated to 26.04. Assert the pin so the rotation is a
# failed check, not a broken download.
if(_ci MATCHES "os: ubuntu-latest")
  message(FATAL_ERROR
    "a CI build job runs on ubuntu-latest; the AppImage's glibc floor would follow "
    "the runner image and outrun the pinned ubuntu:24.04 smoke container. Pin the "
    "Linux build jobs to ubuntu-24.04.")
endif()

message(STATUS "appimage: the artifact carries the game and the runtime, glibc stays on the host, and CI builds it")
