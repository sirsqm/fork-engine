// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/editor/EditorUi.h"

#include "fe/core/Assert.h"
#include "fe/core/Log.h"
#include "fe/editor/EditorContext.h"
#include "fe/platform/Window.h"

#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_opengl3.h"
#include "imgui.h"
#include "imgui_internal.h"

#include <GLFW/glfw3.h>

#include <string>
#include <string_view>

namespace fe::editor {
namespace {

// The dock space covers the whole window; panels dock into it. The id is a
// stable constant, which is what lets the saved layout find the host again
// next launch.
constexpr ImGuiID kDockSpaceId = 0x0FE1D0C4;

} // namespace

EditorUi::EditorUi() = default;

EditorUi::~EditorUi() {
    shutdown();
}

Error EditorUi::initialize(Window& window) {
    if (initialized_) {
        return Error("EditorUi::initialize called twice");
    }

    IMGUI_CHECKVERSION();
    if (ImGui::CreateContext() == nullptr) {
        return Error("failed to create the Dear ImGui context");
    }

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable; // DockSpace + dockable panels.
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    // Multi-viewport is deliberately off: it would create extra OS windows and
    // GL contexts. See docs/adr/0007-editor-ui-toolkit.md.
    io.IniFilename = nullptr; // The shell decides where (and whether) to persist.

    ImGui::StyleColorsDark();

    auto* glfw_window = static_cast<GLFWwindow*>(window.native_handle());
    if (glfw_window == nullptr) {
        ImGui::DestroyContext();
        return Error("EditorUi: the window has no native handle");
    }

    if (!ImGui_ImplGlfw_InitForOpenGL(glfw_window, /*install_callbacks=*/false)) {
        ImGui::DestroyContext();
        return Error("EditorUi: ImGui_ImplGlfw_InitForOpenGL failed");
    }
    // The engine's GL context is an OpenGL 3.3 core context; the backend's GLSL
    // version string must match.
    if (!ImGui_ImplOpenGL3_Init("#version 330 core")) {
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        return Error("EditorUi: ImGui_ImplOpenGL3_Init failed");
    }

    initialized_ = true;
    FE_LOG_INFO("editor: Dear ImGui %s initialised", IMGUI_VERSION);
    return Error::none();
}

void EditorUi::shutdown() {
    if (!initialized_) {
        return;
    }
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    initialized_ = false;
    panels_.clear();
}

void EditorUi::add_panel(std::unique_ptr<Panel> panel) {
    if (panel != nullptr) {
        panels_.push_back(std::move(panel));
    }
}

void EditorUi::begin_frame() {
    FE_CHECK(initialized_);
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
}

void EditorUi::draw_dock_space(unsigned int dockspace_id) {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::SetNextWindowViewport(viewport->ID);

    // The host window fills the viewport and carries no chrome of its own; it
    // exists only to hold the dock space.
    const ImGuiWindowFlags host_flags = ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar |
                                        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                                        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus |
                                        ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoBackground;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::Begin("##ForkEngineDockHost", nullptr, host_flags);
    ImGui::PopStyleVar(3);

    ImGui::DockSpace(dockspace_id, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_None);
    ImGui::End();
}

void EditorUi::draw_panels(EditorContext& context) {
    FE_CHECK(initialized_);

    draw_toolbar(context);
    draw_dock_space(kDockSpaceId);

    // Apply a default layout the first frame after the dock space exists, so a
    // fresh user sees panels arranged rather than floating. The layout is built
    // from the panels actually registered, so it never references a panel that
    // does not exist.
    if (!default_layout_applied_ && !panels_.empty()) {
        if (ImGui::DockBuilderGetNode(kDockSpaceId) != nullptr) {
            ImGui::DockBuilderRemoveNode(kDockSpaceId);
            ImGui::DockBuilderAddNode(kDockSpaceId, ImGuiDockNodeFlags_DockSpace);
            ImGui::DockBuilderSetNodeSize(kDockSpaceId, ImGui::GetMainViewport()->WorkSize);

            ImGuiID centre = kDockSpaceId;
            // Hierarchy on the left, inspector on the right, the viewport and
            // play panel in the centre. The split is by name so it tracks which
            // panels are actually registered.
            const ImGuiID left = ImGui::DockBuilderSplitNode(centre, ImGuiDir_Left, 0.22f, nullptr, &centre);
            const ImGuiID right = ImGui::DockBuilderSplitNode(centre, ImGuiDir_Right, 0.28f, nullptr, &centre);
            const ImGuiID bottom = ImGui::DockBuilderSplitNode(centre, ImGuiDir_Down, 0.28f, nullptr, &centre);

            ImGui::DockBuilderDockWindow("Hierarchy", left);
            ImGui::DockBuilderDockWindow("Inspector", right);
            ImGui::DockBuilderDockWindow("Viewport", centre);
            ImGui::DockBuilderDockWindow("Play", bottom);
            // Any panel not named above (a future one) docks in the centre so it
            // is visible rather than lost.
            for (const auto& panel : panels_) {
                const char* panel_name = panel->name();
                if (std::string_view(panel_name) != "Hierarchy" && std::string_view(panel_name) != "Inspector" &&
                    std::string_view(panel_name) != "Viewport" && std::string_view(panel_name) != "Play") {
                    ImGui::DockBuilderDockWindow(panel_name, centre);
                }
            }
            ImGui::DockBuilderFinish(kDockSpaceId);
        }
        default_layout_applied_ = true;
    }

    for (const auto& panel : panels_) {
        panel->draw(context);
    }
}

void EditorUi::draw_toolbar(EditorContext& context) {
    if (!ImGui::BeginMainMenuBar()) {
        return;
    }

    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("New Scene", "Ctrl+N")) {
            context.request_file(FileRequest::NewScene);
        }
        if (ImGui::MenuItem("Open Scene...", "Ctrl+O")) {
            context.request_file(FileRequest::OpenScene);
        }
        ImGui::Separator();
        // Save is enabled whenever there is a document; it either writes to the
        // known path or the host opens the dialog for a new one.
        if (ImGui::MenuItem("Save Scene", "Ctrl+S")) {
            context.request_file(FileRequest::SaveScene);
        }
        if (ImGui::MenuItem("Save Scene As...", "Ctrl+Shift+S")) {
            context.request_file(FileRequest::SaveSceneAs);
        }
        ImGui::Separator();
        // Revert only makes sense for a document that has a file to revert to.
        if (ImGui::MenuItem("Revert Scene", nullptr, false, !context.scene_path().empty())) {
            context.request_file(FileRequest::ReloadScene);
        }
        if (ImGui::MenuItem("Open Project...")) {
            context.request_file(FileRequest::OpenProject);
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Quit", "Ctrl+Q")) {
            context.request_file(FileRequest::Quit);
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Edit")) {
        // The label names the operation undo/redo would perform, which is the
        // convention users expect. Disabled while playing (the scene is hidden).
        const std::string undo_label =
            context.can_undo() ? std::string("Undo ") + std::string(context.commands().undo_label()) : "Undo";
        const std::string redo_label =
            context.can_redo() ? std::string("Redo ") + std::string(context.commands().redo_label()) : "Redo";
        if (ImGui::MenuItem(undo_label.c_str(), "Ctrl+Z", false, context.can_undo())) {
            context.undo();
        }
        if (ImGui::MenuItem(redo_label.c_str(), "Ctrl+Y", false, context.can_redo())) {
            context.redo();
        }
        ImGui::EndMenu();
    }

    const PlaySession& play = context.play();
    if (play.is_playing()) {
        if (ImGui::Button("Stop")) {
            context.request_stop_play();
        }
    } else {
        if (ImGui::Button("Play")) {
            context.request_start_play();
        }
    }

    ImGui::SameLine();
    const std::string scene_name = context.scene().name();
    const std::string path = context.scene_path().empty() ? "(unsaved)" : context.scene_path();
    // A trailing "*" marks unsaved changes, the convention every editor uses, so
    // the user can tell at a glance whether quitting will lose work.
    const char* dirty_marker = context.is_dirty() ? " *" : "";
    ImGui::Text("  %s%s  -  %s  |  %zu nodes", scene_name.c_str(), dirty_marker, path.c_str(),
                context.scene().node_count());
    if (play.is_playing()) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1.0f), "  PLAYING");
    }

    ImGui::EndMainMenuBar();

    // Keyboard shortcuts, handled outside the menu so they work regardless of
    // which panel has focus. Suppressed while a text field is active so typing
    // does not trigger them. ImGui's Ctrl+Y redo and Ctrl+Shift+Z are both
    // accepted, matching common conventions.
    const ImGuiIO& io = ImGui::GetIO();
    if (!io.WantTextInput && !play.is_playing()) {
        const bool ctrl = io.KeyCtrl;
        if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Z, false)) {
            if (io.KeyShift) {
                context.redo();
            } else {
                context.undo();
            }
        }
        if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Y, false)) {
            context.redo();
        }
        // File shortcuts. These only emit requests; the host performs them, so a
        // Ctrl+S that needs a path opens the dialog rather than writing silently.
        if (ctrl && ImGui::IsKeyPressed(ImGuiKey_N, false)) {
            context.request_file(FileRequest::NewScene);
        }
        if (ctrl && ImGui::IsKeyPressed(ImGuiKey_O, false)) {
            context.request_file(FileRequest::OpenScene);
        }
        if (ctrl && ImGui::IsKeyPressed(ImGuiKey_S, false)) {
            context.request_file(io.KeyShift ? FileRequest::SaveSceneAs : FileRequest::SaveScene);
        }
        if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Q, false)) {
            context.request_file(FileRequest::Quit);
        }
    }
}

void EditorUi::end_frame() {
    FE_CHECK(initialized_);
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

} // namespace fe::editor
