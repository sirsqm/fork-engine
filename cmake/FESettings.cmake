# FESettings.cmake
#
# Build options that affect the exported interface of Fork Engine targets.
# Kept in one place so the flags stay consistent across every module.

if(MSVC)
  # /permissive-  : conforming mode
  # /Zc:__cplusplus : report the real value of __cplusplus
  # /utf-8        : source and execution charset
  add_compile_options(/permissive- /Zc:__cplusplus /utf-8 /MP)
  add_compile_definitions(
    _CRT_SECURE_NO_WARNINGS
    NOMINMAX
    WIN32_LEAN_AND_MEAN
  )
endif()

# NOTE: warning flags are deliberately NOT set here with add_compile_options().
# That form is directory-scoped but inherited by every subdirectory below this
# one, including FetchContent dependencies (GLFW, nlohmann/json, doctest). The
# dependency policy promises that third-party code is never given our flags, so
# warnings are applied per-target instead, through the fe_warnings interface
# target in FEWarnings.cmake. FESettings.cmake keeps only the flags that are
# safe (and necessary) to apply everywhere.

# Symbol visibility: modules are linked statically for now, but the macro is
# already in place so a future shared-library build is a configuration change,
# not a source change.
#
# The platform macros live in FEPlatform.cmake, which classifies the target from
# CMAKE_SYSTEM_NAME so a cross build (the Emscripten/web target) is classified
# correctly, and defines exactly one of FE_PLATFORM_DESKTOP / FE_PLATFORM_WEB.
# Keeping detection there is what lets cmake/tests/test_fe_platform.cmake drive it
# in script mode; this file stays the place that applies the result.
include(FEPlatform)
fe_apply_platform_definitions()

# Warnings-as-errors is applied through the fe_warnings interface target (see
# FEWarnings.cmake), not here, so it cannot leak into third-party targets.
