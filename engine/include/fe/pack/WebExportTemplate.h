// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Error.h"
#include "fe/pack/ExportManifest.h"

#include <string>

namespace fe::pack {

// The web export template: the shell a browser loads, and the names inside it.
//
// The fifth item of the web-first shipping path (docs/adr/0071-web-first-2d-shipping-path.md,
// docs/adr/0073-emscripten-toolchain.md). A desktop export is a copied runtime
// and a pack (ADR-0064); a web build cannot copy a runtime, because the player
// is a wasm module the page has to load asynchronously. So the export writes a
// shell instead: HTML that sizes the canvas and hosts the module, and JavaScript
// that loads the wasm runtime, mounts the pack over HTTP, and hands the frame
// schedule to the engine.
//
// The template is a *string*, not a checked-in file, for the same reason the
// manifest is a value: the tool that writes the shell and the test that checks it
// share this definition, so they cannot disagree about the file names. The
// names are the contract - the shell's `fetch` and the export's writes must
// match, or the page 404s at load and the build looks broken for a reason that
// has nothing to do with the game.
//
// The shell is deliberately dependency-free and offline-safe: no CDN, no
// framework, no build step. A shipped game is one HTML file, one JS module, one
// wasm binary, and one pack, all served from the same directory.

// The file names a web export writes, in the order the shell loads them. A
// caller assembling a directory uses these rather than literals so the export
// and the shell cannot drift apart.
struct WebExportNames {
    // The page a player opens. Written by write_web_shell().
    static constexpr const char* kIndexHtml = "index.html";
    // The loader the page imports: it fetches the wasm module and starts the
    // loop. Written by write_web_shell().
    static constexpr const char* kShellJs = "fork_shell.js";
    // The wasm runtime, produced by the Emscripten build of fork_player and
    // placed here by the export. Not written by the template - the template
    // names it so the shell can load it and the export can check for it.
    static constexpr const char* kRuntimeJs = "fork_player.js";
    // The wasm binary Emscripten emits beside the JS glue.
    static constexpr const char* kWasmBinary = "fork_player.wasm";
};

// Writes the shell pair (index.html and fork_shell.js) into `out_dir`, with the
// pack and scene named inside so the loader knows what to fetch and start.
//
// The shell takes the pack's file name and the scene key from the manifest, so
// the page and the manifest cannot disagree about what to load. `manifest.pack`
// and `manifest.scene` must be set; this is called after the manifest is built,
// which is also when those are known.
//
// Returns an Error if a file cannot be written. It does not check for the wasm
// runtime: assembling that is the caller's job, and a template that refused to
// write without it could not be tested without a full wasm build.
[[nodiscard]] Error write_web_shell(const std::string& out_dir, const ExportManifest& manifest);

// The shell's HTML, as a string. Exposed so a test can assert the file names the
// loader uses without writing to disk.
[[nodiscard]] std::string web_index_html(const ExportManifest& manifest);

// The shell's JavaScript, as a string. Exposed for the same reason: the test
// checks that the names it fetches are exactly the names the export writes. It
// takes no manifest: it fetches one at run time, so the page and the file on
// disk cannot disagree about the pack or the scene.
[[nodiscard]] std::string web_shell_js();

} // namespace fe::pack
