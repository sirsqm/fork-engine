# Android toolchain for the Fork Engine Android target.
#
# Configure the Android build with:
#
#   cmake -S . -B build-android \
#     -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/android.cmake \
#     -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-24 \
#     -DANDROID_NDK=/path/to/android-ndk
#   cmake --build build-android --parallel
#
# What it does:
#   * finds the NDK, from -DANDROID_NDK, $ANDROID_NDK_HOME or $ANDROID_NDK_ROOT,
#     and fails loudly, at configure time, with install instructions when there
#     is none;
#   * sets CMAKE_SYSTEM_NAME to Android before the project() call, which is what
#     makes cmake/FEPlatform.cmake classify the target as "android" (and so
#     defines FE_PLATFORM_ANDROID; see fe_apply_platform_definitions());
#   * delegates the ABI, sysroot, compiler, and API-level setup to the NDK's own
#     toolchain file, which is the one thing that knows them and is versioned
#     with the NDK;
#   * turns the desktop-only build options off by default: the editor is a
#     desktop authoring tool, and the unit tests and benchmark gate are
#     desktop/CI tools. Each is a normal cache variable a caller may override.
#
# What it deliberately does not do: it does not download or install the NDK. The
# NDK is a multi-gigabyte download, so an automation run must not spend its
# budget on one; a configure without it fails here, at the check below, with a
# message that says so. The window backend this toolchain will build lives in the
# same queue item and is not here yet; see
# docs/adr/0105-android-window-backend.md.

# The NDK location: the cache variable first, then the environment variables the
# SDK/NDK installers set. An empty result is the failure the check below reports.
if(NOT DEFINED ANDROID_NDK)
  if(DEFINED ENV{ANDROID_NDK_HOME})
    set(ANDROID_NDK "$ENV{ANDROID_NDK_HOME}")
  elseif(DEFINED ENV{ANDROID_NDK_ROOT})
    set(ANDROID_NDK "$ENV{ANDROID_NDK_ROOT}")
  else()
    set(ANDROID_NDK "")
  endif()
endif()

set(FE_ANDROID_NDK_TOOLCHAIN "${ANDROID_NDK}/build/cmake/android.toolchain.cmake")
if(NOT EXISTS "${FE_ANDROID_NDK_TOOLCHAIN}")
  message(
    FATAL_ERROR
      "Android NDK not found (looked for \"${FE_ANDROID_NDK_TOOLCHAIN}\").\n"
      "Install the NDK, then configure again with it named explicitly:\n"
      "  cmake -S . -B build-android \\\n"
      "    -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/android.cmake \\\n"
      "    -DANDROID_NDK=/path/to/android-ndk -DANDROID_ABI=arm64-v8a\n"
      "Alternatively set ANDROID_NDK_HOME, or pass -DANDROID_NDK=/path/to/android-ndk."
  )
endif()

# The target platform, read by fe_detect_platform() (cmake/FEPlatform.cmake). The
# NDK toolchain sets the same value; setting it here keeps the classification
# correct even if a future NDK file stopped doing so.
set(CMAKE_SYSTEM_NAME Android)

# The NDK's own toolchain sets the compiler, sysroot, ABI, and API level.
# Delegating to it (rather than reimplementing it) is what keeps this file small
# and correct across NDK releases.
include("${FE_ANDROID_NDK_TOOLCHAIN}")

# Android defaults, matching the web toolchain: the editor is desktop-only, and
# the unit tests and benchmark gate are desktop/CI tools.
set(FE_BUILD_EDITOR OFF CACHE BOOL "Build the Fork Engine editor" FORCE)
set(FE_BUILD_TESTS OFF CACHE BOOL "Build the Fork Engine unit tests" FORCE)
set(FE_BUILD_BENCHMARKS OFF CACHE BOOL "Build the Fork Engine benchmarks" FORCE)
