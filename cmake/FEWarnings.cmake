# FEWarnings.cmake
#
# A single place to define the warning interface applied to first-party code.
# Third-party code (FetchContent dependencies, vendored sources) is never given
# these flags, because we cannot fix warnings we do not own.

add_library(fe_warnings INTERFACE)

if(MSVC)
  # /W4 for our code. Dependencies are pulled in as external headers (CMake
  # emits SYSTEM include directories as /external:I), and /external:W0 drops
  # warnings from those headers. Without it MSVC keeps external warnings at the
  # /W4 level, so a warning inside a dependency header (doctest's tuple
  # specialization, C5285) would still be reported inside our translation
  # units and break the /WX build.
  target_compile_options(fe_warnings INTERFACE /W4 /external:W0 /external:anglebrackets)
else()
  target_compile_options(
    fe_warnings
    INTERFACE -Wall
              -Wextra
              -Wpedantic
              -Wshadow
              -Wnon-virtual-dtor
              -Wold-style-cast
              -Wcast-align
              -Wunused
              -Woverloaded-virtual
              -Wdouble-promotion
              -Wformat=2
  )
endif()

# Warnings-as-errors also belongs to the interface, not to a directory, so that
# it never reaches third-party targets (that was the cause of GLFW failing to
# build on the first CI run).
if(FE_WARNINGS_AS_ERRORS)
  if(MSVC)
    target_compile_options(fe_warnings INTERFACE /WX)
  else()
    target_compile_options(fe_warnings INTERFACE -Werror)
  endif()
endif()

# Mark a dependency's include directories as SYSTEM so its headers do not
# produce warnings inside our translation units. Accepts an alias target (for
# example nlohmann_json::nlohmann_json) by resolving it to the real target,
# because set_target_properties() cannot be applied to an alias.
function(fe_mark_system target)
  if(NOT TARGET ${target})
    return()
  endif()
  get_target_property(_aliased ${target} ALIASED_TARGET)
  if(_aliased)
    set(_real ${_aliased})
  else()
    set(_real ${target})
  endif()
  get_target_property(_dirs ${_real} INTERFACE_INCLUDE_DIRECTORIES)
  if(_dirs)
    set_target_properties(${_real} PROPERTIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_dirs}")
  endif()
endfunction()
