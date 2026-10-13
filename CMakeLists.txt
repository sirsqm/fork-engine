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
option(FE_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(FE_USE_SYSTEM_DEPS "Prefer system-installed dependencies over FetchContent" OFF)
option(FE_ENABLE_ASAN "Enable AddressSanitizer and UndefinedBehaviorSanitizer (Debug)" OFF)

include(FESettings)
include(FEWarnings)
include(FEVersion)
include(FEDependencies)

if(FE_ENABLE_ASAN AND NOT MSVC)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

add_subdirectory(engine)

if(FE_BUILD_EXAMPLES)
  add_subdirectory(examples/sandbox)
endif()

if(FE_BUILD_EDITOR)
  add_subdirectory(editor)
endif()

if(FE_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
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
message(STATUS "  Warnings as errs : ${FE_WARNINGS_AS_ERRORS}")
message(STATUS "  System deps      : ${FE_USE_SYSTEM_DEPS}")
message(STATUS "")
