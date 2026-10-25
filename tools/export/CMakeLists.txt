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

# fork_export is a build tool, not a game, and it must run where the build runs.
# Under the web toolchain that means node rather than a browser: the browser's
# module has no filesystem, so the export would fail on its first file write. The
# headless-browser verification needs an exporter to produce the artifact it
# checks, so the wasm exporter is given node's real filesystem here and stays a
# classic script (no MODULARIZE), which node can run directly. A native
# fork_export is still used when one is present; this is what makes a checkout
# that only has the web target self-sufficient.
fe_platform_is_web(FE_TARGETS_WEB)
if(FE_TARGETS_WEB)
  target_link_options(
    fork_export
    PRIVATE
      "-sNODERAWFS=1"
      "-sENVIRONMENT=node"
      "-sALLOW_MEMORY_GROWTH=1"
      "-sSTACK_SIZE=8388608"
      # A failed export must fail the process, so a script that runs this can
      # tell success from failure through the exit code.
      "-sEXIT_RUNTIME=1"
  )
endif()
