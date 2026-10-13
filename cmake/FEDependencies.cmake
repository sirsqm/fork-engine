# FEDependencies.cmake
#
# Central dependency policy for Fork Engine.
#
# Rules (see docs/ARCHITECTURE.md, "Dependencies"):
#   * Prefer a small, well-maintained, permissively licensed library over a
#     hand-rolled subsystem when it provides substantial value.
#   * Every dependency must be pinned to an immutable tag or commit.
#   * Nothing reaches the engine through a transitive, unpinned fetch.
#
# `FE_USE_SYSTEM_DEPS=ON` makes the build use system-installed packages when
# available, which is what distro packagers and CI caches want. The default is
# FetchContent so that a clean checkout builds reproducibly with one command.

include(FetchContent)

set(FE_GLFW_TAG "3.4" CACHE STRING "Pinned GLFW release tag")
set(FE_JSON_TAG "v3.11.3" CACHE STRING "Pinned nlohmann/json release tag")
set(FE_DOCTEST_TAG "v2.4.11" CACHE STRING "Pinned doctest release tag")

# ---------------------------------------------------------------------------
# glad - OpenGL function loader (vendored, generated once, committed).
# ---------------------------------------------------------------------------
# The generated loader is checked in so the build never needs Python, jinja2,
# or network access. Regeneration instructions live in third_party/glad/README.md.
add_library(glad_gl_core_33 STATIC "${PROJECT_SOURCE_DIR}/third_party/glad/src/gl.c")
add_library(FE::glad ALIAS glad_gl_core_33)
target_include_directories(glad_gl_core_33 PUBLIC "${PROJECT_SOURCE_DIR}/third_party/glad/include")
target_link_libraries(glad_gl_core_33 PUBLIC ${CMAKE_DL_LIBS})
# Vendored C code: silence warnings we do not own.
if(NOT MSVC)
  target_compile_options(glad_gl_core_33 PRIVATE -w)
endif()

# ---------------------------------------------------------------------------
# GLFW - windowing, input, and the OpenGL context (platform layer backend).
# ---------------------------------------------------------------------------
set(FE_GLFW_TARGET "")
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

# ---------------------------------------------------------------------------
# stb_image - image decoding (single-header, public domain).
# ---------------------------------------------------------------------------
add_library(fe_stb INTERFACE)
add_library(FE::stb ALIAS fe_stb)
target_include_directories(fe_stb INTERFACE "${PROJECT_SOURCE_DIR}/third_party/stb")
target_compile_definitions(fe_stb INTERFACE STBI_NO_STDIO=0)

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
    FetchContent_Declare(
      doctest
      GIT_REPOSITORY https://github.com/doctest/doctest.git
      GIT_TAG ${FE_DOCTEST_TAG}
      GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(doctest)
    set(FE_DOCTEST_TARGET doctest::doctest)
  endif()
endif()
