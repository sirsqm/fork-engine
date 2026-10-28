# Test that the installed SDK is linkable, run by ctest in script mode.
#
# ADR-0084 installed the engine library and its public headers and recorded, in
# its consequences, that the tree was runnable but not linkable. ADR-0102 closes
# that gap by installing the dependency payload (nlohmann/json, glad, GLFW,
# miniaudio, stb) and a CMake package config. This guards the decision: the
# payload and the config must stay, because the failure they prevent is invisible
# in the build tree, where every dependency is found by path, and shows up only
# for a consumer of an installed tree.
#
# The install smoke test (tests/install/run_install_smoke.sh) proves the tree
# actually links by building a consumer against it. This test proves the pieces
# the tree needs are still named in the install rules, so a refactor that drops
# one fails here with a clear message rather than as a link error in the smoke
# test's suppressed output.
#
# Run by tests/CMakeLists.txt:
#   cmake -DDEP_INSTALL=<path> -DENGINE_CMAKE=<path> -P
#         cmake/tests/test_sdk_consumable.cmake

foreach(_required DEP_INSTALL ENGINE_CMAKE)
  if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
    message(FATAL_ERROR "${_required} must be set (see the header of this file)")
  endif()
  if(NOT EXISTS "${${_required}}")
    message(FATAL_ERROR "${_required} does not exist: ${${_required}}")
  endif()
endforeach()

file(READ "${DEP_INSTALL}" _deps)

# check: <label>|<literal substring the install module must contain>
set(_checks
    "installs the nlohmann/json headers|json_SOURCE_DIR"
    "installs the glad headers and archive|glad_gl_core_33"
    "installs the GLFW headers and archive|glfw"
    "installs the miniaudio header and archive|miniaudio"
    "installs the stb header|stb_image.h"
    "installs a CMake package config|ForkEngineConfig.cmake"
    "installs the version file|write_basic_package_version_file"
    "writes the config into the cmake dir|cmake/ForkEngine"
    "names the platform libraries|_fe_platform_libs"
)

foreach(_check IN LISTS _checks)
  string(REPLACE "|" ";" _parts "${_check}")
  list(GET _parts 0 _label)
  list(GET _parts 1 _needle)
  string(FIND "${_deps}" "${_needle}" _at)
  if(_at EQUAL -1)
    message(FATAL_ERROR "the SDK install is missing '${_label}' (looked for: ${_needle})")
  endif()
  message(STATUS "  ${_label}: ok")
endforeach()

# The config template must exist and must create the imported target a consumer
# links. A config that is installed but names no target is the ADR-0084 tree with
# extra steps.
set(_template "${CMAKE_CURRENT_LIST_DIR}/../../cmake/ForkEngineConfig.cmake.in")
if(NOT EXISTS "${_template}")
  message(FATAL_ERROR "ForkEngineConfig.cmake.in is missing; the config cannot be generated")
endif()
file(READ "${_template}" _config)
if(NOT _config MATCHES "ForkEngine::engine")
  message(FATAL_ERROR "the package config never creates ForkEngine::engine")
endif()
if(NOT _config MATCHES "INTERFACE_INCLUDE_DIRECTORIES")
  message(FATAL_ERROR
    "the package config does not set the include directory; a consumer would not "
    "find the public headers")
endif()
if(NOT _config MATCHES "CMAKE_CURRENT_LIST_DIR")
  message(FATAL_ERROR
    "the package config does not resolve its prefix from its own location, so the "
    "installed tree is not relocatable")
endif()

# The install module has to be included from the root, or none of it runs.
file(READ "${ENGINE_CMAKE}" _root)
if(NOT _root MATCHES "include\\(FEInstallDependencies\\)")
  message(FATAL_ERROR "the root CMakeLists.txt no longer includes FEInstallDependencies")
endif()
if(NOT _root MATCHES "fe_install_sdk_dependencies\\(\\)")
  message(FATAL_ERROR "the SDK dependency install is never called")
endif()

message(STATUS "sdk: the installed tree carries its dependency payload and a package config")
