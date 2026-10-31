# Test for fe_platform_definitions() in cmake/FEPlatform.cmake, run by ctest in
# script mode.
#
# test_fe_platform.cmake pins the *classification* (which string each target
# gets). This pins the *definitions* that classification turns into, which is the
# half a source file actually branches on. The two are separate on purpose: a
# classification that is right but maps to the wrong macro (or to two macros)
# compiles and only misbehaves on the target, and a desktop-only CI never takes
# the cross-build branch, so it cannot notice by building.
#
# fe_platform_definitions() is the pure half on purpose: add_compile_definitions()
# does not exist in `cmake -P` script mode, so the mapping is split out of
# fe_apply_platform_definitions() to be reachable from here.
#
# The macro contract is "exactly one of FE_PLATFORM_DESKTOP / FE_PLATFORM_WEB /
# FE_PLATFORM_ANDROID / FE_PLATFORM_IOS, plus FE_PLATFORM_MOBILE on both mobile
# platforms": Android must not also be handed the desktop backend (it cannot
# compile GLFW), it must not be handed the web backend, and neither mobile
# platform may be handed the other's macro. iOS must not be classified as desktop
# just because it is Apple.
#
# Run by tests/CMakeLists.txt:
#   cmake -DFE_PLATFORM_CMAKE=<path> -P cmake/tests/test_fe_platform_definitions.cmake

if(NOT DEFINED FE_PLATFORM_CMAKE)
  message(FATAL_ERROR "FE_PLATFORM_CMAKE must point at cmake/FEPlatform.cmake")
endif()
include("${FE_PLATFORM_CMAKE}")

# case: <CMAKE_SYSTEM_NAME>:<pipe-separated expected defines>. `:` separates the
# system name from the defines and `|` separates the names, because `;` is a list
# separator inside set() and `=` appears inside a macro name.
set(_cases
    "Emscripten:FE_PLATFORM_WEB=1"
    "Android:FE_PLATFORM_ANDROID=1|FE_PLATFORM_MOBILE=1"
    "iOS:FE_PLATFORM_IOS=1|FE_PLATFORM_MOBILE=1"
    "Linux:FE_PLATFORM_DESKTOP=1|FE_PLATFORM_LINUX=1"
    "Windows:FE_PLATFORM_DESKTOP=1|FE_PLATFORM_WINDOWS=1"
    "Darwin:FE_PLATFORM_DESKTOP=1|FE_PLATFORM_MACOS=1"
)

# Every platform macro this project defines. A define that is present but not
# expected, or expected but absent, fails the case; the per-platform mutual
# exclusion (Android is not desktop, iOS is not macOS, web is not desktop) falls
# out of this.
set(_all_macros
    FE_PLATFORM_DESKTOP=1
    FE_PLATFORM_WEB=1
    FE_PLATFORM_ANDROID=1
    FE_PLATFORM_IOS=1
    FE_PLATFORM_MOBILE=1
    FE_PLATFORM_WINDOWS=1
    FE_PLATFORM_MACOS=1
    FE_PLATFORM_LINUX=1
)

foreach(_case IN LISTS _cases)
  string(REPLACE ":" ";" _parts "${_case}")
  list(GET _parts 0 _system_name)
  list(GET _parts 1 _expected_joined)
  string(REPLACE "|" ";" _expected "${_expected_joined}")

  # A cross build (Android, web) sets CMAKE_SYSTEM_NAME before project(); the
  # host variables are the desktop fallback, so they are cleared here too.
  set(CMAKE_SYSTEM_NAME "${_system_name}")
  set(WIN32 FALSE)
  set(APPLE FALSE)
  set(UNIX FALSE)

  fe_platform_definitions(_defs)

  foreach(_macro IN LISTS _all_macros)
    list(FIND _expected "${_macro}" _should_be_there)
    list(FIND _defs "${_macro}" _is_there)
    if(NOT _should_be_there EQUAL -1 AND _is_there EQUAL -1)
      message(FATAL_ERROR "CMAKE_SYSTEM_NAME=${_system_name}: ${_macro} is missing (got: ${_defs})")
    endif()
    if(_should_be_there EQUAL -1 AND NOT _is_there EQUAL -1)
      message(FATAL_ERROR "CMAKE_SYSTEM_NAME=${_system_name}: ${_macro} must not be defined (got: ${_defs})")
    endif()
  endforeach()

  message(STATUS "  ${_system_name} -> ${_defs}")
endforeach()

message(STATUS "FEPlatform: platform definitions are correct and mutually exclusive")
