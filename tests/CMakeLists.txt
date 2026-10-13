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
    src/test_profiler.cpp
    src/test_profiler_disabled.cpp
    src/test_math.cpp
    src/test_frustum.cpp
    src/test_input_state.cpp
    src/test_scene.cpp
    src/test_components.cpp
    src/test_serialization.cpp
    src/test_reflect.cpp
    src/test_systems.cpp
    src/test_physics2d.cpp
    src/test_broadphase.cpp
    src/test_sleeping.cpp
    src/test_mesh.cpp
    src/test_scene3d.cpp
    src/test_audio.cpp
    src/test_pack.cpp
    src/test_resource_manager.cpp
    src/test_asset_database.cpp
    src/test_import_settings.cpp
    src/test_import_cache.cpp
    src/test_version.cpp
)
target_link_libraries(fork_engine_tests PRIVATE FE::engine ${FE_DOCTEST_TARGET} fe_warnings)
target_compile_features(fork_engine_tests PRIVATE cxx_std_20)
set_target_properties(fork_engine_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# The disabled-instrumentation test must see the macros expand to nothing, so
# this one translation unit is compiled with profiling off. The define is set
# per-source so the rest of the suite keeps instrumentation on.
set_source_files_properties(src/test_profiler_disabled.cpp PROPERTIES COMPILE_DEFINITIONS "FE_PROFILING=0")

add_test(NAME engine_tests COMMAND fork_engine_tests)

# ---------------------------------------------------------------------------
# Editor tests.
#
# The editor-core logic (the hierarchy view model, the document, the play-scene
# copy) is pure and headless, so it runs in its own target that needs no window
# or GPU and never pulls in ImGui. The ImGui panels are deliberately not tested
# here; they are a thin projection of the view model, which is what these tests
# cover. Anything that needs a GL context (the editor host's loop) goes in
# fork_graphics_tests instead.
# ---------------------------------------------------------------------------
if(FE_BUILD_EDITOR)
  add_executable(fork_editor_tests
      src/editor_main.cpp
      src/test_hierarchy_view_model.cpp
      src/test_asset_browser.cpp
      src/test_editor_context.cpp
      src/test_project_settings.cpp
      src/test_play_session.cpp
      src/test_property_editor.cpp
      src/test_viewport.cpp
      src/test_commands.cpp
      src/test_editor_physics_play.cpp
  )
  target_link_libraries(fork_editor_tests PRIVATE FE::editor_core ${FE_DOCTEST_TARGET} fe_warnings)
  target_compile_features(fork_editor_tests PRIVATE cxx_std_20)
  set_target_properties(fork_editor_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

  add_test(NAME editor_tests COMMAND fork_editor_tests)
endif()

# Graphics tests: create a hidden window and a GL context, then render a scene
# and read the framebuffer back. Skipped automatically when no display or
# context is available, so `ctest` stays green on machines without a GPU.
add_executable(fork_graphics_tests
    src/graphics_main.cpp
    src/test_renderer2d.cpp
    src/test_renderer3d.cpp
    src/test_application_profiler.cpp
)
target_link_libraries(fork_graphics_tests PRIVATE FE::engine ${FE_DOCTEST_TARGET} fe_warnings)
target_compile_features(fork_graphics_tests PRIVATE cxx_std_20)
set_target_properties(fork_graphics_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# The play-in-editor test embeds a real Engine and drives frames, so it needs a
# GL context too and belongs with the graphics tests. It links editor_core (not
# the ImGui UI layer): play-in-editor is deliberately designed to be driven
# without the UI toolkit.
if(FE_BUILD_EDITOR)
  target_sources(fork_graphics_tests PRIVATE src/test_editor_play.cpp src/test_editor_host.cpp)
  target_link_libraries(fork_graphics_tests PRIVATE FE::editor_core)
  # The host test runs the whole editor, panels included, so it needs the ImGui
  # UI layer as well.
  target_link_libraries(fork_graphics_tests PRIVATE FE::editor)
endif()

add_test(NAME graphics_tests COMMAND fork_graphics_tests)
