#!/usr/bin/env bash
# Copyright (c) 2026 sqm
# SPDX-License-Identifier: MIT
#
# Exports a project to a web build and runs it in a headless browser, then
# asserts the game actually rendered. It runs the 2D demo and the 3D demo, so a
# regression in either renderer fails the run.
#
# This is the acceptance test for `headless-browser-verification`
# (docs/SHIPPING_QUEUE.md): the web smoke test proves the window backend can
# obtain a WebGL2 context, and this proves a game built on top of it runs end to
# end. It covers the whole shipping path in one run — the export template, the
# pack fetched over HTTP, the player running under Emscripten's browser
# environment, the ES shader dialect, and the renderers.
#
#   tests/web/run_web_game.sh                 # both projects
#   FE_WEB_GAME_PROJECT=DIR tests/web/run_web_game.sh   # one project
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

# A browser that wedges instead of exiting must not wedge the run. `timeout` is
# GNU coreutils and is absent on a stock macOS, so fall back to running without
# it there rather than failing the check on a missing tool.
CHROME_TIMEOUT="${FE_WEB_CHROME_TIMEOUT:-180}"
if ! command -v timeout >/dev/null 2>&1; then
  CHROME_TIMEOUT=""
fi

[[ -f "${BUILD_DIR}/bin/fork_player.js" ]] ||
  fail "no web player at ${BUILD_DIR}/bin/fork_player.js; build the web target first"

# The export is the shipped artifact, so it is produced the same way a release
# produces it: fork_export, against the wasm player. A native fork_export is
# preferred because it needs no extra wasm build; the wasm one runs under node
# with NODERAWFS, which is how a checkout with only the web target is served.
export_project() {
  local project="$1" out="$2"
  if [[ -x "${ROOT_DIR}/build/bin/fork_export" ]]; then
    (cd "${ROOT_DIR}" && ./build/bin/fork_export \
      --project "${project}" \
      --out "${out}" \
      --target web \
      --player "${BUILD_DIR}/bin/fork_player.js")
  else
    command -v node >/dev/null 2>&1 || fail "no native fork_export and node is not on PATH"
    [[ -f "${BUILD_DIR}/bin/fork_export.js" ]] ||
      fail "no fork_export at ${BUILD_DIR}/bin/fork_export.js either"
    (cd "${ROOT_DIR}" && node "${BUILD_DIR}/bin/fork_export.js" \
      --project "${project}" \
      --out "${out}" \
      --target web \
      --player "${BUILD_DIR}/bin/fork_player.js")
  fi
}

# Exports one project, serves it over HTTP, runs it in a headless browser, and
# asserts the page's own verdict. `?frames=N` bounds the run so the page settles
# and can be observed; a game otherwise runs until its tab closes.
run_project() {
  local project="$1" label="$2" base_port="${3:-8733}"
  [[ -d "${project}" ]] || fail "no project at ${project}"

  local export_dir
  export_dir="$(mktemp -d)"
  echo "web game: [${label}] exporting ${project} to ${export_dir}"
  export_project "${project}" "${export_dir}"

  [[ -f "${export_dir}/index.html" ]] || fail "[${label}] the export produced no index.html"
  [[ -f "${export_dir}/fork_shell.js" ]] || fail "[${label}] the export produced no shell"

  # Serve the export over HTTP rather than loading it from file://, because a
  # shipped build is served and the pack is fetched by the engine's HTTP resource
  # source, which needs an origin.
  local http_pid="" dump="" attempt
  cleanup_http() {
    if [[ -n "${http_pid}" ]] && kill -0 "${http_pid}" 2>/dev/null; then
      kill "${http_pid}" 2>/dev/null || true
      wait "${http_pid}" 2>/dev/null || true
    fi
  }

  # Two frames is enough to show the loop is live rather than a single draw, and
  # the budget is generous because the module fetch, the wasm instantiation and
  # the software-rendered frames all share it.
  local frames="${FE_WEB_GAME_FRAMES:-2}"
  local attempts="${FE_WEB_GAME_ATTEMPTS:-3}"
  for attempt in $(seq 1 "${attempts}"); do
    # A distinct port per attempt, so a retry never meets the previous attempt's
    # listener in TIME_WAIT and serves a stale directory. The retry exists for a
    # browser that starts too slowly on a loaded runner; a short backoff gives
    # such a start a chance to recover rather than failing fast three times.
    local port=$((base_port + attempt - 1))
    if (( attempt > 1 )); then
      sleep 2
    fi
    echo "web game: [${label}] attempt ${attempt}/${attempts}"
    python3 -m http.server "${port}" --bind 127.0.0.1 --directory "${export_dir}" >/dev/null 2>&1 &
    http_pid=$!
    local _
    for _ in $(seq 1 40); do
      if (exec 3<>"/dev/tcp/127.0.0.1/${port}") 2>/dev/null; then
        # Close the probe descriptor in a subshell so the redirection cannot leak
        # into this shell: `exec 3>&- 2>/dev/null` run directly would redirect this
        # script's own stderr to /dev/null for the rest of the run, silently
        # swallowing every later error message.
        (exec 3>&-) 2>/dev/null || true
        break
      fi
      sleep 0.25
    done

    # A private profile per attempt: two Chrome instances sharing the default
    # profile directory race on its singleton lock and the loser exits at once
    # without loading the page, which is the fast "reported nothing" failure.
    # `timeout` bounds a browser that hangs instead of exiting, which would
    # otherwise wedge CI; the retry then starts a fresh browser and a fresh port.
    local profile_dir
    profile_dir="$(mktemp -d)"
    # `timeout` is omitted when it is not available (stock macOS), so build the
    # command as an array rather than interpolating an empty duration, which
    # `timeout` would reject as an invalid interval.
    local chrome_cmd=("${CHROME_BIN}")
    if [[ -n "${CHROME_TIMEOUT}" ]]; then
      chrome_cmd=(timeout "${CHROME_TIMEOUT}" "${CHROME_BIN}")
    fi
    dump="$("${chrome_cmd[@]}" \
      --headless=new \
      --no-sandbox \
      --disable-gpu \
      --use-gl=angle \
      --use-angle=swiftshader \
      --enable-unsafe-swiftshader \
      --virtual-time-budget=120000 \
      --user-data-dir="${profile_dir}" \
      --dump-dom \
      "http://127.0.0.1:${port}/index.html?frames=${frames}" 2>/dev/null)" || true
    rm -rf "${profile_dir}"

    cleanup_http
    http_pid=""

    if grep -q "FE_PLAYER_RESULT" <<<"${dump}"; then
      break
    fi
    echo "web game: [${label}] the page reported nothing on attempt ${attempt}; retrying"
  done

  rm -rf "${export_dir}"

  if ! grep -q "FE_PLAYER_RESULT" <<<"${dump}"; then
    echo "web game: [${label}] the game never reported a result" >&2
    echo "---- DOM dump ----" >&2
    echo "${dump}" >&2
    fail "[${label}] no result from the browser after ${attempts} attempts"
  fi

  # The title holds the report, HTML-escaped.
  local report
  report="$(python3 - "${dump}" <<'PY'
import html, re, sys
match = re.search(r"<title>(.*?)</title>", sys.argv[1], re.S)
print(html.unescape(match.group(1)) if match else "")
PY
)"
  echo "web game: [${label}] ${report}"

  # `frames=` proves the loop ran; `canvas=` proves the shell supplied a live
  # WebGL2 canvas; `distinct=` is the blankness check. A blank canvas — a failed
  # shader compile, a dead renderer, a scene that never drew — is one colour, and
  # a rendered scene is several, so this fails when the renderer is deliberately
  # broken without needing a golden image.
  local canvas_reported frames_reported distinct_reported
  canvas_reported="$(sed -n 's/.*canvas=\([0-9]*\).*/\1/p' <<<"${report}")"
  frames_reported="$(sed -n 's/.*frames=\([0-9]*\).*/\1/p' <<<"${report}")"
  distinct_reported="$(sed -n 's/.*distinct=\([0-9]*\).*/\1/p' <<<"${report}")"

  [[ -n "${canvas_reported}" ]] || fail "[${label}] the report carried no canvas flag: ${report}"
  [[ -n "${frames_reported}" ]] || fail "[${label}] the report carried no frame count: ${report}"
  [[ -n "${distinct_reported}" ]] || fail "[${label}] the report carried no colour count: ${report}"

  if (( canvas_reported != 1 )); then
    fail "[${label}] the game has no live canvas; the shell did not provide one"
  fi
  if (( frames_reported < 2 )); then
    fail "[${label}] the game stepped ${frames_reported} frame(s); the loop is not live"
  fi
  if (( distinct_reported < 2 )); then
    fail "[${label}] the rendered frame has ${distinct_reported} distinct colour(s); it is blank"
  fi

  echo "web game: [${label}] PASS (canvas, ${frames_reported} frames, ${distinct_reported} distinct colours)"
}

# A single project can be named for a focused run; by default both renderers are
# exercised, because the 3D path shares the whole shipping path with the 2D one
# and the point of the check is that a regression in either fails the run.
if [[ -n "${FE_WEB_GAME_PROJECT:-}" ]]; then
  run_project "${FE_WEB_GAME_PROJECT}" "$(basename "${FE_WEB_GAME_PROJECT}")" \
    "${FE_WEB_GAME_PORT:-8733}"
else
  run_project "${ROOT_DIR}/examples/demo_project" "2d" "${FE_WEB_GAME_PORT:-8733}"
  run_project "${ROOT_DIR}/examples/demo3d_project" "3d" "$((${FE_WEB_GAME_PORT:-8733} + 4))"
fi

