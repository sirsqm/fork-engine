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
    src/test_frame_arena.cpp
    src/test_spatial_bvh.cpp
    src/test_jobs.cpp
    src/test_math.cpp
    src/test_frustum.cpp
    src/test_input_state.cpp
    src/test_input_actions.cpp
    src/test_action_bindings.cpp
    src/test_key_code.cpp
    src/test_canvas_scale.cpp
    src/test_platform_null.cpp
    src/test_user_data.cpp
    src/test_null_device.cpp
    src/test_shader_dialect.cpp
    src/test_scene.cpp
    src/test_components.cpp
    src/test_serialization.cpp
    src/test_reflect.cpp
    src/test_systems.cpp
    src/test_animation_state_machine.cpp
    src/test_skeletal_animation.cpp
    src/test_script.cpp
    src/test_physics2d.cpp
    src/test_physics3d.cpp
    src/test_physics3d_angular.cpp
    src/test_broadphase.cpp
    src/test_sleeping.cpp
    src/test_mesh.cpp
    src/test_gltf.cpp
    src/test_scene3d.cpp
    src/test_audio.cpp
    src/test_audio_listener.cpp
    src/test_pack.cpp
    src/test_pack_fetch.cpp
    src/test_resource_manager.cpp
    src/test_resource_reload.cpp
    src/test_asset_database.cpp
    src/test_import_settings.cpp
    src/test_import_cache.cpp
    src/test_version.cpp
    src/test_session.cpp
    src/test_net.cpp
    src/test_render_extract.cpp
    src/test_render_parallel.cpp
    src/test_render_graph.cpp
    src/test_text.cpp
    src/test_ui_layout.cpp
    src/test_ui.cpp
    src/test_tilemap.cpp
    src/test_debugdraw.cpp
    src/test_script_module.cpp
    src/test_script_binding.cpp
    src/test_builtin_behaviours.cpp
    src/test_export.cpp
)
target_link_libraries(fork_engine_tests PRIVATE FE::engine ${FE_DOCTEST_TARGET} fe_warnings)
target_compile_features(fork_engine_tests PRIVATE cxx_std_20)

# The breakout vertical-slice tests compile the game's own rules and systems
# straight from the example directory (by path, so they do not depend on
# FE_BUILD_EXAMPLES). This is deliberate: the test proves the *shipped* game
# code, not a copy of it. The example's src/ is on the include path for its
# headers.
target_sources(fork_engine_tests PRIVATE
    src/test_breakout.cpp
    ${CMAKE_SOURCE_DIR}/examples/breakout/src/breakout_state.cpp
    ${CMAKE_SOURCE_DIR}/examples/breakout/src/breakout_scene.cpp
    ${CMAKE_SOURCE_DIR}/examples/breakout/src/breakout_systems.cpp
)
target_include_directories(fork_engine_tests PRIVATE ${CMAKE_SOURCE_DIR}/examples/breakout/src)
set_target_properties(fork_engine_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# ---------------------------------------------------------------------------
# Gameplay module fixtures (M7, ADR-0061).
#
# The loader tests load *real* shared-library images, not mocks, so the dynamic-
# library seam and the reload path are exercised end to end. Two versions of the
# same behaviour (module_a, module_b) let a test observe a hot reload swapping
# code, and a third reports a wrong ABI version so the loader's refusal is tested.
# Their paths are handed to the test as compile definitions.
# ---------------------------------------------------------------------------
include(FEModules)
fe_add_gameplay_module(fe_test_module_a SOURCES modules/module_a.cpp)
fe_add_gameplay_module(fe_test_module_b SOURCES modules/module_b.cpp)
fe_add_gameplay_module(fe_test_module_c SOURCES modules/module_abi_mismatch.cpp)

# The module images must exist before the test binary runs, and must be rebuilt
# when their sources change.
add_dependencies(fork_engine_tests fe_test_module_a fe_test_module_b fe_test_module_c)

# The test locates the images by absolute path. The RUNTIME_OUTPUT_DIRECTORY of a
# MODULE target is its LIBRARY_OUTPUT_DIRECTORY, which fe_add_gameplay_module
# sets to build/bin; use $<TARGET_FILE:...> so the exact name (with the platform
# suffix and any config subdirectory) is resolved by the generator.
target_compile_definitions(fork_engine_tests PRIVATE
    "FE_TEST_MODULE_A=\"$<TARGET_FILE:fe_test_module_a>\""
    "FE_TEST_MODULE_B=\"$<TARGET_FILE:fe_test_module_b>\""
    "FE_TEST_MODULE_C=\"$<TARGET_FILE:fe_test_module_c>\""
)

# The disabled-instrumentation test must see the macros expand to nothing, so
# this one translation unit is compiled with profiling off. The define is set
# per-source so the rest of the suite keeps instrumentation on.
set_source_files_properties(src/test_profiler_disabled.cpp PROPERTIES COMPILE_DEFINITIONS "FE_PROFILING=0")

add_test(NAME engine_tests COMMAND fork_engine_tests)

# Platform classification (cmake/FEPlatform.cmake) is a pure function, but the
# desktop configure never takes the web branch, so it is tested by driving it in
# CMake's script mode rather than by building. It is the one test that pins the
# web target's classification down without an Emscripten toolchain installed.
add_test(
  NAME platform_detection
  COMMAND
    ${CMAKE_COMMAND} -DFE_PLATFORM_CMAKE=${CMAKE_SOURCE_DIR}/cmake/FEPlatform.cmake -P
    ${CMAKE_SOURCE_DIR}/cmake/tests/test_fe_platform.cmake
)

# The classification is only half the contract: what a source file branches on is
# the macro the classification turns into. This drives
# fe_apply_platform_definitions() in script mode and asserts the macro set for
# each target, including that Android gets FE_PLATFORM_ANDROID and *not* the
# desktop or web macros. Without it a wrong mapping compiles on desktop and only
# misbehaves on a phone. (docs/SHIPPING_QUEUE.md, `android-window-backend`).
add_test(
  NAME platform_definitions
  COMMAND
    ${CMAKE_COMMAND} -DFE_PLATFORM_CMAKE=${CMAKE_SOURCE_DIR}/cmake/FEPlatform.cmake -P
    ${CMAKE_SOURCE_DIR}/cmake/tests/test_fe_platform_definitions.cmake
)

# The web CI job is another decision the desktop suite cannot observe by
# building: a green desktop build says nothing about whether the wasm job still
# builds the player or still runs the browser smoke test. Like the platform
# classifier, it is checked by reading the workflow in CMake's script mode
# (docs/SHIPPING_QUEUE.md, `web-smoke-test-ci`).
add_test(
  NAME ci_web_job
  COMMAND
    ${CMAKE_COMMAND} -DCI_WORKFLOW=${CMAKE_SOURCE_DIR}/.github/workflows/ci.yml -P
    ${CMAKE_SOURCE_DIR}/cmake/tests/test_ci_web_job.cmake
)

# The planning documents are three files by decision, and that decision has been
# undone by a later session twice. This guards it mechanically: it fails if a
# merged plan reappears, if a document is missing or truncated, if AGENTS.md
# stops recording the rule, or if a tracked file still points at a merged plan.
add_test(
  NAME planning_docs
  COMMAND
    ${CMAKE_COMMAND} -DREPO_ROOT=${CMAKE_SOURCE_DIR} -P
    ${CMAKE_SOURCE_DIR}/cmake/tests/test_planning_docs.cmake
)

# The release path fires only on a version tag, so a mistake in it is invisible
# until a release is cut and the download is broken. This checks the workflow and
# the packaging script it drives the same way ci.yml is checked: by reading them
# and asserting the decisions they have to make.
add_test(
  NAME release_workflow
  COMMAND
    ${CMAKE_COMMAND}
    -DRELEASE_WORKFLOW=${CMAKE_SOURCE_DIR}/.github/workflows/release.yml
    -DPACKAGE_SCRIPT=${CMAKE_SOURCE_DIR}/packaging/package_release.cmake -P
    ${CMAKE_SOURCE_DIR}/cmake/tests/test_release_workflow.cmake
)

# ADR-0084 installed a tree that was runnable but not linkable, and ADR-0102
# closed that gap. This guards the pieces that make an installed tree consumable:
# the dependency payload and the package config, checked here so a refactor that
# drops one fails with a clear message instead of a link error in the smoke test.
add_test(
  NAME sdk_consumable
  COMMAND
    ${CMAKE_COMMAND}
    -DDEP_INSTALL=${CMAKE_SOURCE_DIR}/cmake/FEInstallDependencies.cmake
    -DENGINE_CMAKE=${CMAKE_SOURCE_DIR}/CMakeLists.txt -P
    ${CMAKE_SOURCE_DIR}/cmake/tests/test_sdk_consumable.cmake
)

# The Linux AppImage is built only on a release tag and in one CI job, so the
# desktop suite cannot observe it by building. Like the other workflow guards it
# is checked by reading the script and asserting the decisions it has to make:
# the content it stages, the libraries it bundles (and the ones it must not), the
# reproducibility pin, and the wiring in the release and CI workflows.
# (docs/SHIPPING_QUEUE.md, `linux-appimage`).
add_test(
  NAME appimage_packaging
  COMMAND
    ${CMAKE_COMMAND}
    -DAPPIMAGE_SCRIPT=${CMAKE_SOURCE_DIR}/packaging/package_appimage.cmake
    -DFETCH_SCRIPT=${CMAKE_SOURCE_DIR}/packaging/fetch_appimagetool.sh
    -DRELEASE_WORKFLOW=${CMAKE_SOURCE_DIR}/.github/workflows/release.yml
    -DCI_WORKFLOW=${CMAKE_SOURCE_DIR}/.github/workflows/ci.yml
    -DICON=${CMAKE_SOURCE_DIR}/packaging/fork-engine.png -P
    ${CMAKE_SOURCE_DIR}/cmake/tests/test_appimage_packaging.cmake
)

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
      src/test_file_dialog.cpp
      src/test_editor_scripts.cpp
      src/test_editor_script_modules.cpp
      src/test_project_settings.cpp
      src/test_play_session.cpp
      src/test_property_editor.cpp
      src/test_viewport.cpp
      src/test_commands.cpp
      src/test_operations.cpp
      src/test_editor_physics_play.cpp
      src/test_profiler_panel.cpp
  )
  target_link_libraries(fork_editor_tests PRIVATE FE::editor_core ${FE_DOCTEST_TARGET} fe_warnings)
  target_compile_features(fork_editor_tests PRIVATE cxx_std_20)
  set_target_properties(fork_editor_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

  # The editor's script tests load the same real gameplay-module images the
  # engine's loader tests use, so the module path and the image itself are
  # available here too (ADR-0061, ADR-0066).
  add_dependencies(fork_editor_tests fe_test_module_a fe_test_module_b)
  target_compile_definitions(fork_editor_tests PRIVATE
      "FE_TEST_MODULE_A=\"$<TARGET_FILE:fe_test_module_a>\""
      "FE_TEST_MODULE_B=\"$<TARGET_FILE:fe_test_module_b>\""
  )

  add_test(NAME editor_tests COMMAND fork_editor_tests)
endif()

# Graphics tests: create a hidden window and a GL context, then render a scene
# and read the framebuffer back. Skipped automatically when no display or
# context is available, so `ctest` stays green on machines without a GPU.
add_executable(fork_graphics_tests
    src/graphics_main.cpp
    src/test_renderer2d.cpp
    src/test_renderer3d.cpp
    src/test_renderer3d_skinned.cpp
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

# ---------------------------------------------------------------------------
# GLES3 dialect parity (docs/SHIPPING_QUEUE.md, `webgl2-mobile-parity`).
#
# WebGL2 and the GLES3 contexts phones expose are close but not identical, and a
# shader the ES seam translates wrongly is invisible until a phone compiles it.
# This target closes that gap on the build machine: it reads the engine's own
# shader sources, translates them through the real seam, and compiles each one in
# a real GLES3 context (Mesa's software GLES is enough, and it needs no display).
#
# It is deliberately Linux/Unix-only: EGL and GLESv2 are the driver entry points
# here, and the test skips itself (CTest's SKIP_RETURN_CODE) on a machine that
# has the libraries but no context. macOS has no separate GLES, and Windows ships
# it through ANGLE, so neither is the place to pin this down.
if(UNIX AND NOT APPLE)
  find_library(FE_EGL_LIBRARY NAMES EGL)
  find_library(FE_GLES2_LIBRARY NAMES GLESv2)
  if(FE_EGL_LIBRARY AND FE_GLES2_LIBRARY)
    add_executable(fork_gles_dialect_tests gles/gles_dialect_compile.cpp)
    target_link_libraries(fork_gles_dialect_tests PRIVATE FE::engine ${FE_EGL_LIBRARY} ${FE_GLES2_LIBRARY} fe_warnings)
    target_compile_features(fork_gles_dialect_tests PRIVATE cxx_std_20)
    set_target_properties(fork_gles_dialect_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
    # The shader sources are read at run time by path, so the check proves the
    # shipped files compile rather than a copy of them.
    add_test(
      NAME gles_dialect_compile
      COMMAND fork_gles_dialect_tests
              ${CMAKE_SOURCE_DIR}/engine/src/renderer2d/Renderer2D.cpp
              ${CMAKE_SOURCE_DIR}/engine/src/renderer3d/Renderer3D.cpp
              ${CMAKE_SOURCE_DIR}/engine/src/renderer3d/PostProcess.cpp
              ${CMAKE_SOURCE_DIR}/engine/src/debugdraw/DebugDraw.cpp
    )
    set_tests_properties(gles_dialect_compile PROPERTIES SKIP_RETURN_CODE 77)
  else()
    message(STATUS "GLES3 dialect test: EGL or GLESv2 not found, skipping")
  endif()
endif()
