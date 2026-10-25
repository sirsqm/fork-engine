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

# --- Web target ---------------------------------------------------------------
#
# Breakout is the shipping slice, so it builds for the browser as well as the
# desktop. Two things are needed beyond a plain wasm link:
#
#   * the game reads its scene and textures by path, and a wasm build has no
#     filesystem, so those files are preloaded into the virtual one at exactly
#     the paths the game already uses -- the code is identical on both
#     platforms and never asks which it is;
#   * a page to host it. `--shell-file` supplies one that provides the #canvas
#     the backend renders into.
#
# The output is renamed to `index.html`/`index.js` so the build directory is
# directly servable and the entry point is the conventional one.
fe_platform_is_web(FE_BREAKOUT_WEB)
if(FE_BREAKOUT_WEB)
  target_link_options(
    fork_breakout
    PRIVATE
      "-sMIN_WEBGL_VERSION=2"
      "-sMAX_WEBGL_VERSION=2"
      "-sALLOW_MEMORY_GROWTH=1"
      # The engine's startup path (scene load, reflection registration) overflows
      # Emscripten's 64 KiB default stack. See docs/adr/0073.
      "-sSTACK_SIZE=8388608"
      "-sENVIRONMENT=web"
      # main() installs the frame loop and returns to the page; the runtime must
      # not tear the page down when a run ends.
      "-sEXIT_RUNTIME=0"
      # The game's content, at the paths it already loads them from.
      "--preload-file=${CMAKE_SOURCE_DIR}/examples/breakout/assets@examples/breakout/assets"
      "--preload-file=${CMAKE_SOURCE_DIR}/examples/breakout/scenes@examples/breakout/scenes"
      "--shell-file=${CMAKE_CURRENT_SOURCE_DIR}/web/shell.html"
  )
  # The toolchain sets CMAKE_EXECUTABLE_SUFFIX to ".js" so the engine's other
  # web targets emit a JS/HTML pair. Emscripten only honours --shell-file when
  # it is generating HTML, so this target overrides the suffix: it emits
  # index.html plus index.js and index.wasm, and the shell page is used.
  set_target_properties(fork_breakout PROPERTIES OUTPUT_NAME "index" SUFFIX ".html")
endif()
