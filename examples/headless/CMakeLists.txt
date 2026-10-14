# Fork Engine headless example.
#
# A program that runs the engine's frame loop with no display and no graphics
# context, using the null platform backend (ADR-0041). It is the executable form
# of the "runs on a machine with no GPU and no X server" claim, and CI runs it
# without xvfb to prove the claim.
#
# Like the sandbox, it is built against the engine's public headers only, so it
# doubles as a check that the headless path needs no private API.

add_executable(fork_headless src/main.cpp)
target_link_libraries(fork_headless PRIVATE FE::engine fe_warnings)
target_compile_features(fork_headless PRIVATE cxx_std_20)

set_target_properties(fork_headless PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
