// UIElement as a SCEN component (M12.5 task 5).
//
// The SCEN bytes here are written from docs/formats/p2b-container.md rather
// than from the C# exporter, the same discipline as test_scene_rigidbody:
// a drift on either side fails a test instead of failing in PCSX2. This
// file exists because exactly that drift shipped: the parser demanded 88
// bytes for an 84-byte payload, and the overrun only fired when a UI
// element was the LAST payload in the section -- which the first real
// Canvas export produced and no synthetic scene had.
#include "ps2ur/p2b_scene.h"

#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

using namespace ps2ur;
using namespace ps2ur::scene;

namespace {

void put_u16(std::vector<uint8_t>& v, uint16_t x)
{
    v.push_back(static_cast<uint8_t>(x));
    v.push_back(static_cast<uint8_t>(x >> 8));
}

void put_u32(std::vector<uint8_t>& v, uint32_t x)
{
    v.push_back(static_cast<uint8_t>(x));
    v.push_back(static_cast<uint8_t>(x >> 8));
    v.push_back(static_cast<uint8_t>(x >> 16));
    v.push_back(static_cast<uint8_t>(x >> 24));
}

void put_i32(std::vector<uint8_t>& v, int32_t x)
{
    put_u32(v, static_cast<uint32_t>(x));
}

void put_f32(std::vector<uint8_t>& v, float f)
{
    uint32_t bits;
    std::memcpy(&bits, &f, 4);
    put_u32(v, bits);
}

// One UI element, in the field order the exporter writes.
struct UISpec {
    uint32_t kind_role = 0;   // draw | managed<<8 | role<<16
    float x = 0, y = 0, w = 32, h = 16;
    uint32_t colour = 0x80FFFFFFu;
    uint32_t texture = 0xFFFFFFFFu;
    int32_t link = -1;
    uint32_t text_scale = 1;
    uint8_t text[48] = {};

    void set_text(const char* s)
    {
        std::memset(text, 0, sizeof(text));
        size_t n = std::strlen(s);
        if (n > 47) {
            n = 47;
        }
        std::memcpy(text, s, n);
    }

    void set_slider(float value, float max_w)
    {
        std::memset(text, 0, sizeof(text));
        std::memcpy(text + 0, &value, 4);
        std::memcpy(text + 4, &max_w, 4);
    }
};

// 'elements[i]' rides on entity i; entities without one carry nothing.
// UI payloads are written LAST with nothing after them, on purpose: the
// final element's payload must end flush with the section, because that
// is the layout that exposed the 88-vs-84 bounds bug.
std::vector<uint8_t> build_scene(uint32_t entity_count,
                                 const std::vector<UISpec>& elements,
                                 uint32_t payload_bytes = 84u)
{
    std::vector<uint8_t> b;
    put_u32(b, entity_count);
    put_u32(b, static_cast<uint32_t>(elements.size()));
    put_u32(b, 0); // name table

    uint32_t next_component = 0;
    for (uint32_t i = 0; i < entity_count; ++i) {
        const bool has_ui = i < elements.size();
        put_i32(b, -1);   // parent: all roots
        put_f32(b, 0.0f); // pos
        put_f32(b, 0.0f);
        put_f32(b, 0.0f);
        put_f32(b, 0.0f); // rot
        put_f32(b, 0.0f);
        put_f32(b, 0.0f);
        put_f32(b, 1.0f);
        put_f32(b, 1.0f); // scale
        put_f32(b, 1.0f);
        put_f32(b, 1.0f);
        put_u32(b, 0);    // name hash
        put_u16(b, 0);    // layer
        put_u16(b, 0);    // tag
        put_u32(b, 0);    // flags
        put_u32(b, next_component);
        put_u16(b, has_ui ? 1 : 0);
        put_u16(b, 0);    // pad
        if (has_ui) {
            ++next_component;
        }
    }

    const uint32_t refs_at = static_cast<uint32_t>(b.size());
    const uint32_t count = static_cast<uint32_t>(elements.size());
    uint32_t payload_at = refs_at + count * 8u;
    for (uint32_t i = 0; i < count; ++i) {
        put_u16(b, kComponentUIElement);
        put_u16(b, 0);
        put_u32(b, payload_at);
        payload_at += payload_bytes;
    }
    for (const UISpec& e : elements) {
        std::vector<uint8_t> one;
        put_u32(one, e.kind_role);
        put_f32(one, e.x);
        put_f32(one, e.y);
        put_f32(one, e.w);
        put_f32(one, e.h);
        put_u32(one, e.colour);
        put_u32(one, e.texture);
        put_i32(one, e.link);
        put_u32(one, e.text_scale);
        one.insert(one.end(), e.text, e.text + 48);
        one.resize(payload_bytes, 0); // short only when provoking truncation
        b.insert(b.end(), one.begin(), one.end());
    }
    return b;
}

// A single-section container around a SCEN payload, 2048-aligned like the
// real writer.
std::vector<uint8_t> wrap_scene(const std::vector<uint8_t>& payload)
{
    std::vector<uint8_t> file(2048 + payload.size(), 0);
    const char magic[4] = {'P', '2', 'B', 'C'};
    std::memcpy(file.data(), magic, 4);
    file[4] = 1; // version_major
    const uint32_t total = static_cast<uint32_t>(file.size());
    std::memcpy(file.data() + 8, &total, 4);
    const uint32_t sections = 1;
    std::memcpy(file.data() + 12, &sections, 4);

    uint8_t* entry = file.data() + 32;
    const uint32_t type = io::kSectionScene;
    const uint32_t offset = 2048;
    const uint32_t size = static_cast<uint32_t>(payload.size());
    const uint32_t checksum = io::crc32(payload.data(), size);
    std::memcpy(entry + 0, &type, 4);
    std::memcpy(entry + 4, &offset, 4);
    std::memcpy(entry + 8, &size, 4);
    std::memcpy(entry + 12, &size, 4);
    std::memcpy(entry + 16, &checksum, 4);
    std::memcpy(file.data() + 2048, payload.data(), payload.size());
    return file;
}

} // namespace

// The regression: a Text element as the one and only component, its 84-byte
// payload ending flush with the section. The 88-byte check refused exactly
// this file as "truncated".
TEST(SceneUI, TheLastPayloadInTheSectionLoadsAndRoundTripsEveryField)
{
    UISpec text;
    text.kind_role = 2u | (2u << 8); // draw text, managed Text, no role
    text.x = 96;
    text.y = 32;
    text.w = 320;
    text.h = 24;
    text.colour = 0x60101820u;
    text.text_scale = 3;
    text.set_text("NEW GAME");

    const std::vector<uint8_t> bytes = wrap_scene(build_scene(1, {text}));
    io::P2bFile file;
    ASSERT_TRUE(file.parse(bytes.data(), static_cast<uint32_t>(bytes.size())));
    static World world;
    ASSERT_TRUE(world.load(file)) << world.error();

    ASSERT_EQ(world.ui_element_count(), 1u);
    const UIElement& ui = world.ui_element(0);
    EXPECT_EQ(ui.entity, 0);
    EXPECT_EQ(ui.kind, 0x0202u);
    // The renderer switches on the DRAW kind; bits 8-15 are the managed
    // class and must be masked off there (scene_renderer.cpp).
    EXPECT_EQ(ui.kind & 0xFFu, 2u);
    EXPECT_EQ(ui.role, 0u);
    EXPECT_FLOAT_EQ(ui.x, 96.0f);
    EXPECT_FLOAT_EQ(ui.y, 32.0f);
    EXPECT_FLOAT_EQ(ui.w, 320.0f);
    EXPECT_FLOAT_EQ(ui.h, 24.0f);
    EXPECT_EQ(ui.colour, 0x60101820u);
    EXPECT_EQ(ui.texture, 0xFFFFFFFFu);
    EXPECT_EQ(ui.link, -1);
    EXPECT_EQ(ui.text_scale, 3u);
    EXPECT_TRUE(ui.visible);
    EXPECT_STREQ(ui.text, "NEW GAME");
}

TEST(SceneUI, TextAlignmentRidesTheScaleWordsHighBits)
{
    UISpec text;
    text.kind_role = 2u | (2u << 8);
    // Scale 2, MiddleCenter: h=1 in bits 8-9, v=1 in bits 10-11.
    text.text_scale = 2u | (1u << 8) | (1u << 10);
    text.set_text("BUTTON");

    const std::vector<uint8_t> bytes = wrap_scene(build_scene(1, {text}));
    io::P2bFile file;
    ASSERT_TRUE(file.parse(bytes.data(), static_cast<uint32_t>(bytes.size())));
    static World world;
    ASSERT_TRUE(world.load(file)) << world.error();

    const UIElement& ui = world.ui_element(0);
    EXPECT_EQ(ui.text_scale, 2u) << "alignment bits must not leak into scale";
    EXPECT_EQ(ui.align_h, 1u);
    EXPECT_EQ(ui.align_v, 1u);
}

TEST(SceneUI, ATruncatedPayloadIsRefusedRatherThanRead)
{
    // 80 bytes where the format says 84: the last four text bytes are gone.
    const std::vector<uint8_t> bytes =
        wrap_scene(build_scene(1, {UISpec{}}, /*payload_bytes=*/80u));
    io::P2bFile file;
    ASSERT_TRUE(file.parse(bytes.data(), static_cast<uint32_t>(bytes.size())));
    static World world;
    EXPECT_FALSE(world.load(file));
    EXPECT_STREQ(world.error(), "ui element payload truncated");
}

TEST(SceneUI, ASliderCarriesItsValueAndFillWidthInTheTextBytes)
{
    UISpec background;
    background.kind_role = 0u | (2u << 16); // draw rect, role slider
    background.link = 1;                    // the fill element below
    background.set_slider(0.75f, 118.0f);
    UISpec fill; // plain rect the slider resizes at runtime

    const std::vector<uint8_t> bytes =
        wrap_scene(build_scene(2, {background, fill}));
    io::P2bFile file;
    ASSERT_TRUE(file.parse(bytes.data(), static_cast<uint32_t>(bytes.size())));
    static World world;
    ASSERT_TRUE(world.load(file)) << world.error();

    ASSERT_EQ(world.ui_element_count(), 2u);
    const UIElement& ui = world.ui_element(0);
    EXPECT_EQ(ui.role, 2u);
    EXPECT_EQ(ui.link, 1);
    float value, max_w;
    std::memcpy(&value, ui.text + 0, 4);
    std::memcpy(&max_w, ui.text + 4, 4);
    EXPECT_FLOAT_EQ(value, 0.75f);
    EXPECT_FLOAT_EQ(max_w, 118.0f);
}

TEST(SceneUI, AnImageCarriesItsNineSliceBordersInTheTextBytes)
{
    // The renderer reads these with memcpy in L, T, R, B order; a drift
    // between the exporter's packing and this order draws corners from the
    // wrong side of the sprite.
    UISpec image;
    image.kind_role = 1u; // draw image, managed Image, no role
    image.texture = 0;
    std::memset(image.text, 0, sizeof(image.text));
    const float borders[4] = {6.0f, 7.0f, 8.0f, 9.0f}; // L, T, R, B
    std::memcpy(image.text, borders, 16);

    const std::vector<uint8_t> bytes = wrap_scene(build_scene(1, {image}));
    io::P2bFile file;
    ASSERT_TRUE(file.parse(bytes.data(), static_cast<uint32_t>(bytes.size())));
    static World world;
    ASSERT_TRUE(world.load(file)) << world.error();

    const UIElement& ui = world.ui_element(0);
    float out[4];
    std::memcpy(out, ui.text, 16);
    EXPECT_FLOAT_EQ(out[0], 6.0f);
    EXPECT_FLOAT_EQ(out[1], 7.0f);
    EXPECT_FLOAT_EQ(out[2], 8.0f);
    EXPECT_FLOAT_EQ(out[3], 9.0f);
}

TEST(SceneUI, AZeroTextScaleIsClampedToOne)
{
    UISpec e;
    e.text_scale = 0; // a hand-edited or hostile file; 0 would divide draw_text
    const std::vector<uint8_t> bytes = wrap_scene(build_scene(1, {e}));
    io::P2bFile file;
    ASSERT_TRUE(file.parse(bytes.data(), static_cast<uint32_t>(bytes.size())));
    static World world;
    ASSERT_TRUE(world.load(file)) << world.error();
    EXPECT_EQ(world.ui_element(0).text_scale, 1u);
}

TEST(SceneUI, MoreElementsThanTheTableHoldsIsALoadFailure)
{
    std::vector<UISpec> many(kMaxUIElements + 1u);
    const std::vector<uint8_t> bytes = wrap_scene(
        build_scene(static_cast<uint32_t>(many.size()), many));
    io::P2bFile file;
    ASSERT_TRUE(file.parse(bytes.data(), static_cast<uint32_t>(bytes.size())));
    static World world;
    EXPECT_FALSE(world.load(file));
    EXPECT_STREQ(world.error(), "too many ui elements");
}

TEST(SceneUI, ASecondLoadDoesNotInheritTheFirstScenesElements)
{
    const std::vector<uint8_t> bytes =
        wrap_scene(build_scene(2, {UISpec{}, UISpec{}}));
    io::P2bFile file;
    ASSERT_TRUE(file.parse(bytes.data(), static_cast<uint32_t>(bytes.size())));
    static World world;
    ASSERT_TRUE(world.load(file)) << world.error();
    ASSERT_EQ(world.ui_element_count(), 2u);
    ASSERT_TRUE(world.load(file)) << world.error();
    EXPECT_EQ(world.ui_element_count(), 2u) << "load must reset, not accumulate";
}

TEST(SceneUI, AdditiveLoadRebasesTheEntityAndTheSliderLink)
{
    UISpec background;
    background.kind_role = 0u | (2u << 16);
    background.link = 1;
    background.set_slider(0.5f, 100.0f);
    UISpec fill;

    const std::vector<uint8_t> bytes =
        wrap_scene(build_scene(2, {background, fill}));
    io::P2bFile file;
    ASSERT_TRUE(file.parse(bytes.data(), static_cast<uint32_t>(bytes.size())));

    static World world;
    ASSERT_TRUE(world.load(file)) << world.error();
    ASSERT_TRUE(world.append(file)) << world.error();

    ASSERT_EQ(world.ui_element_count(), 4u);
    // The merged copy's slider must point at ITS fill, not the first one's,
    // and must ride its own entity.
    EXPECT_EQ(world.ui_element(2).entity, 2);
    EXPECT_EQ(world.ui_element(2).link, 3);
    EXPECT_EQ(world.ui_element(3).entity, 3);
}
