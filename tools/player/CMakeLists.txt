# Fork Engine shipping runtime.
#
# fork_player mounts a pack and runs the game loop: the runtime half of M8's
# export path (docs/adr/0064-export-runtime-and-player.md). It is built against
# the engine's public headers only and links FE::engine and nothing else - no
# editor, no ImGui - because a shipped build should carry only what it runs.

add_executable(fork_player src/main.cpp)
target_link_libraries(fork_player PRIVATE FE::engine fe_warnings)
target_compile_features(fork_player PRIVATE cxx_std_20)

set_target_properties(fork_player PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
