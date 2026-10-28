# The dependency payload an installed Fork Engine needs, and a CMake package
# config that names it.
#
# ADR-0084 installed the engine library and its public headers and recorded, in
# its consequences, that the tree was *runnable but not linkable*: the public
# headers include <nlohmann/json.hpp>, and the engine's link interface names glad
# (PUBLIC) and GLFW and miniaudio (PRIVATE, but a static library's private
# dependencies still have to appear on the final link line). None of those were
# installed, so a consumer who downloaded the SDK archive got a tree that looked
# like an SDK and could not be built against:
#
#   $ g++ -Iinclude app.cpp -Llib -lfork_engine
#   include/fe/serialization/Json.h:9:10: fatal error: nlohmann/json.hpp: No such
#   file or directory
#
# This module closes that gap. It installs the third-party headers and archives
# the public API and the link line need, and writes a CMake package config so
# `find_package(ForkEngine)` produces a target a consumer can link. The layout
# still comes from FEInstall.cmake's GNUInstallDirs variables, so this adds a
# payload to the installed tree rather than a second tree.
#
# What is installed, and why each one:
#
#   nlohmann/json  headers   a public header includes <nlohmann/json.hpp>
#   glad           headers + archive   the engine's PUBLIC GL loader
#   GLFW           headers + archive   the desktop window backend
#   miniaudio      header + archive    the audio backend
#   stb            header              the image loader's header
#
# glad and GLFW headers are only needed because the config points a consumer at
# the same loader the engine uses; a consumer that never names a GL symbol still
# needs the archives at link time.
#
# This is a build-time payload, not a source payload: nothing here is compiled
# for the consumer. See docs/adr/0102-downloadable-release-builds.md.

include_guard(GLOBAL)

include(CMakePackageConfigHelpers)

# Installs a target's archive (and, on a shared build, its runtime) into the
# installed library directory. Kept separate from fe_install_target() because
# these are dependency targets whose own install rules are switched off, and a
# dependency's artifact has no headers of ours to export.
function(fe_install_dependency_archive target)
  if(NOT FE_INSTALL OR NOT TARGET ${target})
    return()
  endif()
  install(
    TARGETS ${target}
    ARCHIVE DESTINATION "${FE_INSTALL_LIBDIR}"
    LIBRARY DESTINATION "${FE_INSTALL_LIBDIR}"
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
  )
endfunction()

# Installs a directory's *contents* under the include root. A caller passes the
# include root itself (the directory that holds nlohmann/, GLFW/, glad/, KHR/),
# and each of those lands directly under the installed include root. The trailing
# slash is what says "contents": without it, `install(DIRECTORY .../include)` would
# install as `include/include/...`, one level too deep. This is the opposite of
# fe_install_header_tree(), which passes `.../include/fe` and wants `include/fe`.
function(fe_install_dependency_headers dir)
  if(NOT FE_INSTALL OR NOT EXISTS "${dir}")
    return()
  endif()
  install(
    DIRECTORY "${dir}/"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
    FILES_MATCHING
    PATTERN "*.h"
    PATTERN "*.hpp"
  )
endfunction()

function(fe_install_sdk_dependencies)
  if(NOT FE_INSTALL)
    return()
  endif()

  # --- nlohmann/json: required by the public serialization headers. ----------
  if(TARGET nlohmann_json::nlohmann_json AND DEFINED json_SOURCE_DIR)
    fe_install_dependency_headers("${json_SOURCE_DIR}/include")
  endif()

  # --- stb_image: the image loader's single header. --------------------------
  # The header is installed flat (include/stb_image.h), which is the include path
  # the engine's own translation units use.
  if(EXISTS "${PROJECT_SOURCE_DIR}/third_party/stb/stb_image.h")
    install(
      FILES "${PROJECT_SOURCE_DIR}/third_party/stb/stb_image.h"
      DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
    )
  endif()

  # --- glad: the engine's PUBLIC GL loader, so a consumer needs its headers. --
  if(TARGET glad_gl_core_33)
    fe_install_dependency_headers("${PROJECT_SOURCE_DIR}/third_party/glad/include")
    fe_install_dependency_archive(glad_gl_core_33)
  endif()

  # --- GLFW: the desktop window backend. ------------------------------------
  if(TARGET glfw AND DEFINED glfw_SOURCE_DIR)
    fe_install_dependency_headers("${glfw_SOURCE_DIR}/include")
    fe_install_dependency_archive(glfw)
  endif()

  # --- miniaudio: the audio backend, a separate static library. -------------
  if(TARGET miniaudio AND DEFINED miniaudio_SOURCE_DIR)
    install(
      FILES "${miniaudio_SOURCE_DIR}/miniaudio.h"
      DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
    )
    fe_install_dependency_archive(miniaudio)
  endif()

  # ---------------------------------------------------------------------------
  # The package config.
  #
  # Written as an imported INTERFACE target that carries the include directory,
  # the engine library, the dependency archives, and the platform libraries the
  # dependencies themselves need (GLFW uses dlopen and threads; on Apple it is
  # frameworks). A consumer writes find_package(ForkEngine) and links
  # ForkEngine::engine; nothing about the payload is their problem.
  #
  # The platform libraries are chosen here, from the target platform, rather than
  # left to the consumer, because they follow from *our* dependency choices.
  # ---------------------------------------------------------------------------
  set(_config_dir "${CMAKE_BINARY_DIR}/generated/ForkEngineConfig")
  file(MAKE_DIRECTORY "${_config_dir}")

  if(APPLE)
    set(_fe_platform_libs "\"-framework Cocoa\" \"-framework IOKit\" \"-framework CoreFoundation\"")
  elseif(WIN32)
    set(_fe_platform_libs "opengl32")
  else()
    set(_fe_platform_libs "m;${CMAKE_DL_LIBS};Threads::Threads")
  endif()

  configure_file(
    "${PROJECT_SOURCE_DIR}/cmake/ForkEngineConfig.cmake.in"
    "${_config_dir}/ForkEngineConfig.cmake"
    @ONLY
  )
  write_basic_package_version_file(
    "${_config_dir}/ForkEngineConfigVersion.cmake"
    VERSION "${PROJECT_VERSION}"
    COMPATIBILITY SameMajorVersion
  )

  install(
    FILES
      "${_config_dir}/ForkEngineConfig.cmake"
      "${_config_dir}/ForkEngineConfigVersion.cmake"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/ForkEngine"
  )
endfunction()
