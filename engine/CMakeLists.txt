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
#   core, scene, systems, platform  <-  script (the module loader; see ADR-0061)
#   core, scene, systems, serialization  <-  script (script binding persistence; see ADR-0063)
#   core, scene, systems, reflect, platform, script  <-  player (the shipped
#     behaviours; compiled into every target including wasm, where no gameplay
#     module can be loaded — see ADR-0078)
#   core, scene, systems, reflect  <-  physics2d
#   core, scene, systems, reflect  <-  physics3d
#   core  <-  physics (the shared, pure island graph; see ADR-0111)
#   core, scene, systems, resources  <-  audio
#   core, resources  <-  pack
#   core  <-  jobs (a thread pool; see ADR-0068)
#   core, scene, scene3d, mesh, resources  <-  render (the extraction seam; its
#     parallel extract/cull take a JobSystem*, see ADR-0052, ADR-0069)
#   everything  <-  runtime
# See docs/ARCHITECTURE.md for the dependency rules.
#
# The `script` module (fe/script/) is header-only: it is the language-agnostic
# gameplay seam (Script, ScriptHost, ScriptRegistry, ScriptSystem) and has no
# translation unit of its own, so it contributes no entry to FE_ENGINE_SOURCES.
# Its headers ship with the library through the public include directory below.
# See docs/adr/0044-scripting-seam.md.


# ---------------------------------------------------------------------------
# The context-bearing window backend.
#
# Exactly one of these two is compiled. GLFW cannot be compiled to wasm at all,
# so a web build must not merely refrain from *calling* it — it must not compile
# it. The platform classifier (cmake/FEPlatform.cmake) already knows which target
# this is, so the choice is made there, and Window.cpp's factory takes the same
# branch from the same FE_PLATFORM_WEB macro. See
# docs/adr/0074-web-window-backend.md.
# ---------------------------------------------------------------------------
fe_platform_is_web(FE_TARGETS_WEB)
if(FE_TARGETS_WEB)
  set(FE_WINDOW_BACKEND_SOURCE src/platform/WebWindow.cpp)
else()
  set(FE_WINDOW_BACKEND_SOURCE src/platform/GlfwWindow.cpp)
endif()

# ---------------------------------------------------------------------------
# The user-data backend is chosen the same way, and for the same reason: a web
# build has no filesystem, so the desktop backend must not be compiled into it,
# and a desktop build has no browser storage. UserData.cpp's factory takes the
# same FE_PLATFORM_WEB branch. See docs/adr/0096-writable-user-data-storage.md.
# ---------------------------------------------------------------------------
if(FE_TARGETS_WEB)
  set(FE_USER_DATA_BACKEND_SOURCE src/platform/WebUserData.cpp)
else()
  set(FE_USER_DATA_BACKEND_SOURCE src/platform/DesktopUserData.cpp)
endif()

set(FE_ENGINE_SOURCES
    src/core/Log.cpp
    src/core/LogBuffer.cpp
    src/core/Assert.cpp
    src/core/Uuid.cpp
    src/core/FileSystem.cpp
    src/core/Profiler.cpp
    src/core/FrameArena.cpp
    src/core/SpatialBvh.cpp
    src/platform/InputState.cpp
    src/platform/InputActions.cpp
    src/platform/KeyCode.cpp
    src/platform/CanvasScale.cpp
    src/platform/NullWindow.cpp
    src/platform/DynamicLibrary.cpp
    src/platform/UserData.cpp
    src/platform/NullUserData.cpp
    src/platform/Window.cpp
    src/platform/WindowIcon.cpp
    # The context-bearing window backend is chosen by the platform: GLFW cannot
    # be compiled to wasm, so a web build compiles the canvas backend instead and
    # never sees GlfwWindow.cpp. Window.cpp's factory picks the same way, from the
    # same macro. See docs/adr/0074-web-window-backend.md.
    ${FE_WINDOW_BACKEND_SOURCE}
    # The persistent user-data backend is chosen the same way: the desktop
    # backend needs a filesystem a web build does not have, and the web backend
    # needs browser storage a desktop build does not have. See
    # docs/adr/0096-writable-user-data-storage.md.
    ${FE_USER_DATA_BACKEND_SOURCE}
    src/rhi/Device.cpp
    src/rhi/ShaderDialect.cpp
    src/rhi/null/NullDevice.cpp
    src/rhi/opengl/OpenGLDevice.cpp
    src/resources/ResourceManager.cpp
    src/resources/Font.cpp
    src/resources/FontLoader.cpp
    src/resources/ImageLoader.cpp
    src/resources/Texture2DLoader.cpp
    src/resources/AssetDatabase.cpp
    src/resources/ImportSettings.cpp
    src/resources/ImportCache.cpp
    src/scene/Node.cpp
    src/scene/Scene.cpp
    src/scene/Camera2D.cpp
    src/scene/Components.cpp
    src/scene/TextLabel.cpp
    src/scene/TextLabelTypes.cpp
    src/scene/Tilemap.cpp
    src/scene/TilemapTypes.cpp
    src/ui/UiLayout.cpp
    src/ui/UiInputSystem.cpp
    src/ui/UiTypesReflect.cpp
    src/reflect/Property.cpp
    src/reflect/Registry.cpp
    src/reflect/ComponentTypes.cpp
    src/systems/System.cpp
    src/systems/SceneSchedule.cpp
    src/systems/SpriteAnimationSystem.cpp
    src/anim/AnimationStateMachine.cpp
    src/anim/AnimationStateMachineSystem.cpp
    src/anim/AnimationTypes.cpp
    src/anim/AnimationClip.cpp
    src/anim/Animator.cpp
    src/anim/SkeletalAnimationTypes.cpp
    src/anim/Skeleton.cpp
    src/anim/SkeletonAnimationSystem.cpp
    src/anim/SkinnedMesh.cpp
    src/anim/SkinnedMeshRenderer3D.cpp
    src/physics/IslandSolver.cpp
    src/physics2d/BroadPhase.cpp
    src/physics2d/Geometry.cpp
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
    src/mesh/GltfScene.cpp
    src/scene3d/Camera3D.cpp
    src/scene3d/Scene3DTypes.cpp
    src/debugdraw/DebugDraw.cpp
    src/audio/AudioDevice.cpp
    src/audio/AudioMixer.cpp
    src/audio/AudioStream.cpp
    src/audio/AudioSystem.cpp
    src/audio/WavLoader.cpp
    src/audio/AudioTypes.cpp
    src/audio/AudioBackend.cpp
    src/audio/MiniAudioDevice.cpp
    src/pack/AssetClosure.cpp
    src/pack/PackFile.cpp
    src/pack/PackWriter.cpp
    src/pack/PackSource.cpp
    src/pack/HttpFetchSource.cpp
    src/pack/ExportManifest.cpp
    src/pack/WebExportTemplate.cpp
    src/pack/AndroidExportTemplate.cpp
    src/serialization/Json.cpp
    src/serialization/PropertyJson.cpp
    src/serialization/SceneSerializer.cpp
    src/serialization/SceneReferences.cpp
    src/serialization/Operation.cpp
    src/session/Session.cpp
    src/net/Message.cpp
    src/net/LoopbackNetwork.cpp
    src/net/SessionServer.cpp
    src/net/SessionClient.cpp
    src/render/RenderExtract.cpp
    src/render/RenderGraph.cpp
    src/renderer2d/Renderer2D.cpp
    src/renderer3d/PostProcess.cpp
    src/renderer3d/Renderer3D.cpp
    src/runtime/Engine.cpp
    src/runtime/Application.cpp
    src/runtime/AudioListenerSystem.cpp
    src/runtime/ComponentRegistration.cpp
    src/jobs/JobSystem.cpp
    src/script/ScriptLibrary.cpp
    src/script/ScriptBinding.cpp
    src/script/ScriptDiagnostics.cpp
    src/player/ActionBindings.cpp
    src/player/BuiltinBehaviours.cpp
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
  PUBLIC ${FE_GL_LOADER_TARGET} ${FE_JSON_TARGET} FE::stb
  PRIVATE ${FE_GLFW_TARGET} fe_warnings miniaudio
)

# The public headers must not leak GLFW. GLFW is a private implementation detail
# of the platform layer; only forward-declared handles appear in public headers.
# The define is harmless on web (nothing includes GLFW there) but it is only
# meaningful on desktop, so it is applied where GLFW exists.
if(NOT FE_TARGETS_WEB)
  target_compile_definitions(fork_engine PRIVATE GLFW_INCLUDE_NONE)
endif()

target_compile_features(fork_engine PUBLIC cxx_std_20)

set_target_properties(fork_engine PROPERTIES OUTPUT_NAME "fork_engine")

# Convenience: propagate the warnings interface to our own translation units.
target_link_libraries(fork_engine PRIVATE fe_warnings)

# The jobs module uses std::thread and std::condition_variable, which on Linux
# and macOS need the platform thread library linked (pthread). Threads::Threads
# resolves to the right flags per platform and is a no-op where the standard
# library already provides them, so it is linked PUBLIC: a consumer of the
# engine that starts its own std::threads (a game) gets the same flags.
find_package(Threads REQUIRED)
target_link_libraries(fork_engine PUBLIC Threads::Threads)

# The HTTP fetch source talks to a socket, which on Windows lives in Winsock.
# Winsock is not part of the default link set, so an unreferenced winsock call
# fails at link time with LNK2019 (__imp_accept, __imp_send, ...). Linked PUBLIC
# because a consumer (a game, or the test that serves a pack) opens sockets too.
if(WIN32)
  target_link_libraries(fork_engine PUBLIC ws2_32)
endif()

# Generated version header; engine sources include it, so they must wait for it.
target_sources(fork_engine PRIVATE "${CMAKE_BINARY_DIR}/generated/fe/version/Version.h")

# ---------------------------------------------------------------------------
# Install rules
#
# An installed engine is the library, its public headers, and the generated
# version header. The private window backend, the fetched dependencies, and the
# internal module headers are not installed: a consumer uses the public API only,
# and the fetched dependencies are static and already inside the library.
#
# The header tree is installed whole (every module under fe/), because the public
# API spans them - a game includes <fe/scene/Scene.h>, <fe/renderer3d/Renderer3D.h>,
# and so on. See cmake/FEInstall.cmake for where each artifact lands.
# ---------------------------------------------------------------------------
fe_install_target(fork_engine)
fe_install_header_tree("${CMAKE_CURRENT_SOURCE_DIR}/include/fe")
# The generated version header lives in the build tree, not the source tree, so
# it is installed separately (Version.h includes only the macros, so it has no
# dependencies of its own). Guarded by FE_INSTALL like every other install rule.
if(FE_INSTALL)
  install(
    FILES "${CMAKE_BINARY_DIR}/generated/fe/version/Version.h"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/fe/version"
  )
endif()
