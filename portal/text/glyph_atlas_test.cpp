#include "gtest/gtest.h"

#include <string_view>

#include "../../support/text_test_support.hpp"

#include "MayaFlux/Portal/Text/GlyphAtlas.hpp"

namespace MayaFlux::Test {
namespace {

    using Portal::Text::FontFace;
    using Portal::Text::GlyphAtlas;
    using Portal::Text::GlyphMetrics;

    constexpr uint32_t k_pixel_size = 24;

    struct PixelRect {
        uint32_t x;
        uint32_t y;
        uint32_t w;
        uint32_t h;
    };

    PixelRect rect_of(const GlyphMetrics& m, uint32_t atlas_size)
    {
        const auto size = static_cast<float>(atlas_size);
        return {
            .x = static_cast<uint32_t>(std::lround(m.uv_x0 * size)),
            .y = static_cast<uint32_t>(std::lround(m.uv_y0 * size)),
            .w = m.width,
            .h = m.height
        };
    }

    std::vector<uint8_t> crop(const GlyphAtlas& atlas, const GlyphMetrics& m)
    {
        const PixelRect r = rect_of(m, atlas.atlas_size());
        const auto pixels = atlas.texture().pixel_bytes(0);
        std::vector<uint8_t> out;
        out.reserve(static_cast<size_t>(r.w) * r.h);
        for (uint32_t row = 0; row < r.h; ++row) {
            const size_t base = static_cast<size_t>(r.y + row) * atlas.atlas_size() + r.x;
            out.insert(out.end(), pixels.begin() + static_cast<std::ptrdiff_t>(base),
                pixels.begin() + static_cast<std::ptrdiff_t>(base + r.w));
        }
        return out;
    }

    bool overlaps(const PixelRect& a, const PixelRect& b)
    {
        return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
    }

    class GlyphAtlasTest : public SystemFontTest {
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

        std::unique_ptr<GlyphAtlas> atlas;
    };

    // ------------------------------------------------------------------
    // Construction and font-wide metrics
    // ------------------------------------------------------------------

    TEST_F(GlyphAtlasTest, ConstructsAnEmptyCleanAtlasOfTheRequestedSize)
    {
        EXPECT_EQ(atlas->pixel_size(), k_pixel_size);
        EXPECT_EQ(atlas->atlas_size(), 512U);
        EXPECT_FALSE(atlas->is_dirty());
        EXPECT_EQ(&atlas->get_face(), &face);
        EXPECT_EQ(atlas->texture().pixel_bytes(0).size(), 512U * 512U);
    }

    TEST_F(GlyphAtlasTest, LineHeightAndAscenderDescribeTheScaledFont)
    {
        EXPECT_GT(atlas->ascender(), 0U);
        EXPECT_GT(atlas->line_height(), atlas->ascender());
        EXPECT_GE(atlas->line_height(), k_pixel_size);
    }

    TEST_F(GlyphAtlasTest, UnloadedFaceFallsBackToPixelSizeMetricsAndYieldsNoGlyphs)
    {
        face.unload();
        EXPECT_EQ(atlas->line_height(), k_pixel_size);
        EXPECT_EQ(atlas->ascender(), k_pixel_size);
        EXPECT_EQ(atlas->get_or_rasterize(static_cast<FT_ULong>('A')), nullptr);
    }

    // ------------------------------------------------------------------
    // Lookup and caching
    // ------------------------------------------------------------------

    TEST_F(GlyphAtlasTest, MetricsDescribeTheRasterizedBitmap)
    {
        const GlyphMetrics* m = atlas->get_or_rasterize(static_cast<FT_ULong>('A'));
        ASSERT_NE(m, nullptr);

        EXPECT_GT(m->width, 0U);
        EXPECT_GT(m->height, 0U);
        EXPECT_GT(m->advance_x, 0);
        EXPECT_GT(m->bearing_y, 0);
        EXPECT_EQ(m->face, &face);
        EXPECT_NE(m->glyph_index, 0U);

        const auto size = static_cast<float>(atlas->atlas_size());
        EXPECT_NEAR((m->uv_x1 - m->uv_x0) * size, static_cast<float>(m->width), 0.01F);
        EXPECT_NEAR((m->uv_y1 - m->uv_y0) * size, static_cast<float>(m->height), 0.01F);
        EXPECT_GE(m->uv_x0, 0.F);
        EXPECT_GE(m->uv_y0, 0.F);
        EXPECT_LE(m->uv_x1, 1.F);
        EXPECT_LE(m->uv_y1, 1.F);
    }

    TEST_F(GlyphAtlasTest, RasterizedRegionCarriesInkAndWhitespaceCarriesNone)
    {
        const GlyphMetrics* inked = atlas->get_or_rasterize(static_cast<FT_ULong>('I'));
        ASSERT_NE(inked, nullptr);
        const auto ink = crop(*atlas, *inked);
        EXPECT_TRUE(std::ranges::any_of(ink, [](uint8_t v) { return v != 0; }));

        const GlyphMetrics* blank = atlas->get_or_rasterize(static_cast<FT_ULong>(' '));
        ASSERT_NE(blank, nullptr);
        EXPECT_EQ(blank->width, 0U);
        EXPECT_EQ(blank->height, 0U);
        EXPECT_GT(blank->advance_x, 0);
    }

    TEST_F(GlyphAtlasTest, RepeatLookupReturnsTheSameEntryWithoutDirtyingTheAtlas)
    {
        const GlyphMetrics* first = atlas->get_or_rasterize(static_cast<FT_ULong>('g'));
        ASSERT_NE(first, nullptr);
        EXPECT_TRUE(atlas->is_dirty());
        atlas->clear_dirty();

        const GlyphMetrics* second = atlas->get_or_rasterize(static_cast<FT_ULong>('g'));
        EXPECT_EQ(second, first);
        EXPECT_FALSE(atlas->is_dirty());

        ASSERT_NE(atlas->get_or_rasterize(static_cast<FT_ULong>('h')), nullptr);
        EXPECT_TRUE(atlas->is_dirty());
    }

    TEST_F(GlyphAtlasTest, GlyphIndexAndCodepointLookupsShareOneEntry)
    {
        const FT_UInt index = FT_Get_Char_Index(face.get_face(), 'A');
        ASSERT_NE(index, 0U);

        const GlyphMetrics* by_index = atlas->get_or_rasterize(index);
        const GlyphMetrics* by_codepoint = atlas->get_or_rasterize(static_cast<FT_ULong>('A'));
        ASSERT_NE(by_index, nullptr);
        EXPECT_EQ(by_codepoint, by_index);
        EXPECT_EQ(by_index->glyph_index, index);
    }

    TEST_F(GlyphAtlasTest, CodepointWithoutAGlyphYieldsNull)
    {
        EXPECT_EQ(atlas->get_or_rasterize(static_cast<FT_ULong>(0x10FFFF)), nullptr);
    }

    // ------------------------------------------------------------------
    // Packing
    // ------------------------------------------------------------------

    TEST_F(GlyphAtlasTest, PackedGlyphsStayInsideTheAtlasAndNeverOverlap)
    {
        constexpr std::string_view chars = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
        for (const char c : chars) {
            ASSERT_NE(atlas->get_or_rasterize(static_cast<FT_ULong>(c)), nullptr) << c;
        }

        std::vector<PixelRect> rects;
        for (const char c : chars) {
            const GlyphMetrics* m = atlas->get_or_rasterize(static_cast<FT_ULong>(c));
            ASSERT_NE(m, nullptr);
            const PixelRect r = rect_of(*m, atlas->atlas_size());
            EXPECT_LE(r.x + r.w, atlas->atlas_size()) << c;
            EXPECT_LE(r.y + r.h, atlas->atlas_size()) << c;
            rects.push_back(r);
        }

        for (size_t i = 0; i < rects.size(); ++i) {
            for (size_t j = i + 1; j < rects.size(); ++j) {
                EXPECT_FALSE(overlaps(rects[i], rects[j])) << chars[i] << " vs " << chars[j];
            }
        }
    }

    // ------------------------------------------------------------------
    // Growth
    // ------------------------------------------------------------------

    TEST_F(GlyphAtlasTest, GrowthKeepsEveryGlyphRetrievableWithItsOriginalPixels)
    {
        constexpr std::string_view chars = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";

        GlyphAtlas reference(face, k_pixel_size, 1024);
        std::vector<std::vector<uint8_t>> expected;
        for (const char c : chars) {
            const GlyphMetrics* m = reference.get_or_rasterize(static_cast<FT_ULong>(c));
            ASSERT_NE(m, nullptr) << c;
            expected.push_back(crop(reference, *m));
        }
        ASSERT_EQ(reference.atlas_size(), 1024U);

        GlyphAtlas small(face, k_pixel_size, 32);
        for (const char c : chars) {
            ASSERT_NE(small.get_or_rasterize(static_cast<FT_ULong>(c)), nullptr) << c;
        }

        EXPECT_GT(small.atlas_size(), 32U);
        EXPECT_EQ(small.texture().pixel_bytes(0).size(),
            static_cast<size_t>(small.atlas_size()) * small.atlas_size());
        EXPECT_TRUE(small.is_dirty());

        std::vector<PixelRect> rects;
        for (size_t i = 0; i < chars.size(); ++i) {
            const GlyphMetrics* m = small.get_or_rasterize(static_cast<FT_ULong>(chars[i]));
            ASSERT_NE(m, nullptr) << chars[i];
            EXPECT_EQ(crop(small, *m), expected[i]) << chars[i];
            rects.push_back(rect_of(*m, small.atlas_size()));
        }

        for (size_t i = 0; i < rects.size(); ++i) {
            for (size_t j = i + 1; j < rects.size(); ++j) {
                EXPECT_FALSE(overlaps(rects[i], rects[j])) << chars[i] << " vs " << chars[j];
            }
        }
    }

    // ------------------------------------------------------------------
    // Fallback faces
    // ------------------------------------------------------------------

    TEST_F(GlyphAtlasTest, PrimaryFaceWinsOverAFallbackThatAlsoHasTheGlyph)
    {
        const auto& other = second_system_font_path();
        if (!other) {
            GTEST_SKIP() << "host exposes only one resolvable font file";
        }

        FontFace fallback;
        ASSERT_TRUE(fallback.load(*other));
        atlas->add_fallback(fallback);

        const GlyphMetrics* m = atlas->get_or_rasterize(static_cast<FT_ULong>('A'));
        ASSERT_NE(m, nullptr);
        EXPECT_EQ(m->face, &face);
    }

    TEST_F(GlyphAtlasTest, FallbackFaceSuppliesGlyphsThePrimaryLacks)
    {
        constexpr uint32_t probes[] = { 0x4E00, 0x0905, 0x0627, 0x05D0, 0x0E01, 0x3042, 0xAC00, 0x2603, 0x2192, 0x00E9 };
        constexpr std::string_view families[] = {
            "DejaVu Sans", "Noto Sans CJK JP", "Noto Sans Arabic", "Noto Sans Hebrew",
            "Noto Sans Devanagari", "Noto Sans Symbols", "Arial Unicode MS", "Segoe UI Symbol",
            "Apple Symbols", "Arial", "Menlo", "Liberation Sans"
        };

        FontFace fallback;
        uint32_t chosen = 0;

        std::vector<std::string> paths;
        if (const auto& other = second_system_font_path()) {
            paths.push_back(*other);
        }
        for (const auto family : families) {
            if (const auto found = Portal::Text::find_font(family);
                found && std::filesystem::exists(*found) && *found != font_path) {
                paths.push_back(*found);
            }
        }

        for (const auto& path : paths) {
            if (!fallback.load(path)) {
                continue;
            }
            for (const uint32_t cp : probes) {
                if (FT_Get_Char_Index(face.get_face(), cp) == 0 && FT_Get_Char_Index(fallback.get_face(), cp) != 0) {
                    chosen = cp;
                    break;
                }
            }
            if (chosen != 0) {
                break;
            }
        }
        if (chosen == 0) {
            GTEST_SKIP() << "no resolvable font covers a probe codepoint the primary lacks";
        }

        EXPECT_EQ(atlas->get_or_rasterize(static_cast<FT_ULong>(chosen)), nullptr);

        atlas->add_fallback(fallback);
        const GlyphMetrics* m = atlas->get_or_rasterize(static_cast<FT_ULong>(chosen));
        ASSERT_NE(m, nullptr);
        EXPECT_EQ(m->face, &fallback);
        EXPECT_NE(m->glyph_index, 0U);
    }

} // namespace
} // namespace MayaFlux::Test
