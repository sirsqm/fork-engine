# FEDependencies.cmake
#
# Central dependency policy for Fork Engine.
#
# Rules (see docs/ARCHITECTURE.md, "Dependencies"):
#   * Prefer a small, well-maintained, permissively licensed library over a
#     hand-rolled subsystem when it provides substantial value.
#   * Every dependency must be pinned to an immutable tag or commit.
#   * Nothing reaches the engine through a transitive, unpinned fetch.
#   * Third-party code is never built with our warning flags; see FEWarnings.
#
# `FE_USE_SYSTEM_DEPS=ON` makes the build use system-installed packages when
# available, which is what distro packagers and CI caches want. The default is
# FetchContent so that a clean checkout builds reproducibly with one command.

include(FetchContent)

set(FE_GLFW_TAG "3.4" CACHE STRING "Pinned GLFW release tag")
set(FE_JSON_TAG "v3.11.3" CACHE STRING "Pinned nlohmann/json release tag")
# doctest 2.4.12 raised its `cmake_minimum_required` to 3.5, which CMake 4.x
# requires (it dropped compatibility with < 3.5). Do not downgrade below 2.4.12.
set(FE_DOCTEST_TAG "v2.4.12" CACHE STRING "Pinned doctest release tag")
# Dear ImGui's docking branch is published as tags of the form
# `<version>-docking`; these are immutable and safe to pin. The editor's ADR
# (docs/adr/0007-editor-ui-toolkit.md) records why the docking branch, and no
# other, is used.
set(FE_IMGUI_TAG "v1.92.9-docking" CACHE STRING "Pinned Dear ImGui docking tag")

# FEPlatform is included before anything else so the platform macros are defined
# and `fe_platform_is_web()` is available here; it is a no-op if FESettings
# already included it.
include(FEPlatform)

# ---------------------------------------------------------------------------
# glad - OpenGL function loader (vendored, generated once, committed).
# ---------------------------------------------------------------------------
# The generated loader is checked in so the build never needs Python, jinja2,
# or network access. Regeneration instructions live in third_party/glad/README.md.
#
# glad is desktop-only. WebGL2 supplies its own entry points, so a web build
# neither links the loader nor asks for it: a target that resolved GL symbols
# through glad would fail to link for wasm, and the whole point of the web
# target is that it takes the WebGL2 path (docs/adr/0073-emscripten-toolchain.md).
#
# FE_GL_LOADER_TARGET is empty for web, so the engine's link line names no loader
# there. It is an empty-string default rather than unset so the variable is
# always defined and a caller can link it unconditionally.
set(FE_GL_LOADER_TARGET "")
fe_platform_is_web(FE_TARGETS_WEB)
if(NOT FE_TARGETS_WEB)
  add_library(glad_gl_core_33 STATIC "${PROJECT_SOURCE_DIR}/third_party/glad/src/gl.c")
  add_library(FE::glad ALIAS glad_gl_core_33)
  set(FE_GL_LOADER_TARGET FE::glad)
  # SYSTEM so the generated loader headers do not warn inside our translation
  # units (their style is not ours to fix).
  target_include_directories(glad_gl_core_33 SYSTEM PUBLIC "${PROJECT_SOURCE_DIR}/third_party/glad/include")
  target_link_libraries(glad_gl_core_33 PUBLIC ${CMAKE_DL_LIBS})
  # Vendored C code: silence warnings we do not own.
  if(NOT MSVC)
    target_compile_options(glad_gl_core_33 PRIVATE -w)
  endif()
endif()

# ---------------------------------------------------------------------------
# GLFW - windowing, input, and the OpenGL context (platform layer backend).
# ---------------------------------------------------------------------------
# GLFW is the *desktop* windowing backend. It cannot be compiled to wasm, so a
# web build skips it entirely and takes the web window backend instead (a later
# queue item); the platform layer's OS-free modules still compile.
set(FE_GLFW_TARGET "")
if(NOT FE_TARGETS_WEB)
  if(FE_USE_SYSTEM_DEPS)
    find_package(glfw3 3.4 QUIET)
    if(TARGET glfw)
      set(FE_GLFW_TARGET glfw)
    endif()
  endif()

  if(NOT FE_GLFW_TARGET)
    set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
      glfw
      GIT_REPOSITORY https://github.com/glfw/glfw.git
      GIT_TAG ${FE_GLFW_TAG}
      GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(glfw)
    set(FE_GLFW_TARGET glfw)
  endif()
endif()

# ---------------------------------------------------------------------------
# nlohmann/json - serialization backend.
# ---------------------------------------------------------------------------
set(FE_JSON_TARGET "")
if(FE_USE_SYSTEM_DEPS)
  find_package(nlohmann_json 3.11 QUIET)
  if(TARGET nlohmann_json::nlohmann_json)
    set(FE_JSON_TARGET nlohmann_json::nlohmann_json)
  endif()
endif()

if(NOT FE_JSON_TARGET)
  FetchContent_Declare(
    json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG ${FE_JSON_TAG}
    GIT_SHALLOW TRUE
  )
  FetchContent_MakeAvailable(json)
  set(FE_JSON_TARGET nlohmann_json::nlohmann_json)
endif()

# Third-party code is never given our warning flags (FEWarnings), and its
# headers are marked SYSTEM so that including them from our translation units
# never produces diagnostics we cannot fix.
#
# The arguments are quoted because an unquoted empty variable expands to *no*
# argument, which would make the call zero-arity and fail at the call site rather
# than inside the helper. FE_GLFW_TARGET is legitimately empty on the web target,
# where GLFW is not fetched at all.
fe_mark_system("${FE_JSON_TARGET}")
fe_mark_system("${FE_GLFW_TARGET}")

# ---------------------------------------------------------------------------
# stb_image - image decoding (single-header, public domain).
# ---------------------------------------------------------------------------
add_library(fe_stb INTERFACE)
add_library(FE::stb ALIAS fe_stb)
# SYSTEM so stb_image.h (public-domain, single header, old-style casts) does not
# produce -Wold-style-cast and friends inside our translation units.
target_include_directories(fe_stb SYSTEM INTERFACE "${PROJECT_SOURCE_DIR}/third_party/stb")
target_compile_definitions(fe_stb INTERFACE STBI_NO_STDIO=0)

# ---------------------------------------------------------------------------
# miniaudio - cross-platform audio output (single-header, public domain / MIT-0).
# ---------------------------------------------------------------------------
# The real backend for the audio module. Single-header and dependency-free. Its
# CMakeLists defines a `miniaudio` target that compiles miniaudio.c (as C) once;
# our code includes the header for declarations only, so the implementation is
# never duplicated. The header is included as an external header (SYSTEM) so its
# warnings never reach us, and the extra node-graph libraries, examples, and
# tests are switched off so FetchContent builds only what we use.
set(FE_MINIAUDIO_TAG "0.11.25" CACHE STRING "Pinned miniaudio release tag")
set(MINIAUDIO_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(MINIAUDIO_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(MINIAUDIO_INSTALL OFF CACHE BOOL "" FORCE)
set(MINIAUDIO_NO_EXTRA_NODES ON CACHE BOOL "" FORCE)
FetchContent_Declare(
  miniaudio
  GIT_REPOSITORY https://github.com/mackron/miniaudio.git
  GIT_TAG ${FE_MINIAUDIO_TAG}
  GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(miniaudio)
fe_mark_system(miniaudio)

# ---------------------------------------------------------------------------
# doctest - unit test framework (only pulled in when tests are enabled).
# ---------------------------------------------------------------------------
if(FE_BUILD_TESTS)
  set(FE_DOCTEST_TARGET "")
  if(FE_USE_SYSTEM_DEPS)
    find_package(doctest 2.4 QUIET)
    if(TARGET doctest::doctest)
      set(FE_DOCTEST_TARGET doctest::doctest)
    endif()
  endif()

  if(NOT FE_DOCTEST_TARGET)
    # doctest ships its own install() rules, which would drop its headers and a
    # CMake package into our installed tree - dependency pollution in a shipped
    # engine. Turn them off before the fetch so the framework stays a build-time
    # artifact. (Same reason a fetched dependency is never installed: only our
    # own targets get install rules.)
    set(DOCTEST_NO_INSTALL ON CACHE BOOL "" FORCE)
    FetchContent_Declare(
      doctest
      GIT_REPOSITORY https://github.com/doctest/doctest.git
      GIT_TAG ${FE_DOCTEST_TAG}
      GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(doctest)
    set(FE_DOCTEST_TARGET doctest::doctest)
  endif()

  # doctest is a header-only dependency included from our test translation
  # units; treat its headers as external so they never warn (MSVC C5285).
  fe_mark_system(${FE_DOCTEST_TARGET})
endif()

# ---------------------------------------------------------------------------
# Dear ImGui - editor tooling UI (docking branch).
#
# Used only by the editor targets; the engine library never includes an ImGui
# header. The docking branch supplies DockSpace/panels. See
# docs/adr/0007-editor-ui-toolkit.md for the decision and its trade-offs.
#
# Fetched only when the editor is being built, so a headless engine build pulls
# nothing extra.
#
# We compile ImGui ourselves as a static target instead of consuming upstream's
# CMake, so the exact file list is explicit, no examples/backends we do not use
# are built, and the sources are never given our warning flags. Its include
# directory is marked SYSTEM, exactly like GLFW and nlohmann/json.
# ---------------------------------------------------------------------------
if(FE_BUILD_EDITOR)
set(FE_IMGUI_SOURCE_DIR "${CMAKE_BINARY_DIR}/_deps/imgui-src")
FetchContent_Declare(
  imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG ${FE_IMGUI_TAG}
  GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(imgui)

# ImGui's OpenGL3 backend ships its own embedded GL3 loader, so it does not
# need the engine's glad loader and pulls in no GL header of ours. The two
# loaders coexist harmlessly: both resolve the same driver entry points, and
# ImGui's are private to its backend translation unit. Attempting to force the
# backend onto glad (IMGUI_IMPL_OPENGL_LOADER_CUSTOM) would require injecting a
# GL header into ImGui's source, which is exactly the coupling the editor ADR
# avoids.
add_library(fe_imgui STATIC
    "${FE_IMGUI_SOURCE_DIR}/imgui.cpp"
    "${FE_IMGUI_SOURCE_DIR}/imgui_draw.cpp"
    "${FE_IMGUI_SOURCE_DIR}/imgui_tables.cpp"
    "${FE_IMGUI_SOURCE_DIR}/imgui_widgets.cpp"
    "${FE_IMGUI_SOURCE_DIR}/imgui_demo.cpp"
    "${FE_IMGUI_SOURCE_DIR}/misc/cpp/imgui_stdlib.cpp"
    "${FE_IMGUI_SOURCE_DIR}/backends/imgui_impl_glfw.cpp"
    "${FE_IMGUI_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp"
)
add_library(FE::imgui ALIAS fe_imgui)

# misc/cpp is added so std::string text widgets (imgui_stdlib.h) are available
# to the panels.
target_include_directories(
  fe_imgui
  SYSTEM PUBLIC "${FE_IMGUI_SOURCE_DIR}" "${FE_IMGUI_SOURCE_DIR}/backends" "${FE_IMGUI_SOURCE_DIR}/misc/cpp"
)
target_compile_definitions(fe_imgui PUBLIC GLFW_INCLUDE_NONE)
target_link_libraries(fe_imgui PUBLIC ${FE_GLFW_TARGET})
target_compile_features(fe_imgui PUBLIC cxx_std_20)

# ImGui is single-translation-unit-friendly and warning-noisy under our strict
# flags; it is third-party code and is never built with fe_warnings.
if(NOT MSVC)
  target_compile_options(fe_imgui PRIVATE -w)
endif()
fe_mark_system(fe_imgui)
endif() # FE_BUILD_EDITOR
