#!/usr/bin/env bash
# Copyright (c) 2026 sqm
# SPDX-License-Identifier: MIT
#
# Installs a build tree and checks the installed tree is complete and runnable.
# This is the acceptance test for the install rules (docs/SHIPPING_QUEUE.md,
# `desktop-export-templates`): an installed tree is what a packager and a
# downstream consumer see, and a missing library or header is invisible in a
# build tree, where everything is found by path.
#
# It asserts the layout (bin/lib/include/share), that every installed binary
# runs from the install prefix (so a missing shared library or a wrong RPATH
# fails here rather than at a user's first run), and that the public headers and
# the license are present. It deliberately does not assert the *content* of the
# headers - the build already compiles against them.
#
#   tests/install/run_install_smoke.sh            # installs build/ to a temp dir
#   FE_INSTALL_BUILD_DIR=DIR tests/install/run_install_smoke.sh
#
# Requirements: a configured build tree with install rules on (the default).

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"
BUILD_DIR="${FE_INSTALL_BUILD_DIR:-${ROOT_DIR}/build}"

fail() {
  echo "install smoke: $*" >&2
  exit 1
}

[[ -f "${BUILD_DIR}/CMakeCache.txt" ]] ||
  fail "no configured build tree at ${BUILD_DIR}; configure one first"

PREFIX="$(mktemp -d)"
trap 'rm -rf "${PREFIX}"' EXIT

echo "install smoke: installing ${BUILD_DIR} to ${PREFIX}"
cmake --install "${BUILD_DIR}" --prefix "${PREFIX}" >/dev/null || fail "cmake --install failed"

# The layout: an installed tree is only useful if a consumer can find the
# library, the headers, and the tools by the platform's convention.
[[ -d "${PREFIX}/bin" ]] || fail "no bin/ in the installed tree"
[[ -d "${PREFIX}/include/fe" ]] || fail "no include/fe/ in the installed tree"
[[ -f "${PREFIX}/include/fe/version/Version.h" ]] ||
  fail "the generated version header was not installed"
[[ -f "${PREFIX}/include/fe/core/FileSystem.h" ]] ||
  fail "the public headers were not installed"
[[ -f "${PREFIX}/share/fork-engine/LICENSE" ]] || fail "the license was not installed"

# The library. On Windows a shared library lives in bin/ beside the executables;
# elsewhere it is in lib/. Accept either, and require at least one library file.
found_lib=0
for dir in "${PREFIX}/lib" "${PREFIX}/bin"; do
  for lib in "${dir}"/libfork_engine.* "${dir}"/fork_engine.*; do
    [[ -f "${lib}" ]] && found_lib=1
  done
done
(( found_lib == 1 )) || fail "no fork_engine library in the installed tree"

# Every installed executable must run from the prefix. `--help` is the cheapest
# invocation that loads the shared libraries and exits 0; a missing library or a
# broken RPATH fails here, which is the point of running them at all.
run_ok() {
  local exe="$1"
  [[ -x "${exe}" ]] || fail "$(basename "${exe}") was not installed or is not executable"
  if ! "${exe}" --help >/dev/null 2>&1; then
    fail "$(basename "${exe}") did not run from the install prefix"
  fi
}

run_ok "${PREFIX}/bin/fork_pack"
run_ok "${PREFIX}/bin/fork_player"
run_ok "${PREFIX}/bin/fork_export"
run_ok "${PREFIX}/bin/fork_editor_shell"

echo "install smoke: PASS (bin, lib, include, share; every tool runs from the prefix)"
