#include "gtest/gtest.h"

#include <string_view>

#include "../../support/text_test_support.hpp"

#include "MayaFlux/Portal/Text/GlyphAtlas.hpp"
#include "MayaFlux/Portal/Text/TypeSetter.hpp"

namespace MayaFlux::Test {
namespace {

    using Portal::Text::GlyphAtlas;
    using Portal::Text::GlyphMetrics;
    using Portal::Text::LayoutResult;

    constexpr uint32_t k_pixel_size = 32;

    class TypeSetterTest : public SystemFontTest {
    protected:
        void SetUp() override
        {
            SystemFontTest::SetUp();
            if (HasFatalFailure()) {
                return;
            }
            atlas = std::make_unique<GlyphAtlas>(face, k_pixel_size, 512);
        }

        void TearDown() override
        {
            atlas.reset();
            SystemFontTest::TearDown();
        }

        [[nodiscard]] const GlyphMetrics& metrics(char32_t cp) const
        {
            const GlyphMetrics* m = atlas->get_or_rasterize(static_cast<FT_ULong>(cp));
            EXPECT_NE(m, nullptr) << "glyph U+" << static_cast<uint32_t>(cp);
            static const GlyphMetrics empty {};
            return m ? *m : empty;
        }

        [[nodiscard]] float advance(char32_t cp) const { return static_cast<float>(metrics(cp).advance_x); }
        [[nodiscard]] float line_height() const { return static_cast<float>(atlas->line_height()); }

        [[nodiscard]] float mid_height_above(float baseline) const
        {
            return baseline - 0.4F * static_cast<float>(atlas->ascender());
        }

        std::unique_ptr<GlyphAtlas> atlas;
    };

    // ------------------------------------------------------------------
    // lay_out: placement
    // ------------------------------------------------------------------

    TEST_F(TypeSetterTest, EmptyTextPassesThePenThrough)
    {
        const LayoutResult r = Portal::Text::lay_out("", *atlas, 3.F, 4.F);
        EXPECT_TRUE(r.quads.empty());
        EXPECT_FLOAT_EQ(r.final_pen_x, 3.F);
        EXPECT_FLOAT_EQ(r.final_pen_y, 4.F);
    }

    TEST_F(TypeSetterTest, SingleGlyphQuadFollowsTheGlyphMetrics)
    {
        const LayoutResult r = Portal::Text::lay_out("A", *atlas);
        const GlyphMetrics& m = metrics('A');

        ASSERT_EQ(r.quads.size(), 1U);
        const auto& q = r.quads.front();
        EXPECT_FLOAT_EQ(q.x0, static_cast<float>(m.bearing_x));
        EXPECT_FLOAT_EQ(q.y0, -static_cast<float>(m.bearing_y));
        EXPECT_FLOAT_EQ(q.x1 - q.x0, static_cast<float>(m.width));
        EXPECT_FLOAT_EQ(q.y1 - q.y0, static_cast<float>(m.height));
        EXPECT_FLOAT_EQ(q.uv_x0, m.uv_x0);
        EXPECT_FLOAT_EQ(q.uv_y0, m.uv_y0);
        EXPECT_FLOAT_EQ(q.uv_x1, m.uv_x1);
        EXPECT_FLOAT_EQ(q.uv_y1, m.uv_y1);
        EXPECT_EQ(q.codepoint, static_cast<uint32_t>('A'));
        EXPECT_EQ(q.byte_offset, 0U);
        EXPECT_FLOAT_EQ(r.final_pen_x, advance('A'));
        EXPECT_FLOAT_EQ(r.final_pen_y, 0.F);
    }

    TEST_F(TypeSetterTest, StartingPenOffsetsEveryQuad)
    {
        const LayoutResult r = Portal::Text::lay_out("A", *atlas, 10.F, 20.F);
        const GlyphMetrics& m = metrics('A');

        ASSERT_EQ(r.quads.size(), 1U);
        EXPECT_FLOAT_EQ(r.quads[0].x0, 10.F + static_cast<float>(m.bearing_x));
        EXPECT_FLOAT_EQ(r.quads[0].y0, 20.F - static_cast<float>(m.bearing_y));
        EXPECT_FLOAT_EQ(r.final_pen_x, 10.F + advance('A'));
    }

    TEST_F(TypeSetterTest, WhitespaceAdvancesThePenWithoutAQuad)
    {
        const LayoutResult r = Portal::Text::lay_out(" ", *atlas);
        EXPECT_TRUE(r.quads.empty());
        EXPECT_FLOAT_EQ(r.final_pen_x, advance(' '));
    }

    TEST_F(TypeSetterTest, OnlyInkedGlyphsProduceQuads)
    {
        const LayoutResult r = Portal::Text::lay_out("a b\tc\nd", *atlas);
        ASSERT_EQ(r.quads.size(), 4U);
        EXPECT_EQ(r.quads[0].codepoint, static_cast<uint32_t>('a'));
        EXPECT_EQ(r.quads[1].codepoint, static_cast<uint32_t>('b'));
        EXPECT_EQ(r.quads[2].codepoint, static_cast<uint32_t>('c'));
        EXPECT_EQ(r.quads[3].codepoint, static_cast<uint32_t>('d'));
    }

    TEST_F(TypeSetterTest, ByteOffsetsAreSourceByteIndicesAcrossMultibyteText)
    {
        const std::string text = "a\xC3\xA9" "b";
        const LayoutResult r = Portal::Text::lay_out(text, *atlas);

        ASSERT_EQ(r.quads.size(), 3U);
        EXPECT_EQ(r.quads[0].byte_offset, 0U);
        EXPECT_EQ(r.quads[1].byte_offset, 1U);
        EXPECT_EQ(r.quads[1].codepoint, 0xE9U);
        EXPECT_EQ(r.quads[2].byte_offset, 3U);
    }

    // ------------------------------------------------------------------
    // lay_out: lines, tabs, wrapping
    // ------------------------------------------------------------------

    TEST_F(TypeSetterTest, NewlineReturnsThePenAndDropsOneLine)
    {
        const LayoutResult r = Portal::Text::lay_out("a\na", *atlas);

        ASSERT_EQ(r.quads.size(), 2U);
        EXPECT_FLOAT_EQ(r.quads[1].x0, r.quads[0].x0);
        EXPECT_FLOAT_EQ(r.quads[1].y0 - r.quads[0].y0, line_height());
        EXPECT_FLOAT_EQ(r.final_pen_y, line_height());
        EXPECT_FLOAT_EQ(r.final_pen_x, advance('a'));
    }

    struct TabCase {
        std::string_view name;
        std::string_view text;
        uint32_t tab_width;
        float stops_before_glyph;
    };

    class TypeSetterTabTest : public TypeSetterTest,
                              public ::testing::WithParamInterface<TabCase> { };

    TEST_P(TypeSetterTabTest, TabSnapsThePenToTheNextStop)
    {
        const auto& c = GetParam();
        const LayoutResult r = Portal::Text::lay_out(c.text, *atlas, 0.F, 0.F, 0, c.tab_width);

        ASSERT_EQ(r.quads.size(), 1U);
        const float stop = advance(' ') * static_cast<float>(c.tab_width);
        EXPECT_FLOAT_EQ(r.quads[0].x0, c.stops_before_glyph * stop + static_cast<float>(metrics('a').bearing_x));
    }

    INSTANTIATE_TEST_SUITE_P(
        Tabs, TypeSetterTabTest,
        ::testing::Values(
            TabCase { "LeadingTab", "\ta", 4, 1.F },
            TabCase { "LeadingTabWidthTwo", "\ta", 2, 1.F },
            TabCase { "LeadingTabWidthEight", "\ta", 8, 1.F }),
        [](const ::testing::TestParamInfo<TabCase>& info) {
            return std::string(info.param.name);
        });

    TEST_F(TypeSetterTest, TabAfterAGlyphSnapsToTheFirstStopPastIt)
    {
        const LayoutResult r = Portal::Text::lay_out("i\ta", *atlas, 0.F, 0.F, 0, 4);
        const float stop = advance(' ') * 4.F;
        ASSERT_GT(stop, advance('i'));

        ASSERT_EQ(r.quads.size(), 2U);
        EXPECT_FLOAT_EQ(r.quads[1].x0, stop + static_cast<float>(metrics('a').bearing_x));
    }

    TEST_F(TypeSetterTest, ZeroTabWidthMakesTabInert)
    {
        const LayoutResult r = Portal::Text::lay_out("\ta", *atlas, 0.F, 0.F, 0, 0);
        ASSERT_EQ(r.quads.size(), 1U);
        EXPECT_FLOAT_EQ(r.quads[0].x0, static_cast<float>(metrics('a').bearing_x));
    }

    TEST_F(TypeSetterTest, ZeroWrapWidthNeverWraps)
    {
        const LayoutResult r = Portal::Text::lay_out("aaaaaaaaaaaaaaaa", *atlas, 0.F, 0.F, 0);
        ASSERT_EQ(r.quads.size(), 16U);
        for (const auto& q : r.quads) {
            EXPECT_FLOAT_EQ(q.y0, r.quads.front().y0);
        }
        EXPECT_FLOAT_EQ(r.final_pen_y, 0.F);
    }

    TEST_F(TypeSetterTest, NarrowWrapWidthPutsEachGlyphOnItsOwnLine)
    {
        const auto wrap = static_cast<uint32_t>(2.F * advance('a'));
        const LayoutResult r = Portal::Text::lay_out("aaaa", *atlas, 0.F, 0.F, wrap);

        ASSERT_EQ(r.quads.size(), 4U);
        for (size_t i = 0; i < r.quads.size(); ++i) {
            EXPECT_FLOAT_EQ(r.quads[i].x0, r.quads[0].x0) << i;
            EXPECT_FLOAT_EQ(r.quads[i].y0 - r.quads[0].y0, static_cast<float>(i) * line_height()) << i;
        }
        EXPECT_FLOAT_EQ(r.final_pen_y, 3.F * line_height());
    }

    // ------------------------------------------------------------------
    // lay_out: malformed and control input
    // ------------------------------------------------------------------

    TEST_F(TypeSetterTest, InvalidUtf8ByteIsSkippedAndLaterOffsetsStayInSource)
    {
        const std::string text = "a\xFF" "b";
        const LayoutResult r = Portal::Text::lay_out(text, *atlas);

        ASSERT_EQ(r.quads.size(), 2U);
        EXPECT_EQ(r.quads[0].byte_offset, 0U);
        EXPECT_EQ(r.quads[1].byte_offset, 2U);
        EXPECT_FLOAT_EQ(r.quads[1].x0, advance('a') + static_cast<float>(metrics('b').bearing_x));
    }

    TEST_F(TypeSetterTest, ControlCodepointEmitsNothingAndAdvancesNothing)
    {
        const std::string text = "a\x01" "b";
        const LayoutResult r = Portal::Text::lay_out(text, *atlas);

        ASSERT_EQ(r.quads.size(), 2U);
        EXPECT_FLOAT_EQ(r.quads[1].x0, advance('a') + static_cast<float>(metrics('b').bearing_x));
        EXPECT_EQ(r.quads[1].byte_offset, 2U);
    }

    // ------------------------------------------------------------------
    // Cursor queries
    // ------------------------------------------------------------------

    TEST_F(TypeSetterTest, XAtIsThePenAfterThePrefix)
    {
        const std::string_view text = "hello";
        for (size_t k = 0; k <= text.size(); ++k) {
            const LayoutResult prefix = Portal::Text::lay_out(text.substr(0, k), *atlas);
            const auto p = Portal::Text::x_at(text, *atlas, k);
            EXPECT_FLOAT_EQ(p.x, prefix.final_pen_x) << k;
            EXPECT_FLOAT_EQ(p.y, prefix.final_pen_y) << k;
        }
    }

    TEST_F(TypeSetterTest, XAtClampsPastTheEndAndHonoursTheStartingPen)
    {
        const std::string_view text = "hi";
        const auto end = Portal::Text::x_at(text, *atlas, text.size());
        const auto beyond = Portal::Text::x_at(text, *atlas, 99);
        EXPECT_FLOAT_EQ(beyond.x, end.x);
        EXPECT_FLOAT_EQ(beyond.y, end.y);

        const auto origin = Portal::Text::x_at(text, *atlas, 0, 7.F, 9.F);
        EXPECT_FLOAT_EQ(origin.x, 7.F);
        EXPECT_FLOAT_EQ(origin.y, 9.F);
    }

    TEST_F(TypeSetterTest, XAtMovesToTheNextLineAfterANewline)
    {
        const std::string_view text = "ab\ncd";
        const auto p = Portal::Text::x_at(text, *atlas, 3);
        EXPECT_FLOAT_EQ(p.x, 0.F);
        EXPECT_FLOAT_EQ(p.y, line_height());
    }

    TEST_F(TypeSetterTest, IndexAtInvertsXAtOnEveryBoundaryOfASingleLine)
    {
        const std::string text = "hello";
        const LayoutResult layout = Portal::Text::lay_out(text, *atlas);
        for (size_t k = 0; k <= text.size(); ++k) {
            const auto p = Portal::Text::x_at(text, *atlas, k);
            EXPECT_EQ(Portal::Text::index_at(text, layout, *atlas, p.x, mid_height_above(p.y)), k) << k;
        }
    }

    TEST_F(TypeSetterTest, IndexAtInvertsXAtAcrossALineBreak)
    {
        const std::string text = "ab\ncd";
        const LayoutResult layout = Portal::Text::lay_out(text, *atlas);
        for (const size_t k : { size_t { 0 }, size_t { 1 }, size_t { 2 }, size_t { 3 }, size_t { 4 }, size_t { 5 } }) {
            const auto p = Portal::Text::x_at(text, *atlas, k);
            EXPECT_EQ(Portal::Text::index_at(text, layout, *atlas, p.x, mid_height_above(p.y)), k) << k;
        }
    }

    TEST_F(TypeSetterTest, IndexAtOnEmptyLayoutIsZero)
    {
        const LayoutResult layout = Portal::Text::lay_out("", *atlas);
        EXPECT_EQ(Portal::Text::index_at("", layout, *atlas, 50.F, 0.F), 0U);
    }

    TEST_F(TypeSetterTest, IndexAtFarRightOfTheLineIsTheEndOfThatLine)
    {
        const std::string text = "ab\ncd";
        const LayoutResult layout = Portal::Text::lay_out(text, *atlas);
        EXPECT_EQ(Portal::Text::index_at(text, layout, *atlas, 10000.F, mid_height_above(0.F)), 2U);
        EXPECT_EQ(Portal::Text::index_at(text, layout, *atlas, 10000.F, mid_height_above(line_height())), text.size());
    }

    // ------------------------------------------------------------------
    // copy
    // ------------------------------------------------------------------

    struct CopyCase {
        std::string_view name;
        size_t a;
        size_t b;
        std::string_view text;
        size_t start;
        size_t end;
        size_t quads;
    };

    class TypeSetterCopyTest : public TypeSetterTest,
                               public ::testing::WithParamInterface<CopyCase> { };

    TEST_P(TypeSetterCopyTest, CopyNormalizesTheRangeAndKeepsMatchingQuads)
    {
        const auto& c = GetParam();
        const std::string source = "abc";
        const LayoutResult layout = Portal::Text::lay_out(source, *atlas);

        const auto out = Portal::Text::copy(source, layout, c.a, c.b);
        EXPECT_EQ(out.text, c.text);
        EXPECT_EQ(out.start, c.start);
        EXPECT_EQ(out.end, c.end);
        ASSERT_EQ(out.quads.size(), c.quads);
        for (const auto& q : out.quads) {
            EXPECT_GE(q.byte_offset, c.start);
            EXPECT_LT(q.byte_offset, c.end);
            EXPECT_FLOAT_EQ(q.x0, layout.quads[q.byte_offset].x0);
        }
    }

    INSTANTIATE_TEST_SUITE_P(
        Ranges, TypeSetterCopyTest,
        ::testing::Values(
            CopyCase { "Middle", 1, 2, "b", 1, 2, 1 },
            CopyCase { "Reversed", 2, 1, "b", 1, 2, 1 },
            CopyCase { "Whole", 0, 3, "abc", 0, 3, 3 },
            CopyCase { "EmptyRange", 1, 1, "", 1, 1, 0 },
            CopyCase { "ClampedPastEnd", 2, 99, "c", 2, 3, 1 },
            CopyCase { "BothPastEnd", 50, 99, "", 3, 3, 0 }),
        [](const ::testing::TestParamInfo<CopyCase>& info) {
            return std::string(info.param.name);
        });

    // ------------------------------------------------------------------
    // UTF-8 helpers (no glyphs involved)
    // ------------------------------------------------------------------

    struct ControlCase {
        uint32_t codepoint;
        bool control;
    };

    class IsControlTest : public ::testing::TestWithParam<ControlCase> { };

    TEST_P(IsControlTest, ClassifiesByUnicodeCategory)
    {
        EXPECT_EQ(Portal::Text::is_control(GetParam().codepoint), GetParam().control);
    }

    INSTANTIATE_TEST_SUITE_P(
        Categories, IsControlTest,
        ::testing::Values(
            ControlCase { 0x00, true },
            ControlCase { '\n', true },
            ControlCase { '\t', true },
            ControlCase { 0x7F, true },
            ControlCase { 0x200B, true },
            ControlCase { 0xD800, true },
            ControlCase { 0xE000, true },
            ControlCase { 'A', false },
            ControlCase { ' ', false },
            ControlCase { 0xE9, false },
            ControlCase { 0x20AC, false }));

    struct EncodeCase {
        uint32_t codepoint;
        std::string_view bytes;
    };

    class EncodeUtf8Test : public ::testing::TestWithParam<EncodeCase> { };

    TEST_P(EncodeUtf8Test, EncodesToTheShortestSequence)
    {
        EXPECT_EQ(Portal::Text::encode_utf8(GetParam().codepoint), std::string(GetParam().bytes));
    }

    INSTANTIATE_TEST_SUITE_P(
        Codepoints, EncodeUtf8Test,
        ::testing::Values(
            EncodeCase { 0x41, "A" },
            EncodeCase { 0xE9, "\xC3\xA9" },
            EncodeCase { 0x20AC, "\xE2\x82\xAC" },
            EncodeCase { 0x1F600, "\xF0\x9F\x98\x80" },
            EncodeCase { 0xD800, "" },
            EncodeCase { 0x110000, "" }));

    struct StepCase {
        size_t offset;
        size_t next;
    };

    class NextCodepointTest : public ::testing::TestWithParam<StepCase> { };

    TEST_P(NextCodepointTest, StepsOverWholeSequencesAndClampsAtTheEnd)
    {
        const std::string text = "a\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80";
        EXPECT_EQ(Portal::Text::next_codepoint_offset(text, GetParam().offset), GetParam().next);
    }

    INSTANTIATE_TEST_SUITE_P(
        Offsets, NextCodepointTest,
        ::testing::Values(
            StepCase { 0, 1 }, StepCase { 1, 3 }, StepCase { 3, 6 },
            StepCase { 6, 10 }, StepCase { 10, 10 }, StepCase { 99, 10 }));

    class PreviousCodepointTest : public ::testing::TestWithParam<StepCase> { };

    TEST_P(PreviousCodepointTest, StepsBackToTheStartOfTheSequence)
    {
        const std::string text = "a\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80";
        EXPECT_EQ(Portal::Text::previous_codepoint_offset(text, GetParam().offset), GetParam().next);
    }

    INSTANTIATE_TEST_SUITE_P(
        Offsets, PreviousCodepointTest,
        ::testing::Values(
            StepCase { 0, 0 }, StepCase { 1, 0 }, StepCase { 2, 1 }, StepCase { 3, 1 },
            StepCase { 5, 3 }, StepCase { 6, 3 }, StepCase { 10, 6 }));

    TEST(NextCodepointInvalidInputTest, InvalidByteIsSteppedOverAsOne)
    {
        EXPECT_EQ(Portal::Text::next_codepoint_offset(std::string_view("\xFF" "a", 2), 0), 1U);
    }

} // namespace
} // namespace MayaFlux::Test
