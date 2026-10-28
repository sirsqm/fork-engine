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
CONSUMER="$(mktemp -d)"
trap 'rm -rf "${PREFIX}" "${CONSUMER}"' EXIT

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

# The installed tree must be *linkable*, not only runnable (ADR-0102). A consumer
# builds against it through find_package, so a missing third-party header or a
# missing dependency archive is the failure this catches: the public serialization
# headers include <nlohmann/json.hpp>, and the engine is a static library whose
# GLFW and miniaudio dependencies still have to appear on the consumer's link
# line. Compiling a real consumer is the only way to see that, because the build
# tree supplies all of it by path.
cat > "${CONSUMER}/main.cpp" <<'CPP'
#include "fe/platform/Window.h"
#include "fe/renderer2d/Renderer2D.h"
#include "fe/scene/Scene.h"
#include "fe/serialization/Json.h"

#include <cstdio>

int main() {
  fe::Scene scene;
  nlohmann::json doc;
  doc["scene"] = "consumer";
  fe::WindowDesc desc;
  std::printf("consumer linked: %s\n", doc["scene"].get<std::string>().c_str());
  return 0;
}
CPP

cat > "${CONSUMER}/CMakeLists.txt" <<'CMAKE'
cmake_minimum_required(VERSION 3.20)
project(fe_consumer CXX)
find_package(ForkEngine REQUIRED)
add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE ForkEngine::engine)
target_compile_features(consumer PRIVATE cxx_std_20)
CMAKE

echo "install smoke: building a consumer against the installed tree"
cmake -S "${CONSUMER}" -B "${CONSUMER}/build" \
  -DCMAKE_PREFIX_PATH="${PREFIX}" >/dev/null 2>&1 ||
  fail "find_package(ForkEngine) failed; the installed tree is not consumable"
cmake --build "${CONSUMER}/build" >/dev/null 2>&1 ||
  fail "a consumer failed to compile or link against the installed tree"
"${CONSUMER}/build/consumer" >/dev/null 2>&1 ||
  fail "the consumer built against the installed tree but did not run"

echo "install smoke: PASS (bin, lib, include, share; every tool runs; a consumer links)"
