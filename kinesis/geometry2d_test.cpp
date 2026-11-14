#include "gtest/gtest.h"
#include "../support/test_metadata.hpp"

#include "MayaFlux/Kinesis/Geometry2D.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {

namespace {

    using Tri = std::array<uint32_t, 3>;

    float triangle_area2(glm::vec2 a, glm::vec2 b, glm::vec2 c)
    {
        return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
    }

    float total_area(std::span<const glm::vec2> pts, const std::vector<Tri>& tris)
    {
        float sum = 0.F;
        for (const Tri& t : tris) {
            sum += 0.5F * std::abs(triangle_area2(pts[t[0]], pts[t[1]], pts[t[2]]));
        }
        return sum;
    }

    std::vector<glm::vec2> square_ccw()
    {
        return { { 0.F, 0.F }, { 1.F, 0.F }, { 1.F, 1.F }, { 0.F, 1.F } };
    }

    std::vector<glm::vec2> dart()
    {
        return { { 0.F, 0.F }, { 0.5F, 0.5F }, { 1.F, 0.F }, { 0.5F, 1.F } };
    }

    std::vector<glm::vec2> regular(uint32_t sides)
    {
        std::vector<glm::vec2> pts;
        for (uint32_t i = 0; i < sides; ++i) {
            const float a = glm::two_pi<float>() * static_cast<float>(i) / static_cast<float>(sides);
            pts.emplace_back(std::cos(a), std::sin(a));
        }
        return pts;
    }

}

// =============================================================================
// triangulate_polygon
// =============================================================================

TEST(Geometry2DTriangulatePolygonTest, ASquareIsTwoTrianglesCoveringItsArea)
{
    const auto pts = square_ccw();
    const auto tris = Kinesis::triangulate_polygon(pts);

    ASSERT_EQ(tris.size(), 2U);
    EXPECT_NEAR(total_area(pts, tris), 1.F, 1e-5F);
    for (const Tri& t : tris) {
        for (uint32_t k : t) {
            EXPECT_LT(k, pts.size());
        }
    }
}

TEST(Geometry2DTriangulatePolygonTest, ClockwiseInputGivesTheSameCoverage)
{
    auto pts = square_ccw();
    std::ranges::reverse(pts);
    const auto tris = Kinesis::triangulate_polygon(pts);

    ASSERT_EQ(tris.size(), 2U);
    EXPECT_NEAR(total_area(pts, tris), 1.F, 1e-5F);
}

TEST(Geometry2DTriangulatePolygonTest, AConcaveDartIsTriangulatedWithoutLosingArea)
{
    const auto pts = dart();
    const auto tris = Kinesis::triangulate_polygon(pts);

    ASSERT_EQ(tris.size(), 2U);
    EXPECT_NEAR(total_area(pts, tris), 0.25F, 1e-5F);
}

TEST(Geometry2DTriangulatePolygonTest, ARegularPolygonHasNMinusTwoTrianglesAndItsArea)
{
    const auto pts = regular(32);
    const auto tris = Kinesis::triangulate_polygon(pts);

    ASSERT_EQ(tris.size(), 30U);
    const float expected = 0.5F * 32.F * std::sin(glm::two_pi<float>() / 32.F);
    EXPECT_NEAR(total_area(pts, tris), expected, 1e-4F);
}

TEST(Geometry2DTriangulatePolygonTest, FewerThanThreePointsGiveNothing)
{
    const std::vector<glm::vec2> none;
    const std::vector<glm::vec2> two { { 0.F, 0.F }, { 1.F, 1.F } };

    EXPECT_TRUE(Kinesis::triangulate_polygon(none).empty());
    EXPECT_TRUE(Kinesis::triangulate_polygon(two).empty());
}

TEST(Geometry2DTriangulatePolygonTest, ACollinearOutlineGivesNothing)
{
    const std::vector<glm::vec2> line { { 0.F, 0.F }, { 1.F, 0.F }, { 2.F, 0.F } };

    EXPECT_TRUE(Kinesis::triangulate_polygon(line).empty());
}

TEST(Geometry2DTriangulatePolygonTest, ASelfIntersectingBowtieGivesNothing)
{
    const std::vector<glm::vec2> bowtie { { 0.F, 0.F }, { 1.F, 1.F }, { 1.F, 0.F }, { 0.F, 1.F } };

    EXPECT_TRUE(Kinesis::triangulate_polygon(bowtie).empty());
}

// =============================================================================
// fill_polygon
// =============================================================================

TEST(Geometry2DFillPolygonTest, EmitsThreeVerticesPerTriangleWithTheGivenColor)
{
    const auto pts = square_ccw();
    const glm::vec4 color { 0.2F, 0.4F, 0.6F, 0.8F };
    const auto verts = Kinesis::fill_polygon(pts, color);

    ASSERT_EQ(verts.size(), 6U);
    for (const auto& v : verts) {
        EXPECT_EQ(v.color, color);
        EXPECT_FLOAT_EQ(v.position.z, 0.F);
    }
}

TEST(Geometry2DFillPolygonTest, VertexPositionsMatchTheTriangulation)
{
    const auto pts = dart();
    const auto tris = Kinesis::triangulate_polygon(pts);
    const auto verts = Kinesis::fill_polygon(pts);

    ASSERT_EQ(verts.size(), tris.size() * 3);
    for (size_t i = 0; i < tris.size(); ++i) {
        for (size_t k = 0; k < 3; ++k) {
            const glm::vec2 expected = pts[tris[i][k]];
            EXPECT_FLOAT_EQ(verts[i * 3 + k].position.x, expected.x);
            EXPECT_FLOAT_EQ(verts[i * 3 + k].position.y, expected.y);
        }
    }
}

TEST(Geometry2DFillPolygonTest, InvalidOutlinesEmitNothing)
{
    const std::vector<glm::vec2> bowtie { { 0.F, 0.F }, { 1.F, 1.F }, { 1.F, 0.F }, { 0.F, 1.F } };
    const std::vector<glm::vec2> two { { 0.F, 0.F }, { 1.F, 1.F } };

    EXPECT_TRUE(Kinesis::fill_polygon(bowtie).empty());
    EXPECT_TRUE(Kinesis::fill_polygon(two).empty());
}

// =============================================================================
// Named filled shapes
// =============================================================================

namespace {

    void expect_at(glm::vec2 actual, glm::vec2 expected)
    {
        EXPECT_NEAR(actual.x, expected.x, 1e-5F);
        EXPECT_NEAR(actual.y, expected.y, 1e-5F);
    }

    glm::vec2 xy(glm::vec3 p)
    {
        return { p.x, p.y };
    }

    glm::vec2 xy(const Kakshya::LineVertex& v)
    {
        return xy(v.position);
    }

    glm::vec2 xy(const Kakshya::Vertex& v)
    {
        return xy(v.position);
    }

}

TEST(Geometry2DFilledCircleTest, SegmentsBelowThreeAreClampedToThree)
{
    const auto verts = Kinesis::filled_circle({ 0.F, 0.F }, 1.F, 0);
    EXPECT_EQ(verts.size(), 9U);
}

TEST(Geometry2DFilledCircleTest, EachFanHasItsCentreAndTwoRimPointsOnTheCircle)
{
    const glm::vec2 center { 0.5F, -0.5F };
    const auto verts = Kinesis::filled_circle(center, 2.F, 6);

    ASSERT_EQ(verts.size(), 18U);
    for (size_t i = 0; i < 6; ++i) {
        expect_at(xy(verts[i * 3]), center);
        EXPECT_NEAR(glm::distance(xy(verts[i * 3 + 1]), center), 2.F, 1e-4F);
        EXPECT_NEAR(glm::distance(xy(verts[i * 3 + 2]), center), 2.F, 1e-4F);
    }
}

TEST(Geometry2DFilledCircleTest, NeighbouringFansShareTheirRimPoint)
{
    const auto verts = Kinesis::filled_circle({ 0.F, 0.F }, 1.F, 5);

    for (size_t i = 0; i + 1 < 5; ++i) {
        expect_at(xy(verts[i * 3 + 2]), xy(verts[(i + 1) * 3 + 1]));
    }
}

TEST(Geometry2DFilledRingTest, EachQuadAlternatesInnerAndOuterRims)
{
    const glm::vec2 center { 0.F, 0.F };
    const auto verts = Kinesis::filled_ring(center, 0.5F, 1.F, 4);

    ASSERT_EQ(verts.size(), 24U);
    for (size_t q = 0; q < 4; ++q) {
        const size_t base = q * 6;
        EXPECT_NEAR(glm::length(xy(verts[base])), 0.5F, 1e-4F);
        EXPECT_NEAR(glm::length(xy(verts[base + 1])), 1.F, 1e-4F);
        EXPECT_NEAR(glm::length(xy(verts[base + 2])), 1.F, 1e-4F);
        EXPECT_NEAR(glm::length(xy(verts[base + 3])), 0.5F, 1e-4F);
        EXPECT_NEAR(glm::length(xy(verts[base + 4])), 1.F, 1e-4F);
        EXPECT_NEAR(glm::length(xy(verts[base + 5])), 0.5F, 1e-4F);
    }
}

TEST(Geometry2DFilledRingTest, SegmentsBelowThreeAreClampedToThree)
{
    EXPECT_EQ(Kinesis::filled_ring({ 0.F, 0.F }, 0.2F, 0.4F, 1).size(), 18U);
}

TEST(Geometry2DFilledPolygonTest, SidesBelowThreeAreClampedAndStartAtRotation)
{
    const auto verts = Kinesis::filled_polygon({ 0.F, 0.F }, 1.F, 2, 0.F);

    ASSERT_EQ(verts.size(), 9U);
    expect_at(xy(verts[1]), { 1.F, 0.F });
}

TEST(Geometry2DFilledPolygonTest, EveryRimPointLiesOnTheCircumcircle)
{
    const auto verts = Kinesis::filled_polygon({ 0.F, 0.F }, 2.F, 5, 0.3F);

    ASSERT_EQ(verts.size(), 15U);
    for (size_t i = 0; i < 5; ++i) {
        EXPECT_NEAR(glm::length(xy(verts[i * 3 + 1])), 2.F, 1e-4F);
        EXPECT_NEAR(glm::length(xy(verts[i * 3 + 2])), 2.F, 1e-4F);
    }
}

TEST(Geometry2DFilledRectGradientTest, CornersFollowTheDocumentedOrder)
{
    const Kinesis::AABB2D region { .min = { -1.F, -2.F }, .max = { 3.F, 4.F } };
    const glm::vec4 bl { 1.F, 0.F, 0.F, 1.F };
    const glm::vec4 br { 0.F, 1.F, 0.F, 1.F };
    const glm::vec4 tl { 0.F, 0.F, 1.F, 1.F };
    const glm::vec4 tr { 1.F, 1.F, 1.F, 1.F };

    const auto verts = Kinesis::filled_rect_gradient(region, bl, br, tl, tr);

    expect_at(xy(verts[0]), { -1.F, -2.F });
    expect_at(xy(verts[1]), { -1.F, 4.F });
    expect_at(xy(verts[2]), { 3.F, -2.F });
    expect_at(xy(verts[3]), { 3.F, 4.F });
    EXPECT_EQ(verts[0].color, bl);
    EXPECT_EQ(verts[1].color, tl);
    EXPECT_EQ(verts[2].color, br);
    EXPECT_EQ(verts[3].color, tr);
}

TEST(Geometry2DFilledRoundedRectTest, VertexCountIsBodyPlusCornerFans)
{
    const Kinesis::AABB2D region { .min = { 0.F, 0.F }, .max = { 4.F, 2.F } };

    EXPECT_EQ(Kinesis::filled_rounded_rect(region, 0.5F, 2).size(), 18U + 12U * 2U);
    EXPECT_EQ(Kinesis::filled_rounded_rect(region, 0.5F, 0).size(), 18U + 12U * 1U);
}

TEST(Geometry2DFilledRoundedRectTest, AnOversizedRadiusStillFillsExactlyTheRegion)
{
    const Kinesis::AABB2D region { .min = { 0.F, 0.F }, .max = { 4.F, 2.F } };
    const auto verts = Kinesis::filled_rounded_rect(region, 100.F, 4);

    glm::vec2 lo(std::numeric_limits<float>::max());
    glm::vec2 hi(std::numeric_limits<float>::lowest());
    for (const auto& v : verts) {
        lo = glm::min(lo, xy(v));
        hi = glm::max(hi, xy(v));
    }
    expect_at(lo, region.min);
    expect_at(hi, region.max);
}

TEST(Geometry2DFilledArcTest, SpansFromStartToEndAroundTheCentre)
{
    const auto verts = Kinesis::filled_arc({ 0.F, 0.F }, 1.F, 0.F, glm::pi<float>(), 4);

    ASSERT_EQ(verts.size(), 12U);
    expect_at(xy(verts[0]), { 0.F, 0.F });
    expect_at(xy(verts[1]), { 1.F, 0.F });
    expect_at(xy(verts[11]), { -1.F, 0.F });
}

TEST(Geometry2DFilledArcTest, SegmentsBelowOneAreClampedToOne)
{
    EXPECT_EQ(Kinesis::filled_arc({ 0.F, 0.F }, 1.F, 0.F, 1.F, 0).size(), 3U);
}

// =============================================================================
// Paths and outlines
// =============================================================================

TEST(Geometry2DArcPathTest, ProducesSegmentsPlusOnePointsIncludingBothEnds)
{
    const auto pts = Kinesis::arc_path({ 0.F, 0.F }, 2.F, 1.F, 0.F, glm::pi<float>(), 4);

    ASSERT_EQ(pts.size(), 5U);
    expect_at(pts[0], { 2.F, 0.F });
    expect_at(pts[2], { 0.F, 1.F });
    EXPECT_NEAR(pts[4].x, -2.F, 1e-4F);
    EXPECT_NEAR(pts[4].y, 0.F, 1e-4F);
}

TEST(Geometry2DArcPathTest, ZeroSegmentsGivesTwoPoints)
{
    EXPECT_EQ(Kinesis::arc_path({ 0.F, 0.F }, 1.F, 1.F, 0.F, 1.F, 0).size(), 2U);
}

TEST(Geometry2DCircleOutlineTest, IsAClosedLoopOfAdjacentPairs)
{
    const auto verts = Kinesis::circle_outline({ 0.F, 0.F }, 1.F, 4, glm::vec4(1.F), 2.F);

    ASSERT_EQ(verts.size(), 8U);
    expect_at(xy(verts[1]), xy(verts[2]));
    expect_at(xy(verts[7]), xy(verts[0]));
    for (const auto& v : verts) {
        EXPECT_FLOAT_EQ(v.thickness, 2.F);
    }
}

TEST(Geometry2DCircleOutlineTest, SegmentsBelowThreeAreClampedToThree)
{
    EXPECT_EQ(Kinesis::circle_outline({ 0.F, 0.F }, 1.F, 1).size(), 6U);
}

TEST(Geometry2DArcOutlineTest, RunsFromStartToEndAsAChain)
{
    const auto verts = Kinesis::arc_outline({ 0.F, 0.F }, 1.F, 0.F, glm::half_pi<float>(), 2);

    ASSERT_EQ(verts.size(), 4U);
    expect_at(xy(verts[0]), { 1.F, 0.F });
    expect_at(xy(verts[1]), xy(verts[2]));
    expect_at(xy(verts[3]), { 0.F, 1.F });
}

TEST(Geometry2DArcOutlineTest, ZeroSegmentsIsClampedToOne)
{
    EXPECT_EQ(Kinesis::arc_outline({ 0.F, 0.F }, 1.F, 0.F, 1.F, 0).size(), 2U);
}

TEST(Geometry2DRectOutlineTest, TracesFourEdgesInOrder)
{
    const Kinesis::AABB2D region { .min = { 0.F, 0.F }, .max = { 2.F, 1.F } };
    const auto verts = Kinesis::rect_outline(region);

    expect_at(xy(verts[0]), { 0.F, 0.F });
    expect_at(xy(verts[1]), { 2.F, 0.F });
    expect_at(xy(verts[3]), { 2.F, 1.F });
    expect_at(xy(verts[5]), { 0.F, 1.F });
    expect_at(xy(verts[7]), { 0.F, 0.F });
    for (size_t i = 1; i + 1 < 7; i += 2) {
        expect_at(xy(verts[i]), xy(verts[i + 1]));
    }
}

TEST(Geometry2DPolylineTest, PairsEachPointWithItsNeighbour)
{
    const std::vector<glm::vec2> pts { { 0.F, 0.F }, { 1.F, 0.F }, { 1.F, 1.F } };
    const auto verts = Kinesis::polyline(pts);

    ASSERT_EQ(verts.size(), 4U);
    expect_at(xy(verts[0]), { 0.F, 0.F });
    expect_at(xy(verts[1]), { 1.F, 0.F });
    expect_at(xy(verts[2]), { 1.F, 0.F });
    expect_at(xy(verts[3]), { 1.F, 1.F });
}

TEST(Geometry2DPolylineTest, FewerThanTwoPointsGiveNothing)
{
    const std::vector<glm::vec2> one { { 0.F, 0.F } };

    EXPECT_TRUE(Kinesis::polyline(std::span<const glm::vec2>()).empty());
    EXPECT_TRUE(Kinesis::polyline(one).empty());
}

TEST(Geometry2DPolylineColoredTest, EachEndpointKeepsItsOwnColour)
{
    const std::vector<glm::vec2> pts { { 0.F, 0.F }, { 1.F, 0.F }, { 1.F, 1.F } };
    const std::vector<glm::vec4> colors { { 1.F, 0.F, 0.F, 1.F }, { 0.F, 1.F, 0.F, 1.F }, { 0.F, 0.F, 1.F, 1.F } };
    const auto verts = Kinesis::polyline_colored(pts, colors);

    ASSERT_EQ(verts.size(), 4U);
    EXPECT_EQ(verts[0].color, colors[0]);
    EXPECT_EQ(verts[1].color, colors[1]);
    EXPECT_EQ(verts[2].color, colors[1]);
    EXPECT_EQ(verts[3].color, colors[2]);
}

TEST(Geometry2DPolylineColoredTest, MismatchedOrTooShortInputGivesNothing)
{
    const std::vector<glm::vec2> pts { { 0.F, 0.F }, { 1.F, 0.F } };
    const std::vector<glm::vec4> colors { glm::vec4(1.F) };
    const std::vector<glm::vec2> one { glm::vec2(0.F) };
    const std::vector<glm::vec4> one_color { glm::vec4(1.F) };

    EXPECT_TRUE(Kinesis::polyline_colored(pts, colors).empty());
    EXPECT_TRUE(Kinesis::polyline_colored(one, one_color).empty());
}

TEST(Geometry2DPolygonOutlineTest, IsAClosedLoopOfTheRequestedSides)
{
    const auto verts = Kinesis::polygon_outline({ 0.F, 0.F }, 1.F, 4, 0.F);

    ASSERT_EQ(verts.size(), 8U);
    expect_at(xy(verts[0]), { 1.F, 0.F });
    expect_at(xy(verts[1]), xy(verts[2]));
    expect_at(xy(verts[7]), xy(verts[0]));
}

TEST(Geometry2DPolygonOutlineTest, SidesBelowThreeAreClampedToThree)
{
    EXPECT_EQ(Kinesis::polygon_outline({ 0.F, 0.F }, 1.F, 1).size(), 6U);
}

// =============================================================================
// triangulate_polygon with holes
// =============================================================================

TEST(Geometry2DTriangulateHolesTest, ASquareWithASquareHoleCoversTheRemainingArea)
{
    std::vector<glm::vec2> pts = square_ccw();
    pts.insert(pts.end(), { { 0.3F, 0.3F }, { 0.7F, 0.3F }, { 0.7F, 0.7F }, { 0.3F, 0.7F } });
    const std::vector<uint32_t> starts { 4 };

    const auto tris = Kinesis::triangulate_polygon(pts, starts);

    ASSERT_EQ(tris.size(), 8U);
    EXPECT_NEAR(total_area(pts, tris), 0.84F, 1e-5F);
}

TEST(Geometry2DTriangulateHolesTest, HoleWindingDoesNotChangeTheResult)
{
    std::vector<glm::vec2> pts = square_ccw();
    pts.insert(pts.end(), { { 0.3F, 0.7F }, { 0.7F, 0.7F }, { 0.7F, 0.3F }, { 0.3F, 0.3F } });
    const std::vector<uint32_t> starts { 4 };

    const auto tris = Kinesis::triangulate_polygon(pts, starts);

    ASSERT_EQ(tris.size(), 8U);
    EXPECT_NEAR(total_area(pts, tris), 0.84F, 1e-5F);
}

TEST(Geometry2DTriangulateHolesTest, TwoHolesAreBothCut)
{
    std::vector<glm::vec2> pts {
        { 0.F, 0.F }, { 4.F, 0.F }, { 4.F, 1.F }, { 0.F, 1.F },
        { 1.F, 0.25F }, { 1.5F, 0.25F }, { 1.5F, 0.75F }, { 1.F, 0.75F },
        { 2.5F, 0.25F }, { 3.F, 0.25F }, { 3.F, 0.75F }, { 2.5F, 0.75F },
    };
    const std::vector<uint32_t> starts { 4, 8 };

    const auto tris = Kinesis::triangulate_polygon(pts, starts);

    ASSERT_EQ(tris.size(), 14U);
    EXPECT_NEAR(total_area(pts, tris), 3.5F, 1e-4F);
}

TEST(Geometry2DTriangulateHolesTest, InvalidHolesGiveNothing)
{
    const std::vector<uint32_t> starts { 4 };

    std::vector<glm::vec2> outside = square_ccw();
    outside.insert(outside.end(), { { 2.F, 2.F }, { 3.F, 2.F }, { 3.F, 3.F }, { 2.F, 3.F } });
    EXPECT_TRUE(Kinesis::triangulate_polygon(outside, starts).empty());

    std::vector<glm::vec2> touching = square_ccw();
    touching.insert(touching.end(), { { 0.F, 0.3F }, { 0.5F, 0.3F }, { 0.5F, 0.7F }, { 0.F, 0.7F } });
    EXPECT_TRUE(Kinesis::triangulate_polygon(touching, starts).empty());

    std::vector<glm::vec2> crossing = square_ccw();
    crossing.insert(crossing.end(), { { 0.5F, 0.5F }, { 1.5F, 0.5F }, { 1.5F, 0.7F }, { 0.5F, 0.7F } });
    EXPECT_TRUE(Kinesis::triangulate_polygon(crossing, starts).empty());
}

TEST(Geometry2DTriangulateHolesTest, AHoleStartBeforeTheOuterRingIsRejected)
{
    std::vector<glm::vec2> pts = square_ccw();
    pts.insert(pts.end(), { { 0.3F, 0.3F }, { 0.7F, 0.3F }, { 0.7F, 0.7F }, { 0.3F, 0.7F } });
    const std::vector<uint32_t> starts { 0 };

    EXPECT_TRUE(Kinesis::triangulate_polygon(pts, starts).empty());
}

} // namespace MayaFlux::Test
