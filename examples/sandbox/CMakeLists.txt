# Fork Engine sandbox example.
#
# A small runnable program that demonstrates the milestone-1 foundation. It is
# built against the engine's public headers only, exactly as an external game
# would be, so it doubles as a check that the public API is self-sufficient.

add_executable(fork_sandbox src/main.cpp)
target_link_libraries(fork_sandbox PRIVATE FE::engine fe_warnings)
target_compile_features(fork_sandbox PRIVATE cxx_std_20)

# The example loads assets by relative path; run it from the repository root.
set_target_properties(fork_sandbox PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
