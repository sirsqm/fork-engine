# Test for the release workflow and the packaging script it drives, run by ctest
# in script mode.
#
# The release path is the one part of the project a contributor never runs: it
# fires on a version tag, so a mistake in it is invisible until a release is cut
# and the download is broken. Nothing in the desktop suite can observe it -- a
# green build says nothing about whether the workflow still packages every
# platform, still checksums what it publishes, or still refuses to publish a tag
# that disagrees with the engine version.
#
# A workflow is YAML, not code, so it is checked here the same way ci.yml is (see
# test_ci_web_job.cmake): by reading it as text and asserting the decisions it has
# to make. The packaging script is checked for the same reason: its contract is a
# set of inputs and a set of outputs, and the failure this guards against is a
# silent one -- an archive that is written uncompressed under a `.tar.gz` name, or
# a game archive that no longer carries the content the game loads.
#
# Run by tests/CMakeLists.txt:
#   cmake -DRELEASE_WORKFLOW=<path> -DPACKAGE_SCRIPT=<path> \
#         -P cmake/tests/test_release_workflow.cmake

foreach(_required RELEASE_WORKFLOW PACKAGE_SCRIPT)
  if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
    message(FATAL_ERROR "${_required} must be set (see the header of this file)")
  endif()
  if(NOT EXISTS "${${_required}}")
    message(FATAL_ERROR "${_required} does not exist: ${${_required}}")
  endif()
endforeach()

file(READ "${RELEASE_WORKFLOW}" _workflow)
string(REPLACE "\r\n" "\n" _workflow "${_workflow}")

# check: <label>|<literal substring the workflow must contain>
set(_checks
    "triggers on a version tag|tags:"
    "matches v-prefixed tags|\"v*\""
    "runs by hand too|workflow_dispatch:"
    "packages Linux|platform: linux"
    "packages Windows|platform: windows"
    "packages macOS|platform: macos"
    "uses the packaging script|packaging/package_release.cmake"
    "runs the packaged game as a smoke test|FE_SMOKE_PREFIX"
    "supplies a virtual display on Linux|xvfb-run"
    "checks the tag against the engine version|does not match the engine version"
    "uploads the artifacts|actions/upload-artifact@v4"
    "fails when no artifact is produced|if-no-files-found: error"
    "publishes only after packaging|needs: package"
    "publishes only for a tag|startsWith(github.ref, 'refs/tags/')"
    "creates the release|gh release create"
    "checksums the published assets|sha256sum"
)

# The Linux packaging job must not track `ubuntu-latest`. The AppImage's glibc
# floor is the build host's, and this is the job that publishes the one file a
# Linux player downloads; a runner-image rotation would silently raise that floor
# until older machines -- and the pinned `ubuntu:24.04` smoke container -- can no
# longer run the artifact. The pin is the whole defence, so assert it here.
if(_workflow MATCHES "os: ubuntu-latest")
  message(FATAL_ERROR
    "the release workflow builds on ubuntu-latest; the AppImage's glibc floor "
    "would follow the runner image. Pin the Linux job to ubuntu-24.04.")
endif()

foreach(_check IN LISTS _checks)
  string(REPLACE "|" ";" _parts "${_check}")
  list(GET _parts 0 _label)
  list(GET _parts 1 _needle)
  string(FIND "${_workflow}" "${_needle}" _at)
  if(_at EQUAL -1)
    message(FATAL_ERROR "release workflow is missing '${_label}' (looked for: ${_needle})")
  endif()
  message(STATUS "  ${_label}: ok")
endforeach()

# The publish job must be gated on every platform having packaged, or a release
# could appear with only some of its downloads. `needs: package` is the gate;
# a `continue-on-error` anywhere in the workflow would let a failed platform
# still publish.
if(_workflow MATCHES "continue-on-error")
  message(FATAL_ERROR "release workflow sets continue-on-error; a failed platform must fail the release")
endif()

# The workflow must be read-only by default, with the write permission granted
# only to the job that creates the release: a packaging step should never be able
# to write to the repository.
if(NOT _workflow MATCHES "permissions:\n  contents: read")
  message(FATAL_ERROR "release workflow does not default to read-only contents")
endif()
if(NOT _workflow MATCHES "contents: write")
  message(FATAL_ERROR "release workflow never grants contents: write; it cannot create a release")
endif()

# ---------------------------------------------------------------------------
# The packaging script.
# ---------------------------------------------------------------------------
file(READ "${PACKAGE_SCRIPT}" _package)

# The script decides compression from the platform, and the operation it uses has
# to be the compressing one for a `.tar.gz`. `cf` writes an uncompressed tar that
# `cmake -E tar xf` still reads by sniffing the format, so this mistake passes the
# smoke test and fails only for a downloader. Assert both halves are present.
if(NOT _package MATCHES "set\\(FE_TAR_OP \"czf\"\\)")
  message(FATAL_ERROR
    "the packaging script no longer uses the compressing tar operation for the "
    "tarball; a `.tar.gz` written with `cf` is a plain tar that `tar xzf` cannot read.")
endif()
if(NOT _package MATCHES "FE_TAR_OP")
  message(FATAL_ERROR "the packaging script never applies FE_TAR_OP to its tar calls")
endif()
if(NOT _package MATCHES "fe_assert_archive_format")
  message(FATAL_ERROR
    "the packaging script no longer asserts the archive format; the gzip-magic "
    "check is what makes an uncompressed `.tar.gz` fail loudly.")
endif()

# All three archives the release promises must be produced, and the game archive
# must carry the scene and the assets the game loads or it will not run from a
# download.
foreach(_needle "FE_GAME_ARCHIVE" "FE_SDK_ARCHIVE" "FE_RUNTIME_ARCHIVE" "SHA256SUMS")
  string(FIND "${_package}" "${_needle}" _at)
  if(_at EQUAL -1)
    message(FATAL_ERROR "the packaging script no longer produces ${_needle}")
  endif()
endforeach()

# The runtime archive is a prebuilt fork_player for `fork_export --player`, and
# the queue item is only satisfied if the export path actually accepts it. Assert
# the script checks that flow, not merely that it writes a file: it must invoke
# fork_export with --player against the unpacked runtime, or "a developer without
# the toolchain can export" is an assumption rather than a tested claim.
if(NOT _package MATCHES "--player")
  message(FATAL_ERROR
    "the packaging script no longer exports a game with the prebuilt runtime via "
    "--player; the runtime archive's whole purpose is that export flow.")
endif()
if(NOT _package MATCHES "fork_export accepted the prebuilt runtime")
  message(FATAL_ERROR
    "the packaging script no longer asserts that fork_export accepted the "
    "prebuilt runtime; the runtime smoke must check the export flow, not just "
    "that the archive carries a binary.")
endif()
# The runtime is staged from the installed tree, whose RPATH is relative, so it
# works after unpacking anywhere; the build tree's player has an absolute RPATH
# that would break the moment it was moved.
if(NOT _package MATCHES "stage-runtime-prefix")
  message(FATAL_ERROR
    "the runtime archive is no longer staged from an installed prefix; a player "
    "copied from the build tree carries an absolute RPATH and breaks when moved.")
endif()

# The game archive is staged by copying the example's content, so the script must
# still name the example and the subdirectories the game loads from. A rename of
# the example, or a dropped subdirectory, would ship an archive missing its scene
# or its assets, and the game would fail only at a player's first run.
foreach(_needle "examples/breakout" "foreach(_dir assets scenes)")
  string(FIND "${_package}" "${_needle}" _at)
  if(_at EQUAL -1)
    message(FATAL_ERROR "the packaging script no longer stages ${_needle}")
  endif()
endforeach()

# The launcher a player is told to run must be executable *in the archive*.
# `file(WRITE)` creates it 0644, the mode travels into the tarball, and a player
# who unpacks and runs the documented `./run.sh` gets "Permission denied". The
# smoke run does not catch this on its own if it invokes the binary directly, so
# assert the CHMOD is there.
if(NOT _package MATCHES "file\\(CHMOD \"\\$\\{FE_GAME_STAGE\\}/run\\.sh\"")
  message(FATAL_ERROR
    "the packaging script writes run.sh without making it executable; an "
    "unpacked tarball then fails on the documented './run.sh' with "
    "'Permission denied'.")
endif()
if(NOT _package MATCHES "FE_SMOKE_RUNNER")
  message(FATAL_ERROR
    "the packaging smoke run no longer goes through the launcher; running the "
    "binary directly hides a launcher that ships non-executable.")
endif()

message(STATUS "release: every platform packages, the tag is checked, and the archives are compressed")
