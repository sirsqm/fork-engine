// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/pack/WebExportTemplate.h"

#include "fe/core/FileSystem.h"

#include <nlohmann/json.hpp>

#include <string>

namespace fe::pack {
namespace {

// The canvas the engine's web window backend renders into when a descriptor
// names none. Kept in step with WebWindow's default so the shell and the
// backend agree on the element id without either importing the other: the
// backend is internal to the platform module and a shipped shell cannot link it.
constexpr const char* kCanvasId = "canvas";

// The Emscripten module factory the shell expects the wasm runtime to export.
// The web build sets `-sMODULARIZE=1 -sEXPORT_NAME=createForkPlayer`; if it does
// not, the shell reports that the runtime did not load rather than failing with
// an opaque "not a function" from inside the loader.
constexpr const char* kModuleFactory = "createForkPlayer";

// A JSON string literal for embedding a value in the generated JavaScript. Using
// the same serialiser the manifest uses means a pack name with a quote or a
// backslash cannot break out of the literal and cannot silently corrupt the
// shell - a hand-rolled escape here would be the obvious place for that bug.
[[nodiscard]] std::string js_literal(const std::string& value) {
    return nlohmann::json(value).dump();
}

} // namespace

std::string web_index_html(const ExportManifest& manifest) {
    const std::string title = manifest.name.empty() ? std::string("Fork Engine") : manifest.name;

    // The page is deliberately minimal. Everything that needs to fail
    // gracefully (a missing runtime, a wrong target, a fetch error) happens in
    // the module, where it can write a sentence into the status element instead
    // of leaving a blank canvas. The page itself cannot fail.
    //
    // The canvas fills the whole viewport and the page never scrolls. That is the
    // mobile shape (docs/SHIPPING_QUEUE.md, `mobile-viewport-meta`): a fixed-aspect box
    // centred in a scrollable page renders at desktop scale on a phone, with
    // browser chrome and scrollbars around it, and a touch near the edge lands
    // outside the canvas the backend binds its listeners to. Full-bleed is also
    // what makes a safe-area inset meaningful - there is no letterbox to hide in.
    //
    // `viewport-fit=cover` lets the page paint under a notch or a home indicator,
    // which is what a full-bleed game wants; `user-scalable=no` stops a
    // double-tap from zooming the page out from under the game; `touch-action:
    // none` on the canvas stops the browser from claiming a drag as a scroll or a
    // pinch before the game sees it.
    std::string html;
    html += "<!DOCTYPE html>\n";
    html += "<html lang=\"en\">\n";
    html += "<head>\n";
    html += "  <meta charset=\"utf-8\">\n";
    html += "  <meta name=\"viewport\" content=\"width=device-width, initial-scale=1,"
            " viewport-fit=cover, user-scalable=no\">\n";
    html += "  <meta name=\"mobile-web-app-capable\" content=\"yes\">\n";
    html += "  <meta name=\"apple-mobile-web-app-capable\" content=\"yes\">\n";
    html += "  <meta name=\"apple-mobile-web-app-status-bar-style\" content=\"black-translucent\">\n";
    html += "  <meta name=\"theme-color\" content=\"#101014\">\n";
    html += "  <title>" + title + "</title>\n";
    html += "  <style>\n";
    html += "    html, body { margin: 0; height: 100%; background: #101014;"
            " overflow: hidden; overscroll-behavior: none; }\n";
    html += "    body { touch-action: none; -webkit-user-select: none; user-select: none; }\n";
    html += "    #" + std::string(kCanvasId) +
            " { position: fixed; inset: 0; width: 100%; height: 100%; display: block;"
            " touch-action: none; }\n";
    html += "    #status { position: fixed; inset: 0; display: none; align-items: center;"
            " justify-content: center; color: #e6e6e6; font: 14px/1.5 system-ui, sans-serif;"
            " padding: 2rem; text-align: center; white-space: pre-wrap; }\n";
    // A hidden element the shell writes its layout measurement into. It is a
    // sibling of the canvas rather than the title because the runtime already
    // owns the title for its own result, and because the measurement is read
    // from a DOM dump rather than shown to a player.
    html += "    #layout { display: none; }\n";
    html += "  </style>\n";
    html += "</head>\n";
    html += "<body>\n";
    // The canvas exists before the module loads, so the backend finds the
    // element the descriptor names rather than creating one at the wrong size.
    html += "  <canvas id=\"" + std::string(kCanvasId) + "\" width=\"1280\" height=\"720\"></canvas>\n";
    html += "  <div id=\"status\"></div>\n";
    html += "  <div id=\"layout\"></div>\n";
    html += "  <script type=\"module\" src=\"" + std::string(WebExportNames::kShellJs) + "\"></script>\n";
    html += "</body>\n";
    html += "</html>\n";
    return html;
}

std::string web_shell_js() {
    // The manifest is fetched rather than baked in, so the page and the file on
    // disk cannot disagree about the pack or the scene. The runtime file names
    // are constants because the module loader must know them before any fetch
    // completes; a test asserts they are the names the export writes.
    std::string js;
    js += "// Generated by fork_export --target web. Do not edit.\n";
    js += "//\n";
    js += "// Loads the wasm runtime, mounts the pack, and hands the frame schedule to\n";
    js += "// the engine: the browser half of the web export template\n";
    js += "// (docs/adr/0071-web-first-2d-shipping-path.md).\n";
    js += "\n";
    js += "const RUNTIME_JS = " + js_literal(WebExportNames::kRuntimeJs) + ";\n";
    js += "const MANIFEST = " + js_literal(ExportManifest::kDefaultFileName) + ";\n";
    js += "const CANVAS_ID = " + js_literal(kCanvasId) + ";\n";
    js += "const MODULE_FACTORY = " + js_literal(kModuleFactory) + ";\n";
    js += "\n";
    js += "function fail(message) {\n";
    js += "  const status = document.getElementById('status');\n";
    js += "  if (status) {\n";
    js += "    status.textContent = message;\n";
    js += "    status.style.display = 'flex';\n";
    js += "  }\n";
    js += "  console.error('fork_shell: ' + message);\n";
    js += "}\n";
    js += "\n";
    // The page's own layout measurement, written into the hidden `#layout`
    // element so an automated check can read it out of a DOM dump. It proves the
    // canvas fills the window: a phone-sized window must not letterbox or scroll.
    // `fullbleed` is the canvas covering the window's whole CSS box; the
    // tolerance absorbs a sub-pixel layout rounding, not a real gap.
    js += "function reportLayout() {\n";
    js += "  const canvas = document.getElementById(CANVAS_ID);\n";
    js += "  const rect = canvas.getBoundingClientRect();\n";
    js += "  const w = window.innerWidth;\n";
    js += "  const h = window.innerHeight;\n";
    js += "  const fullbleed = Math.abs(rect.width - w) <= 1 && Math.abs(rect.height - h) <= 1;\n";
    js += "  const element = document.getElementById('layout');\n";
    js += "  if (element) {\n";
    js += "    element.textContent = 'FE_SHELL_LAYOUT canvas=' + Math.round(rect.width) + 'x' +\n";
    js += "      Math.round(rect.height) + ' window=' + w + 'x' + h + ' fullbleed=' + (fullbleed ? 1 : 0);\n";
    js += "  }\n";
    js += "}\n";
    js += "\n";
    js += "async function start() {\n";
    js += "  const response = await fetch(MANIFEST, { cache: 'no-store' });\n";
    js += "  if (!response.ok) {\n";
    js += "    throw new Error('could not read ' + MANIFEST + ' (HTTP ' + response.status + ')');\n";
    js += "  }\n";
    js += "  const manifest = await response.json();\n";
    js += "  // Refuse a build that is not ours. Serving a desktop export from a web\n";
    js += "  // server is an easy mistake, and a sentence beats a blank canvas.\n";
    js += "  if (manifest.target && manifest.target !== 'web') {\n";
    js += "    throw new Error('this build targets \\'' + manifest.target + '\\', not the web');\n";
    js += "  }\n";
    js += "\n";
    js += "  const module = await import('./' + RUNTIME_JS);\n";
    js += "  const factory = module[MODULE_FACTORY] || module.default;\n";
    js += "  if (typeof factory !== 'function') {\n";
    js += "    throw new Error(RUNTIME_JS + ' does not export ' + MODULE_FACTORY);\n";
    js += "  }\n";
    js += "\n";
    js += "  // The module locates its .wasm beside itself, and the pack is fetched by\n";
    js += "  // the engine's HTTP resource source from the page's own origin, so a build\n";
    js += "  // served from any subdirectory works without a base-path setting.\n";
    js += "  //\n";
    js += "  // A `?frames=N` on the page bounds the run to N frames. A game otherwise\n";
    js += "  // runs until its tab closes, which is right for a player and impossible for\n";
    js += "  // an automated check to observe: the headless-browser verification uses it\n";
    js += "  // to let the shipped shell run, stop, and report what it drew.\n";
    js += "  const runtimeArguments = ['--pack', manifest.pack, '--scene', manifest.scene];\n";
    js += "  // A game's own declarative input bindings, if the build ships them. The\n";
    js += "  // key is inside the pack, so it is passed through verbatim. Without this\n";
    js += "  // the runtime would fall back to the engine's built-in layout and the\n";
    js += "  // game's own regions would never load.\n";
    js += "  if (manifest.bindings) {\n";
    js += "    runtimeArguments.push('--bindings', manifest.bindings);\n";
    js += "  }\n";
    js += "  reportLayout();\n";
    js += "  const frames = new URLSearchParams(location.search).get('frames');\n";
    js += "  if (frames && /^[0-9]+$/.test(frames)) {\n";
    js += "    runtimeArguments.push('--frames', frames);\n";
    js += "  }\n";
    js += "  // A `?probe=1` on the page turns on the input probe: the runtime presses\n";
    js += "  // movement keys with synthetic DOM events and reports how far the watched\n";
    js += "  // node moved. It is how the headless-browser verification proves input\n";
    js += "  // reaches gameplay, which a browser harness cannot do by pressing a key.\n";
    js += "  // `?probeNode=NAME` watches a node other than the default (Camera).\n";
    js += "  // `?touch=1` makes the probe a touch on the canvas instead of a key press,\n";
    js += "  // which is how the headless check proves touch reaches gameplay; `?touchAt=x,y`\n";
    js += "  // aims it at a normalised point (docs/SHIPPING_QUEUE.md, `touch-gesture-regions`).\n";
    js += "  const probe = new URLSearchParams(location.search).get('probe');\n";
    js += "  if (probe === '1') {\n";
    js += "    const touch = new URLSearchParams(location.search).get('touch');\n";
    js += "    runtimeArguments.push(touch === '1' ? '--probe-touch' : '--probe-input');\n";
    js += "    const touchAt = new URLSearchParams(location.search).get('touchAt');\n";
    js += "    if (touch === '1' && touchAt && /^[0-9]*\\.?[0-9]+,[0-9]*\\.?[0-9]+$/.test(touchAt)) {\n";
    js += "      runtimeArguments.push('--probe-touch-at', touchAt);\n";
    js += "    }\n";
    js += "    const probeNode = new URLSearchParams(location.search).get('probeNode');\n";
    js += "    if (probeNode) {\n";
    js += "      runtimeArguments.push('--probe-node', probeNode);\n";
    js += "    }\n";
    js += "  }\n";
    js += "\n";
    js += "  const instance = await factory({\n";
    js += "    locateFile: (path) => path,\n";
    js += "    canvas: document.getElementById(CANVAS_ID),\n";
    js += "    arguments: runtimeArguments,\n";
    js += "  });\n";
    js += "\n";
    js += "  // The runtime drives its own loop through requestAnimationFrame once it\n";
    js += "  // has mounted the pack, so there is nothing to schedule here.\n";
    js += "  if (instance && typeof instance.then === 'function') {\n";
    js += "    await instance;\n";
    js += "  }\n";
    js += "}\n";
    js += "\n";
    // A phone game wants the whole screen. A browser only grants fullscreen from
    // a user gesture, and the first touch is the earliest one a game reliably
    // gets, so it is taken as the cue. It degrades silently: a browser that
    // refuses (desktop, an embedded webview, a policy) leaves the game playable
    // in the frame it already has, so a rejection is not an error and is not
    // reported as one (docs/SHIPPING_QUEUE.md, `touch-fullscreen-prompt`).
    //
    // `touchstart` is registered `{ once: true }` so the request is made at most
    // once rather than on every touch, and passive so the listener cannot delay
    // the first frame's input. The element is the canvas, matching where the
    // backend binds its own touch listener.
    js += "function requestFullscreenOnFirstTouch() {\n";
    js += "  const canvas = document.getElementById(CANVAS_ID);\n";
    js += "  if (!canvas) {\n";
    js += "    return;\n";
    js += "  }\n";
    js += "  const enter = () => {\n";
    js += "    const target = document.documentElement;\n";
    js += "    const request = target.requestFullscreen || target.webkitRequestFullscreen;\n";
    js += "    if (typeof request !== 'function') {\n";
    js += "      return;\n";
    js += "    }\n";
    js += "    try {\n";
    js += "      const result = request.call(target);\n";
    js += "      if (result && typeof result.catch === 'function') {\n";
    js += "        result.catch(() => {});\n";
    js += "      }\n";
    js += "    } catch (error) {\n";
    js += "      // A refusal is not a failure: the game is already usable.\n";
    js += "    }\n";
    js += "  };\n";
    js += "  canvas.addEventListener('touchstart', enter, { once: true, passive: true });\n";
    js += "  canvas.addEventListener('pointerdown', enter, { once: true, passive: true });\n";
    js += "}\n";
    js += "\n";
    js += "requestFullscreenOnFirstTouch();\n";
    js += "\n";
    js += "start().catch((error) => fail(String(error && error.message ? error.message : error)));\n";
    return js;
}

Error write_web_shell(const std::string& out_dir, const ExportManifest& manifest) {
    if (manifest.pack.empty() || manifest.scene.empty()) {
        return Error("web export template: the manifest names no pack or scene");
    }

    const std::string index_path = fs::join(out_dir, WebExportNames::kIndexHtml);
    if (Error error = fs::write_text_file(index_path, web_index_html(manifest)); error) {
        return error;
    }

    const std::string shell_path = fs::join(out_dir, WebExportNames::kShellJs);
    if (Error error = fs::write_text_file(shell_path, web_shell_js()); error) {
        return error;
    }

    return Error::none();
}

} // namespace fe::pack
