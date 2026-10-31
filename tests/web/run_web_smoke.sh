#!/usr/bin/env bash
# Copyright (c) 2026 sqm
# SPDX-License-Identifier: MIT
#
# Builds the web window smoke test to wasm and runs it in a headless browser.
#
# This is the acceptance test for the web window backend (ADR-0074): it proves
# the engine can obtain a WebGL2 context from a browser, not merely that the code
# compiles. It is deliberately self-contained so CI can run it as one step:
#
#   tests/web/run_web_smoke.sh
#
# Requirements, all of which the web CI job provides:
#   * an activated emsdk (em++ on PATH), e.g. `source /path/to/emsdk_env.sh`
#   * a Chromium/Chrome binary (CHROME_BIN, or one of the usual names on PATH)
#
# The browser runs with SwiftShader so no GPU is needed: a software WebGL2
# implementation is enough to prove the context is real, and it makes the test
# pass on a plain CI runner.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"
BUILD_DIR="${FE_WEB_SMOKE_BUILD_DIR:-${ROOT_DIR}/build-web-smoke}"

fail() {
  echo "web smoke: $*" >&2
  exit 1
}

command -v em++ >/dev/null 2>&1 || fail "em++ is not on PATH; activate emsdk first (source emsdk_env.sh)"

CHROME_BIN="${CHROME_BIN:-}"
if [[ -z "${CHROME_BIN}" ]]; then
  for candidate in chromium chromium-browser google-chrome google-chrome-stable chrome; do
    if command -v "${candidate}" >/dev/null 2>&1; then
      CHROME_BIN="$(command -v "${candidate}")"
      break
    fi
  done
fi
[[ -n "${CHROME_BIN}" ]] || fail "no Chromium/Chrome found; set CHROME_BIN to its path"

# A browser that wedges instead of exiting must not wedge the run. `timeout` is
# GNU coreutils and is absent on a stock macOS, so fall back to running without
# it there rather than failing the check on a missing tool.
CHROME_TIMEOUT="${FE_WEB_CHROME_TIMEOUT:-180}"
if ! command -v timeout >/dev/null 2>&1; then
  CHROME_TIMEOUT=""
fi

# The engine is compiled from source here rather than through the CMake build so
# the test exercises the same translation units a shipped web build would
# (WebWindow.cpp, the GLES3 device, and the platform-free Window.cpp), and can be
# run without a configured web build tree.
SOURCES=(
  "${ROOT_DIR}/engine/src/platform/Window.cpp"
  "${ROOT_DIR}/engine/src/platform/WindowIcon.cpp"
  "${ROOT_DIR}/engine/src/platform/NullWindow.cpp"
  "${ROOT_DIR}/engine/src/platform/InputState.cpp"
  "${ROOT_DIR}/engine/src/platform/InputActions.cpp"
  "${ROOT_DIR}/engine/src/platform/KeyCode.cpp"
  "${ROOT_DIR}/engine/src/platform/CanvasScale.cpp"
  "${ROOT_DIR}/engine/src/platform/WebWindow.cpp"
  "${ROOT_DIR}/engine/src/platform/UserData.cpp"
  "${ROOT_DIR}/engine/src/platform/WebUserData.cpp"
  "${ROOT_DIR}/engine/src/platform/NullUserData.cpp"
  "${ROOT_DIR}/engine/src/core/Log.cpp"
  "${ROOT_DIR}/engine/src/core/Assert.cpp"
  "${SCRIPT_DIR}/web_window_smoke.cpp"
)

mkdir -p "${BUILD_DIR}"

echo "web smoke: compiling the smoke test to wasm"
em++ \
  -std=c++20 \
  -O1 \
  -DFE_PLATFORM_WEB \
  -I"${ROOT_DIR}/engine/include" \
  "${SOURCES[@]}" \
  -sWASM=1 \
  -sMIN_WEBGL_VERSION=2 \
  -sMAX_WEBGL_VERSION=2 \
  -sEXIT_RUNTIME=0 \
  -sENVIRONMENT=web \
  -sALLOW_MEMORY_GROWTH=1 \
  --shell-file "${SCRIPT_DIR}/shell.html" \
  -o "${BUILD_DIR}/web_window_smoke.html"

echo "web smoke: running headless in ${CHROME_BIN}"

# Serve the build directory over HTTP rather than loading it from file://.
# Two reasons, and both matter. A file:// page cannot fetch its own .wasm sibling
# without a non-default flag, and a shipped web build is served over HTTP anyway,
# so this is the environment the code actually runs in. The server is started and
# torn down inside the retry loop below.
HTTP_PID=""
cleanup() {
  if [[ -n "${HTTP_PID}" ]] && kill -0 "${HTTP_PID}" 2>/dev/null; then
    kill "${HTTP_PID}" 2>/dev/null || true
    wait "${HTTP_PID}" 2>/dev/null || true
  fi
}
trap cleanup EXIT

# A headless browser start is not perfectly deterministic — the module fetch and
# the wasm instantiation race the virtual-time budget — so the run is retried a
# few times before it is called a failure. The retry is not hiding a real fault:
# a genuine failure (no WebGL2, a broken backend) reports the same result every
# time and still fails after the last attempt. It only absorbs a browser that
# started too slowly to run the page, which must not turn into a red CI job for
# the shipping automation to trip over.
ATTEMPTS="${FE_WEB_SMOKE_ATTEMPTS:-3}"
DUMP=""
BASE_PORT="${FE_WEB_SMOKE_PORT:-8731}"
for attempt in $(seq 1 "${ATTEMPTS}"); do
  # A distinct port per attempt, so a retry never meets the previous attempt's
  # listener in TIME_WAIT, and a short backoff so a browser that started too
  # slowly on a loaded runner gets a real second chance instead of failing fast.
  PORT=$((BASE_PORT + attempt - 1))
  if (( attempt > 1 )); then
    sleep 2
  fi
  echo "web smoke: attempt ${attempt}/${ATTEMPTS}"
  python3 -m http.server "${PORT}" --bind 127.0.0.1 --directory "${BUILD_DIR}" >/dev/null 2>&1 &
  HTTP_PID=$!
  # Wait for the port to accept a connection before pointing the browser at it.
  for _ in $(seq 1 40); do
    if (exec 3<>"/dev/tcp/127.0.0.1/${PORT}") 2>/dev/null; then
      # Close the probe descriptor in a subshell so the redirection cannot leak
      # into this shell: `exec 3>&- 2>/dev/null` run directly would redirect this
      # script's own stderr to /dev/null for the rest of the run, silently
      # swallowing every later error message.
      (exec 3>&-) 2>/dev/null || true
      break
    fi
    sleep 0.25
  done

  # SwiftShader gives a software WebGL2 implementation, so this needs no GPU.
  # --dump-dom prints the DOM after the page has settled; --virtual-time-budget
  # bounds how long the page may take. The test writes its verdict into the
  # title, so it is read back out of the dump. A private profile per attempt
  # keeps this Chrome from attaching to another instance that is already running
  # with different flags, which would make it ignore --dump-dom and exit at once.
  PROFILE_DIR="$(mktemp -d)"
  # `timeout` is omitted when it is not available (stock macOS), so build the
  # command as an array rather than interpolating an empty duration, which
  # `timeout` would reject as an invalid interval.
  CHROME_CMD=("${CHROME_BIN}")
  if [[ -n "${CHROME_TIMEOUT}" ]]; then
    CHROME_CMD=(timeout "${CHROME_TIMEOUT}" "${CHROME_BIN}")
  fi
  DUMP="$("${CHROME_CMD[@]}" \
    --headless=new \
    --no-sandbox \
    --disable-gpu \
    --use-gl=angle \
    --use-angle=swiftshader \
    --enable-unsafe-swiftshader \
    --virtual-time-budget=30000 \
    --user-data-dir="${PROFILE_DIR}" \
    --dump-dom \
    "http://127.0.0.1:${PORT}/web_window_smoke.html" 2>/dev/null)" || true
  rm -rf "${PROFILE_DIR}"

  cleanup
  HTTP_PID=""

  if grep -q "FE_SMOKE_RESULT" <<<"${DUMP}"; then
    break
  fi
  echo "web smoke: the page reported nothing on attempt ${attempt}; retrying"
done

# The title element holds the report, newlines and all, HTML-escaped.
if ! grep -q "FE_SMOKE_RESULT" <<<"${DUMP}"; then
  echo "web smoke: the page never reported a result" >&2
  echo "---- DOM dump ----" >&2
  echo "${DUMP}" >&2
  fail "no result from the browser after ${ATTEMPTS} attempts"
fi

# Unescape the report the test wrote into the title and print it for the log, so
# a failure shows which check failed rather than only that something did.
REPORT="$(python3 - "${DUMP}" <<'PY'
import html, re, sys
match = re.search(r"<title>(.*?)</title>", sys.argv[1], re.S)
print(html.unescape(match.group(1)) if match else "")
PY
)"
echo "${REPORT}"

if grep -q "FE_SMOKE_RESULT: PASS" <<<"${DUMP}"; then
  echo "web smoke: PASS"
  exit 0
fi

fail "the smoke test reported a failure (see the report above)"
