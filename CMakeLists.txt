cmake_minimum_required(VERSION 3.24)

# --- Fork Engine -------------------------------------------------------------
# A modular 2D/3D game engine. See docs/ARCHITECTURE.md for the design and
# docs/ROADMAP.md for the milestone plan.
# -----------------------------------------------------------------------------

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

project(
  fork_engine
  VERSION 0.1.0
  DESCRIPTION "A modular open-source 2D/3D game engine"
  HOMEPAGE_URL "https://github.com/sqmyou/fork-engine"
  LANGUAGES C CXX
)

# ---------------------------------------------------------------------------
# Global language settings
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE
      "Debug"
      CACHE STRING "Build type" FORCE
  )
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
endif()

# Never mix a different standard library dialect into the exported interface.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# ---------------------------------------------------------------------------
# Options
#
# Defined before the dependency module is included, because that module's
# behaviour (whether to fetch the test framework) depends on them.
# ---------------------------------------------------------------------------
option(FE_BUILD_TESTS "Build the Fork Engine unit tests" ON)
option(FE_BUILD_EXAMPLES "Build the Fork Engine example programs" ON)
option(FE_BUILD_EDITOR "Build the Fork Engine editor" ON)
option(FE_BUILD_BENCHMARKS "Build the Fork Engine benchmarks" ON)
option(FE_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(FE_USE_SYSTEM_DEPS "Prefer system-installed dependencies over FetchContent" OFF)
option(FE_ENABLE_ASAN "Enable AddressSanitizer and UndefinedBehaviorSanitizer (Debug)" OFF)
option(FE_ENABLE_TSAN "Enable ThreadSanitizer (Debug; see ADR-0068)" OFF)
# Install rules. ON by default so `cmake --install` works out of the box; a build
# tree that is never installed (a CI job that only tests) can turn them off.
option(FE_INSTALL "Generate install rules for the engine, editor, and tools" ON)
# Bake a relative RPATH into installed binaries on Linux, so a relocated tree
# runs without LD_LIBRARY_PATH. Windows and macOS do not use it.
option(FE_INSTALL_RPATH "Set a relative install RPATH on Linux" ON)

include(FESettings)
include(FEWarnings)
include(FEVersion)
include(FEDependencies)
include(FEInstall)
# The SDK payload: the third-party headers and archives the public API and the
# link line need, plus a CMake package config. Separate from FEInstall.cmake
# because it is a dependency payload, not our own targets. See ADR-0102.
include(FEInstallDependencies)

if(FE_ENABLE_ASAN AND NOT MSVC)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

# ThreadSanitizer (ADR-0068). It is mutually exclusive with ASan: they install
# incompatible interceptors, so asking for both is a configure error rather than
# an obscure runtime failure.
if(FE_ENABLE_TSAN AND FE_ENABLE_ASAN)
  message(FATAL_ERROR "FE_ENABLE_TSAN and FE_ENABLE_ASAN are mutually exclusive")
endif()
if(FE_ENABLE_TSAN AND NOT MSVC)
  add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
  add_link_options(-fsanitize=thread)
endif()

add_subdirectory(engine)

if(FE_BUILD_EXAMPLES)
  add_subdirectory(examples/sandbox)
  add_subdirectory(examples/breakout)
  # The headless example needs no display, so it is built (and can be run) even
  # where the sandbox cannot open a window. See ADR-0041.
  add_subdirectory(examples/headless)
endif()

# The packaging tools are part of the shipping path, not the editor, so they are
# built with the engine (the examples ride on the same switch).
if(FE_BUILD_EXAMPLES)
  add_subdirectory(tools/pack)
  # The runtime and the export step (ADR-0064): a project becomes a pack plus
  # the player that mounts it.
  add_subdirectory(tools/player)
  add_subdirectory(tools/export)
endif()

if(FE_BUILD_EDITOR)
  add_subdirectory(editor)
endif()

if(FE_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

# The benchmark is a dev/CI tool, not a shipping artifact, so it is gated on its
# own option and lives outside the examples.
if(FE_BUILD_BENCHMARKS)
  add_subdirectory(benchmarks)
endif()

# ---------------------------------------------------------------------------
# Install rules
#
# The engine library, the editor, and the shipping tools install; the examples,
# tests, and benchmark do not (they are development artifacts). The layout is
# GNUInstallDirs', decided in one place by cmake/FEInstall.cmake. See AGENTS.md
# for the install command.
#
# The license and the readme travel with the binaries: a redistributed engine
# must carry its license, and the readme says what the tree is.
# ---------------------------------------------------------------------------
if(FE_INSTALL)
  install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE" DESTINATION "${CMAKE_INSTALL_DATADIR}/fork-engine")
  install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/README.md" DESTINATION "${CMAKE_INSTALL_DATADIR}/fork-engine")

  # The SDK payload: dependency headers and archives plus a CMake package config,
  # so an installed tree is linkable and not merely runnable (ADR-0102). Called
  # here, after every dependency target exists, so it can see them.
  fe_install_sdk_dependencies()
endif()

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
message(STATUS "")
message(STATUS "Fork Engine ${PROJECT_VERSION}")
message(STATUS "  Build type       : ${CMAKE_BUILD_TYPE}")
message(STATUS "  Compiler         : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  Tests            : ${FE_BUILD_TESTS}")
message(STATUS "  Examples         : ${FE_BUILD_EXAMPLES}")
message(STATUS "  Editor           : ${FE_BUILD_EDITOR}")
message(STATUS "  Benchmarks       : ${FE_BUILD_BENCHMARKS}")
message(STATUS "  Warnings as errs : ${FE_WARNINGS_AS_ERRORS}")
message(STATUS "  System deps      : ${FE_USE_SYSTEM_DEPS}")
message(STATUS "  Install rules    : ${FE_INSTALL}")
message(STATUS "")
