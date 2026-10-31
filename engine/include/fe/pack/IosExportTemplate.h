// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Error.h"
#include "fe/pack/ExportManifest.h"

#include <string>

namespace fe::pack {

// The iOS export template: the CMake/Xcode project an iOS build is, and the
// names inside it.
//
// The iOS half of the phone phase (docs/SHIPPING_QUEUE.md,
// `ios-export-template`; docs/adr/0114-ios-window-backend.md). A desktop export
// is a copied runtime and a pack (ADR-0064); a web export is a wasm pair and an
// HTML shell (ADR-0071); an Android export is a Gradle project
// (ADR-0113). iOS is a fifth shape: an Xcode-buildable app bundle, which CMake
// produces from a small project file when it is configured with the iOS
// toolchain (cmake/toolchains/ios.cmake).
//
// The template is a *string*, not a checked-in directory, for the same reason
// the web shell and the Android project are: the tool that writes the project
// and the test that checks it share this definition, so they cannot disagree
// about the file names or the shape. The names are the contract - CMake resolves
// the subdirectory, the app target is a bundle, the Info.plist names it, and the
// pack travels in its resources - and a mismatch is a build that fails on a Mac,
// the least debuggable place for a developer without one.
//
// The template writes the project and the pack's *place* in it; it does not
// build the app. Building needs a macOS host with Xcode, which an export step
// must not require and cannot fetch - exactly as an Android export does not
// invoke Gradle and a web export does not invoke Emscripten. The template is
// pure: it turns values into text and writes files, and it never invokes a
// toolchain, so it is testable on a Linux or Windows CI.

// The files and directories an iOS export writes, relative to the export
// directory. A caller assembling the project uses these rather than literals so
// the export and the template cannot drift apart.
struct IosExportNames {
    // The project file the caller configures with the iOS toolchain
    // (`cmake -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/ios.cmake`). It adds the
    // engine and builds the player as an app bundle.
    static constexpr const char* kProjectCMakeLists = "CMakeLists.txt";
    // The app's Info.plist. Names the bundle and its identifier, and records the
    // minimum OS version and the orientations a game supports.
    static constexpr const char* kInfoPlist = "app/Info.plist";
    // The directory the pack and the manifest are copied into, which the project
    // copies into the app bundle's Resources so the runtime reads them from the
    // bundle the way it reads them from disk on desktop.
    static constexpr const char* kAssetsDir = "app/assets";
};

// The iOS-specific inputs an export needs, with the defaults the ordinary
// invocation uses so the tool and the tests share one definition of them.
struct IosProjectOptions {
    // The bundle identifier (the reverse-DNS id). The default is a valid, stable
    // id for a game whose project file names none.
    std::string bundle_id = "com.forkengine.game";
    // The engine source tree the project adds as a subdirectory, so the player is
    // compiled from source for the target. Defaulted to the directory the export
    // runs from, which is the checkout root in the common case.
    std::string engine_source_dir = ".";
    // The player's translation unit, built into the app's executable. It is the
    // runtime the export ships, built for iOS rather than copied. Defaulted
    // relative to the engine source tree, which is where the checkout keeps it.
    std::string player_source = "tools/player/src/main.cpp";
    // The minimum iOS version the app targets. Matches the toolchain file's
    // default so a configure and the plist agree.
    std::string deployment_target = "15.0";
};

// Writes the iOS project into `out_dir`, with the pack and the manifest's file
// name named inside so the app bundle carries what the export wrote.
//
// `manifest.pack` and `manifest.scene` must be set; this is called after the
// manifest is built, which is also when those are known. The pack itself is
// copied by the caller (into IosExportNames::kAssetsDir); this writes the project
// that consumes it.
//
// Returns an Error if a file cannot be written.
[[nodiscard]] Error write_ios_project(const std::string& out_dir, const ExportManifest& manifest,
                                      const IosProjectOptions& options);

// The generated files, as strings. Exposed so a test can assert the names and
// the shape without invoking CMake or Xcode or writing to disk - the same reason
// the web shell's HTML and JavaScript are exposed (ADR-0071).
[[nodiscard]] std::string ios_project_cmake_lists(const ExportManifest& manifest, const IosProjectOptions& options);
[[nodiscard]] std::string ios_info_plist(const ExportManifest& manifest, const IosProjectOptions& options);

} // namespace fe::pack
