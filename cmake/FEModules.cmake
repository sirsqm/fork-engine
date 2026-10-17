# FEModules.cmake
#
# Build helper for C++ gameplay modules (M7, ADR-0044, ADR-0061).
#
# A gameplay module is a shared library built against the engine that registers
# its behaviours through the versioned entry point in
# `fe/script/ScriptModuleApi.h`. The engine loads it at runtime with
# `ScriptLibrary` and can replace its code without a restart.
#
# A module must be a shared library even on a platform whose default is static,
# and its symbols must be exported even under a hidden-visibility build, because
# the engine resolves its entry point by name at load time. This function sets
# both, links the engine, and places the image next to the other build output so
# a loader can find it by name.

# fe_add_gameplay_module(<target> SOURCES <src>...)
function(fe_add_gameplay_module target)
  cmake_parse_arguments(ARG "" "" "SOURCES" ${ARGN})
  if(NOT ARG_SOURCES)
    message(FATAL_ERROR "fe_add_gameplay_module(${target}): no SOURCES given")
  endif()

  add_library(${target} MODULE ${ARG_SOURCES})

  # Link the engine and inherit its warning flags, exactly like an example.
  target_link_libraries(${target} PRIVATE FE::engine fe_warnings)
  target_compile_features(${target} PRIVATE cxx_std_20)

  # A module and the engine exchange `std::` types and the Script vtable across
  # the binary boundary, so the C++ runtime must be shared rather than statically
  # duplicated into the module. Position-independent code is already on globally
  # (see the root CMakeLists.txt).
  set_target_properties(${target} PROPERTIES
    PREFIX ""
    CXX_VISIBILITY_PRESET default
    VISIBILITY_INLINES_HIDDEN OFF
  )

  # Put the image beside the other build output (build/bin) so a loader can find
  # it by a relative name.
  set_target_properties(${target} PROPERTIES LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
endfunction()
