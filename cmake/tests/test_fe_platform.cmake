# Test for cmake/FEPlatform.cmake, run by ctest in script mode.
#
# The web classification is the one decision the desktop suite cannot observe by
# building: the desktop configure never takes the web branch. So it is tested
# directly, as a function, by driving fe_detect_platform()/fe_platform_is_web()
# through CMake's own script interpreter.
#
# Run by tests/CMakeLists.txt:
#   cmake -DFE_PLATFORM_CMAKE=<path> -P cmake/tests/test_fe_platform.cmake

if(NOT DEFINED FE_PLATFORM_CMAKE)
  message(FATAL_ERROR "FE_PLATFORM_CMAKE must point at cmake/FEPlatform.cmake")
endif()
include("${FE_PLATFORM_CMAKE}")

# case: <CMAKE_SYSTEM_NAME>=<expected classification>
# `=` rather than `;` because a `;` inside a set() argument is a list separator.
set(_cases
    "Emscripten=web"
    "Windows=windows"
    "Darwin=macos"
    "Linux=linux"
    "Android=android"
    "iOS=ios"
)

foreach(_case IN LISTS _cases)
  string(REPLACE "=" ";" _parts "${_case}")
  list(GET _parts 0 _system_name)
  list(GET _parts 1 _expected)

  # A cross build (which is what the web target is) sets CMAKE_SYSTEM_NAME before
  # project(), so that is what the classifier must read.
  set(CMAKE_SYSTEM_NAME "${_system_name}")
  set(WIN32 FALSE)
  set(APPLE FALSE)
  set(UNIX FALSE)

  fe_detect_platform(_detected)
  if(NOT _detected STREQUAL _expected)
    message(FATAL_ERROR "CMAKE_SYSTEM_NAME=${_system_name}: expected '${_expected}', got '${_detected}'")
  endif()

  fe_platform_is_web(_is_web)
  if(_system_name STREQUAL "Emscripten")
    if(NOT _is_web)
      message(FATAL_ERROR "CMAKE_SYSTEM_NAME=Emscripten: fe_platform_is_web() returned FALSE")
    endif()
  elseif(_is_web)
    message(FATAL_ERROR "CMAKE_SYSTEM_NAME=${_system_name}: fe_platform_is_web() returned TRUE")
  endif()

  # Android is a cross build with its own window backend: it is not web, but it
  # is not desktop either, and the build picks the backend on this predicate.
  fe_platform_is_android(_is_android)
  if(_system_name STREQUAL "Android")
    if(NOT _is_android)
      message(FATAL_ERROR "CMAKE_SYSTEM_NAME=Android: fe_platform_is_android() returned FALSE")
    endif()
  elseif(_is_android)
    message(FATAL_ERROR "CMAKE_SYSTEM_NAME=${_system_name}: fe_platform_is_android() returned TRUE")
  endif()

  message(STATUS "  ${_system_name} -> ${_detected} (web=${_is_web} android=${_is_android})")
endforeach()

# A plain desktop configure sets no CMAKE_SYSTEM_NAME; the host variables decide.
set(CMAKE_SYSTEM_NAME "")
set(WIN32 FALSE)
set(APPLE FALSE)
set(UNIX TRUE)
fe_detect_platform(_host_detected)
if(NOT _host_detected STREQUAL "unix")
  message(FATAL_ERROR "host fallback: expected 'unix', got '${_host_detected}'")
endif()
fe_platform_is_web(_host_is_web)
if(_host_is_web)
  message(FATAL_ERROR "host fallback: a plain UNIX configure must not be web")
endif()

message(STATUS "FEPlatform: platform detection is correct")
