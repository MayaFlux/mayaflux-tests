#include "gtest/gtest.h"

#include <string_view>

#include "../../support/text_test_support.hpp"

#include "MayaFlux/Portal/Text/InkPress.hpp"
#include "MayaFlux/Portal/Text/TypeFaceFoundry.hpp"
#include "MayaFlux/Portal/Text/TypeSetter.hpp"

namespace MayaFlux::Test {
namespace {

    using Buffers::TextBuffer;
    using Portal::Text::GlyphAtlas;
    using Portal::Text::GlyphQuad;
    using Portal::Text::ImpressResult;
    using Portal::Text::LayoutResult;
    using Portal::Text::PressParams;
    using Portal::Text::RedrawPolicy;
    using Portal::Text::StyleSpan;
    using Portal::Text::TypeFaceFoundry;

    constexpr uint32_t k_pixel_size = 24;
    const glm::vec4 k_red { 1.F, 0.F, 0.F, 1.F };
    const glm::vec4 k_white { 1.F, 1.F, 1.F, 1.F };

    uint8_t to_byte(float v)
    {
        return static_cast<uint8_t>(std::clamp(v, 0.F, 1.F) * 255.F);
    }

    std::vector<uint8_t> filled(uint32_t w, uint32_t h, glm::vec4 color)
    {
        std::vector<uint8_t> out(static_cast<size_t>(w) * h * 4);
        for (size_t i = 0; i < out.size(); i += 4) {
            out[i + 0] = to_byte(color.r);
            out[i + 1] = to_byte(color.g);
            out[i + 2] = to_byte(color.b);
            out[i + 3] = to_byte(color.a);
        }
        return out;
    }

    size_t ink_count(const std::vector<uint8_t>& rgba)
    {
        size_t n = 0;
        for (size_t i = 3; i < rgba.size(); i += 4) {
            n += rgba[i] != 0 ? 1U : 0U;
        }
        return n;
    }

    std::array<uint8_t, 4> pixel_at(const std::vector<uint8_t>& rgba, uint32_t width, uint32_t x, uint32_t y)
    {
        const size_t at = (static_cast<size_t>(y) * width + x) * 4;
        return { rgba[at], rgba[at + 1], rgba[at + 2], rgba[at + 3] };
    }

    std::vector<uint8_t> coverage_reference(
        const std::vector<GlyphQuad>& quads,
        const GlyphAtlas& atlas,
        glm::vec4 color,
        uint32_t w,
        uint32_t h)
    {
        std::vector<uint8_t> out(static_cast<size_t>(w) * h * 4, 0);
        const auto atlas_pixels = atlas.texture().pixel_bytes(0);
        const uint32_t atlas_size = atlas.atlas_size();
        const uint32_t ca = to_byte(color.a);

        for (const auto& q : quads) {
            const auto gx = static_cast<int32_t>(std::floor(q.x0));
            const auto gy = static_cast<int32_t>(std::floor(q.y0));
            const auto gw = static_cast<uint32_t>(std::ceil(q.x1 - q.x0));
            const auto gh = static_cast<uint32_t>(std::ceil(q.y1 - q.y0));
            const auto sx = static_cast<uint32_t>(q.uv_x0 * static_cast<float>(atlas_size));
            const auto sy = static_cast<uint32_t>(q.uv_y0 * static_cast<float>(atlas_size));

            for (uint32_t row = 0; row < gh; ++row) {
                for (uint32_t col = 0; col < gw; ++col) {
                    const int32_t x = gx + static_cast<int32_t>(col);
                    const int32_t y = gy + static_cast<int32_t>(row);
                    if (x < 0 || y < 0 || static_cast<uint32_t>(x) >= w || static_cast<uint32_t>(y) >= h) {
                        continue;
                    }
                    const uint32_t coverage = atlas_pixels[static_cast<size_t>(sy + row) * atlas_size + sx + col];
                    const uint32_t a = (coverage * ca) / 255U;
                    const size_t at = (static_cast<size_t>(y) * w + static_cast<uint32_t>(x)) * 4;
                    if (a == 0) {
                        continue;
                    }
                    out[at + 0] = to_byte(color.r);
                    out[at + 1] = to_byte(color.g);
                    out[at + 2] = to_byte(color.b);
                    out[at + 3] = static_cast<uint8_t>(a);
                }
            }
        }
        return out;
    }

    class InkPressTest : public SystemFontTest {
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

        [[nodiscard]] uint32_t line() const { return atlas->line_height(); }

        [[nodiscard]] PressParams params(uint32_t w, uint32_t h) const
        {
            PressParams p;
            p.atlas = atlas;
            p.render_bounds = { w, h };
            return p;
        }

        [[nodiscard]] LayoutResult layout(std::string_view text, uint32_t wrap, float pen_x = 0.F) const
        {
            return Portal::Text::lay_out(text, *atlas, pen_x, static_cast<float>(line()), wrap);
        }

        [[nodiscard]] uint32_t content_height(std::string_view text, uint32_t wrap) const
        {
            return static_cast<uint32_t>(std::ceil(layout(text, wrap).final_pen_y)) + line();
        }

        GlyphAtlas* atlas { nullptr };
    };

    // ------------------------------------------------------------------
    // rasterize_quads
    // ------------------------------------------------------------------

    TEST_F(InkPressTest, RasterizeQuadsWritesTheAtlasCoverageInTheRequestedColour)
    {
        constexpr uint32_t w = 48;
        constexpr uint32_t h = 48;
        const LayoutResult l = Portal::Text::lay_out("A", *atlas, 4.F, static_cast<float>(atlas->ascender()));

        std::vector<uint8_t> dst(static_cast<size_t>(w) * h * 4, 0);
        Portal::Text::rasterize_quads(l.quads, *atlas, k_red, dst.data(), w, h);

        EXPECT_EQ(dst, coverage_reference(l.quads, *atlas, k_red, w, h));
        EXPECT_GT(ink_count(dst), 0U);
        for (size_t i = 0; i < dst.size(); i += 4) {
            if (dst[i + 3] != 0) {
                EXPECT_EQ(dst[i + 0], 255);
                EXPECT_EQ(dst[i + 1], 0);
                EXPECT_EQ(dst[i + 2], 0);
            }
        }
    }

    TEST_F(InkPressTest, RasterizeQuadsScalesCoverageByTheColourAlpha)
    {
        constexpr uint32_t w = 48;
        constexpr uint32_t h = 48;
        const glm::vec4 half { 0.F, 1.F, 0.F, 0.5F };
        const LayoutResult l = Portal::Text::lay_out("A", *atlas, 4.F, static_cast<float>(atlas->ascender()));

        std::vector<uint8_t> dst(static_cast<size_t>(w) * h * 4, 0);
        Portal::Text::rasterize_quads(l.quads, *atlas, half, dst.data(), w, h);

        EXPECT_EQ(dst, coverage_reference(l.quads, *atlas, half, w, h));
        for (size_t i = 3; i < dst.size(); i += 4) {
            EXPECT_LE(dst[i], 127);
        }
    }

    TEST_F(InkPressTest, RasterizeQuadsNeverWritesOutsideTheBuffer)
    {
        constexpr uint32_t w = 16;
        constexpr uint32_t h = 16;
        constexpr size_t guard = 256;
        const size_t body = static_cast<size_t>(w) * h * 4;

        std::vector<uint8_t> storage(guard + body + guard, 0xCD);
        std::fill(storage.begin() + static_cast<std::ptrdiff_t>(guard),
            storage.begin() + static_cast<std::ptrdiff_t>(guard + body), uint8_t { 0 });

        for (const auto [pen_x, pen_y] : { std::array<float, 2> { -6.F, 12.F }, std::array<float, 2> { 13.F, 14.F } }) {
            const LayoutResult l = Portal::Text::lay_out("W", *atlas, pen_x, pen_y);
            Portal::Text::rasterize_quads(l.quads, *atlas, k_white, storage.data() + guard, w, h);
        }

        EXPECT_TRUE(std::all_of(storage.begin(), storage.begin() + static_cast<std::ptrdiff_t>(guard),
            [](uint8_t v) { return v == 0xCD; }));
        EXPECT_TRUE(std::all_of(storage.end() - static_cast<std::ptrdiff_t>(guard), storage.end(),
            [](uint8_t v) { return v == 0xCD; }));
    }

    TEST_F(InkPressTest, RasterizeQuadsCompositesOverAnOpaqueBackground)
    {
        constexpr uint32_t w = 48;
        constexpr uint32_t h = 48;
        const LayoutResult l = Portal::Text::lay_out("A", *atlas, 4.F, static_cast<float>(atlas->ascender()));
        const std::vector<uint8_t> coverage = coverage_reference(l.quads, *atlas, k_white, w, h);

        std::vector<uint8_t> dst = filled(w, h, { 0.F, 0.F, 1.F, 1.F });
        Portal::Text::rasterize_quads(l.quads, *atlas, k_white, dst.data(), w, h);

        size_t full = 0;
        for (uint32_t y = 0; y < h; ++y) {
            for (uint32_t x = 0; x < w; ++x) {
                const auto got = pixel_at(dst, w, x, y);
                const uint8_t cov = pixel_at(coverage, w, x, y)[3];
                if (cov == 255) {
                    ++full;
                    EXPECT_EQ(got, (std::array<uint8_t, 4> { 255, 255, 255, 255 })) << x << "," << y;
                } else if (cov == 0) {
                    EXPECT_EQ(got, (std::array<uint8_t, 4> { 0, 0, 255, 255 })) << x << "," << y;
                } else {
                    EXPECT_EQ(got[3], 255) << x << "," << y;
                }
            }
        }
        EXPECT_GT(full, 0U);
    }

    TEST_F(InkPressTest, RasterizeQuadsOverlapOnlyAddsOpacity)
    {
        constexpr uint32_t w = 48;
        constexpr uint32_t h = 48;
        const glm::vec4 half { 1.F, 1.F, 1.F, 0.5F };
        const LayoutResult l = Portal::Text::lay_out("A", *atlas, 4.F, static_cast<float>(atlas->ascender()));

        std::vector<uint8_t> once(static_cast<size_t>(w) * h * 4, 0);
        Portal::Text::rasterize_quads(l.quads, *atlas, half, once.data(), w, h);
        std::vector<uint8_t> twice = once;
        Portal::Text::rasterize_quads(l.quads, *atlas, half, twice.data(), w, h);

        bool grew = false;
        for (size_t i = 3; i < once.size(); i += 4) {
            EXPECT_GE(twice[i], once[i]);
            grew = grew || twice[i] > once[i];
        }
        EXPECT_TRUE(grew);
    }

    // ------------------------------------------------------------------
    // ink_quads
    // ------------------------------------------------------------------

    TEST_F(InkPressTest, InkQuadsRedrawsTheTargetOverItsBackground)
    {
        PressParams p = params(64, 64);
        p.background = { 0.F, 0.F, 1.F, 1.F };
        const auto target = Portal::Text::press("", p);
        ASSERT_NE(target, nullptr);

        const LayoutResult l = Portal::Text::lay_out("Q", *atlas, 2.F, static_cast<float>(atlas->ascender()));
        Portal::Text::ink_quads(target, l.quads, k_red, atlas);

        std::vector<uint8_t> expected = filled(target->get_budget_width(), target->get_budget_height(), p.background);
        Portal::Text::rasterize_quads(l.quads, *atlas, k_red, expected.data(),
            target->get_budget_width(), target->get_budget_height());
        EXPECT_EQ(target->get_pixel_data(), expected);
        EXPECT_TRUE(target->is_texture_dirty());
    }

    TEST_F(InkPressTest, InkQuadsOnANullTargetIsHarmless)
    {
        const LayoutResult l = Portal::Text::lay_out("Q", *atlas);
        EXPECT_NO_THROW(Portal::Text::ink_quads(nullptr, l.quads, k_red, atlas));
    }

    // ------------------------------------------------------------------
    // press
    // ------------------------------------------------------------------

    TEST_F(InkPressTest, PressBuildsABufferMatchingAManualComposite)
    {
        const PressParams p = params(200, 100);
        const auto buf = Portal::Text::press("Hi", p);
        ASSERT_NE(buf, nullptr);

        const uint32_t content = content_height("Hi", 200);
        const uint32_t expected_h = std::max(std::min(content * 8, 100U), content);
        EXPECT_EQ(buf->get_width(), 200U);
        EXPECT_EQ(buf->get_height(), expected_h);
        EXPECT_EQ(buf->get_budget_width(), 200U);
        EXPECT_EQ(buf->get_budget_height(), expected_h);
        EXPECT_EQ(buf->get_render_bounds_w(), 200U);
        EXPECT_EQ(buf->get_render_bounds_h(), 100U);
        EXPECT_EQ(buf->get_accumulated_text(), "Hi");
        EXPECT_EQ(buf->get_format(), Portal::Graphics::ImageFormat::RGBA8);

        const LayoutResult l = layout("Hi", 200);
        EXPECT_EQ(buf->get_cursor_x(), static_cast<uint32_t>(std::ceil(l.final_pen_x)));
        EXPECT_EQ(buf->get_cursor_y(), static_cast<uint32_t>(std::ceil(l.final_pen_y)));

        std::vector<uint8_t> expected = filled(200, expected_h, p.background);
        Portal::Text::rasterize_quads(l.quads, *atlas, p.color, expected.data(), 200, expected_h);
        EXPECT_EQ(buf->get_pixel_data(), expected);
        EXPECT_GT(ink_count(buf->get_pixel_data()), 0U);
    }

    TEST_F(InkPressTest, PressWithoutAnExplicitAtlasUsesTheFoundryDefault)
    {
        PressParams explicit_atlas = params(120, 80);
        PressParams implicit_atlas = explicit_atlas;
        implicit_atlas.atlas = nullptr;

        const auto a = Portal::Text::press("x", explicit_atlas);
        const auto b = Portal::Text::press("x", implicit_atlas);
        ASSERT_NE(a, nullptr);
        ASSERT_NE(b, nullptr);
        EXPECT_EQ(a->get_pixel_data(), b->get_pixel_data());
    }

    TEST_F(InkPressTest, PressHonoursAnExplicitBudgetHeight)
    {
        PressParams p = params(200, 400);
        p.budget_h = 300;
        const auto buf = Portal::Text::press("a", p);
        ASSERT_NE(buf, nullptr);
        EXPECT_EQ(buf->get_height(), 300U);
        EXPECT_EQ(buf->get_budget_height(), 300U);
    }

    TEST_F(InkPressTest, PressFillsUnwrittenRowsWithTheBackground)
    {
        PressParams p = params(100, 100);
        p.background = { 0.F, 0.F, 1.F, 1.F };
        const auto buf = Portal::Text::press("a", p);
        ASSERT_NE(buf, nullptr);

        EXPECT_EQ(buf->get_background(), p.background);
        EXPECT_EQ(pixel_at(buf->get_pixel_data(), 100, 99, buf->get_height() - 1),
            (std::array<uint8_t, 4> { 0, 0, 255, 255 }));
    }

    TEST_F(InkPressTest, PressWithZeroWidthBoundsProducesNothing)
    {
        EXPECT_EQ(Portal::Text::press("abc", params(0, 100)), nullptr);
    }

    TEST_F(InkPressTest, PressOfEmptyTextIsABlankBuffer)
    {
        const auto buf = Portal::Text::press("", params(100, 100));
        ASSERT_NE(buf, nullptr);
        EXPECT_EQ(ink_count(buf->get_pixel_data()), 0U);
        EXPECT_TRUE(buf->get_accumulated_text().empty());
    }

    TEST_F(InkPressTest, PressWrapsAtTheRenderBoundsWidth)
    {
        const auto adv = static_cast<uint32_t>(atlas->get_or_rasterize(static_cast<FT_ULong>('a'))->advance_x);
        const auto buf = Portal::Text::press("aaaaaaaa", params(3 * adv, 400));
        ASSERT_NE(buf, nullptr);
        EXPECT_GT(buf->get_cursor_y(), line());
    }

    TEST_F(InkPressTest, PressAppliesSpanColoursAndFallsBackToTheBaseColour)
    {
        PressParams p = params(200, 100);
        p.spans = { StyleSpan { .start = 0, .end = 1, .color = k_red } };
        const auto buf = Portal::Text::press("ab", p);
        ASSERT_NE(buf, nullptr);

        const LayoutResult l = layout("ab", 200);
        ASSERT_EQ(l.quads.size(), 2U);

        const auto solid = [&](const GlyphQuad& q, std::array<uint8_t, 3> rgb) {
            size_t seen = 0;
            const auto gx = static_cast<uint32_t>(std::floor(q.x0));
            const auto gy = static_cast<uint32_t>(std::floor(q.y0));
            const auto gw = static_cast<uint32_t>(std::ceil(q.x1 - q.x0));
            const auto gh = static_cast<uint32_t>(std::ceil(q.y1 - q.y0));
            for (uint32_t y = gy; y < gy + gh; ++y) {
                for (uint32_t x = gx; x < gx + gw; ++x) {
                    const auto px = pixel_at(buf->get_pixel_data(), 200, x, y);
                    if (px[3] >= 200) {
                        ++seen;
                        EXPECT_EQ(px[0], rgb[0]);
                        EXPECT_EQ(px[1], rgb[1]);
                        EXPECT_EQ(px[2], rgb[2]);
                    }
                }
            }
            return seen;
        };

        EXPECT_GT(solid(l.quads[0], { 255, 0, 0 }), 0U);
        EXPECT_GT(solid(l.quads[1], { 255, 255, 255 }), 0U);
    }

    // ------------------------------------------------------------------
    // repress
    // ------------------------------------------------------------------

    TEST_F(InkPressTest, RepressReplacesTheContentAsIfTheTextHadBeenPressedFresh)
    {
        const PressParams p = params(200, 100);
        const auto target = Portal::Text::press("hello", p);
        ASSERT_NE(target, nullptr);

        EXPECT_TRUE(Portal::Text::repress(target, "world"));

        const auto fresh = Portal::Text::press("world", p);
        ASSERT_NE(fresh, nullptr);
        EXPECT_EQ(target->get_pixel_data(), fresh->get_pixel_data());
        EXPECT_EQ(target->get_accumulated_text(), "world");
        EXPECT_EQ(target->get_cursor_x(), fresh->get_cursor_x());
        EXPECT_EQ(target->get_cursor_y(), fresh->get_cursor_y());
    }

    TEST_F(InkPressTest, RepressOnANullTargetFails)
    {
        EXPECT_FALSE(Portal::Text::repress(std::shared_ptr<TextBuffer> {}, "x"));
    }

    TEST_F(InkPressTest, RepressClipKeepsTheBudgetWhenTheTextOutgrowsIt)
    {
        PressParams p = params(200, 400);
        p.budget_h = 2 * line();
        const auto target = Portal::Text::press("a", p);
        ASSERT_NE(target, nullptr);
        const uint32_t before = target->get_height();

        EXPECT_TRUE(Portal::Text::repress(target, "a\nb\nc\nd", k_white, RedrawPolicy::Clip));

        EXPECT_EQ(target->get_height(), before);
        EXPECT_EQ(target->get_budget_height(), before);
        EXPECT_EQ(target->get_accumulated_text(), "a\nb\nc\nd");
    }

    TEST_F(InkPressTest, RepressFitGrowsTheTextureToTheContent)
    {
        PressParams p = params(200, 400);
        p.budget_h = 2 * line();
        const auto target = Portal::Text::press("a", p);
        ASSERT_NE(target, nullptr);

        EXPECT_TRUE(Portal::Text::repress(target, "a\nb\nc\nd", k_white, RedrawPolicy::Fit));

        const uint32_t expected = std::min(content_height("a\nb\nc\nd", 200), 400U);
        EXPECT_EQ(target->get_height(), expected);
        EXPECT_EQ(target->get_budget_height(), expected);
        EXPECT_EQ(target->get_accumulated_text(), "a\nb\nc\nd");
        EXPECT_EQ(target->get_pixel_data().size(), static_cast<size_t>(200) * expected * 4);
        EXPECT_GT(ink_count(target->get_pixel_data()), 0U);
    }

    // ------------------------------------------------------------------
    // impress
    // ------------------------------------------------------------------

    TEST_F(InkPressTest, ImpressAppendsRunsAtTheCursorAndAccumulatesTheText)
    {
        const PressParams p = params(200, 400);
        const auto target = Portal::Text::press("", p);
        ASSERT_NE(target, nullptr);
        EXPECT_EQ(target->get_cursor_x(), 0U);
        EXPECT_EQ(target->get_cursor_y(), line());

        EXPECT_EQ(Portal::Text::impress(target, "ab"), ImpressResult::Ok);
        const LayoutResult first = layout("ab", 200);
        EXPECT_EQ(target->get_cursor_x(), static_cast<uint32_t>(std::ceil(first.final_pen_x)));
        EXPECT_EQ(target->get_cursor_y(), static_cast<uint32_t>(std::ceil(first.final_pen_y)));
        EXPECT_EQ(target->get_accumulated_text(), "ab");

        const float pen = static_cast<float>(target->get_cursor_x());
        EXPECT_EQ(Portal::Text::impress(target, "cd"), ImpressResult::Ok);
        const LayoutResult second = layout("cd", 200, pen);
        EXPECT_EQ(target->get_cursor_x(), static_cast<uint32_t>(std::ceil(second.final_pen_x)));
        EXPECT_EQ(target->get_accumulated_text(), "abcd");

        std::vector<uint8_t> expected = filled(target->get_budget_width(), target->get_budget_height(), p.background);
        Portal::Text::rasterize_quads(first.quads, *atlas, k_white, expected.data(),
            target->get_budget_width(), target->get_budget_height());
        Portal::Text::rasterize_quads(second.quads, *atlas, k_white, expected.data(),
            target->get_budget_width(), target->get_budget_height());
        EXPECT_EQ(target->get_pixel_data(), expected);
        EXPECT_TRUE(target->is_texture_dirty());
    }

    TEST_F(InkPressTest, ImpressOnANullTargetReportsOverflow)
    {
        EXPECT_EQ(Portal::Text::impress(std::shared_ptr<TextBuffer> {}, "x"), ImpressResult::Overflow);
    }

    TEST_F(InkPressTest, ImpressOfANewlineAdvancesTheCursorByOneLine)
    {
        const auto target = Portal::Text::press("", params(200, 400));
        ASSERT_NE(target, nullptr);
        const uint32_t before = target->get_cursor_y();

        Portal::Text::impress(target, "\n");

        EXPECT_EQ(target->get_cursor_y(), before + line());
        EXPECT_EQ(target->get_cursor_x(), 0U);
    }

    TEST_F(InkPressTest, ImpressOfABareSpaceAdvancesTheCursorByTheSpaceAdvance)
    {
        const auto target = Portal::Text::press("", params(200, 400));
        ASSERT_NE(target, nullptr);

        Portal::Text::impress(target, " ");

        const auto space = static_cast<uint32_t>(atlas->get_or_rasterize(static_cast<FT_ULong>(' '))->advance_x);
        EXPECT_EQ(target->get_cursor_x(), space);
        EXPECT_EQ(target->get_accumulated_text(), " ");
    }

    TEST_F(InkPressTest, ImpressBeyondTheRenderBoundsOverflowsWithoutTouchingPixels)
    {
        const auto target = Portal::Text::press("", params(200, 2 * line()));
        ASSERT_NE(target, nullptr);
        const std::vector<uint8_t> before = target->get_pixel_data();

        EXPECT_EQ(Portal::Text::impress(target, "a\nb"), ImpressResult::Overflow);

        EXPECT_EQ(target->get_pixel_data(), before);
        EXPECT_EQ(target->get_height(), 2 * line());
    }

    TEST_F(InkPressTest, ImpressGrowsWithinTheBoundsAndRepaintsAsIfPressedFresh)
    {
        PressParams p = params(200, 400);
        p.budget_h = 2 * line();
        const auto target = Portal::Text::press("", p);
        ASSERT_NE(target, nullptr);

        EXPECT_EQ(Portal::Text::impress(target, "a\nb"), ImpressResult::Overflow);

        const uint32_t grown = std::min(3 * line() * 8, 400U);
        EXPECT_EQ(target->get_height(), grown);
        EXPECT_EQ(target->get_budget_height(), grown);
        EXPECT_EQ(target->get_accumulated_text(), "a\nb");

        PressParams fresh_params = params(200, 400);
        fresh_params.budget_h = grown;
        const auto fresh = Portal::Text::press("a\nb", fresh_params);
        ASSERT_NE(fresh, nullptr);
        EXPECT_EQ(target->get_pixel_data(), fresh->get_pixel_data());
    }

} // namespace
} // namespace MayaFlux::Test
