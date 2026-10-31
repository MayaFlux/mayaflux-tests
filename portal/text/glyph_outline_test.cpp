#include "gtest/gtest.h"

#include <string_view>

#include "../../support/text_test_support.hpp"

#include "MayaFlux/Portal/Text/GlyphAtlas.hpp"
#include "MayaFlux/Portal/Text/GlyphOutline.hpp"
#include "MayaFlux/Portal/Text/TypeFaceFoundry.hpp"

namespace MayaFlux::Test {
namespace {

    using Portal::Text::decompose_glyph;
    using Portal::Text::GlyphAtlas;
    using Portal::Text::GlyphOutline;
    using Portal::Text::TypeFaceFoundry;

    constexpr uint32_t k_size = 64;

    class GlyphOutlineTest : public SystemFontTest { };

    // ------------------------------------------------------------------
    // Contour topology
    // ------------------------------------------------------------------

    struct ContourCase {
        std::string_view name;
        uint32_t codepoint;
        size_t contours;
    };

    class GlyphOutlineContourTest : public GlyphOutlineTest,
                                    public ::testing::WithParamInterface<ContourCase> { };

    TEST_P(GlyphOutlineContourTest, ContourCountMatchesTheGlyphTopology)
    {
        const auto& c = GetParam();
        const GlyphOutline g = decompose_glyph(face, c.codepoint, k_size);
        EXPECT_EQ(g.codepoint, c.codepoint);
        EXPECT_EQ(g.contour_ends.size(), c.contours);
        EXPECT_EQ(g.points.empty(), c.contours == 0);
    }

    INSTANTIATE_TEST_SUITE_P(
        Glyphs, GlyphOutlineContourTest,
        ::testing::Values(
            ContourCase { "Space", ' ', 0 },
            ContourCase { "CapitalI", 'I', 1 },
            ContourCase { "Period", '.', 1 },
            ContourCase { "LowerI", 'i', 2 },
            ContourCase { "CapitalO", 'O', 2 },
            ContourCase { "CapitalB", 'B', 3 },
            ContourCase { "DigitEight", '8', 3 }),
        [](const ::testing::TestParamInfo<ContourCase>& info) {
            return std::string(info.param.name);
        });

    class GlyphOutlineStructureTest : public GlyphOutlineTest,
                                      public ::testing::WithParamInterface<uint32_t> { };

    TEST_P(GlyphOutlineStructureTest, ContourEndsPartitionThePointsMonotonically)
    {
        const GlyphOutline g = decompose_glyph(face, GetParam(), k_size);
        ASSERT_FALSE(g.points.empty());
        ASSERT_FALSE(g.contour_ends.empty());

        EXPECT_EQ(g.contour_ends.back(), g.points.size());
        uint32_t start = 0;
        for (const uint32_t end : g.contour_ends) {
            EXPECT_GE(end - start, 3U);
            start = end;
        }
    }

    INSTANTIATE_TEST_SUITE_P(
        Glyphs, GlyphOutlineStructureTest,
        ::testing::Values(uint32_t { 'I' }, uint32_t { 'O' }, uint32_t { 'B' }, uint32_t { 'g' }, uint32_t { '@' }));

    // ------------------------------------------------------------------
    // Geometry
    // ------------------------------------------------------------------

    TEST_F(GlyphOutlineTest, YAxisPointsDownSoCapitalsSitAboveTheBaseline)
    {
        const GlyphOutline g = decompose_glyph(face, 'I', k_size);
        ASSERT_FALSE(g.points.empty());

        const auto [lo, hi] = std::ranges::minmax_element(g.points,
            [](const glm::vec2& a, const glm::vec2& b) { return a.y < b.y; });

        EXPECT_LT(lo->y, -0.4F * static_cast<float>(k_size));
        EXPECT_NEAR(hi->y, 0.F, 1.5F);
    }

    TEST_F(GlyphOutlineTest, ExtentScalesWithPixelSize)
    {
        const auto height = [&](uint32_t px) {
            const GlyphOutline g = decompose_glyph(face, 'I', px);
            const auto [lo, hi] = std::ranges::minmax_element(g.points,
                [](const glm::vec2& a, const glm::vec2& b) { return a.y < b.y; });
            return hi->y - lo->y;
        };

        EXPECT_NEAR(height(128), 2.F * height(64), 3.F);
    }

    TEST_F(GlyphOutlineTest, AdvanceMatchesTheAtlasAdvanceAtTheSameSize)
    {
        GlyphAtlas atlas(face, k_size, 512);
        for (const uint32_t cp : { uint32_t { 'A' }, uint32_t { 'i' }, uint32_t { 'W' }, uint32_t { ' ' } }) {
            const GlyphOutline g = decompose_glyph(face, cp, k_size);
            const auto* m = atlas.get_or_rasterize(static_cast<FT_ULong>(cp));
            ASSERT_NE(m, nullptr) << cp;
            EXPECT_EQ(g.advance_x, m->advance_x) << cp;
            EXPECT_GT(g.advance_x, 0) << cp;
        }
    }

    TEST_F(GlyphOutlineTest, SmallerToleranceNeverProducesFewerPoints)
    {
        const size_t coarse = decompose_glyph(face, 'O', 128, 4.F).points.size();
        const size_t mid = decompose_glyph(face, 'O', 128, 0.5F).points.size();
        const size_t fine = decompose_glyph(face, 'O', 128, 0.05F).points.size();

        EXPECT_LE(coarse, mid);
        EXPECT_LE(mid, fine);
        EXPECT_LT(coarse, fine);
    }

    TEST_F(GlyphOutlineTest, FlatteningStaysWithinTheGlyphBounds)
    {
        const GlyphOutline coarse = decompose_glyph(face, 'O', 128, 4.F);
        const GlyphOutline fine = decompose_glyph(face, 'O', 128, 0.05F);

        const auto extent = [](const GlyphOutline& g) {
            const auto [xl, xh] = std::ranges::minmax_element(g.points,
                [](const glm::vec2& a, const glm::vec2& b) { return a.x < b.x; });
            return xh->x - xl->x;
        };

        EXPECT_NEAR(extent(coarse), extent(fine), 4.F);
    }

    // ------------------------------------------------------------------
    // Failure paths
    // ------------------------------------------------------------------

    TEST_F(GlyphOutlineTest, CodepointWithoutAGlyphIsEmptyButKeepsItsCodepoint)
    {
        const GlyphOutline g = decompose_glyph(face, 0x10FFFF, k_size);
        EXPECT_EQ(g.codepoint, 0x10FFFFU);
        EXPECT_TRUE(g.points.empty());
        EXPECT_TRUE(g.contour_ends.empty());
        EXPECT_EQ(g.advance_x, 0);
    }

    TEST_F(GlyphOutlineTest, UnloadedFaceYieldsAnEmptyOutline)
    {
        Portal::Text::FontFace unloaded;
        const GlyphOutline g = decompose_glyph(unloaded, 'A', k_size);
        EXPECT_EQ(g.codepoint, static_cast<uint32_t>('A'));
        EXPECT_TRUE(g.points.empty());
        EXPECT_TRUE(g.contour_ends.empty());
    }

    // ------------------------------------------------------------------
    // Default font overload
    // ------------------------------------------------------------------

    TEST_F(GlyphOutlineTest, DefaultFontOverloadAgreesWithTheExplicitFaceAtTheAtlasSize)
    {
        auto& foundry = TypeFaceFoundry::instance();
        if (foundry.get_default_glyph_atlas()) {
            GTEST_SKIP() << "default font already set earlier in this process";
        }

        ASSERT_TRUE(foundry.set_default_font(font_path, 48, 512));
        ASSERT_NE(foundry.get_default_face(), nullptr);

        const GlyphOutline viaDefault = decompose_glyph('O');
        const GlyphOutline viaFace = decompose_glyph(*foundry.get_default_face(), 'O', 48);

        EXPECT_EQ(viaDefault.points, viaFace.points);
        EXPECT_EQ(viaDefault.contour_ends, viaFace.contour_ends);
        EXPECT_EQ(viaDefault.advance_x, viaFace.advance_x);
    }

    TEST_F(GlyphOutlineTest, DefaultFontOverloadWithoutADefaultIsEmpty)
    {
        if (TypeFaceFoundry::instance().get_default_glyph_atlas()) {
            GTEST_SKIP() << "default font already set earlier in this process";
        }

        const GlyphOutline g = decompose_glyph('A');
        EXPECT_EQ(g.codepoint, static_cast<uint32_t>('A'));
        EXPECT_TRUE(g.points.empty());
    }

} // namespace
} // namespace MayaFlux::Test
