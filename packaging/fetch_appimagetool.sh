#!/usr/bin/env bash
# Copyright (c) 2026 sqm
# SPDX-License-Identifier: MIT
#
# Fetches the pinned appimagetool and verifies it against a recorded checksum.
#
# The AppImage packaging step needs a tool that is not part of the project and is
# not in a distro repository, so it is downloaded rather than vendored. A
# download that is not pinned is a supply-chain hole and a build that changes
# under us, so the version and the SHA-256 of the exact release asset are both
# recorded here, and the hash is checked before the tool is used. Bumping the
# version means bumping both.
#
# appimagetool ships *as* an AppImage, which needs FUSE to run on a bare host.
# Every caller therefore runs it with APPIMAGE_EXTRACT_AND_RUN=1 (see
# packaging/package_appimage.cmake), which unpacks to a temporary directory
# instead of mounting - the same reason a CI container has no FUSE to give it.
#
# Usage:
#   packaging/fetch_appimagetool.sh [DESTINATION]
#
# Prints the path to the verified tool on stdout. Diagnostics go to stderr, so
# `TOOL=$(packaging/fetch_appimagetool.sh)` is safe.

set -euo pipefail

# Pinned release asset and its SHA-256. Both are updated together.
FE_APPIMAGETOOL_VERSION="1.9.1"
FE_APPIMAGETOOL_URL="https://github.com/AppImage/appimagetool/releases/download/${FE_APPIMAGETOOL_VERSION}/appimagetool-x86_64.AppImage"
FE_APPIMAGETOOL_SHA256="ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0"

DEST="${1:-}"
if [[ -z "${DEST}" ]]; then
  DEST="$(mktemp -d)/appimagetool"
fi

log() { echo "fetch appimagetool: $*" >&2; }

if [[ -f "${DEST}" ]]; then
  log "using the existing ${DEST}"
else
  log "downloading appimagetool ${FE_APPIMAGETOOL_VERSION}"
  mkdir -p "$(dirname "${DEST}")"
  # --fail so an HTTP error is an error rather than an HTML page written to the
  # destination and then failing the checksum with a confusing message.
  if ! curl -fsSL -o "${DEST}" "${FE_APPIMAGETOOL_URL}"; then
    log "download failed: ${FE_APPIMAGETOOL_URL}"
    exit 1
  fi
fi

# The pin is only meaningful if it is checked. This catches a changed upstream
# asset, a truncated download, and a compromised mirror alike.
actual="$(sha256sum "${DEST}" | awk '{print $1}')"
if [[ "${actual}" != "${FE_APPIMAGETOOL_SHA256}" ]]; then
  log "checksum mismatch for ${DEST}"
  log "  expected ${FE_APPIMAGETOOL_SHA256}"
  log "  actual   ${actual}"
  exit 1
fi

chmod +x "${DEST}"
log "verified ${DEST}"
echo "${DEST}"
