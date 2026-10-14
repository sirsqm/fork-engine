# Fork Engine breakout example — the M2.5 vertical slice.
#
# A complete, playable game built against the engine's public headers only,
# exactly as an external project would build it. It is the integration test the
# roadmap asks for: one artifact that exercises the scene graph, resources,
# physics2d, audio, serialization, input, and the systems layer together.
#
# The rules (BreakoutState) and the gameplay systems are ordinary sources, not a
# separate library: the unit tests compile them straight from here by path, so
# the tests do not depend on FE_BUILD_EXAMPLES being on.

add_executable(fork_breakout
  src/main.cpp
  src/breakout_state.cpp
  src/breakout_scene.cpp
  src/breakout_systems.cpp
)
target_link_libraries(fork_breakout PRIVATE FE::engine fe_warnings)
target_include_directories(fork_breakout PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_features(fork_breakout PRIVATE cxx_std_20)

# The game loads assets and its scene by relative path; run it from the
# repository root, exactly like the sandbox.
set_target_properties(fork_breakout PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
