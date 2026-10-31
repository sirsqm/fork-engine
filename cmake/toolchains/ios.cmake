# iOS toolchain for the Fork Engine iOS target.
#
# Configure the iOS build with:
#
#   cmake -S . -B build-ios \
#     -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/ios.cmake \
#     -DCMAKE_OSX_ARCHITECTURES=arm64
#   cmake --build build-ios --parallel
#
# What it does:
#   * refuses to run on anything but a macOS host, loudly, at configure time:
#     an iOS build needs Xcode's toolchain and SDKs, which exist only on macOS.
#     This is the constraint docs/SHIPPING_QUEUE.md (`ios-window-backend`) says
#     to record rather than stub, and it is a *host* constraint, not a missing
#     download the way the Android NDK is;
#   * sets CMAKE_SYSTEM_NAME to iOS before the project() call, which is what
#     makes cmake/FEPlatform.cmake classify the target as "ios" (and so defines
#     FE_PLATFORM_IOS; see fe_apply_platform_definitions());
#   * pins the deployment target and the default architecture, so a configure on
#     a macOS host does not silently depend on the host's SDK or CPU;
#   * turns the desktop-only build options off by default: the editor is a
#     desktop authoring tool, and the unit tests and benchmark gate are
#     desktop/CI tools. Each is a normal cache variable a caller may override.
#
# What it deliberately does not do: it does not download or install Xcode, and it
# cannot. There is no iOS SDK to fetch the way the NDK is fetched; Xcode is a
# licensed Apple install and only macOS has it. A configure on any other host
# fails at the check below, with a message that says so. The window backend this
# toolchain will build (a Metal or GLES surface, lifecycle, and touch) lives in
# the same queue item and is not here yet; see
# docs/adr/0114-ios-window-backend.md.

# The host constraint. Xcode ships only on macOS, so an iOS cross build can only
# start there. Checked before anything else, so a Linux or Windows configure
# fails with the reason rather than a confusing compiler-not-found further on.
if(NOT CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
  message(
    FATAL_ERROR
      "The iOS target needs a macOS host with Xcode, and this host is "
      "\"${CMAKE_HOST_SYSTEM_NAME}\".\n"
      "iOS builds are not cross-compilable from Linux or Windows: the toolchain "
      "and SDKs are part of Xcode, which Apple licenses for macOS only.\n"
      "Configure this target on a macOS machine, or use the desktop, web, or "
      "Android targets instead. See docs/adr/0114-ios-window-backend.md."
  )
endif()

# The target platform, read by fe_detect_platform() (cmake/FEPlatform.cmake).
# Setting it before project() is what makes CMake use Apple's toolchain and SDK
# for the target rather than the host.
set(CMAKE_SYSTEM_NAME iOS)

# A modern-but-not-bleeding deployment floor, so the build does not depend on the
# host's SDK choosing one. A caller may override either.
if(NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET)
  set(CMAKE_OSX_DEPLOYMENT_TARGET "15.0" CACHE STRING "Minimum iOS version to target")
endif()
if(NOT DEFINED CMAKE_OSX_ARCHITECTURES)
  set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "Architectures to build for")
endif()

# iOS defaults, matching the web and Android toolchains: the editor is
# desktop-only, and the unit tests and benchmark gate are desktop/CI tools.
set(FE_BUILD_EDITOR OFF CACHE BOOL "Build the Fork Engine editor" FORCE)
set(FE_BUILD_TESTS OFF CACHE BOOL "Build the Fork Engine unit tests" FORCE)
set(FE_BUILD_BENCHMARKS OFF CACHE BOOL "Build the Fork Engine benchmarks" FORCE)
