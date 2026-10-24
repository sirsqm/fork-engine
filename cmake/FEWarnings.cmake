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

  # GCC 15.2.0 mis-reports -Wfree-nonheap-object (pulled in by -Wextra) on a
  # correct std::vector<std::string> destruction: it inlines a sized delete on
  # a stack temporary in new_allocator.h and claims the pointer has a nonzero
  # offset. The same code builds clean under GCC 13 and 14, and the Debug,
  # ASan/UBSan and TSan jobs - which would catch an actual bad free - all pass
  # on it, so there is no real defect to fix. Disable the check only for the
  # compiler versions that mis-report it; a genuine bad free on those versions
  # is still caught by the sanitizer jobs.
  if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 15)
    target_compile_options(fe_warnings INTERFACE -Wno-free-nonheap-object)
  endif()
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
#
# An empty name is a no-op, not an error: a platform that does not use a
# dependency leaves its target variable empty (FE_GLFW_TARGET on the web target,
# where GLFW is not fetched at all), and every such call site should read the
# same as one that named a target.
function(fe_mark_system target)
  if("${target}" STREQUAL "")
    return()
  endif()
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
