# Fork Engine - core library
#
# A single static library built from several internal modules. Each module has
# its own source directory and a header namespace under `fe/<module>/`. The
# public include root is `engine/include`, so consumers write
# `#include <fe/core/Log.h>`.
#
# Modules may depend "downwards" only:
#   core  <-  platform  <-  rhi  <-  resources  <-  scene  <-  renderer2d
#   core  <-  reflect  <-  scene, resources
#   core  <-  serialization  <-  scene, resources, reflect
#   core, scene, resources  <-  systems
#   core, scene, systems  <-  script
#   core, scene, systems, reflect  <-  physics2d
#   core, scene, systems, reflect  <-  physics3d
#   core, scene, systems, resources  <-  audio
#   core, resources  <-  pack
#   everything  <-  runtime
# See docs/ARCHITECTURE.md for the dependency rules.
#
# The `script` module (fe/script/) is header-only: it is the language-agnostic
# gameplay seam (Script, ScriptHost, ScriptRegistry, ScriptSystem) and has no
# translation unit of its own, so it contributes no entry to FE_ENGINE_SOURCES.
# Its headers ship with the library through the public include directory below.
# See docs/adr/0044-scripting-seam.md.

set(FE_ENGINE_SOURCES
    src/core/Log.cpp
    src/core/Assert.cpp
    src/core/Uuid.cpp
    src/core/FileSystem.cpp
    src/core/Profiler.cpp
    src/platform/InputState.cpp
    src/platform/NullWindow.cpp
    src/platform/Window.cpp
    src/rhi/Device.cpp
    src/rhi/null/NullDevice.cpp
    src/rhi/opengl/OpenGLDevice.cpp
    src/resources/ResourceManager.cpp
    src/resources/ImageLoader.cpp
    src/resources/Texture2DLoader.cpp
    src/resources/AssetDatabase.cpp
    src/resources/ImportSettings.cpp
    src/resources/ImportCache.cpp
    src/scene/Node.cpp
    src/scene/Scene.cpp
    src/scene/Camera2D.cpp
    src/scene/Components.cpp
    src/reflect/Registry.cpp
    src/reflect/ComponentTypes.cpp
    src/systems/System.cpp
    src/systems/SceneSchedule.cpp
    src/systems/SpriteAnimationSystem.cpp
    src/physics2d/BroadPhase.cpp
    src/physics2d/Geometry.cpp
    src/physics2d/IslandSolver.cpp
    src/physics2d/JointSolver.cpp
    src/physics2d/Physics2DQueries.cpp
    src/physics2d/Physics2DSystem.cpp
    src/physics2d/PhysicsTypes.cpp
    src/physics3d/BroadPhase3D.cpp
    src/physics3d/Geometry3D.cpp
    src/physics3d/JointSolver3D.cpp
    src/physics3d/Physics3DQueries.cpp
    src/physics3d/Physics3DSystem.cpp
    src/physics3d/Physics3DTypes.cpp
    src/mesh/Mesh.cpp
    src/mesh/ObjLoader.cpp
    src/mesh/GltfLoader.cpp
    src/scene3d/Camera3D.cpp
    src/scene3d/Scene3DTypes.cpp
    src/audio/AudioDevice.cpp
    src/audio/AudioMixer.cpp
    src/audio/AudioSystem.cpp
    src/audio/WavLoader.cpp
    src/audio/AudioTypes.cpp
    src/audio/AudioBackend.cpp
    src/audio/MiniAudioDevice.cpp
    src/pack/PackFile.cpp
    src/pack/PackWriter.cpp
    src/pack/PackSource.cpp
    src/serialization/Json.cpp
    src/serialization/SceneSerializer.cpp
    src/serialization/Operation.cpp
    src/session/Session.cpp
    src/net/Message.cpp
    src/net/LoopbackNetwork.cpp
    src/net/SessionServer.cpp
    src/net/SessionClient.cpp
    src/renderer2d/Renderer2D.cpp
    src/renderer3d/Renderer3D.cpp
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
  PRIVATE ${FE_GLFW_TARGET} fe_warnings miniaudio
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
