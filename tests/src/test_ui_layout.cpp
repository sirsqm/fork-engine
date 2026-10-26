// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
//
// Tests for the in-game UI system (ADR-0084).
//
// The layout solver and the hit test are pure functions of a scene and a
// viewport, so most of this runs with no display, no GL context, and no window.
// The extraction and the renderer pass need a device, so they use the null RHI
// device exactly as the sprite, tilemap, and text tests do.
//
// The property that matters most, and the one several of these tests exist to
// pin down, is that the pixels and the hit test agree: `ui::layout` is called by
// both `render::extract_ui` and `UiInputSystem`, so a button must draw where it
// responds. The tests assert on the *same* resolved rectangle through both
// paths.

#include <doctest/doctest.h>

#include "fe/core/FrameArena.h"
#include "fe/platform/InputState.h"
#include "fe/reflect/Registry.h"
#include "fe/render/RenderExtract.h"
#include "fe/renderer2d/Renderer2D.h"
#include "fe/resources/Font.h"
#include "fe/resources/Texture2D.h"
#include "fe/rhi/Device.h"
#include "fe/rhi/Resources.h"
#include "fe/rhi/Types.h"
#include "fe/scene/Node.h"
#include "fe/scene/Scene.h"
#include "fe/systems/FrameContext.h"
#include "fe/ui/UiElement.h"
#include "fe/ui/UiInputSystem.h"
#include "fe/ui/UiLayout.h"
#include "fe/ui/UiTypes.h"
#include "fe/ui/UiTypesReflect.h"

#include <memory>
#include <string>
#include <vector>

using namespace fe;

namespace {

// The null RHI device: no display, no GL context. Extraction is pure CPU work,
// so this is the whole graphics environment these tests need.
[[nodiscard]] rhi::Device& null_device() {
    static std::unique_ptr<rhi::Device> device = [] {
        auto created = rhi::create_null_device();
        REQUIRE(created.ok());
        return std::move(created.value());
    }();
    return *device;
}

// A canvas node with a `UiCanvas`, appended to `scene`.
Node* add_canvas(Scene& scene, f32 width = 1280.0f, f32 height = 720.0f) {
    Node* node = scene.create_node("Canvas");
    auto* canvas = node->add_component<ui::UiCanvas>();
    canvas->design_width = width;
    canvas->design_height = height;
    return node;
}

// A child element of `parent` with the given size and anchor.
Node* add_element(Node* parent, std::string name, ui::UiSize width, ui::UiSize height,
                  ui::UiAnchor anchor = ui::UiAnchor::TopLeft, Vec2 offset = {}) {
    Node* node = parent->create_child<Node>(std::move(name));
    auto* element = node->add_component<ui::UiElement>();
    element->width = width;
    element->height = height;
    element->anchor = anchor;
    element->offset = offset;
    return node;
}

// The one resolved element whose component matches, or nullptr. A test that
// looks up its own element rather than indexing the array stays correct if the
// array order changes.
[[nodiscard]] const ui::UiResolved* find_resolved(const ui::UiLayoutResult& result, const ui::UiElement* element) {
    for (const ui::UiResolved& resolved : result) {
        if (resolved.element == element) {
            return &resolved;
        }
    }
    return nullptr;
}

// A `Texture2D` on the null device, for the atlas and texture cases.
[[nodiscard]] std::shared_ptr<Texture2D> make_texture(rhi::Device& device, u32 size = 16) {
    std::vector<u8> pixels(size * size * 4, 255);
    rhi::TextureDesc desc;
    desc.name = "ui-test-texture";
    desc.width = size;
    desc.height = size;
    desc.pixels = pixels.data();
    auto texture = device.create_texture(desc);
    REQUIRE(texture.ok());
    return std::make_shared<Texture2D>("tex:ui-test.png", texture.value());
}

// A font over `atlas`, with 'A' and 'B' inked, ' ' blank, and a line height of
// 1.25 em. The same document the text tests use, so the two agree on metrics.
[[nodiscard]] std::shared_ptr<Font> make_font(const std::shared_ptr<Texture2D>& atlas) {
    std::string glyphs;
    for (int code = 0; code <= 'B'; ++code) {
        if (code > 0) {
            glyphs += ",";
        }
        if (code == ' ') {
            glyphs +=
                R"({"u0":0.0,"v0":0.0,"u1":0.0,"v1":0.0,"advance":0.5,"bearing_x":0.0,"bearing_y":0.0,"width":0.0,"height":0.0})";
        } else if (code == 'A' || code == 'B') {
            glyphs +=
                R"({"u0":0.0,"v0":0.0,"u1":0.5,"v1":0.5,"advance":0.5,"bearing_x":0.0,"bearing_y":0.0,"width":0.5,"height":1.0})";
        } else {
            glyphs += "null";
        }
    }
    const std::string metrics = R"({"atlas":"ui-atlas.png","line_height":1.25,"glyphs":[)" + glyphs + "]}";
    std::string error;
    auto font = Font::from_json("ui.fefont", metrics, atlas, error);
    // The extra parens stop MSVC's doctest from stringifying the shared_ptr and
    // reaching for Font's operator<<, which does not exist (see AGENTS.md).
    REQUIRE((font != nullptr));
    return font;
}

} // namespace

// ---------------------------------------------------------------------------
// Canvas transform
// ---------------------------------------------------------------------------

TEST_CASE("a Fit canvas scales the design to fit and centres it") {
    ui::UiCanvas canvas;
    canvas.design_width = 1280.0f;
    canvas.design_height = 720.0f;

    // A viewport twice the design: the uniform scale is 2 on both axes, so the
    // design exactly fills the viewport and there is no margin.
    const ui::UiCanvasTransform exact = ui::canvas_transform(canvas, 2560, 1440);
    CHECK(exact.scale.x == doctest::Approx(2.0f));
    CHECK(exact.scale.y == doctest::Approx(2.0f));
    CHECK(exact.offset.x == doctest::Approx(0.0f));
    CHECK(exact.offset.y == doctest::Approx(0.0f));

    // A wider viewport: the fit is bounded by height, so the scale is 1 and the
    // extra width becomes equal margins on either side.
    const ui::UiCanvasTransform wide = ui::canvas_transform(canvas, 2560, 720);
    CHECK(wide.scale.x == doctest::Approx(1.0f));
    CHECK(wide.scale.y == doctest::Approx(1.0f));
    CHECK(wide.offset.x == doctest::Approx(640.0f)); // (2560 - 1280) / 2
    CHECK(wide.offset.y == doctest::Approx(0.0f));
}

TEST_CASE("a Stretch canvas fills the viewport with no margin") {
    ui::UiCanvas canvas;
    canvas.design_width = 1280.0f;
    canvas.design_height = 720.0f;
    canvas.scale_mode = ui::UiScaleMode::Stretch;

    const ui::UiCanvasTransform transform = ui::canvas_transform(canvas, 2560, 720);
    CHECK(transform.scale.x == doctest::Approx(2.0f));
    CHECK(transform.scale.y == doctest::Approx(1.0f));
    CHECK(transform.offset.x == doctest::Approx(0.0f));
    CHECK(transform.offset.y == doctest::Approx(0.0f));
}

TEST_CASE("a canvas with a degenerate design size yields the identity") {
    ui::UiCanvas canvas;
    canvas.design_width = 0.0f;

    // No division by zero, and a predictable mapping: the design at 1:1 from the
    // viewport origin.
    const ui::UiCanvasTransform transform = ui::canvas_transform(canvas, 1280, 720);
    CHECK(transform.scale.x == doctest::Approx(1.0f));
    CHECK(transform.scale.y == doctest::Approx(1.0f));
    CHECK(transform.offset.x == doctest::Approx(0.0f));
}

TEST_CASE("the canvas transform round-trips a point") {
    ui::UiCanvas canvas;
    canvas.design_width = 1280.0f;
    canvas.design_height = 720.0f;

    const ui::UiCanvasTransform transform = ui::canvas_transform(canvas, 1920, 1080);
    const Vec2 design_point{100.0f, 50.0f};
    const Vec2 viewport_point = transform.to_viewport(design_point);
    const Vec2 back = transform.to_design(viewport_point);
    CHECK(back.x == doctest::Approx(design_point.x));
    CHECK(back.y == doctest::Approx(design_point.y));
}

// ---------------------------------------------------------------------------
// Layout: anchoring
// ---------------------------------------------------------------------------

TEST_CASE("an anchored element is placed against the canvas corner") {
    Scene scene;
    Node* canvas_node = add_canvas(scene);

    // A 100x50 box pinned to the top-left, 20px in from each edge.
    Node* element_node = add_element(canvas_node, "Score", ui::UiSize::fixed(100.0f), ui::UiSize::fixed(50.0f),
                                     ui::UiAnchor::TopLeft, Vec2{20.0f, 20.0f});
    auto* element = element_node->get_component<ui::UiElement>();

    // The viewport matches the design, so design units and pixels are the same.
    ui::UiLayoutResult result;
    ui::layout(scene, 1280, 720, result);

    const ui::UiResolved* resolved = find_resolved(result, element);
    REQUIRE(resolved != nullptr);
    CHECK(resolved->rect.position.x == doctest::Approx(20.0f));
    CHECK(resolved->rect.position.y == doctest::Approx(20.0f));
    CHECK(resolved->rect.size.x == doctest::Approx(100.0f));
    CHECK(resolved->rect.size.y == doctest::Approx(50.0f));

    // The same box pinned to the top-right, nudged inward: `offset` is a screen
    // nudge, so moving away from the right edge is a negative x. Its right edge
    // then sits 20px from the canvas's right edge, which is what "pin to a
    // corner" has to mean.
    element->anchor = ui::UiAnchor::TopRight;
    element->offset = Vec2{-20.0f, 20.0f};
    ui::layout(scene, 1280, 720, result);
    resolved = find_resolved(result, element);
    REQUIRE(resolved != nullptr);
    CHECK(resolved->rect.right() == doctest::Approx(1280.0f - 20.0f));
    CHECK(resolved->rect.position.y == doctest::Approx(20.0f));
}

TEST_CASE("the centre anchor centres an element on its parent") {
    Scene scene;
    Node* canvas_node = add_canvas(scene);

    Node* element_node = add_element(canvas_node, "Title", ui::UiSize::fixed(200.0f), ui::UiSize::fixed(40.0f),
                                     ui::UiAnchor::MiddleCenter);
    const auto* element = element_node->get_component<ui::UiElement>();

    ui::UiLayoutResult result;
    ui::layout(scene, 1280, 720, result);

    const ui::UiResolved* resolved = find_resolved(result, element);
    REQUIRE(resolved != nullptr);
    // The element's centre is the canvas's centre.
    CHECK(resolved->rect.center().x == doctest::Approx(640.0f));
    CHECK(resolved->rect.center().y == doctest::Approx(360.0f));
}

// ---------------------------------------------------------------------------
// Layout: sizing
// ---------------------------------------------------------------------------

TEST_CASE("Fill sizes an element to its parent's content box") {
    Scene scene;
    Node* canvas_node = add_canvas(scene);

    // A panel that fills the canvas, with 32px padding all round.
    Node* panel_node = add_element(canvas_node, "Panel", ui::UiSize::fill(), ui::UiSize::fill());
    auto* panel = panel_node->get_component<ui::UiElement>();
    panel->padding = ui::EdgeInsets::all(32.0f);

    // A child that fills the panel's content box.
    Node* child_node = add_element(panel_node, "Child", ui::UiSize::fill(), ui::UiSize::fill());
    const auto* child = child_node->get_component<ui::UiElement>();

    ui::UiLayoutResult result;
    ui::layout(scene, 1280, 720, result);

    const ui::UiResolved* resolved = find_resolved(result, child);
    REQUIRE(resolved != nullptr);
    // The child fills the panel inside its padding: 1280 - 64 wide, 720 - 64 tall.
    CHECK(resolved->rect.position.x == doctest::Approx(32.0f));
    CHECK(resolved->rect.position.y == doctest::Approx(32.0f));
    CHECK(resolved->rect.size.x == doctest::Approx(1280.0f - 64.0f));
    CHECK(resolved->rect.size.y == doctest::Approx(720.0f - 64.0f));
}

TEST_CASE("Percent sizes an element as a fraction of its parent") {
    Scene scene;
    Node* canvas_node = add_canvas(scene);

    Node* element_node = add_element(canvas_node, "Half", ui::UiSize::percent(0.5f), ui::UiSize::percent(0.25f));
    const auto* element = element_node->get_component<ui::UiElement>();

    ui::UiLayoutResult result;
    ui::layout(scene, 1280, 720, result);

    const ui::UiResolved* resolved = find_resolved(result, element);
    REQUIRE(resolved != nullptr);
    CHECK(resolved->rect.size.x == doctest::Approx(640.0f));
    CHECK(resolved->rect.size.y == doctest::Approx(180.0f));
}

// ---------------------------------------------------------------------------
// Layout: flow
// ---------------------------------------------------------------------------

TEST_CASE("a Column stacks its children with a gap") {
    Scene scene;
    Node* canvas_node = add_canvas(scene);

    Node* menu_node =
        add_element(canvas_node, "Menu", ui::UiSize::fixed(200.0f), ui::UiSize::content(), ui::UiAnchor::MiddleCenter);
    auto* menu = menu_node->get_component<ui::UiElement>();
    menu->direction = ui::UiDirection::Column;
    menu->gap = 10.0f;

    Node* first_node = add_element(menu_node, "First", ui::UiSize::fill(), ui::UiSize::fixed(40.0f));
    Node* second_node = add_element(menu_node, "Second", ui::UiSize::fill(), ui::UiSize::fixed(40.0f));
    const auto* first = first_node->get_component<ui::UiElement>();
    const auto* second = second_node->get_component<ui::UiElement>();

    ui::UiLayoutResult result;
    ui::layout(scene, 1280, 720, result);

    const ui::UiResolved* first_resolved = find_resolved(result, first);
    const ui::UiResolved* second_resolved = find_resolved(result, second);
    REQUIRE(first_resolved != nullptr);
    REQUIRE(second_resolved != nullptr);

    // The second child sits one 40px row plus the 10px gap below the first.
    CHECK(second_resolved->rect.position.y == doctest::Approx(first_resolved->rect.position.y + 50.0f));
    // `fill` across a column's cross axis makes each child the menu's width.
    CHECK(first_resolved->rect.size.x == doctest::Approx(200.0f));
    CHECK(second_resolved->rect.size.x == doctest::Approx(200.0f));
}

TEST_CASE("a Column sized to content is as tall as its children plus the gaps") {
    Scene scene;
    Node* canvas_node = add_canvas(scene);

    Node* menu_node = add_element(canvas_node, "Menu", ui::UiSize::fixed(200.0f), ui::UiSize::content());
    auto* menu = menu_node->get_component<ui::UiElement>();
    menu->direction = ui::UiDirection::Column;
    menu->gap = 10.0f;

    add_element(menu_node, "First", ui::UiSize::fill(), ui::UiSize::fixed(40.0f));
    add_element(menu_node, "Second", ui::UiSize::fill(), ui::UiSize::fixed(40.0f));
    add_element(menu_node, "Third", ui::UiSize::fill(), ui::UiSize::fixed(40.0f));
    const auto* menu_element = menu_node->get_component<ui::UiElement>();

    ui::UiLayoutResult result;
    ui::layout(scene, 1280, 720, result);

    const ui::UiResolved* resolved = find_resolved(result, menu_element);
    REQUIRE(resolved != nullptr);
    // Three 40px rows and two 10px gaps.
    CHECK(resolved->rect.size.y == doctest::Approx(140.0f));
}

TEST_CASE("SpaceBetween spreads a Row's children to the edges") {
    Scene scene;
    Node* canvas_node = add_canvas(scene);

    Node* bar_node = add_element(canvas_node, "Bar", ui::UiSize::fixed(400.0f), ui::UiSize::fixed(50.0f));
    auto* bar = bar_node->get_component<ui::UiElement>();
    bar->direction = ui::UiDirection::Row;
    bar->justify = ui::UiJustify::SpaceBetween;

    Node* left_node = add_element(bar_node, "Left", ui::UiSize::fixed(80.0f), ui::UiSize::fixed(40.0f));
    Node* right_node = add_element(bar_node, "Right", ui::UiSize::fixed(80.0f), ui::UiSize::fixed(40.0f));
    const auto* left = left_node->get_component<ui::UiElement>();
    const auto* right = right_node->get_component<ui::UiElement>();

    ui::UiLayoutResult result;
    ui::layout(scene, 1280, 720, result);

    const ui::UiResolved* left_resolved = find_resolved(result, left);
    const ui::UiResolved* right_resolved = find_resolved(result, right);
    REQUIRE(left_resolved != nullptr);
    REQUIRE(right_resolved != nullptr);
    // The first child is at the start edge and the last child's right edge is at
    // the bar's right edge, with the free space between them.
    CHECK(left_resolved->rect.position.x == doctest::Approx(0.0f));
    CHECK(right_resolved->rect.right() == doctest::Approx(400.0f));
}

TEST_CASE("Stretch makes a Row's children the full cross extent") {
    Scene scene;
    Node* canvas_node = add_canvas(scene);

    Node* bar_node = add_element(canvas_node, "Bar", ui::UiSize::fixed(400.0f), ui::UiSize::fixed(60.0f));
    auto* bar = bar_node->get_component<ui::UiElement>();
    bar->direction = ui::UiDirection::Row;
    bar->align = ui::UiAlign::Stretch;

    Node* child_node = add_element(bar_node, "Button", ui::UiSize::fixed(100.0f), ui::UiSize::fixed(10.0f));
    const auto* child = child_node->get_component<ui::UiElement>();

    ui::UiLayoutResult result;
    ui::layout(scene, 1280, 720, result);

    const ui::UiResolved* resolved = find_resolved(result, child);
    REQUIRE(resolved != nullptr);
    // The child's authored 10px height is overridden by the stretch.
    CHECK(resolved->rect.size.y == doctest::Approx(60.0f));
    CHECK(resolved->rect.size.x == doctest::Approx(100.0f));
}

// ---------------------------------------------------------------------------
// Layout: content, visibility, and draw order
// ---------------------------------------------------------------------------

TEST_CASE("a Content element sizes to its text") {
    rhi::Device& device = null_device();
    Scene scene;
    Node* canvas_node = add_canvas(scene);
    canvas_node->get_component<ui::UiCanvas>()->font = make_font(make_texture(device));

    Node* label_node = add_element(canvas_node, "Label", ui::UiSize::content(), ui::UiSize::content());
    auto* label = label_node->get_component<ui::UiElement>();
    label->text = "AB";
    label->text_size = 20.0f;

    ui::UiLayoutResult result;
    ui::layout(scene, 1280, 720, result);

    const ui::UiResolved* resolved = find_resolved(result, label);
    REQUIRE(resolved != nullptr);
    // Each glyph advances 0.5 em, so "AB" is one em wide: 20px at size 20. The
    // line height is 1.25 em: 25px.
    CHECK(resolved->rect.size.x == doctest::Approx(20.0f));
    CHECK(resolved->rect.size.y == doctest::Approx(25.0f));
}

TEST_CASE("an element inherits the canvas's text size when it sets none") {
    rhi::Device& device = null_device();
    Scene scene;
    Node* canvas_node = add_canvas(scene);
    auto* canvas = canvas_node->get_component<ui::UiCanvas>();
    canvas->font = make_font(make_texture(device));
    canvas->text_size = 40.0f;

    // The element leaves `text_size` at its 0 default, so it must measure at the
    // canvas's 40, not at some per-element fallback.
    Node* label_node = add_element(canvas_node, "Label", ui::UiSize::content(), ui::UiSize::content());
    auto* label = label_node->get_component<ui::UiElement>();
    label->text = "AB";
    REQUIRE(label->text_size == 0.0f);

    ui::UiLayoutResult result;
    ui::layout(scene, 1280, 720, result);

    const ui::UiResolved* resolved = find_resolved(result, label);
    REQUIRE(resolved != nullptr);
    // "AB" is one em wide, the line height 1.25 em: 40px and 50px.
    CHECK(resolved->rect.size.x == doctest::Approx(40.0f));
    CHECK(resolved->rect.size.y == doctest::Approx(50.0f));

    // An explicit size on the element wins over the canvas's.
    label->text_size = 20.0f;
    ui::layout(scene, 1280, 720, result);
    resolved = find_resolved(result, label);
    REQUIRE(resolved != nullptr);
    CHECK(resolved->rect.size.x == doctest::Approx(20.0f));
}

TEST_CASE("an invisible element and its subtree are skipped") {
    Scene scene;
    Node* canvas_node = add_canvas(scene);

    Node* menu_node = add_element(canvas_node, "Menu", ui::UiSize::fixed(200.0f), ui::UiSize::fixed(200.0f));
    auto* menu = menu_node->get_component<ui::UiElement>();
    Node* item_node = add_element(menu_node, "Item", ui::UiSize::fixed(100.0f), ui::UiSize::fixed(40.0f));
    const auto* item = item_node->get_component<ui::UiElement>();

    ui::UiLayoutResult result;
    ui::layout(scene, 1280, 720, result);
    CHECK(find_resolved(result, item) != nullptr);

    // Hiding the parent hides the child: a hidden menu costs nothing, which is
    // what makes "hide a menu" a single flag rather than a tree walk.
    menu->visible = false;
    ui::layout(scene, 1280, 720, result);
    CHECK(find_resolved(result, item) == nullptr);
}

TEST_CASE("a hidden canvas draws nothing") {
    Scene scene;
    Node* canvas_node = add_canvas(scene);
    add_element(canvas_node, "Score", ui::UiSize::fixed(100.0f), ui::UiSize::fixed(50.0f));

    ui::UiLayoutResult result;
    ui::layout(scene, 1280, 720, result);
    CHECK(result.size() == 1);

    canvas_node->get_component<ui::UiCanvas>()->visible = false;
    ui::layout(scene, 1280, 720, result);
    CHECK(result.empty());
}

TEST_CASE("layout emits a parent before its children") {
    Scene scene;
    Node* canvas_node = add_canvas(scene);

    Node* parent_node = add_element(canvas_node, "Parent", ui::UiSize::fixed(200.0f), ui::UiSize::fixed(200.0f));
    Node* child_node = add_element(parent_node, "Child", ui::UiSize::fixed(50.0f), ui::UiSize::fixed(50.0f));
    const auto* parent = parent_node->get_component<ui::UiElement>();
    const auto* child = child_node->get_component<ui::UiElement>();

    ui::UiLayoutResult result;
    ui::layout(scene, 1280, 720, result);
    REQUIRE(result.size() == 2);
    // Draw order is array order, so the parent must come first or the child would
    // be painted behind it.
    CHECK(result[0].element == parent);
    CHECK(result[1].element == child);
    CHECK(result[1].depth == result[0].depth + 1);
}

TEST_CASE("an element's layer is its canvas's layer plus its own") {
    Scene scene;
    Node* canvas_node = add_canvas(scene);
    canvas_node->get_component<ui::UiCanvas>()->layer = 100;

    Node* element_node = add_element(canvas_node, "Popup", ui::UiSize::fixed(10.0f), ui::UiSize::fixed(10.0f));
    element_node->get_component<ui::UiElement>()->layer = 5;
    const auto* element = element_node->get_component<ui::UiElement>();

    ui::UiLayoutResult result;
    ui::layout(scene, 1280, 720, result);

    const ui::UiResolved* resolved = find_resolved(result, element);
    REQUIRE(resolved != nullptr);
    CHECK(resolved->layer == 105);
}

TEST_CASE("layout maps design units to viewport pixels") {
    Scene scene;
    Node* canvas_node = add_canvas(scene, 1280.0f, 720.0f);

    // 100 design pixels at the top-left, in a viewport twice the design size, is
    // 200 viewport pixels.
    Node* element_node = add_element(canvas_node, "Score", ui::UiSize::fixed(100.0f), ui::UiSize::fixed(50.0f),
                                     ui::UiAnchor::TopLeft, Vec2{10.0f, 10.0f});
    const auto* element = element_node->get_component<ui::UiElement>();

    ui::UiLayoutResult result;
    ui::layout(scene, 2560, 1440, result);

    const ui::UiResolved* resolved = find_resolved(result, element);
    REQUIRE(resolved != nullptr);
    CHECK(resolved->design_rect.position.x == doctest::Approx(10.0f));
    CHECK(resolved->rect.position.x == doctest::Approx(20.0f));
    CHECK(resolved->rect.size.x == doctest::Approx(200.0f));
}

// ---------------------------------------------------------------------------
// Hit testing
// ---------------------------------------------------------------------------

TEST_CASE("hit testing finds the element under a point") {
    Scene scene;
    Node* canvas_node = add_canvas(scene);

    Node* element_node = add_element(canvas_node, "Button", ui::UiSize::fixed(100.0f), ui::UiSize::fixed(50.0f),
                                     ui::UiAnchor::TopLeft, Vec2{100.0f, 100.0f});
    const auto* element = element_node->get_component<ui::UiElement>();

    ui::UiLayoutResult result;
    ui::layout(scene, 1280, 720, result);

    // Inside.
    CHECK(ui::hit_test(result, Vec2{150.0f, 125.0f}) != nullptr);
    // Outside on each side.
    CHECK(ui::hit_test(result, Vec2{99.0f, 125.0f}) == nullptr);
    CHECK(ui::hit_test(result, Vec2{150.0f, 99.0f}) == nullptr);
    // The far edges are half-open, so a point exactly on the right edge is not
    // inside: two abutting elements never both claim a boundary pixel.
    CHECK(ui::hit_test(result, Vec2{200.0f, 125.0f}) == nullptr);
    CHECK(ui::hit_test(result, Vec2{150.0f, 150.0f}) == nullptr);
    (void)element;
}

TEST_CASE("hit testing prefers the topmost element") {
    Scene scene;
    Node* canvas_node = add_canvas(scene);

    // Two overlapping elements; the second is later in the tree, so it draws on
    // top and must win the hit test.
    Node* under_node = add_element(canvas_node, "Under", ui::UiSize::fixed(200.0f), ui::UiSize::fixed(200.0f));
    Node* over_node = add_element(canvas_node, "Over", ui::UiSize::fixed(200.0f), ui::UiSize::fixed(200.0f));
    const auto* under = under_node->get_component<ui::UiElement>();
    const auto* over = over_node->get_component<ui::UiElement>();

    ui::UiLayoutResult result;
    ui::layout(scene, 1280, 720, result);

    const ui::UiResolved* hit = ui::hit_test(result, Vec2{100.0f, 100.0f});
    REQUIRE(hit != nullptr);
    CHECK(hit->element == over);
    (void)under;
}

TEST_CASE("a non-interactable element is skipped by the hit test") {
    Scene scene;
    Node* canvas_node = add_canvas(scene);

    Node* over_node = add_element(canvas_node, "Over", ui::UiSize::fixed(200.0f), ui::UiSize::fixed(200.0f));
    Node* under_node = add_element(canvas_node, "Under", ui::UiSize::fixed(200.0f), ui::UiSize::fixed(200.0f));
    auto* over = over_node->get_component<ui::UiElement>();
    const auto* under = under_node->get_component<ui::UiElement>();
    // The topmost element does not take clicks, so a click falls through to the
    // element behind it. A disabled button is the case.
    over->interactable = false;

    ui::UiLayoutResult result;
    ui::layout(scene, 1280, 720, result);

    const ui::UiResolved* hit = ui::hit_test(result, Vec2{100.0f, 100.0f});
    REQUIRE(hit != nullptr);
    CHECK(hit->element == under);
}

TEST_CASE("pointer_over_ui answers whether a point is on any element") {
    Scene scene;
    Node* canvas_node = add_canvas(scene);
    add_element(canvas_node, "Button", ui::UiSize::fixed(100.0f), ui::UiSize::fixed(50.0f), ui::UiAnchor::TopLeft,
                Vec2{10.0f, 10.0f});

    ui::UiLayoutResult result;
    ui::layout(scene, 1280, 720, result);

    CHECK(ui::pointer_over_ui(result, Vec2{50.0f, 30.0f}));
    CHECK_FALSE(ui::pointer_over_ui(result, Vec2{500.0f, 500.0f}));
}
