# Fork Engine editor.
#
# The editor is built as two libraries and a shell executable, not as part of
# the engine library:
#
#   fork_editor_core  - document + hierarchy view model + play-in-editor. No UI
#                       toolkit, no window: linkable and testable headlessly.
#   fork_editor       - the Dear ImGui UI layer and the editor host that ties an
#                       embedded Engine, the document, and the UI together.
#   fork_editor_shell - the executable entry point that parses arguments and
#                       runs an EditorHost.
#
# Dependency direction (docs/adr/0007-editor-ui-toolkit.md): the editor depends
# on the engine, never the reverse. The engine library must never include an
# ImGui header; only `fork_editor` links FE::imgui.

# ---------------------------------------------------------------------------
# fork_editor_core - headless editor logic
# ---------------------------------------------------------------------------
add_library(fork_editor_core STATIC
    src/HierarchyViewModel.cpp
    src/AssetBrowserViewModel.cpp
    src/EditorContext.cpp
    src/EditorPalette.cpp
    src/ProjectSettings.cpp
    src/PlaySession.cpp
    src/PropertyEditor.cpp
    src/ProfilerPanelViewModel.cpp
    src/ConsolePanelViewModel.cpp
    src/Viewport.cpp
    src/Viewport3D.cpp
    src/Command.cpp
    src/FileDialogViewModel.cpp
    src/Commands.cpp
    src/OperationCommand.cpp
)
add_library(FE::editor_core ALIAS fork_editor_core)

target_include_directories(
  fork_editor_core
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(fork_editor_core PUBLIC FE::engine PRIVATE fe_warnings)
target_compile_features(fork_editor_core PUBLIC cxx_std_20)

# ---------------------------------------------------------------------------
# fork_editor - ImGui UI layer + editor host
# ---------------------------------------------------------------------------
add_library(fork_editor STATIC
    src/EditorUi.cpp
    src/EditorStyle.cpp
    src/EditorWidgets.cpp
    src/EditorHost.cpp
    src/HierarchyPanel.cpp
    src/AssetBrowserPanel.cpp
    src/PlayPanel.cpp
    src/InspectorPanel.cpp
    src/ProfilerPanel.cpp
    src/ConsolePanel.cpp
    src/FileDialogPanel.cpp
    src/ConfirmPanel.cpp
    src/ViewportPanel.cpp
)
add_library(FE::editor ALIAS fork_editor)

# ---------------------------------------------------------------------------
# Embedded fonts
#
# The UI draws with a real typeface, not Dear ImGui's built-in bitmap font. The
# bytes are embedded in the binary so the same code finds the font from a build
# tree, an install tree, and the AppImage; see cmake/EmbedFonts.cmake. The
# generated unit is added to fork_editor rather than committed.
# ---------------------------------------------------------------------------
set(FE_EDITOR_FONT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts")
set(FE_EDITOR_EMBEDDED_FONTS "${CMAKE_CURRENT_BINARY_DIR}/generated/EmbeddedFonts.cpp")
add_custom_command(
  OUTPUT "${FE_EDITOR_EMBEDDED_FONTS}"
  COMMAND ${CMAKE_COMMAND}
          -DINPUT_DIR=${FE_EDITOR_FONT_DIR}
          -DOUTPUT=${FE_EDITOR_EMBEDDED_FONTS}
          -P "${CMAKE_SOURCE_DIR}/cmake/EmbedFonts.cmake"
  DEPENDS
    "${FE_EDITOR_FONT_DIR}/Inter-Regular.ttf"
    "${FE_EDITOR_FONT_DIR}/Inter-SemiBold.ttf"
    "${FE_EDITOR_FONT_DIR}/JetBrainsMono-Regular.ttf"
    "${CMAKE_SOURCE_DIR}/cmake/EmbedFonts.cmake"
  COMMENT "Embedding editor fonts"
  VERBATIM
)
target_sources(fork_editor PRIVATE "${FE_EDITOR_EMBEDDED_FONTS}")

target_include_directories(
  fork_editor
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
# FE::imgui carries its include directories as SYSTEM, so <imgui.h> never warns
# inside our translation units.
target_link_libraries(fork_editor PUBLIC FE::editor_core FE::imgui PRIVATE fe_warnings)
target_compile_features(fork_editor PUBLIC cxx_std_20)

# ---------------------------------------------------------------------------
# fork_editor_shell - the editor executable
# ---------------------------------------------------------------------------
add_executable(fork_editor_shell shell/main.cpp)
target_link_libraries(fork_editor_shell PRIVATE FE::editor fe_warnings)
target_compile_features(fork_editor_shell PRIVATE cxx_std_20)
set_target_properties(fork_editor_shell PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# ---------------------------------------------------------------------------
# Install rules
#
# The editor shell is the user-facing executable; the two editor libraries are
# implementation details of it and are not installed (they are static and linked
# into the shell). The engine library the shell needs is installed by
# engine/CMakeLists.txt.
# ---------------------------------------------------------------------------
fe_install_target(fork_editor_shell)
