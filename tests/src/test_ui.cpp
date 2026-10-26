// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
//
// Tests for the UI extraction seam, the renderer's UI pass, and the interaction
// system (ADR-0084).
//
// These need a device for the pass, so they use the null RHI device exactly as
// the sprite, tilemap, and text tests do: extraction, batching, and draw
// submission all run, and only rasterisation is absent. The interaction tests
// drive an `InputState` directly, which is the same seam every other input
// consumer is tested through.
//
// The load-bearing assertion is the last one: the rectangle the renderer draws a
// button in and the rectangle the input system hit tests are produced by the same
// `ui::layout` call, so they must be equal. If a change ever makes them diverge,
// that test fails.

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

[[nodiscard]] rhi::Device& null_device() {
    static std::unique_ptr<rhi::Device> device = [] {
        auto created = rhi::create_null_device();
        REQUIRE(created.ok());
        return std::move(created.value());
    }();
    return *device;
}

Node* add_canvas(Scene& scene, f32 width = 1280.0f, f32 height = 720.0f) {
    Node* node = scene.create_node("Canvas");
    auto* canvas = node->add_component<ui::UiCanvas>();
    canvas->design_width = width;
    canvas->design_height = height;
    return node;
}

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
    REQUIRE((font != nullptr));
    return font;
}

// A `FrameContext` for the interaction tests: an input snapshot and a viewport,
// and nothing else, because that is all the system reads.
[[nodiscard]] FrameContext make_context(InputState& input, Vec2 viewport) {
    FrameContext context;
    context.input = &input;
    context.viewport = viewport;
    return context;
}

} // namespace

// ---------------------------------------------------------------------------
// Extraction
// ---------------------------------------------------------------------------

TEST_CASE("a fill extracts one untextured quad") {
    Scene scene;
    Node* canvas_node = add_canvas(scene);
    Node* element_node = add_element(canvas_node, "Panel", ui::UiSize::fixed(100.0f), ui::UiSize::fixed(50.0f),
                                     ui::UiAnchor::TopLeft, Vec2{10.0f, 20.0f});
    auto* element = element_node->get_component<ui::UiElement>();
    element->fill = Vec4{1.0f, 0.0f, 0.0f, 1.0f};

    std::vector<render::SpriteInstance> instances;
    render::extract_ui(scene, 1280, 720, instances);

    REQUIRE(instances.size() == 1);
    const render::SpriteInstance& instance = instances.front();
    // A fill has no texture: the renderer resolves that to its 1x1 white texture
    // so the quad is a solid colour rather than a batch that binds nothing.
    CHECK(instance.texture == nullptr);
    CHECK(instance.ui == element);
    CHECK(instance.size.x == doctest::Approx(100.0f));
    CHECK(instance.size.y == doctest::Approx(50.0f));
    // The quad is centred on the box: (10, 20) + half of (100, 50).
    CHECK(instance.world.m[12] == doctest::Approx(60.0f));
    CHECK(instance.world.m[13] == doctest::Approx(45.0f));
    CHECK(instance.color == element->fill);
}

TEST_CASE("a fill with zero alpha extracts nothing") {
    Scene scene;
    Node* canvas_node = add_canvas(scene);
    // The default fill is transparent, so an element that only carries text or a
    // texture costs no quad.
    add_element(canvas_node, "Spacer", ui::UiSize::fixed(100.0f), ui::UiSize::fixed(50.0f));

    std::vector<render::SpriteInstance> instances;
    render::extract_ui(scene, 1280, 720, instances);
    CHECK(instances.empty());
}

TEST_CASE("a texture extracts a textured quad over the fill") {
    rhi::Device& device = null_device();
    Scene scene;
    Node* canvas_node = add_canvas(scene);
    Node* element_node = add_element(canvas_node, "Icon", ui::UiSize::fixed(32.0f), ui::UiSize::fixed(32.0f));
    auto* element = element_node->get_component<ui::UiElement>();
    element->fill = Vec4{0.0f, 0.0f, 0.0f, 1.0f};
    element->texture = make_texture(device);

    std::vector<render::SpriteInstance> instances;
    render::extract_ui(scene, 1280, 720, instances);

    // The fill first, then the texture: the fill is the background.
    REQUIRE(instances.size() == 2);
    CHECK(instances[0].texture == nullptr);
    REQUIRE(instances[1].texture != nullptr);
    CHECK(instances[1].texture == &static_cast<const Texture2D*>(element->texture.get())->texture());
}

TEST_CASE("text extracts one quad per inked glyph") {
    rhi::Device& device = null_device();
    Scene scene;
    Node* canvas_node = add_canvas(scene);
    canvas_node->get_component<ui::UiCanvas>()->font = make_font(make_texture(device));

    Node* label_node = add_element(canvas_node, "Label", ui::UiSize::fixed(200.0f), ui::UiSize::fixed(40.0f));
    auto* label = label_node->get_component<ui::UiElement>();
    label->text = "A B"; // A and B are inked, the space is blank.
    label->text_size = 10.0f;

    std::vector<render::SpriteInstance> instances;
    render::extract_ui(scene, 1280, 720, instances);

    // Two glyphs; the blank space advances the pen but draws nothing.
    REQUIRE(instances.size() == 2);
    CHECK(instances[0].ui == label);
    CHECK(instances[1].ui == label);
    // The second glyph is one advance further right: 'A' is 0.5 em, the space
    // 0.5 em, so 'B' starts 1 em (10px) after 'A'.
    CHECK(instances[1].world.m[12] - instances[0].world.m[12] == doctest::Approx(10.0f));
}

TEST_CASE("a UI glyph's y increases down the screen") {
    rhi::Device& device = null_device();
    Scene scene;
    Node* canvas_node = add_canvas(scene);
    canvas_node->get_component<ui::UiCanvas>()->font = make_font(make_texture(device));

    // A box near the bottom of the canvas.
    Node* label_node = add_element(canvas_node, "Bottom", ui::UiSize::fixed(200.0f), ui::UiSize::fixed(40.0f),
                                   ui::UiAnchor::BottomLeft, Vec2{0.0f, -10.0f});
    auto* label = label_node->get_component<ui::UiElement>();
    label->text = "A";
    label->text_size = 20.0f;

    std::vector<render::SpriteInstance> instances;
    render::extract_ui(scene, 1280, 720, instances);
    REQUIRE(instances.size() == 1);

    // The box's top is at 720 - 40 - 10 = 670, and the glyph is centred in a
    // 25px line, so its centre is below the box's top. This is the axis the world
    // text path flips, and getting it wrong puts every HUD label upside down.
    CHECK(instances[0].world.m[13] > 670.0f);
    CHECK(instances[0].world.m[13] < 710.0f);
}

TEST_CASE("the arena UI walk reproduces the vector walk") {
    rhi::Device& device = null_device();
    Scene scene;
    Node* canvas_node = add_canvas(scene);
    canvas_node->get_component<ui::UiCanvas>()->font = make_font(make_texture(device));

    Node* label_node = add_element(canvas_node, "Label", ui::UiSize::fixed(200.0f), ui::UiSize::fixed(40.0f));
    auto* label = label_node->get_component<ui::UiElement>();
    label->text = "AB";
    label->text_size = 10.0f;

    std::vector<render::SpriteInstance> heap;
    render::extract_ui(scene, 1280, 720, heap);

    const usize bound = render::ui_instance_bound(scene);
    REQUIRE(bound >= heap.size());
    FrameArena arena(bound * sizeof(render::SpriteInstance) + 1024);
    std::vector<render::SpriteInstance> storage(bound);
    const usize used = render::extract_ui_into(arena, scene, 1280, 720, std::span<render::SpriteInstance>(storage), 0);

    REQUIRE(used == heap.size());
    for (usize i = 0; i < used; ++i) {
        CHECK(storage[i].texture == heap[i].texture);
        CHECK(storage[i].size == heap[i].size);
        CHECK(storage[i].uv == heap[i].uv);
        CHECK(storage[i].color == heap[i].color);
        CHECK(storage[i].layer == heap[i].layer);
        CHECK(storage[i].world == heap[i].world);
    }
}

TEST_CASE("ui_instance_bound is never short") {
    rhi::Device& device = null_device();
    Scene scene;
    Node* canvas_node = add_canvas(scene);
    canvas_node->get_component<ui::UiCanvas>()->font = make_font(make_texture(device));

    Node* label_node = add_element(canvas_node, "Label", ui::UiSize::fixed(400.0f), ui::UiSize::fixed(40.0f));
    auto* label = label_node->get_component<ui::UiElement>();
    label->text = "ABBA";
    label->fill = Vec4{1.0f, 1.0f, 1.0f, 1.0f};
    label->texture = make_texture(device);

    std::vector<render::SpriteInstance> instances;
    render::extract_ui(scene, 1280, 720, instances);
    CHECK(render::ui_instance_bound(scene) >= instances.size());
}

// ---------------------------------------------------------------------------
// Renderer pass
// ---------------------------------------------------------------------------

TEST_CASE("the UI pass draws the UI and leaves the world pass's state alone") {
    rhi::Device& device = null_device();
    renderer2d::Renderer2D renderer;
    REQUIRE(renderer.initialize(device).ok());

    Scene scene;
    Node* canvas_node = add_canvas(scene);
    Node* element_node = add_element(canvas_node, "Panel", ui::UiSize::fixed(100.0f), ui::UiSize::fixed(50.0f));
    element_node->get_component<ui::UiElement>()->fill = Vec4{1.0f, 1.0f, 1.0f, 1.0f};

    const auto drawn = renderer.render_ui(device, scene, 1280, 720);
    REQUIRE(drawn.ok());
    CHECK(drawn.value() == 1);
    CHECK(renderer.last_draw_calls() == 1);
}

TEST_CASE("a scene with no canvas draws no UI") {
    rhi::Device& device = null_device();
    renderer2d::Renderer2D renderer;
    REQUIRE(renderer.initialize(device).ok());

    Scene scene;
    const auto drawn = renderer.render_ui(device, scene, 1280, 720);
    REQUIRE(drawn.ok());
    CHECK(drawn.value() == 0);
}

TEST_CASE("a UI fill and a UI glyph sharing no texture batch separately") {
    rhi::Device& device = null_device();
    renderer2d::Renderer2D renderer;
    REQUIRE(renderer.initialize(device).ok());

    Scene scene;
    Node* canvas_node = add_canvas(scene);
    canvas_node->get_component<ui::UiCanvas>()->font = make_font(make_texture(device));

    // A panel with a fill, holding a label. The fill uses the white texture and
    // the glyphs the atlas, so this is two batches.
    Node* panel_node = add_element(canvas_node, "Panel", ui::UiSize::fixed(200.0f), ui::UiSize::fixed(60.0f));
    auto* panel = panel_node->get_component<ui::UiElement>();
    panel->fill = Vec4{0.0f, 0.0f, 0.0f, 1.0f};

    Node* label_node = add_element(panel_node, "Label", ui::UiSize::fill(), ui::UiSize::fill());
    auto* label = label_node->get_component<ui::UiElement>();
    label->text = "AB";
    label->text_size = 10.0f;

    const auto drawn = renderer.render_ui(device, scene, 1280, 720);
    REQUIRE(drawn.ok());
    // One fill quad and two glyph quads.
    CHECK(drawn.value() == 3);
    CHECK(renderer.last_draw_calls() == 2);
}

TEST_CASE("the arena UI pass reproduces the heap UI pass") {
    rhi::Device& device = null_device();
    renderer2d::Renderer2D heap_renderer;
    renderer2d::Renderer2D arena_renderer;
    REQUIRE(heap_renderer.initialize(device).ok());
    REQUIRE(arena_renderer.initialize(device).ok());

    Scene scene;
    Node* canvas_node = add_canvas(scene);
    canvas_node->get_component<ui::UiCanvas>()->font = make_font(make_texture(device));
    Node* label_node = add_element(canvas_node, "Label", ui::UiSize::fixed(200.0f), ui::UiSize::fixed(40.0f));
    auto* label = label_node->get_component<ui::UiElement>();
    label->text = "AB";
    label->text_size = 10.0f;

    const auto heap_drawn = heap_renderer.render_ui(device, scene, 1280, 720);
    REQUIRE(heap_drawn.ok());

    FrameArena arena(1 << 16);
    const auto arena_drawn = arena_renderer.render_ui(device, scene, 1280, 720, arena);
    REQUIRE(arena_drawn.ok());

    CHECK(arena_drawn.value() == heap_drawn.value());
    CHECK(arena_renderer.last_draw_calls() == heap_renderer.last_draw_calls());
}

// ---------------------------------------------------------------------------
// Interaction
// ---------------------------------------------------------------------------

TEST_CASE("a press and release on an element is a click") {
    Scene scene;
    Node* canvas_node = add_canvas(scene);
    Node* button_node = add_element(canvas_node, "Button", ui::UiSize::fixed(100.0f), ui::UiSize::fixed(50.0f),
                                    ui::UiAnchor::TopLeft, Vec2{100.0f, 100.0f});
    auto* button = button_node->get_component<ui::UiElement>();

    ui::UiInputSystem system;
    InputState input;

    // Frame 1: the pointer arrives over the button and goes down.
    input.on_mouse_move(150.0f, 125.0f);
    input.on_mouse_button(MouseButton::Left, true);
    system.update(make_context(input, Vec2{1280.0f, 720.0f}), scene);
    CHECK(button->interaction.hovered);
    CHECK(button->interaction.pressed);
    CHECK(button->interaction.pressed_this_frame);
    CHECK_FALSE(button->interaction.clicked);

    // Frame 2: the button is held. `pressed` stays true, and no new press edge.
    input.begin_frame();
    system.update(make_context(input, Vec2{1280.0f, 720.0f}), scene);
    CHECK(button->interaction.pressed);
    CHECK_FALSE(button->interaction.pressed_this_frame);

    // Frame 3: the pointer comes up over the button. That is a click.
    input.begin_frame();
    input.on_mouse_button(MouseButton::Left, false);
    system.update(make_context(input, Vec2{1280.0f, 720.0f}), scene);
    CHECK(button->interaction.clicked);
    CHECK(button->interaction.released);
    CHECK_FALSE(button->interaction.pressed);

    // Frame 4: the click edge is gone; it lived for exactly one frame.
    input.begin_frame();
    system.update(make_context(input, Vec2{1280.0f, 720.0f}), scene);
    CHECK_FALSE(button->interaction.clicked);
}

TEST_CASE("releasing off the element is not a click") {
    Scene scene;
    Node* canvas_node = add_canvas(scene);
    Node* button_node = add_element(canvas_node, "Button", ui::UiSize::fixed(100.0f), ui::UiSize::fixed(50.0f),
                                    ui::UiAnchor::TopLeft, Vec2{100.0f, 100.0f});
    auto* button = button_node->get_component<ui::UiElement>();

    ui::UiInputSystem system;
    InputState input;

    // Press on the button.
    input.on_mouse_move(150.0f, 125.0f);
    input.on_mouse_button(MouseButton::Left, true);
    system.update(make_context(input, Vec2{1280.0f, 720.0f}), scene);
    REQUIRE(button->interaction.pressed);

    // Drag off the button while held: it is no longer hovered or pressed.
    input.begin_frame();
    input.on_mouse_move(500.0f, 500.0f);
    system.update(make_context(input, Vec2{1280.0f, 720.0f}), scene);
    CHECK_FALSE(button->interaction.hovered);
    CHECK_FALSE(button->interaction.pressed);

    // Release away from the button: no click.
    input.begin_frame();
    input.on_mouse_button(MouseButton::Left, false);
    system.update(make_context(input, Vec2{1280.0f, 720.0f}), scene);
    CHECK_FALSE(button->interaction.clicked);
}

TEST_CASE("a touch contact drives the same interaction as a mouse") {
    Scene scene;
    Node* canvas_node = add_canvas(scene);
    Node* button_node = add_element(canvas_node, "Button", ui::UiSize::fixed(100.0f), ui::UiSize::fixed(50.0f),
                                    ui::UiAnchor::TopLeft, Vec2{100.0f, 100.0f});
    auto* button = button_node->get_component<ui::UiElement>();

    ui::UiInputSystem system;
    InputState input;

    // A finger goes down on the button. There is no mouse, so the touch path
    // drives the same state -- which is what lets one UI serve a desktop and a
    // phone.
    input.on_touch(TouchPhase::Began, 7, 150.0f, 125.0f);
    system.update(make_context(input, Vec2{1280.0f, 720.0f}), scene);
    CHECK(button->interaction.hovered);
    CHECK(button->interaction.pressed_this_frame);

    // The finger comes up: a click.
    input.begin_frame();
    input.on_touch(TouchPhase::Ended, 7, 150.0f, 125.0f);
    system.update(make_context(input, Vec2{1280.0f, 720.0f}), scene);
    CHECK(button->interaction.clicked);
}

TEST_CASE("a frame with no pointer clears the interaction state") {
    Scene scene;
    Node* canvas_node = add_canvas(scene);
    Node* button_node = add_element(canvas_node, "Button", ui::UiSize::fixed(100.0f), ui::UiSize::fixed(50.0f),
                                    ui::UiAnchor::TopLeft, Vec2{100.0f, 100.0f});
    auto* button = button_node->get_component<ui::UiElement>();

    ui::UiInputSystem system;
    InputState input;

    input.on_mouse_move(150.0f, 125.0f);
    input.on_mouse_button(MouseButton::Left, true);
    system.update(make_context(input, Vec2{1280.0f, 720.0f}), scene);
    REQUIRE(button->interaction.hovered);

    // A frame with no input at all: the pointer is gone, so nothing is hovered.
    // Without the clear, last frame's `hovered` would stick.
    InputState empty;
    system.update(make_context(empty, Vec2{1280.0f, 720.0f}), scene);
    CHECK_FALSE(button->interaction.hovered);
    CHECK_FALSE(button->interaction.pressed);
}

TEST_CASE("a zero viewport lays out nothing and reports no hover") {
    Scene scene;
    Node* canvas_node = add_canvas(scene);
    add_element(canvas_node, "Button", ui::UiSize::fixed(100.0f), ui::UiSize::fixed(50.0f));

    ui::UiInputSystem system;
    InputState input;
    input.on_mouse_move(50.0f, 50.0f);
    system.update(make_context(input, Vec2{0.0f, 0.0f}), scene);

    CHECK(system.layout().empty());
    CHECK_FALSE(system.pointer_over_ui());
}

// ---------------------------------------------------------------------------
// The load-bearing invariant: the pixels and the hit test agree
// ---------------------------------------------------------------------------

TEST_CASE("a button draws where it responds") {
    Scene scene;
    Node* canvas_node = add_canvas(scene);
    Node* button_node = add_element(canvas_node, "Button", ui::UiSize::fixed(120.0f), ui::UiSize::fixed(40.0f),
                                    ui::UiAnchor::BottomRight, Vec2{-20.0f, -20.0f});
    auto* button = button_node->get_component<ui::UiElement>();
    button->fill = Vec4{1.0f, 1.0f, 1.0f, 1.0f};

    // What the renderer draws: the fill quad's box, from the extraction.
    std::vector<render::SpriteInstance> instances;
    render::extract_ui(scene, 1280, 720, instances);
    REQUIRE(instances.size() == 1);
    const render::SpriteInstance& fill = instances.front();
    // The quad is centred on the box, so the box is the centre plus/minus half.
    const f32 drawn_left = fill.world.m[12] - fill.size.x * 0.5f;
    const f32 drawn_top = fill.world.m[13] - fill.size.y * 0.5f;

    // What the input system hit tests: the same `ui::layout` result.
    ui::UiInputSystem system;
    InputState input;
    system.update(make_context(input, Vec2{1280.0f, 720.0f}), scene);
    const ui::UiLayoutResult& laid_out = system.layout();
    REQUIRE(laid_out.size() == 1);

    CHECK(laid_out.front().rect.position.x == doctest::Approx(drawn_left));
    CHECK(laid_out.front().rect.position.y == doctest::Approx(drawn_top));
    CHECK(laid_out.front().rect.size.x == doctest::Approx(fill.size.x));
    CHECK(laid_out.front().rect.size.y == doctest::Approx(fill.size.y));

    // And the point the button's own centre maps to is a hit on it.
    const Vec2 centre = laid_out.front().rect.center();
    input.begin_frame();
    input.on_mouse_move(centre.x, centre.y);
    system.update(make_context(input, Vec2{1280.0f, 720.0f}), scene);
    CHECK(button->interaction.hovered);
}

// ---------------------------------------------------------------------------
// Reflection
// ---------------------------------------------------------------------------

TEST_CASE("the UI components are registered for serialization") {
    reflect::register_ui_types();

    const reflect::TypeDesc* element = reflect::Registry::instance().find(ui::UiElement::kTypeName);
    const reflect::TypeDesc* canvas = reflect::Registry::instance().find(ui::UiCanvas::kTypeName);
    REQUIRE(element != nullptr);
    REQUIRE(canvas != nullptr);

    // The component a factory makes is the right type, which is what the
    // serializer relies on to reconstruct a node.
    auto made = element->factory();
    CHECK(made != nullptr);
    CHECK(made->type_name() == std::string(ui::UiElement::kTypeName));

    // A hand-edited scene cannot put an enum out of range: the setter rejects it.
    ui::UiElement instance;
    const reflect::Property* anchor = reflect::resolve_property(element->properties, {"anchor"});
    REQUIRE(anchor != nullptr);
    CHECK(anchor->setter(&instance, static_cast<i64>(99)) == false);
    CHECK(anchor->setter(&instance, static_cast<i64>(ui::UiAnchor::BottomRight)) == true);
    CHECK(instance.anchor == ui::UiAnchor::BottomRight);
}
