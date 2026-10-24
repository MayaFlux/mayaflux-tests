#include "gtest/gtest.h"

#include <string_view>

#include "../../support/text_test_support.hpp"

#include "MayaFlux/Portal/Text/TextRun.hpp"
#include "MayaFlux/Portal/Text/TypeFaceFoundry.hpp"

namespace MayaFlux::Test {
namespace {

    using Portal::Text::coverage_weight;
    using Portal::Text::GlyphAtlas;
    using Portal::Text::GlyphMap;
    using Portal::Text::PressParams;
    using Portal::Text::TextRun;
    using Portal::Text::TypeFaceFoundry;

    constexpr uint32_t k_pixel_size = 24;

    class TextRunTest : public SystemFontTest {
    protected:
        void SetUp() override
        {
            SystemFontTest::SetUp();
            if (HasFatalFailure()) {
                return;
            }
            auto& foundry = TypeFaceFoundry::instance();
            ASSERT_TRUE(foundry.set_default_font(font_path, k_pixel_size, 512));
            atlas = foundry.get_default_glyph_atlas();
            ASSERT_NE(atlas, nullptr);
        }

        [[nodiscard]] PressParams params(uint32_t w, uint32_t h) const
        {
            PressParams p;
            p.render_bounds = { w, h };
            return p;
        }

        [[nodiscard]] float line() const { return static_cast<float>(atlas->line_height()); }

        GlyphAtlas* atlas { nullptr };
    };

    TEST_F(TextRunTest, AFreshRunHasNoQuadsAndNoVertices)
    {
        TextRun run(params(400, 100));

        EXPECT_TRUE(run.layout().quads.empty());
        EXPECT_TRUE(run.vertices({}, 0).empty());
    }

    TEST_F(TextRunTest, SettingTextLaysOutOneQuadPerInkedGlyphOnOneLine)
    {
        TextRun run(params(400, 100));

        run.set("Hi");

        EXPECT_EQ(run.text(), "Hi");
        EXPECT_EQ(run.layout().quads.size(), 2U);
        EXPECT_FLOAT_EQ(run.content_height(), line());
    }

    TEST_F(TextRunTest, ANewlineAddsALineToTheContentHeight)
    {
        TextRun run(params(400, 100));

        run.set("a\nb");

        EXPECT_FLOAT_EQ(run.content_height(), 2.F * line());
    }

    TEST_F(TextRunTest, WrappingAtTheRenderWidthAddsLines)
    {
        TextRun wide(params(2000, 100));
        TextRun narrow(params(40, 100));

        wide.set("aaaaaaaaaaaaaaaaaaaa");
        narrow.set("aaaaaaaaaaaaaaaaaaaa");

        EXPECT_FLOAT_EQ(wide.content_height(), line());
        EXPECT_GT(narrow.content_height(), line());
    }

    TEST_F(TextRunTest, VerticesAreSixPerGlyphAllCarryingTheCoverageWeightOfTheSlot)
    {
        TextRun run(params(400, 100));
        run.set("abc");

        const auto v = run.vertices({}, 3);

        ASSERT_EQ(v.size(), run.layout().quads.size() * 6);
        for (const auto& vertex : v) {
            EXPECT_FLOAT_EQ(vertex.weight, coverage_weight(3));
        }
    }

    TEST_F(TextRunTest, ABackgroundWithAlphaIsOneFlatQuadBeneathTheGlyphs)
    {
        auto p = params(400, 100);
        p.background = { 0.2F, 0.4F, 0.6F, 1.F };
        TextRun run(p);
        run.set("ab");

        const auto map = GlyphMap::fit({ .min = { -1.F, -1.F }, .max = { 1.F, 1.F } }, { 400.F, 100.F });
        const auto v = run.vertices(map, 0);

        ASSERT_EQ(v.size(), 6U + run.layout().quads.size() * 6);
        for (size_t i = 0; i < 6; ++i) {
            EXPECT_FLOAT_EQ(v[i].weight, 0.F);
            EXPECT_EQ(v[i].color, glm::vec3(0.2F, 0.4F, 0.6F));
            EXPECT_GE(v[i].position.x, -1.F);
            EXPECT_LE(v[i].position.x, 1.F);
        }
        EXPECT_FLOAT_EQ(v[6].weight, coverage_weight(0));
    }

    TEST_F(TextRunTest, ATransparentBackgroundAddsNoQuad)
    {
        TextRun run(params(400, 100));
        run.set("ab");

        EXPECT_EQ(run.vertices({}, 0).size(), run.layout().quads.size() * 6);
    }

    TEST_F(TextRunTest, TheCaretAtTheStartIsOneLineTallAtTheOrigin)
    {
        TextRun run(params(400, 100));
        run.set("abc");

        const auto caret = run.caret(0, 2.F);

        EXPECT_FLOAT_EQ(caret.min.x, 0.F);
        EXPECT_FLOAT_EQ(caret.min.y, 0.F);
        EXPECT_FLOAT_EQ(caret.max.x, 2.F);
        EXPECT_FLOAT_EQ(caret.max.y, line());
    }

    TEST_F(TextRunTest, TheCaretAtTheEndSitsWhereTheLayoutEnds)
    {
        TextRun run(params(400, 100));
        run.set("abc");

        const auto caret = run.caret(run.text().size(), 2.F);

        EXPECT_FLOAT_EQ(caret.min.x, run.layout().final_pen_x);
    }

    TEST_F(TextRunTest, TheCaretFollowsTheLineItsOffsetFallsOn)
    {
        TextRun run(params(400, 100));
        run.set("a\nb");

        const auto caret = run.caret(2, 2.F);

        EXPECT_FLOAT_EQ(caret.min.y, line());
    }

    TEST_F(TextRunTest, IndexAtACaretPositionFindsThatOffset)
    {
        TextRun run(params(400, 100));
        run.set("abcd");

        for (const size_t offset : { size_t { 1 }, size_t { 2 }, size_t { 3 } }) {
            const auto caret = run.caret(offset, 2.F);
            EXPECT_EQ(run.index_at({ caret.min.x, caret.min.y + line() * 0.5F }), offset);
        }
    }

    TEST_F(TextRunTest, AGrownAtlasMakesTheRunStaleAndVerticesLayItOutAgain)
    {
        GlyphAtlas small(face, k_pixel_size, 32);
        auto p = params(400, 100);
        p.atlas = &small;
        TextRun run(p);
        run.set("a");
        ASSERT_FALSE(run.stale());
        const uint32_t before = small.atlas_size();

        (void)Portal::Text::lay_out(
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789", small);
        ASSERT_GT(small.atlas_size(), before);

        EXPECT_TRUE(run.stale());
        EXPECT_EQ(run.vertices({}, 0).size(), 6U);
        EXPECT_FALSE(run.stale());
    }

    TEST_F(TextRunTest, ChangingTheAtlasInTheParamsLaysTheTextOutAgainstIt)
    {
        GlyphAtlas other(face, k_pixel_size, 256);
        TextRun run(params(400, 100));
        run.set("a");

        auto p = params(400, 100);
        p.atlas = &other;
        run.set_params(p);

        const auto* metrics = other.get_or_rasterize(static_cast<FT_ULong>('a'));
        ASSERT_NE(metrics, nullptr);
        EXPECT_FALSE(run.stale());
        ASSERT_EQ(run.layout().quads.size(), 1U);
        EXPECT_FLOAT_EQ(run.layout().quads.front().uv_x1, metrics->uv_x1);
    }

} // namespace
} // namespace MayaFlux::Test
