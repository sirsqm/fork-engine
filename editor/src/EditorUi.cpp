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

namespace fe::editor {
namespace {

// The dock space covers the whole window; panels dock into it. This id is
// stable, which is what lets the saved layout find the host again next launch.
constexpr const char* kDockSpaceName = "ForkEngineDockSpace";
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
            // The hierarchy takes the left third; the remainder is left as the
            // centre dock node for the viewport and future panels.
            const ImGuiID left = ImGui::DockBuilderSplitNode(centre, ImGuiDir_Left, 0.30f, nullptr, &centre);

            ImGui::DockBuilderDockWindow(panels_.front()->name(), left);
            for (usize i = 1; i < panels_.size(); ++i) {
                ImGui::DockBuilderDockWindow(panels_[i]->name(), centre);
            }
            ImGui::DockBuilderFinish(kDockSpaceId);
        }
        default_layout_applied_ = true;
    }

    for (const auto& panel : panels_) {
        panel->draw(context);
    }
}

void EditorUi::end_frame() {
    FE_CHECK(initialized_);
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

} // namespace fe::editor
