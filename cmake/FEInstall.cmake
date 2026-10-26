# Fork Engine install rules.
#
# One place decides where an installed tree puts things, so the engine, the
# editor, and the tools cannot disagree. GNUInstallDirs supplies the platform's
# directories (bin, lib, include, share), which is what makes `cmake --install`
# produce a layout a packager can consume without per-platform special cases.
#
# What is installed: the engine library and its public headers, the editor
# shell, the shipping tools (fork_pack, fork_player, fork_export), the runtime
# shared libraries an installed binary needs, and the license/readme. What is
# deliberately not installed: the examples, the unit tests, and the benchmark -
# they are development artifacts, and a shipped tree should not carry them.
#
# There is no `find_package(ForkEngine)` config yet. An installed tree is
# runnable, not yet linkable from another CMake project; adding the export set
# and the config file is a separate, larger change (it has to name the fetched
# dependencies, which are not installed). This is recorded rather than
# half-done, so a consumer is not misled by a config that does not work.

include_guard(GLOBAL)

include(GNUInstallDirs)

# On Windows a shared library must sit beside the executable that loads it, so
# the DLL-search rule wins over the tidy `lib` layout. Everywhere else the
# library directory is `lib` (or `lib64`). This is the one place the choice is
# made.
if(WIN32)
  set(FE_INSTALL_LIBDIR "${CMAKE_INSTALL_BINDIR}")
else()
  set(FE_INSTALL_LIBDIR "${CMAKE_INSTALL_LIBDIR}")
endif()

# Installs `target`, putting each artifact where its platform expects it. A
# target that does not produce an artifact of a given kind simply does not
# install one, so a static library, a shared library, and an executable can all
# be passed here without a type check.
function(fe_install_target target)
  if(NOT FE_INSTALL)
    return()
  endif()

  install(
    TARGETS ${target}
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
    LIBRARY DESTINATION "${FE_INSTALL_LIBDIR}"
    ARCHIVE DESTINATION "${FE_INSTALL_LIBDIR}"
  )

  # A relocated tree should run without LD_LIBRARY_PATH. $ORIGIN is the
  # executable's directory, so `../lib` finds the shared libraries beside it;
  # this is a no-op for the static engine but matters for anything shared.
  if(FE_INSTALL_RPATH AND NOT WIN32 AND NOT APPLE)
    set_target_properties(
      ${target}
      PROPERTIES INSTALL_RPATH "$ORIGIN/../${FE_INSTALL_LIBDIR}"
                 INSTALL_RPATH_USE_LINK_PATH TRUE
    )
  endif()
endfunction()

# Installs a header tree under the include root, preserving the tree. A caller
# passes the directory whose *contents* belong at the include root, so
# `engine/include/fe` installs as `include/fe/...`, not `include/fe/fe/...`.
function(fe_install_header_tree dir)
  if(NOT FE_INSTALL)
    return()
  endif()
  install(
    DIRECTORY "${dir}"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
    FILES_MATCHING
    PATTERN "*.h"
    PATTERN "*.hpp"
  )
endfunction()
