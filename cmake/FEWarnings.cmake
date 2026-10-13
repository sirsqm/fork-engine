# FEWarnings.cmake
#
# A single place to define the warning interface applied to first-party code.
# Third-party code (FetchContent dependencies, vendored sources) is never given
# these flags, because we cannot fix warnings we do not own.

add_library(fe_warnings INTERFACE)

if(MSVC)
  target_compile_options(fe_warnings INTERFACE /W4)
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

# Mark a dependency's include directories as SYSTEM so its headers do not
# produce warnings inside our translation units.
function(fe_mark_system target)
  if(TARGET ${target})
    get_target_property(_dirs ${target} INTERFACE_INCLUDE_DIRECTORIES)
    if(_dirs)
      set_target_properties(${target} PROPERTIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_dirs}")
    endif()
  endif()
endfunction()
