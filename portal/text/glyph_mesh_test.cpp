#include "gtest/gtest.h"

#include <cmath>
#include <vector>

#include "MayaFlux/Portal/Text/GlyphMesh.hpp"

namespace MayaFlux::Test {
namespace {

    using Portal::Text::coverage_weight;
    using Portal::Text::glyph_vertices;
    using Portal::Text::GlyphMap;
    using Portal::Text::GlyphQuad;
    using Portal::Text::StyleSpan;

    const glm::vec4 k_white { 1.F, 1.F, 1.F, 1.F };
    const glm::vec4 k_red { 1.F, 0.F, 0.F, 1.F };
    const glm::vec4 k_green { 0.F, 1.F, 0.F, 1.F };

    GlyphQuad quad_at(size_t byte_offset)
    {
        GlyphQuad q {};
        q.x0 = 10.F;
        q.y0 = 20.F;
        q.x1 = 30.F;
        q.y1 = 40.F;
        q.uv_x0 = 0.1F;
        q.uv_y0 = 0.2F;
        q.uv_x1 = 0.3F;
        q.uv_y1 = 0.4F;
        q.byte_offset = byte_offset;
        return q;
    }

    TEST(GlyphMeshTest, CoverageWeightIsNegativeAndSelectsTheSlotAfterIt)
    {
        EXPECT_FLOAT_EQ(coverage_weight(0), -1.F);
        EXPECT_FLOAT_EQ(coverage_weight(7), -8.F);
    }

    TEST(GlyphMeshTest, FitMapsTheSourceCornersOntoTheRegionWithYFlipped)
    {
        const auto map = GlyphMap::fit({ .min = { -1.F, -1.F }, .max = { 1.F, 1.F } }, { 200.F, 100.F });

        const glm::vec2 top_left = map({ 0.F, 0.F });
        const glm::vec2 bottom_right = map({ 200.F, 100.F });
        EXPECT_FLOAT_EQ(top_left.x, -1.F);
        EXPECT_FLOAT_EQ(top_left.y, 1.F);
        EXPECT_FLOAT_EQ(bottom_right.x, 1.F);
        EXPECT_FLOAT_EQ(bottom_right.y, -1.F);
    }

    TEST(GlyphMeshTest, FitTreatsASourceBelowOnePixelAsOne)
    {
        const auto map = GlyphMap::fit({ .min = { 0.F, 0.F }, .max = { 2.F, 2.F } }, { 0.F, 0.F });

        EXPECT_TRUE(std::isfinite(map.scale.x));
        EXPECT_TRUE(std::isfinite(map.scale.y));
        EXPECT_FLOAT_EQ(map.scale.x, 2.F);
        EXPECT_FLOAT_EQ(map.scale.y, -2.F);
    }

    TEST(GlyphMeshTest, NoQuadsYieldNoVertices)
    {
        EXPECT_TRUE(glyph_vertices({}, k_white, {}, {}, 0).empty());
    }

    TEST(GlyphMeshTest, EachQuadBecomesTwoTrianglesCarryingItsUVsAndTheCoverageWeight)
    {
        const std::vector<GlyphQuad> quads { quad_at(0) };

        const auto v = glyph_vertices(quads, k_red, {}, {}, 2);

        ASSERT_EQ(v.size(), 6U);
        EXPECT_FLOAT_EQ(v[0].position.x, 10.F);
        EXPECT_FLOAT_EQ(v[0].position.y, 40.F);
        EXPECT_FLOAT_EQ(v[0].uv.x, 0.1F);
        EXPECT_FLOAT_EQ(v[0].uv.y, 0.4F);
        EXPECT_FLOAT_EQ(v[1].position.x, 30.F);
        EXPECT_FLOAT_EQ(v[1].position.y, 40.F);
        EXPECT_FLOAT_EQ(v[2].position.x, 10.F);
        EXPECT_FLOAT_EQ(v[2].position.y, 20.F);
        EXPECT_FLOAT_EQ(v[2].uv.y, 0.2F);
        EXPECT_FLOAT_EQ(v[4].position.x, 30.F);
        EXPECT_FLOAT_EQ(v[4].position.y, 20.F);
        EXPECT_FLOAT_EQ(v[4].uv.x, 0.3F);
        EXPECT_EQ(v[3].position, v[1].position);
        EXPECT_EQ(v[5].position, v[2].position);

        for (const auto& vertex : v) {
            EXPECT_FLOAT_EQ(vertex.weight, coverage_weight(2));
            EXPECT_EQ(vertex.color, glm::vec4(glm::vec3(1.F, 0.F, 0.F), 1.F));
        }
    }

    TEST(GlyphMeshTest, PositionsGoThroughTheMap)
    {
        const std::vector<GlyphQuad> quads { quad_at(0) };
        const GlyphMap map { .origin = { 100.F, 200.F }, .scale = { 2.F, -1.F } };

        const auto v = glyph_vertices(quads, k_white, {}, map, 0);

        ASSERT_EQ(v.size(), 6U);
        EXPECT_FLOAT_EQ(v[2].position.x, 120.F);
        EXPECT_FLOAT_EQ(v[2].position.y, 180.F);
        EXPECT_FLOAT_EQ(v[4].position.x, 160.F);
        EXPECT_FLOAT_EQ(v[0].position.y, 160.F);
    }

    TEST(GlyphMeshTest, ASpanColoursTheQuadsInItsRangeAndTheRestKeepTheBaseColour)
    {
        const std::vector<GlyphQuad> quads { quad_at(0), quad_at(5) };
        const std::vector<StyleSpan> spans { { .start = 0, .end = 3, .color = k_green } };

        const auto v = glyph_vertices(quads, k_red, spans, {}, 0);

        ASSERT_EQ(v.size(), 12U);
        EXPECT_EQ(v[0].color, glm::vec4(glm::vec3(0.F, 1.F, 0.F), 1.F));
        EXPECT_EQ(v[6].color, glm::vec4(glm::vec3(1.F, 0.F, 0.F), 1.F));
    }

    TEST(GlyphMeshTest, TheLaterOfTwoOverlappingSpansWins)
    {
        const std::vector<GlyphQuad> quads { quad_at(1) };
        const std::vector<StyleSpan> spans {
            { .start = 0, .end = 4, .color = k_red },
            { .start = 0, .end = 4, .color = k_green },
        };

        const auto v = glyph_vertices(quads, k_white, spans, {}, 0);

        ASSERT_EQ(v.size(), 6U);
        EXPECT_EQ(v[0].color, glm::vec4(glm::vec3(0.F, 1.F, 0.F), 1.F));
    }

    TEST(GlyphMeshTest, ASpanWithReversedBoundsCoversTheSameRange)
    {
        const std::vector<GlyphQuad> quads { quad_at(1) };
        const std::vector<StyleSpan> spans { { .start = 4, .end = 0, .color = k_green } };

        const auto v = glyph_vertices(quads, k_red, spans, {}, 0);

        ASSERT_EQ(v.size(), 6U);
        EXPECT_EQ(v[0].color, glm::vec4(glm::vec3(0.F, 1.F, 0.F), 1.F));
    }

} // namespace
} // namespace MayaFlux::Test
