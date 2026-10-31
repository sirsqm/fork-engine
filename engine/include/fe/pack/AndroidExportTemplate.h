// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Error.h"
#include "fe/pack/ExportManifest.h"

#include <string>

namespace fe::pack {

// The Android export template: the Gradle project an Android build is, and the
// names inside it.
//
// The second phone item of the shipping path (docs/SHIPPING_QUEUE.md,
// `android-export-template`; docs/adr/0071-web-first-2d-shipping-path.md). A
// desktop export is a copied runtime and a pack (ADR-0064); a web export is a
// wasm pair and an HTML shell (ADR-0071). Android is a third shape: there is no
// single executable to copy. An Android build is a *Gradle project* whose app
// module compiles the player for the phone's ABI and packages it, the pack, and
// the manifest into an APK.
//
// The template is a *string*, not a checked-in directory, for the same reason
// the web shell is (ADR-0071): the tool that writes the project and the test
// that checks it share this definition, so they cannot disagree about the file
// names or the shape. The names are the contract - Gradle resolves the modules,
// the manifest names the native library, and the JNI CMakeLists builds it - and
// a mismatch is a build that fails on a phone, the least debuggable place.
//
// The template writes the project and the pack's *place* in it; it does not
// build the APK. Building needs the Android SDK, an NDK, and Gradle, none of
// which an export step should require - exactly as a web export assembles a wasm
// pair Emscripten already produced rather than invoking Emscripten. The template
// is pure: it turns values into text and writes files, and it never invokes a
// toolchain, so it is testable without an Android SDK on the machine.

// The files and directories an Android export writes, relative to the export
// directory. A caller assembling the project uses these rather than literals so
// the export and the template cannot drift apart.
struct AndroidExportNames {
    // The Gradle settings file. Names the app module, so Gradle knows what to
    // build without a convention it has to infer.
    static constexpr const char* kSettingsGradle = "settings.gradle";
    // The root build file. Declares the Android Gradle Plugin version once.
    static constexpr const char* kRootBuildGradle = "build.gradle";
    // The Gradle properties. Pins the AndroidX flag and the JVM heap, both of
    // which the Android Gradle Plugin expects to find here.
    static constexpr const char* kGradleProperties = "gradle.properties";
    // The app module's build file: the application, its native build, and its
    // assets.
    static constexpr const char* kAppBuildGradle = "app/build.gradle";
    // The Android manifest, which declares the native activity and the library
    // it loads.
    static constexpr const char* kAndroidManifest = "app/src/main/AndroidManifest.xml";
    // The CMake file the app module's `externalNativeBuild` runs, which builds
    // the player for the target ABI.
    static constexpr const char* kJniCMakeLists = "app/src/main/jni/CMakeLists.txt";
    // The directory the pack and the manifest are copied into, where the Android
    // Gradle Plugin packages them into the APK's assets.
    static constexpr const char* kAssetsDir = "app/src/main/assets";
};

// The Android-specific inputs an export needs, with the defaults the ordinary
// invocation uses so the tool and the tests share one definition of them.
struct AndroidProjectOptions {
    // The application id (the Java package). The default is a valid, stable id
    // for a game whose project file names none.
    std::string application_id = "com.forkengine.game";
    // The engine source tree the JNI CMakeLists adds as a subdirectory, so the
    // player is compiled from source for the phone's ABI. Defaulted to the
    // directory the export runs from, which is the checkout root in the common
    // case.
    std::string engine_source_dir = ".";
    // The player's translation unit, compiled into the native library the
    // activity loads. It is the runtime the export ships, built for Android
    // rather than copied. Defaulted relative to the engine source tree, which is
    // where the checkout keeps it.
    std::string player_source = "tools/player/src/main.cpp";
    // The native library the manifest tells the activity to load. It must match
    // the CMake target's output name, or the activity starts and finds no
    // library.
    std::string native_library = "fork_player";
    // The minimum and target API levels. 24 is the floor the toolchain file
    // documents; 34 is a current target.
    i32 min_sdk = 24;
    i32 target_sdk = 34;
};

// Writes the Android Gradle project into `out_dir`, with the pack and the
// manifest's file name named inside so the app module packages what the export
// wrote.
//
// `manifest.pack` and `manifest.scene` must be set; this is called after the
// manifest is built, which is also when those are known. The pack itself is
// copied by the caller (into AndroidExportNames::kAssetsDir); this writes the
// project that consumes it.
//
// Returns an Error if a file cannot be written.
[[nodiscard]] Error write_android_project(const std::string& out_dir, const ExportManifest& manifest,
                                          const AndroidProjectOptions& options);

// The generated files, as strings. Exposed so a test can assert the names and
// the shape without invoking Gradle or writing to disk - the same reason the web
// shell's HTML and JavaScript are exposed (ADR-0071).
[[nodiscard]] std::string android_settings_gradle();
[[nodiscard]] std::string android_root_build_gradle();
[[nodiscard]] std::string android_gradle_properties();
[[nodiscard]] std::string android_app_build_gradle(const ExportManifest& manifest,
                                                   const AndroidProjectOptions& options);
[[nodiscard]] std::string android_manifest_xml(const ExportManifest& manifest, const AndroidProjectOptions& options);
[[nodiscard]] std::string android_jni_cmake_lists(const AndroidProjectOptions& options);

} // namespace fe::pack
