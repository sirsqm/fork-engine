# Fork Engine - core library
#
# A single static library built from several internal modules. Each module has
# its own source directory and a header namespace under `fe/<module>/`. The
# public include root is `engine/include`, so consumers write
# `#include <fe/core/Log.h>`.
#
# Modules may depend "downwards" only:
#   core  <-  platform  <-  rhi  <-  resources  <-  scene  <-  renderer2d
#   core  <-  serialization  <-  scene
#   everything  <-  runtime
# See docs/ARCHITECTURE.md for the dependency rules.

set(FE_ENGINE_SOURCES
    src/core/Log.cpp
    src/core/Assert.cpp
    src/core/Uuid.cpp
    src/core/FileSystem.cpp
    src/platform/InputState.cpp
    src/platform/Window.cpp
    src/rhi/Device.cpp
    src/rhi/opengl/OpenGLDevice.cpp
    src/resources/ResourceManager.cpp
    src/resources/ImageLoader.cpp
    src/resources/Texture2DLoader.cpp
    src/scene/Node.cpp
    src/scene/Scene.cpp
    src/scene/Camera2D.cpp
    src/scene/Components.cpp
    src/serialization/Json.cpp
    src/serialization/SceneSerializer.cpp
    src/renderer2d/Renderer2D.cpp
    src/runtime/Engine.cpp
    src/runtime/Application.cpp
)

add_library(fork_engine STATIC ${FE_ENGINE_SOURCES})
add_library(FE::engine ALIAS fork_engine)

target_include_directories(
  fork_engine
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/generated>
         $<INSTALL_INTERFACE:include>
)

target_link_libraries(
  fork_engine
  PUBLIC FE::glad ${FE_JSON_TARGET} FE::stb
  PRIVATE ${FE_GLFW_TARGET} fe_warnings
)

# The public headers must not leak GLFW. GLFW is a private implementation detail
# of the platform layer; only forward-declared handles appear in public headers.
target_compile_definitions(fork_engine PRIVATE GLFW_INCLUDE_NONE)

target_compile_features(fork_engine PUBLIC cxx_std_20)

set_target_properties(fork_engine PROPERTIES OUTPUT_NAME "fork_engine")

# Convenience: propagate the warnings interface to our own translation units.
target_link_libraries(fork_engine PRIVATE fe_warnings)

# Generated version header; engine sources include it, so they must wait for it.
target_sources(fork_engine PRIVATE "${CMAKE_BINARY_DIR}/generated/fe/version/Version.h")
