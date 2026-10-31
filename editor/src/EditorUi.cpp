// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/editor/EditorUi.h"

#include "fe/core/Assert.h"
#include "fe/core/Log.h"
#include "fe/editor/EditorContext.h"
#include "fe/editor/EditorStyle.h"
#include "fe/editor/EditorWidgets.h"
#include "fe/platform/Window.h"

#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_opengl3.h"
#include "imgui.h"
#include "imgui_internal.h"

#include <GLFW/glfw3.h>

#include <cstdio>
#include <string>
#include <string_view>

namespace fe::editor {
namespace {

// The dock space covers the whole window; panels dock into it. The id is a
// stable constant, which is what lets the saved layout find the host again
// next launch.
constexpr ImGuiID kDockSpaceId = 0x0FE1D0C4;

// The glyphs the toolbar draws. Taken from the embedded Inter face (which
// carries the geometric shapes and arrows below), so the buttons are typographic
// rather than images and scale with the font.
constexpr const char* kGlyphPlay = "\u25B6"; // BLACK RIGHT-POINTING TRIANGLE
constexpr const char* kGlyphStop = "\u25A0"; // BLACK SQUARE
constexpr const char* kGlyphUndo = "\u21BB"; // CLOCKWISE OPEN CIRCLE ARROW

[[nodiscard]] ImVec4 to_imvec4(const Rgba& c) noexcept {
    return ImVec4{c.r, c.g, c.b, c.a};
}

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

    // The editor's own look, not a stock ImGui theme, and its own typeface rather
    // than the built-in bitmap font. Applied before the backends so the first
    // frame is already correct.
    const EditorScheme initial = EditorScheme::for_theme(EditorTheme::Dark, EditorDensity::Comfortable);
    apply_editor_style(initial);
    ui::set_active_palette(initial.palette);
    ui::set_active_metrics(initial.metrics);
    applied_scheme_ = initial;
    style_applied_ = true;

    if (!load_editor_fonts(fonts_, initial.metrics)) {
        // Not fatal: ImGui keeps its built-in font, so the editor still runs. The
        // warning is worth having, because the UI will look wrong and this is the
        // one place that says why.
        FE_LOG_WARN("editor: embedded UI fonts failed to load; falling back to the built-in font");
    }

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
    // A density change from the previous frame asked for a font re-bake. This is
    // the one place it is safe: the previous frame has been rendered and nothing
    // is bound yet, so clearing the atlas does not remove a live font.
    if (fonts_need_rebuild_) {
        fonts_need_rebuild_ = false;
        (void)rebuild_editor_fonts(fonts_, applied_scheme_.metrics);
    }
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

void EditorUi::apply_theme(EditorTheme theme, EditorDensity density) {
    // Re-applying a style is cheap but not free, and a per-frame call would reset
    // any colour the user had tweaked, so this is a no-op unless the scheme
    // actually changed. `initialize()` already applied the initial scheme.
    const EditorScheme wanted = EditorScheme::for_theme(theme, density);
    const bool same_theme = wanted.palette.window_bg.r == applied_scheme_.palette.window_bg.r &&
                            wanted.palette.window_bg.g == applied_scheme_.palette.window_bg.g &&
                            wanted.palette.window_bg.b == applied_scheme_.palette.window_bg.b;
    const bool same_density = wanted.metrics.font_size == applied_scheme_.metrics.font_size;
    if (style_applied_ && same_theme && same_density) {
        return;
    }
    apply_editor_style(wanted);
    applied_scheme_ = wanted;
    style_applied_ = true;
    // Keep the shared widgets' palette in step, so a panel that calls ui::labelled
    // or ui::badge draws in the scheme now on screen.
    ui::set_active_palette(wanted.palette);
    ui::set_active_metrics(wanted.metrics);
    // A density change moves the font size, so the faces must be re-baked at the
    // new size. That rebuild cannot happen here: this runs inside a frame, where
    // the atlas's current font is bound, and clearing it mid-frame aborts. Set a
    // flag and do it in begin_frame(), between frames.
    if (!same_density) {
        fonts_need_rebuild_ = true;
    }
}

void EditorUi::draw_panels(EditorContext& context) {
    FE_CHECK(initialized_);

    apply_theme(context.theme(), context.density());
    // Keep the context's copy of the scheme in step with what is on screen, so a
    // panel reading context.scheme() reads the applied colours and metrics.
    context.set_scheme(applied_scheme_);

    // The whole frame draws in the regular face; a panel that wants the mono or
    // semibold face pushes it for its own draws. Pushing once here rather than
    // per panel means the default is right even before a panel runs.
    if (fonts_.regular != nullptr) {
        ImGui::PushFont(fonts_.regular, applied_scheme_.metrics.font_size);
    }

    draw_toolbar(context);
    draw_transport_bar(context);
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

    if (fonts_.regular != nullptr) {
        ImGui::PopFont();
    }
}

void EditorUi::draw_transport_bar(EditorContext& context) {
    // A strip docked to the top of the viewport, below the menu bar. It is drawn
    // on the `surface` colour rather than the panel body, so it reads as chrome
    // above the working area rather than as a floating panel, and a hairline
    // under it separates the two.
    const float height = ImGui::GetFrameHeight() + applied_scheme_.metrics.frame_padding_y * 2.0f + 4.0f;
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::PushStyleColor(ImGuiCol_WindowBg, to_imvec4(applied_scheme_.palette.surface));
    const bool open = ImGui::BeginViewportSideBar(
        "##ForkEngineTransport", const_cast<ImGuiViewport*>(viewport), ImGuiDir_Up, height,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDecoration);
    ImGui::PopStyleColor();
    if (!open) {
        ImGui::End();
        return;
    }

    // The hairline along the bottom edge.
    {
        const ImVec2 pos = ImGui::GetWindowPos();
        const float y = pos.y + ImGui::GetWindowHeight() - 1.0f;
        ImGui::GetWindowDrawList()->AddLine(ImVec2{pos.x, y}, ImVec2{pos.x + ImGui::GetWindowWidth(), y},
                                            ImGui::GetColorU32(to_imvec4(applied_scheme_.palette.border)));
    }

    const PlaySession& play = context.play();

    // --- Transport ---------------------------------------------------------
    // A filled accent button for the primary action (play), a quiet one to stop,
    // so the eye lands on what the editor wants you to do next.
    if (play.is_playing()) {
        ImGui::PushStyleColor(ImGuiCol_Button, to_imvec4(applied_scheme_.palette.danger));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, to_imvec4(applied_scheme_.palette.danger));
        if (ImGui::Button((std::string(kGlyphStop) + "  Stop").c_str())) {
            context.request_stop_play();
        }
        ImGui::PopStyleColor(2);
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button, to_imvec4(applied_scheme_.palette.accent));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, to_imvec4(applied_scheme_.palette.accent_hovered));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, to_imvec4(applied_scheme_.palette.accent_active));
        // The text on the accent button is the background colour, so it stays
        // legible in both schemes rather than being a fixed dark that fails on the
        // light accent.
        ImGui::PushStyleColor(ImGuiCol_Text, to_imvec4(applied_scheme_.palette.window_bg));
        if (ImGui::Button((std::string(kGlyphPlay) + "  Play").c_str())) {
            context.request_start_play();
        }
        ImGui::PopStyleColor(4);
    }

    ImGui::SameLine();
    ImGui::BeginDisabled(!context.can_undo());
    if (ImGui::Button((std::string(kGlyphUndo) + "  Undo").c_str())) {
        context.undo();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!context.can_redo());
    if (ImGui::Button("Redo")) {
        context.redo();
    }
    ImGui::EndDisabled();

    // --- Viewport mode -----------------------------------------------------
    // A segmented control rather than three loose radios: the three are one
    // choice, so they are drawn as one control.
    ImGui::SameLine();
    ImGui::TextDisabled("View");
    ImGui::SameLine();
    const int mode = context.viewport_mode() == ViewportMode::TwoD     ? 0
                     : context.viewport_mode() == ViewportMode::ThreeD ? 2
                                                                       : 1;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2{0.0f, 0.0f});
    for (int i = 0; i < 3; ++i) {
        const char* label = i == 0 ? "2D" : (i == 1 ? "Auto" : "3D");
        const bool selected = mode == i;
        if (selected) {
            ImGui::PushStyleColor(ImGuiCol_Button, to_imvec4(applied_scheme_.palette.selection_bg));
        }
        if (i > 0) {
            ImGui::SameLine();
        }
        if (ImGui::Button(label)) {
            context.set_viewport_mode(i == 0 ? ViewportMode::TwoD : i == 1 ? ViewportMode::Auto : ViewportMode::ThreeD);
        }
        if (selected) {
            ImGui::PopStyleColor();
        }
    }
    ImGui::PopStyleVar();

    // --- Status ------------------------------------------------------------
    // Right-aligned: the scene identity and its dirty state. `GetContentRegionAvail`
    // after a SameLine gives the width left, so the status sits against the right
    // edge the way a status bar does.
    const std::string scene_name = context.scene().name();
    const std::string path = context.scene_path().empty() ? "(unsaved)" : context.scene_path();
    const std::string status = scene_name + "  -  " + path;
    const float status_width = ImGui::CalcTextSize(status.c_str()).x + 150.0f;
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetWindowWidth() - status_width);
    ImGui::TextDisabled("%s", scene_name.c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("-");
    ImGui::SameLine();
    ImGui::TextDisabled("%s", path.c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    ImGui::TextDisabled("%zu nodes", context.scene().node_count());

    if (context.is_dirty()) {
        ImGui::SameLine();
        ui::badge("modified", applied_scheme_.palette.accent);
    }
    if (play.is_playing()) {
        ImGui::SameLine();
        ui::badge("PLAYING", applied_scheme_.palette.success);
    }

    ImGui::End();
}

void EditorUi::draw_toolbar(EditorContext& context) {
    if (!ImGui::BeginMainMenuBar()) {
        return;
    }

    // The engine's name in the semibold face, as the menu bar's leading mark. It
    // is not a menu; it is the app identity, the way a real editor labels itself.
    if (fonts_.semibold != nullptr) {
        ImGui::PushFont(fonts_.semibold, applied_scheme_.metrics.font_size);
    }
    ImGui::TextUnformatted("Fork Engine");
    if (fonts_.semibold != nullptr) {
        ImGui::PopFont();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("|");

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

    if (ImGui::BeginMenu("View")) {
        // The colour scheme. A radio pair so the current choice is visible, the
        // same idiom the viewport mode uses. The change is applied by
        // apply_theme() on this frame, so it is immediate.
        ImGui::SeparatorText("Theme");
        if (ImGui::MenuItem("Dark", nullptr, context.theme() == EditorTheme::Dark)) {
            context.set_theme(EditorTheme::Dark);
        }
        if (ImGui::MenuItem("Light", nullptr, context.theme() == EditorTheme::Light)) {
            context.set_theme(EditorTheme::Light);
        }
        ImGui::SeparatorText("Density");
        if (ImGui::MenuItem("Comfortable", nullptr, context.density() == EditorDensity::Comfortable)) {
            context.set_density(EditorDensity::Comfortable);
        }
        if (ImGui::MenuItem("Compact", nullptr, context.density() == EditorDensity::Compact)) {
            context.set_density(EditorDensity::Compact);
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Help")) {
        ImGui::MenuItem("Fork Engine Editor", nullptr, false, false);
        ImGui::Separator();
        ImGui::MenuItem("Save Scene: Ctrl+S", nullptr, false, false);
        ImGui::MenuItem("Undo / Redo: Ctrl+Z / Ctrl+Y", nullptr, false, false);
        ImGui::MenuItem("Rename node: F2   Delete node: Del", nullptr, false, false);
        ImGui::EndMenu();
    }

    ImGui::EndMainMenuBar();

    // Keyboard shortcuts, handled outside the menu so they work regardless of
    // which panel has focus. Suppressed while a text field is active so typing
    // does not trigger them. ImGui's Ctrl+Y redo and Ctrl+Shift+Z are both
    // accepted, matching common conventions.
    const ImGuiIO& io = ImGui::GetIO();
    if (!io.WantTextInput && !context.play().is_playing()) {
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
