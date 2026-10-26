// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Error.h"
#include "fe/core/Types.h"

#include <string>
#include <string_view>
#include <vector>

namespace fe::pack {

// What an exported build is: the pack to mount and the scene to start in.
//
// The export step ([ADR-0064](../../docs/adr/0064-export-runtime-and-player.md))
// writes this beside the runtime and the pack so a launcher, a packager, or a
// later per-target template has one file naming what to run rather than a
// convention it has to infer from file names.
//
// It is a pure value: it parses and serialises itself, touches no filesystem,
// and knows nothing about the player. The tool that writes it and the runtime
// that reads it share this type, so the two cannot disagree about the document's
// shape - which is the same reason `PackFile` is a value rather than a pair of
// ad-hoc readers (docs/adr/0030-asset-pack-format.md).
//
// Paths inside the document are relative to the manifest, so an export
// directory can be moved or copied whole without rewriting it.
struct ExportManifest {
    // The document's format marker and version. The marker catches a JSON file
    // that is not an export manifest; the version catches one from the future.
    // A reader rejects both rather than guessing.
    static constexpr const char* kFormatName = "fork_export";
    static constexpr i32 kFormatVersion = 1;

    // The on-disk name the runtime looks for when it is pointed at an export
    // directory rather than a pack.
    static constexpr const char* kDefaultFileName = "fork.export.json";

    // The target an export was built for. The web template (ADR-0071, item 5)
    // writes "web"; everything else writes the default. A launcher reads this to
    // know what it has without probing file names, and the web shell refuses a
    // build that is not its own so a desktop export served by mistake fails with
    // a sentence rather than a blank canvas.
    static constexpr const char* kTargetDesktop = "desktop";
    static constexpr const char* kTargetWeb = "web";

    // The project's name, for a launcher to display. Not used to find anything.
    std::string name;
    // What this build runs on: kTargetDesktop or kTargetWeb. Absent in a
    // document written before targets existed, which reads as desktop - the
    // only target that could have produced it.
    std::string target = kTargetDesktop;
    // The pack file to mount, relative to the manifest's directory.
    std::string pack;
    // The key the startup scene is stored under inside the pack, which is the
    // path the scene reader is given ("scenes/main.fescene").
    std::string scene;
    // The key a declarative input-binding document is stored under inside the
    // pack ("input/bindings.json"), or empty for a build that declares no
    // bindings of its own (docs/PLAN.md, `touch-gesture-regions`). The player
    // reads it after the engine's own declarations, so a game's regions are data
    // rather than code. Empty is the common case and is omitted from the
    // document, so a build without bindings is byte-for-byte what it was before
    // this field existed.
    std::string bindings;
    // The gameplay modules to load, in order, relative to the manifest's
    // directory. Each is a shared library the player loads before it reads the
    // scene, so the behaviours the scene binds are registered by the time the
    // scene asks for them (ADR-0061, ADR-0063). Empty for a game whose scene
    // binds no behaviours, which is the common case for a data-only build.
    std::vector<std::string> modules;

    // Serialises to the document text. Always writes the format marker and the
    // current version, so a manifest is self-describing.
    [[nodiscard]] Result<std::string> to_json() const;

    // Parses a document. Fails on malformed JSON, a wrong marker, a version
    // newer than supported, or a missing pack/scene. Never returns a partial
    // manifest, so a caller that got an Error has no half-read value to reason
    // about.
    [[nodiscard]] static Error from_json(std::string_view text, ExportManifest& out);

    // Reads and writes the file.
    [[nodiscard]] static Result<ExportManifest> load(std::string_view path);
    [[nodiscard]] Error save(std::string_view path) const;
};

} // namespace fe::pack
