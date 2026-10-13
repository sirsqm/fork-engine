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
else()
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# Symbol visibility: modules are linked statically for now, but the macro is
# already in place so a future shared-library build is a configuration change,
# not a source change.
add_compile_definitions(FE_PLATFORM_DESKTOP=1)

if(UNIX AND NOT APPLE)
  add_compile_definitions(FE_PLATFORM_LINUX=1)
elseif(APPLE)
  add_compile_definitions(FE_PLATFORM_MACOS=1)
elseif(WIN32)
  add_compile_definitions(FE_PLATFORM_WINDOWS=1)
endif()

if(FE_WARNINGS_AS_ERRORS)
  if(MSVC)
    add_compile_options(/WX)
  else()
    add_compile_options(-Werror)
  endif()
endif()
