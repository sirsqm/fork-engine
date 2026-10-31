# FEPlatform.cmake
#
# The single place that decides which platform the build targets, and the only
# place that defines the `FE_PLATFORM_*` macros. The desktop build and the
# Emscripten and Android toolchains all funnel through it, so they cannot disagree
# about what "web" or "android" means.
#
# Detection is a pure function of CMake's own platform variables: it creates no
# target and calls no `add_*`, so tests/cmake/test_fe_platform.cmake can drive it
# in `cmake -P` script mode and assert the classification for every platform
# this project builds on. fe_apply_platform_definitions() is the side-effecting
# half, called once from the root CMakeLists.
#
# Why a macro rather than a compiler check: the platform layer's OS-specific
# implementations (the GLFW window backend, and the web and Android ones) must be
# *absent* from a target they cannot compile for, not merely unused — GLFW cannot
# be compiled to wasm. The build already knows the target platform from
# CMAKE_SYSTEM_NAME, so it is the build's job to say so, not a `#ifdef
# __EMSCRIPTEN__` scattered through the sources. See
# docs/adr/0073-emscripten-toolchain.md and
# docs/adr/0105-android-window-backend.md.

# Included from FESettings.cmake, FEDependencies.cmake, and the web and Android
# toolchains; a second include is a no-op rather than a redefinition of the
# functions below.
include_guard(GLOBAL)

# Classifies the target platform as one of:
#   web, windows, macos, linux, android, ios, unix, unknown
#
# CMAKE_SYSTEM_NAME is read first because a toolchain file sets it before the
# project() call, which is what makes it correct for a cross build (the whole
# point of the web target). CMake's host variables are the fallback for a plain
# desktop configure, where no toolchain set it.
function(fe_detect_platform out_var)
  if(CMAKE_SYSTEM_NAME STREQUAL "Emscripten")
    set(${out_var}
        "web"
        PARENT_SCOPE
    )
  elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    set(${out_var}
        "windows"
        PARENT_SCOPE
    )
  elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    set(${out_var}
        "macos"
        PARENT_SCOPE
    )
  elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(${out_var}
        "linux"
        PARENT_SCOPE
    )
  elseif(CMAKE_SYSTEM_NAME STREQUAL "Android")
    set(${out_var}
        "android"
        PARENT_SCOPE
    )
  elseif(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set(${out_var}
        "ios"
        PARENT_SCOPE
    )
  elseif(WIN32)
    set(${out_var}
        "windows"
        PARENT_SCOPE
    )
  elseif(APPLE)
    set(${out_var}
        "macos"
        PARENT_SCOPE
    )
  elseif(UNIX)
    set(${out_var}
        "unix"
        PARENT_SCOPE
    )
  else()
    set(${out_var}
        "unknown"
        PARENT_SCOPE
    )
  endif()
endfunction()

# True when the build targets WebAssembly. A cross build to wasm is neither a
# desktop build nor a hosted one: it has no OS window, no `std::filesystem`
# backing store, and no glad/GLFW, so code that asks "is this desktop?" must get
# a no here.
function(fe_platform_is_web out_var)
  fe_detect_platform(_fe_platform)
  if(_fe_platform STREQUAL "web")
    set(${out_var}
        TRUE
        PARENT_SCOPE
    )
  else()
    set(${out_var}
        FALSE
        PARENT_SCOPE
    )
  endif()
endfunction()

# True when the build targets Android. Like web, Android is a cross build with
# its own window backend: it is not a desktop build, so it must not be handed the
# GLFW/desktop backend, but it is also not web. The build selects its backends on
# this predicate rather than on the desktop macro, the way the web target does
# (engine/CMakeLists.txt). See docs/adr/0105-android-window-backend.md.
function(fe_platform_is_android out_var)
  fe_detect_platform(_fe_platform)
  if(_fe_platform STREQUAL "android")
    set(${out_var}
        TRUE
        PARENT_SCOPE
    )
  else()
    set(${out_var}
        FALSE
        PARENT_SCOPE
    )
  endif()
endfunction()

# The compile definitions for the configured target, as a list, *without* applying
# them. Pure, like fe_detect_platform(): add_compile_definitions() does not exist
# in `cmake -P` script mode, so a test cannot call the applying half, and the
# mapping from a classification to a macro is exactly the mistake that compiles on
# desktop and only misbehaves on the cross target. Keeping it pure is what lets
# cmake/tests/test_fe_platform_definitions.cmake assert it.
#
# Exactly one of FE_PLATFORM_DESKTOP / FE_PLATFORM_WEB / FE_PLATFORM_ANDROID is
# returned: a source that branches on "is there a desktop OS?" cannot accidentally
# take both paths. Android and web are their own platforms, not desktop and not
# each other; a source that needs the Android backend asks for FE_PLATFORM_ANDROID
# and a source that needs the browser backend asks for FE_PLATFORM_WEB, and no
# build defines both.
function(fe_platform_definitions out_var)
  fe_detect_platform(_fe_platform)
  if(_fe_platform STREQUAL "web")
    set(${out_var}
        FE_PLATFORM_WEB=1
        PARENT_SCOPE
    )
    return()
  endif()
  if(_fe_platform STREQUAL "android")
    set(${out_var}
        FE_PLATFORM_ANDROID=1
        PARENT_SCOPE
    )
    return()
  endif()

  set(_defs FE_PLATFORM_DESKTOP=1)
  if(_fe_platform STREQUAL "windows")
    list(APPEND _defs FE_PLATFORM_WINDOWS=1)
  elseif(_fe_platform STREQUAL "macos")
    list(APPEND _defs FE_PLATFORM_MACOS=1)
  elseif(_fe_platform STREQUAL "linux")
    list(APPEND _defs FE_PLATFORM_LINUX=1)
  endif()
  set(${out_var}
      ${_defs}
      PARENT_SCOPE
  )
endfunction()

# Applies fe_platform_definitions() for the configured target. Called once from
# the root CMakeLists, at the same directory scope the definitions have always
# been made, so a desktop build is byte-for-byte what it was.
function(fe_apply_platform_definitions)
  fe_platform_definitions(_fe_defs)
  add_compile_definitions(${_fe_defs})
endfunction()
