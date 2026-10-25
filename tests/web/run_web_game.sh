#!/usr/bin/env bash
# Copyright (c) 2026 sqm
# SPDX-License-Identifier: MIT
#
# Exports a 2D project to a web build and runs it in a headless browser, then
# asserts the game actually rendered.
#
# This is the acceptance test for `headless-browser-verification`
# (docs/ROADMAP.md): the web smoke test proves the window backend can obtain a
# WebGL2 context, and this proves a game built on top of it runs end to end. It
# covers the whole shipping path in one run — the export template, the pack
# fetched over HTTP, the player running under Emscripten's browser environment,
# the ES shader dialect, and the 2D renderer.
#
#   tests/web/run_web_game.sh
#
# Requirements, all of which the web CI job provides:
#   * an activated emsdk (em++ on PATH), e.g. `source /path/to/emsdk_env.sh`
#   * a web build tree (build-web) containing fork_export.js and fork_player.js
#   * node, to run fork_export (a desktop build of fork_export works too)
#   * a Chromium/Chrome binary (CHROME_BIN, or one of the usual names on PATH)
#
# The browser runs with SwiftShader so no GPU is needed.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"
BUILD_DIR="${FE_WEB_GAME_BUILD_DIR:-${ROOT_DIR}/build-web}"
PROJECT_DIR="${FE_WEB_GAME_PROJECT:-${ROOT_DIR}/examples/demo_project}"

fail() {
  echo "web game: $*" >&2
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

[[ -f "${BUILD_DIR}/bin/fork_player.js" ]] ||
  fail "no web player at ${BUILD_DIR}/bin/fork_player.js; build the web target first"
[[ -d "${PROJECT_DIR}" ]] || fail "no project at ${PROJECT_DIR}"

EXPORT_DIR="$(mktemp -d)"
cleanup_export() { rm -rf "${EXPORT_DIR}"; }
trap cleanup_export EXIT

# The export is the shipped artifact, so it is produced the same way a release
# produces it: fork_export, against the wasm player. A native fork_export is
# preferred because it needs no extra wasm build; the wasm one runs under node
# with NODERAWFS, which is how a checkout with only the web target is served.
echo "web game: exporting ${PROJECT_DIR} to ${EXPORT_DIR}"
if [[ -x "${ROOT_DIR}/build/bin/fork_export" ]]; then
  (cd "${ROOT_DIR}" && ./build/bin/fork_export \
    --project "${PROJECT_DIR}" \
    --out "${EXPORT_DIR}" \
    --target web \
    --player "${BUILD_DIR}/bin/fork_player.js")
else
  command -v node >/dev/null 2>&1 || fail "no native fork_export and node is not on PATH"
  [[ -f "${BUILD_DIR}/bin/fork_export.js" ]] ||
    fail "no fork_export at ${BUILD_DIR}/bin/fork_export.js either"
  (cd "${ROOT_DIR}" && node "${BUILD_DIR}/bin/fork_export.js" \
    --project "${PROJECT_DIR}" \
    --out "${EXPORT_DIR}" \
    --target web \
    --player "${BUILD_DIR}/bin/fork_player.js")
fi

[[ -f "${EXPORT_DIR}/index.html" ]] || fail "the export produced no index.html"
[[ -f "${EXPORT_DIR}/fork_shell.js" ]] || fail "the export produced no shell"

# Serve the export over HTTP rather than loading it from file://, because a
# shipped build is served and the pack is fetched by the engine's HTTP resource
# source, which needs an origin.
PORT="${FE_WEB_GAME_PORT:-8733}"
HTTP_PID=""
cleanup() {
  if [[ -n "${HTTP_PID}" ]] && kill -0 "${HTTP_PID}" 2>/dev/null; then
    kill "${HTTP_PID}" 2>/dev/null || true
    wait "${HTTP_PID}" 2>/dev/null || true
  fi
}
trap 'cleanup; cleanup_export' EXIT

# `?frames=N` bounds the run so the page settles and can be observed; a game
# otherwise runs until its tab closes. Two frames is enough to show the loop is
# live rather than a single draw, and the budget is generous because the module
# fetch, the wasm instantiation and the software-rendered frames all share it.
FRAMES="${FE_WEB_GAME_FRAMES:-2}"
ATTEMPTS="${FE_WEB_GAME_ATTEMPTS:-3}"
DUMP=""
for attempt in $(seq 1 "${ATTEMPTS}"); do
  echo "web game: attempt ${attempt}/${ATTEMPTS}"
  python3 -m http.server "${PORT}" --bind 127.0.0.1 --directory "${EXPORT_DIR}" >/dev/null 2>&1 &
  HTTP_PID=$!
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

  DUMP="$("${CHROME_BIN}" \
    --headless=new \
    --no-sandbox \
    --disable-gpu \
    --use-gl=angle \
    --use-angle=swiftshader \
    --enable-unsafe-swiftshader \
    --virtual-time-budget=120000 \
    --dump-dom \
    "http://127.0.0.1:${PORT}/index.html?frames=${FRAMES}" 2>/dev/null)" || true

  cleanup
  HTTP_PID=""

  if grep -q "FE_PLAYER_RESULT" <<<"${DUMP}"; then
    break
  fi
  echo "web game: the page reported nothing on attempt ${attempt}; retrying"
done

if ! grep -q "FE_PLAYER_RESULT" <<<"${DUMP}"; then
  echo "web game: the game never reported a result" >&2
  echo "---- DOM dump ----" >&2
  echo "${DUMP}" >&2
  fail "no result from the browser after ${ATTEMPTS} attempts"
fi

# The title holds the report, HTML-escaped.
REPORT="$(python3 - "${DUMP}" <<'PY'
import html, re, sys
match = re.search(r"<title>(.*?)</title>", sys.argv[1], re.S)
print(html.unescape(match.group(1)) if match else "")
PY
)"
echo "web game: ${REPORT}"

# `frames=` proves the loop ran; `canvas=` proves the shell supplied a live
# WebGL2 canvas; `distinct=` is the blankness check. A blank canvas — a failed
# shader compile, a dead renderer, a scene that never drew — is one colour, and a
# rendered 2D scene is several, so this fails when the renderer is deliberately
# broken without needing a golden image.
CANVAS_REPORTED="$(sed -n 's/.*canvas=\([0-9]*\).*/\1/p' <<<"${REPORT}")"
FRAMES_REPORTED="$(sed -n 's/.*frames=\([0-9]*\).*/\1/p' <<<"${REPORT}")"
DISTINCT_REPORTED="$(sed -n 's/.*distinct=\([0-9]*\).*/\1/p' <<<"${REPORT}")"

[[ -n "${CANVAS_REPORTED}" ]] || fail "the report carried no canvas flag: ${REPORT}"
[[ -n "${FRAMES_REPORTED}" ]] || fail "the report carried no frame count: ${REPORT}"
[[ -n "${DISTINCT_REPORTED}" ]] || fail "the report carried no colour count: ${REPORT}"

if (( CANVAS_REPORTED != 1 )); then
  fail "the game has no live canvas; the shell did not provide one"
fi
if (( FRAMES_REPORTED < 2 )); then
  fail "the game stepped ${FRAMES_REPORTED} frame(s); the loop is not live"
fi
if (( DISTINCT_REPORTED < 2 )); then
  fail "the rendered frame has ${DISTINCT_REPORTED} distinct colour(s); it is blank"
fi

echo "web game: PASS (canvas, ${FRAMES_REPORTED} frames, ${DISTINCT_REPORTED} distinct colours)"
