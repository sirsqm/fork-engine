# FEVersion.cmake
#
# Generates the C++-visible engine version header from the version declared in
# the root project() call, so the version string has exactly one source of
# truth (CMakeLists.txt) and cannot drift from the CMake package version.
#
# Must be included after project().

if(NOT DEFINED PROJECT_VERSION)
  message(FATAL_ERROR "FEVersion.cmake must be included after project()")
endif()

configure_file(
  "${CMAKE_CURRENT_LIST_DIR}/FEVersion.h.in" "${CMAKE_BINARY_DIR}/generated/fe/version/Version.h" @ONLY
)
