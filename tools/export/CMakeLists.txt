# Fork Engine export tool.
#
# fork_export turns a project into a runnable build: a pack, the fork_player
# runtime, and a manifest (docs/adr/0064-export-runtime-and-player.md). It is
# built against the engine's public headers only and links FE::engine, like
# fork_pack: exporting is part of the shipping path, not an editor feature.

add_executable(fork_export src/main.cpp)
target_link_libraries(fork_export PRIVATE FE::engine fe_warnings)
target_compile_features(fork_export PRIVATE cxx_std_20)

set_target_properties(fork_export PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
