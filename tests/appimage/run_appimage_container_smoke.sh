#!/usr/bin/env bash
# Copyright (c) 2026 sqm
# SPDX-License-Identifier: MIT
#
# Proves a built AppImage launches on a machine that has never seen the engine.
#
# This is the acceptance test for the Linux AppImage (docs/SHIPPING_QUEUE.md,
# `linux-appimage`): "launches the game on a clean container". The build machine
# is not a clean machine -- it has the C++ runtime the game was linked against,
# the graphics driver, and the engine's own libraries on its search path -- so a
# bundle that silently does nothing still passes a smoke test run there. A
# container that has none of that is the only place the bundle is actually
# exercised.
#
# Two checks, in order of how much they prove:
#
#   1. `--help` in a bare container. No display, no GPU, no engine libraries.
#      This proves the AppImage unpacks, AppRun runs, the loader resolves the
#      bundled C++ runtime, and the binary starts. It needs nothing installed.
#
#   2. `--headless N` in a container with a virtual display and Mesa's software
#      driver. This proves the game *plays*: it initialises the engine, creates a
#      GL context, and runs frames. The GL packages are the only thing the
#      container needs, and they are the ones a desktop already has. The editor
#      is run the same way (`--editor --headless --frames N`), because it is the
#      default entry point and the path a downloader takes first.
#
# It is opt-in rather than part of ctest because it needs Docker and a network to
# pull the image, which the unit-test matrix does not have. CI calls it in the
# Linux job, where Docker is present.
#
# Usage:
#   tests/appimage/run_appimage_container_smoke.sh PATH.AppImage [IMAGE]
#
#   IMAGE defaults to ubuntu:24.04, the distribution the Linux CI runner uses, so
#   the container matches the environment the artifact was built in. Pass an older
#   tag (ubuntu:22.04) to check the bundle on a distribution older than the build
#   host, which is what the bundled C++ runtime is for.

set -euo pipefail

APPIMAGE="${1:-}"
IMAGE="${2:-ubuntu:24.04}"

fail() {
  echo "appimage container smoke: $*" >&2
  exit 1
}

[[ -n "${APPIMAGE}" ]] || fail "usage: $0 PATH.AppImage [IMAGE]"
[[ -f "${APPIMAGE}" ]] || fail "no AppImage at ${APPIMAGE}"

command -v docker >/dev/null 2>&1 || fail "docker is not on PATH; this check needs it"

APPIMAGE="$(cd "$(dirname "${APPIMAGE}")" && pwd)/$(basename "${APPIMAGE}")"

# Docker needs to reach the daemon. `sudo` is how the container this runs in
# starts dockerd; on a runner the user is already in the docker group.
DOCKER="docker"
if ! docker info >/dev/null 2>&1; then
  if sudo docker info >/dev/null 2>&1; then
    DOCKER="sudo docker"
  else
    fail "cannot talk to the Docker daemon (tried docker and sudo docker)"
  fi
fi

echo "appimage container smoke: ${APPIMAGE##*/} on ${IMAGE}"

# The AppImage is mounted read-only at a fixed path. APPIMAGE_EXTRACT_AND_RUN
# makes it unpack rather than mount, so the container needs no FUSE.
run_in_container() {
  local script="$1"
  ${DOCKER} run --rm \
    -e APPIMAGE_EXTRACT_AND_RUN=1 \
    -e DEBIAN_FRONTEND=noninteractive \
    -v "${APPIMAGE}:/fork-engine.AppImage:ro" \
    "${IMAGE}" \
    bash -c "${script}"
}

# --- 1. It runs at all, on a container with nothing installed. ---------------
echo "appimage container smoke: running --help in a bare ${IMAGE}"
if ! run_in_container '/fork-engine.AppImage --help'; then
  fail "the AppImage did not run on a clean ${IMAGE}"
fi

# The point of the bundle: the game must resolve its C++ runtime from inside the
# AppImage, not from the container. A dangling symlink or a missing copy would
# make this fall back to the container's copy, which is exactly the failure a
# clean machine exposes and the build machine hides. Check the loader's own
# search report, not just the exit code.
echo "appimage container smoke: checking the bundled runtime is used"
loader_report=$(run_in_container \
  'LD_DEBUG=libs /fork-engine.AppImage --help 2>&1 || true')
if ! grep -q "calling init: .*usr/lib/libstdc++" <<<"${loader_report}"; then
  echo "${loader_report}" | grep -i 'libstdc' >&2 || true
  fail "the AppImage did not load its own libstdc++; the bundle is not being used"
fi

# --- 2. It plays, on a container with a virtual display and Mesa. ------------
# The game needs a GL 3.3 context even headless (the engine creates a hidden
# window), so this is where a real frame is rendered.
#
# Xvfb is started directly rather than through `xvfb-run`. `xvfb-run` waits on a
# signal trap that does not fire when it is PID 1 in a container, so it hangs
# after the game has already exited -- a run that looks like a slow game and is
# really a stuck wrapper. Starting the server, waiting for its socket to answer,
# and killing it is both quicker and honest about what is being waited for.
echo "appimage container smoke: playing headless frames on ${IMAGE}"
play_script='
set -e
apt-get update -qq
apt-get install -y -qq xvfb x11-utils libgl1 libglx-mesa0 libgl1-mesa-dri >/dev/null

Xvfb :99 -screen 0 640x480x24 >/tmp/xvfb.log 2>&1 &
xvfb_pid=$!
trap "kill ${xvfb_pid} 2>/dev/null || true" EXIT

# Wait for the display to answer, not for a fixed number of seconds: the server
# needs longer than a sleep on a loaded runner, and a shorter sleep races it.
for _ in $(seq 1 100); do
  xdpyinfo -display :99 >/dev/null 2>&1 && break
  sleep 0.2
done
xdpyinfo -display :99 >/dev/null 2>&1 || { echo "Xvfb never came up" >&2; cat /tmp/xvfb.log >&2; exit 1; }

DISPLAY=:99 /fork-engine.AppImage --headless 60

# The editor is the default entry point, so a downloader meets it first; run it
# too, on the project the AppImage ships. `--editor` is explicit here because the
# script passes an argument (the argument would otherwise go to the game).
DISPLAY=:99 /fork-engine.AppImage --editor --headless --frames 60
'
if ! run_in_container "${play_script}"; then
  fail "the AppImage did not play on a clean ${IMAGE}"
fi

echo "appimage container smoke: PASS (runs, plays, and opens the editor on a clean ${IMAGE})"
