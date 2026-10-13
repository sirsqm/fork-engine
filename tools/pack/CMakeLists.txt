# Fork Engine packaging tools.
#
# fork_pack assembles a project into one pack file: the first shipping step of
# M8 (docs/ROADMAP.md). It is built against the engine's public headers only,
# exactly as an external tool would be, and it links FE::engine rather than the
# editor: packing a project is not an editor feature (docs/adr/0030).

add_executable(fork_pack src/main.cpp)
target_link_libraries(fork_pack PRIVATE FE::engine fe_warnings)
target_compile_features(fork_pack PRIVATE cxx_std_20)

set_target_properties(fork_pack PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
