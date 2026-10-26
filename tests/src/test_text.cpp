// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
//
// Tests for in-engine text rendering (ADR-0083).
//
// Text is glyph quads on the sprite path, so these tests exercise the font's
// metrics, the layout (advance, alignment, a missing glyph), the component's
// measurement, and the cell-to-instance extraction -- including that the
// arena-backed walk reproduces the vector one. Extraction is pure CPU work, so
// they run with no display and no GL context, against the null RHI device,
// exactly as the sprite and tilemap tests do.

#include <doctest/doctest.h>

#include "fe/core/FrameArena.h"
#include "fe/jobs/JobSystem.h"
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
#include "fe/scene/Sprite2D.h"
#include "fe/scene/TextLabel.h"
#include "fe/scene/TextLabelTypes.h"

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

// A matrix's translation. Mat4 is column-major, so the translation lives in the
// last column: element (row 3, column 0..2) is m[12..14].
[[nodiscard]] f32 translation_x(const Mat4& m) noexcept {
    return m.m[12];
}

// A `Texture2D` built on the null device. The text walk only extracts glyphs
// whose font has a `Texture2D` atlas, so this is the smallest atlas: no decode,
// no file, no GPU object.
[[nodiscard]] std::shared_ptr<Texture2D> make_atlas(rhi::Device& device) {
    std::vector<u8> pixels(16 * 16 * 4, 255);
    rhi::TextureDesc desc;
    desc.name = "font-atlas";
    desc.width = 16;
    desc.height = 16;
    desc.pixels = pixels.data();
    auto texture = device.create_texture(desc);
    REQUIRE(texture.ok());
    return std::make_shared<Texture2D>("tex:test-atlas.png", texture.value());
}

// One glyph entry for the metrics document. The metrics are em units, matching
// what the generator writes.
[[nodiscard]] std::string glyph_entry(f32 advance, f32 width, f32 height) {
    return R"({"u0":0.0,"v0":0.0,"u1":0.5,"v1":0.5,"advance":)" + std::to_string(advance) + R"(,"bearing_x":0.0,
            "bearing_y":0.0,"width":)" +
           std::to_string(width) + R"(,"height":)" + std::to_string(height) + "}";
}

// A glyph the atlas holds no ink for: a zero-area cell. It advances and draws
// nothing, which is how the generator records a space.
[[nodiscard]] std::string blank_entry(f32 advance) {
    return R"({"u0":0.0,"v0":0.0,"u1":0.0,"v1":0.0,"advance":)" + std::to_string(advance) + R"(,"bearing_x":0.0,
            "bearing_y":0.0,"width":0.0,"height":0.0})";
}

// A metrics document whose table is indexed by byte, so a lookup is a bounds
// check. 'A' and 'B' are inked and one em tall; ' ' is blank and advances.
[[nodiscard]] std::string make_metrics() {
    std::string glyphs;
    for (int code = 0; code <= 'B'; ++code) {
        if (code > 0) {
            glyphs += ",";
        }
        if (code == ' ') {
            glyphs += blank_entry(0.5f);
        } else if (code == 'A' || code == 'B') {
            glyphs += glyph_entry(0.5f, 0.5f, 1.0f);
        } else {
            glyphs += "null";
        }
    }
    return R"({"atlas":"test-atlas.png","line_height":1.25,"glyphs":[)" + glyphs + "]}";
}

// A font over `atlas`, parsed from `make_metrics()` by default so the real
// document path is exercised rather than a hand-built object.
[[nodiscard]] std::shared_ptr<Font> make_font(const std::shared_ptr<Texture2D>& atlas,
                                              std::string metrics = make_metrics()) {
    std::string error;
    auto font = Font::from_json("test.fefont", metrics, atlas, error);
    // The extra parens stop MSVC's doctest from stringifying the shared_ptr and
    // reaching for Font's operator<<, which does not exist (see AGENTS.md).
    REQUIRE((font != nullptr));
    return font;
}

// A label node with `text` in `font`, appended to `scene`. Returns the node so a
// test can move or re-align it; a test that only needs the label to exist
// ignores it.
Node* add_label(Scene& scene, const std::shared_ptr<Font>& font, std::string text, f32 size = 1.0f) {
    Node* node = scene.create_node("label");
    auto* label = node->add_component<TextLabel>();
    label->font = font;
    label->text = std::move(text);
    label->size = size;
    return node;
}

} // namespace

TEST_CASE("font metrics are indexed by byte and measure a string") {
    rhi::Device& device = null_device();
    auto font = make_font(make_atlas(device));

    // The table is dense up to the highest byte it addresses, so a byte with no
    // glyph is a null slot rather than a hole.
    CHECK(font->glyph_count() == 0x43);
    CHECK(font->glyph('A') != nullptr);
    CHECK(font->glyph('B') != nullptr);
    // A byte past the table has no glyph rather than reading out of bounds.
    CHECK(font->glyph('Z') == nullptr);
    CHECK(font->glyph(static_cast<u8>(0xFF)) == nullptr);

    // The blank glyph advances, which is what makes a space occupy width.
    CHECK(font->advance(' ') == doctest::Approx(0.5f));
    // A byte with no glyph advances by zero: an unsupported character is a gap.
    CHECK(font->advance('Z') == doctest::Approx(0.0f));

    // "A B" is 0.5 + 0.5 + 0.5 em.
    CHECK(font->measure("A B") == doctest::Approx(1.5f));
    CHECK(font->measure("") == doctest::Approx(0.0f));
}

TEST_CASE("a malformed font document is rejected rather than half-loaded") {
    rhi::Device& device = null_device();
    auto atlas = make_atlas(device);
    std::string error;

    // Not JSON at all.
    CHECK((Font::from_json("bad.fefont", "{ not json", atlas, error) == nullptr));
    CHECK_FALSE(error.empty());

    // JSON, but with no line height.
    error.clear();
    CHECK((Font::from_json("bad.fefont", R"({"glyphs":[]})", atlas, error) == nullptr));
    CHECK_FALSE(error.empty());

    // JSON with no glyph table.
    error.clear();
    CHECK((Font::from_json("bad.fefont", R"({"line_height":1.0})", atlas, error) == nullptr));
    CHECK_FALSE(error.empty());

    // A glyph entry missing a metric is rejected, so a corrupt atlas cannot
    // silently place every glyph at the origin.
    error.clear();
    CHECK((Font::from_json("bad.fefont", R"({"line_height":1.0,"glyphs":[{"u0":0.0}]})", atlas, error) == nullptr));
    CHECK_FALSE(error.empty());
}

TEST_CASE("a label lays out one instance per inked glyph") {
    rhi::Device& device = null_device();
    Scene scene;
    auto font = make_font(make_atlas(device));
    add_label(scene, font, "AB");

    std::vector<render::SpriteInstance> instances;
    render::extract_text(scene, instances);

    // Two inked glyphs, two quads. The blank case is covered separately.
    REQUIRE(instances.size() == 2);
    for (const auto& instance : instances) {
        CHECK(instance.label != nullptr);
        CHECK(instance.sprite == nullptr);
        CHECK(instance.tilemap == nullptr);
        CHECK(instance.texture != nullptr);
        CHECK(instance.color == Vec4{1.0f, 1.0f, 1.0f, 1.0f});
    }

    // 'A' starts at the pen (x=0) and 'B' one advance later. Both are one em
    // wide, so their centres are 0.25 and 0.75 em from the origin.
    const f32 first_x = translation_x(instances[0].world);
    const f32 second_x = translation_x(instances[1].world);
    CHECK(first_x == doctest::Approx(0.25f));
    CHECK(second_x == doctest::Approx(0.75f));
    // The advance is the metrics' 0.5 em, so the two centres are 0.5 apart.
    CHECK((second_x - first_x) == doctest::Approx(0.5f));

    // The quad is one em tall at the glyph's size.
    CHECK(instances[0].size.y == doctest::Approx(1.0f));
    CHECK(instances[0].size.x == doctest::Approx(0.5f));
}

TEST_CASE("a blank advances without drawing and a missing glyph is a gap") {
    rhi::Device& device = null_device();
    Scene scene;
    auto font = make_font(make_atlas(device));
    add_label(scene, font, "A B");

    std::vector<render::SpriteInstance> instances;
    render::extract_text(scene, instances);

    // The space is blank, so it advances but contributes no quad: two inked
    // glyphs out of three bytes.
    REQUIRE(instances.size() == 2);

    // 'B' sits after the blank's advance, so its centre is 1.25 em along, not
    // 0.75: the gap kept its width.
    const f32 second_x = translation_x(instances[1].world);
    CHECK(second_x == doctest::Approx(1.25f));

    // A byte with no glyph at all also draws nothing and advances nothing, so
    // the line keeps its length without inventing a character.
    Scene other;
    add_label(other, font, "AZA");
    std::vector<render::SpriteInstance> with_missing;
    render::extract_text(other, with_missing);
    REQUIRE(with_missing.size() == 2);
    // 'Z' advances by zero, so the second 'A' is one advance after the first.
    CHECK((translation_x(with_missing[1].world) - translation_x(with_missing[0].world)) == doctest::Approx(0.5f));
}

TEST_CASE("alignment shifts the line about the node origin") {
    rhi::Device& device = null_device();
    auto font = make_font(make_atlas(device));
    // "AB" is 1.0 em wide.

    // Left: the pen starts at the origin, so the first glyph's left edge is at 0.
    {
        Scene scene;
        add_label(scene, font, "AB");
        std::vector<render::SpriteInstance> instances;
        render::extract_text(scene, instances);
        REQUIRE(instances.size() == 2);
        CHECK((translation_x(instances[0].world) - instances[0].size.x * 0.5f) == doctest::Approx(0.0f));
    }

    // Centre: the line's centre is at the origin, so it starts at -0.5 em.
    {
        Scene scene;
        Node* node = add_label(scene, font, "AB");
        node->get_component<TextLabel>()->align = TextLabel::Align::Center;
        std::vector<render::SpriteInstance> instances;
        render::extract_text(scene, instances);
        REQUIRE(instances.size() == 2);
        CHECK((translation_x(instances[0].world) - instances[0].size.x * 0.5f) == doctest::Approx(-0.5f));
    }

    // Right: the line ends at the origin, so it starts at -1.0 em.
    {
        Scene scene;
        Node* node = add_label(scene, font, "AB");
        node->get_component<TextLabel>()->align = TextLabel::Align::Right;
        std::vector<render::SpriteInstance> instances;
        render::extract_text(scene, instances);
        REQUIRE(instances.size() == 2);
        CHECK((translation_x(instances[0].world) - instances[0].size.x * 0.5f) == doctest::Approx(-1.0f));
    }
}

TEST_CASE("size scales the layout and the measured width") {
    rhi::Device& device = null_device();
    Scene scene;
    auto font = make_font(make_atlas(device));
    Node* node = add_label(scene, font, "AB", 2.0f);

    // The component measures in world units, so the scale applies.
    CHECK(node->get_component<TextLabel>()->measured_width() == doctest::Approx(2.0f));

    std::vector<render::SpriteInstance> instances;
    render::extract_text(scene, instances);
    REQUIRE(instances.size() == 2);

    // At twice the size the advance and the quad both double.
    CHECK(instances[0].size.y == doctest::Approx(2.0f));
    CHECK((translation_x(instances[1].world) - translation_x(instances[0].world)) == doctest::Approx(1.0f));
}

TEST_CASE("a label with no font or no text draws nothing") {
    rhi::Device& device = null_device();
    auto font = make_font(make_atlas(device));

    // No font.
    {
        Scene scene;
        Node* node = scene.create_node("label");
        auto* label = node->add_component<TextLabel>();
        label->text = "AB";
        CHECK(label->empty());
        CHECK(label->measured_width() == doctest::Approx(0.0f));
        std::vector<render::SpriteInstance> instances;
        render::extract_text(scene, instances);
        CHECK(instances.empty());
    }

    // No text.
    {
        Scene scene;
        add_label(scene, font, "");
        std::vector<render::SpriteInstance> instances;
        render::extract_text(scene, instances);
        CHECK(instances.empty());
    }

    // A font that is not a `Font` is skipped, the way an unrecognised texture
    // skips a sprite.
    {
        Scene scene;
        Node* node = scene.create_node("label");
        auto* label = node->add_component<TextLabel>();
        label->font = make_atlas(device);
        label->text = "AB";
        std::vector<render::SpriteInstance> instances;
        render::extract_text(scene, instances);
        CHECK(instances.empty());
    }
}

TEST_CASE("the bound covers text and the arena walk matches the vector walk") {
    rhi::Device& device = null_device();
    Scene scene;
    auto font = make_font(make_atlas(device));
    add_label(scene, font, "AB");
    add_label(scene, font, "A B");

    // The bound counts every byte of every label, so it can never be short even
    // for a byte that draws nothing.
    const usize bound = render::sprite_instance_bound(scene);
    CHECK(bound >= 6);

    std::vector<render::SpriteInstance> expected;
    render::extract_text(scene, expected);
    REQUIRE(expected.size() == 4);

    // The arena walk writes the same values in the same order, so a caller that
    // sizes from the bound gets a byte-identical array.
    FrameArena arena(4096);
    std::vector<render::SpriteInstance> storage(bound);
    const usize used = render::extract_text_into(arena, scene, storage, 0);
    CHECK(used == expected.size());
    REQUIRE(used == expected.size());
    for (usize i = 0; i < used; ++i) {
        CHECK(storage[i] == expected[i]);
    }

    // The arena walk respects a short destination rather than overflowing it.
    std::vector<render::SpriteInstance> small(2);
    const usize clipped = render::extract_text_into(arena, scene, small, 0);
    CHECK(clipped == 2);
}

TEST_CASE("the arena render path includes text and the layer sort orders it") {
    rhi::Device& device = null_device();
    Scene scene;
    auto atlas = make_atlas(device);
    auto font = make_font(atlas);

    // A sprite under the text, so the layer sort has something to order against.
    Node* background = scene.create_node("background");
    auto* sprite = background->add_component<Sprite2D>();
    sprite->texture = atlas;
    sprite->size = Vec2{1.0f, 1.0f};
    sprite->layer = -1;

    Node* node = add_label(scene, font, "AB");
    node->get_component<TextLabel>()->layer = 5;

    renderer2d::Renderer2D renderer;
    REQUIRE(renderer.initialize(device).ok());

    std::vector<render::SpriteInstance> instances;
    renderer.extract(scene, instances);
    // One sprite plus one quad per inked glyph.
    REQUIRE(instances.size() == 3);
    CHECK(instances[0].sprite != nullptr);
    CHECK(instances[1].label != nullptr);
    CHECK(instances[2].label != nullptr);

    // The arena-backed render path produces the same instances, so text needs no
    // separate handling there.
    FrameArena arena(1 << 16);
    const usize drawn = renderer.render(device, scene, 1280, 720, arena, nullptr).value();
    CHECK(drawn == 3);
}

TEST_CASE("the text component is registered for reflection") {
    // Registration is what makes a label survive a scene save and appear in the
    // inspector. The type must be known to the registry once text types land.
    reflect::register_text_types();
    const auto* desc = reflect::Registry::instance().find(TextLabel::kTypeName);
    REQUIRE(desc != nullptr);

    // The font is a resource reference, so a saved label resolves it back
    // through the manager rather than storing a bare path.
    bool has_font = false;
    for (const auto& property : desc->properties) {
        if (property.name == "font") {
            has_font = true;
            CHECK(property.value_kind == reflect::ValueKind::ResourceRef);
        }
    }
    CHECK(has_font);
}
