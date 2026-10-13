# Fork Engine unit tests.
#
# Tests are split so that the pure-logic tests can run anywhere, while the
# graphics tests are isolated behind a separate target that needs an OpenGL
# context (real or a software implementation such as Mesa's llvmpipe).

add_executable(fork_engine_tests
    src/main.cpp
    src/test_log.cpp
    src/test_assert.cpp
    src/test_uuid.cpp
    src/test_filesystem.cpp
    src/test_math.cpp
    src/test_frustum.cpp
    src/test_input_state.cpp
    src/test_scene.cpp
    src/test_components.cpp
    src/test_serialization.cpp
    src/test_reflect.cpp
    src/test_systems.cpp
    src/test_physics2d.cpp
    src/test_mesh.cpp
    src/test_scene3d.cpp
    src/test_resource_manager.cpp
    src/test_version.cpp
)
target_link_libraries(fork_engine_tests PRIVATE FE::engine ${FE_DOCTEST_TARGET} fe_warnings)
target_compile_features(fork_engine_tests PRIVATE cxx_std_20)
set_target_properties(fork_engine_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

add_test(NAME engine_tests COMMAND fork_engine_tests)

# Graphics tests: create a hidden window and a GL context, then render a scene
# and read the framebuffer back. Skipped automatically when no display or
# context is available, so `ctest` stays green on machines without a GPU.
add_executable(fork_graphics_tests
    src/graphics_main.cpp
    src/test_renderer2d.cpp
    src/test_renderer3d.cpp
)
target_link_libraries(fork_graphics_tests PRIVATE FE::engine ${FE_DOCTEST_TARGET} fe_warnings)
target_compile_features(fork_graphics_tests PRIVATE cxx_std_20)
set_target_properties(fork_graphics_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

add_test(NAME graphics_tests COMMAND fork_graphics_tests)
