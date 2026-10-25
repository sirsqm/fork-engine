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

# The web build of the player is loaded by the export's shell as an ES module,
# so it must be a modularized module that exports a factory by name rather than a
# classic script that runs on load. Without these flags the emitted
# `fork_player.js` defines nothing the shell can call -- it only exposes `_main`
# -- and the export fails in the browser with "does not export
# createForkPlayer". The factory name is a constant the shell and the template
# share (see WebExportTemplate.cpp). The stack is raised because the engine's
# startup path (scene load, reflection registration) overflows Emscripten's 64
# KiB default. See docs/adr/0073-emscripten-toolchain.md and
# docs/adr/0071-web-first-2d-shipping-path.md.
fe_platform_is_web(FE_TARGETS_WEB)
if(FE_TARGETS_WEB)
  target_link_options(
    fork_player
    PRIVATE
      "-sMODULARIZE=1"
      "-sEXPORT_NAME=createForkPlayer"
      "-sEXPORT_ES6=1"
      "-sALLOW_MEMORY_GROWTH=1"
      "-sMIN_WEBGL_VERSION=2"
      "-sMAX_WEBGL_VERSION=2"
      "-sSTACK_SIZE=8388608"
      "-sENVIRONMENT=web"
      # The module is instantiated by the page and must not tear the page down
      # when a run ends; the shell decides what happens next.
      "-sEXIT_RUNTIME=0"
  )
endif()
