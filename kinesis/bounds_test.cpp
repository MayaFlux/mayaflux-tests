#include "../test_config.h"

#include "MayaFlux/Kinesis/Spatial/Bounds.hpp"

namespace MayaFlux::Test {

using Kinesis::AABB2D;
using Kinesis::AABB3D;
using Kinesis::Align;
using Kinesis::BoundingSphere;
using Kinesis::Side;

namespace {

    void expect_near(const AABB2D& a, const AABB2D& b)
    {
        EXPECT_NEAR(a.min.x, b.min.x, 1e-5F);
        EXPECT_NEAR(a.min.y, b.min.y, 1e-5F);
        EXPECT_NEAR(a.max.x, b.max.x, 1e-5F);
        EXPECT_NEAR(a.max.y, b.max.y, 1e-5F);
    }

    struct Anchor {
        AABB2D box;
        [[nodiscard]] AABB2D bounds() const { return box; }
    };

    static_assert(Kinesis::HasBounds<Anchor>);
    static_assert(!Kinesis::HasBounds<int>);

}

// =============================================================================
// AABB2D
// =============================================================================

TEST(AABB2DTest, ContainsIsInclusiveOnEveryEdge)
{
    const AABB2D box { .min = { -1.F, -2.F }, .max = { 3.F, 4.F } };

    EXPECT_TRUE(box.contains({ 0.F, 0.F }));
    EXPECT_TRUE(box.contains({ -1.F, -2.F }));
    EXPECT_TRUE(box.contains({ 3.F, 4.F }));
    EXPECT_FALSE(box.contains({ 3.01F, 0.F }));
    EXPECT_FALSE(box.contains({ 0.F, -2.01F }));
}

TEST(AABB2DTest, OverlapsCountsTouchingEdgesButNotGaps)
{
    const AABB2D box { .min = { 0.F, 0.F }, .max = { 2.F, 2.F } };

    EXPECT_TRUE(box.overlaps({ .min = { 1.F, 1.F }, .max = { 3.F, 3.F } }));
    EXPECT_TRUE(box.overlaps({ .min = { 2.F, 0.F }, .max = { 4.F, 2.F } }));
    EXPECT_TRUE(box.overlaps({ .min = { 0.5F, 0.5F }, .max = { 1.F, 1.F } }));
    EXPECT_FALSE(box.overlaps({ .min = { 2.01F, 0.F }, .max = { 4.F, 2.F } }));
    EXPECT_FALSE(box.overlaps({ .min = { 0.F, -3.F }, .max = { 2.F, -0.01F } }));
}

TEST(AABB2DTest, ExtentsAndCentre)
{
    const AABB2D box { .min = { -1.F, -2.F }, .max = { 3.F, 4.F } };

    EXPECT_FLOAT_EQ(box.width(), 4.F);
    EXPECT_FLOAT_EQ(box.height(), 6.F);
    EXPECT_EQ(box.center(), glm::vec2(1.F, 1.F));
}

TEST(AABB2DTest, TranslatedMovesBothCorners)
{
    const AABB2D box { .min = { 0.F, 0.F }, .max = { 1.F, 2.F } };

    expect_near(box.translated({ 3.F, -1.F }), AABB2D { .min = { 3.F, -1.F }, .max = { 4.F, 1.F } });
}

TEST(AABB2DTest, ExpandedGrowsOnEverySideAndANegativeMarginShrinks)
{
    const AABB2D box { .min = { 0.F, 0.F }, .max = { 2.F, 2.F } };

    expect_near(box.expanded(0.5F), AABB2D { .min = { -0.5F, -0.5F }, .max = { 2.5F, 2.5F } });
    expect_near(box.expanded(-0.5F), AABB2D { .min = { 0.5F, 0.5F }, .max = { 1.5F, 1.5F } });
}

TEST(AABB2DTest, FromNdcBuildsACentredBox)
{
    expect_near(AABB2D::from_ndc({ 1.F, 1.F }, { 0.5F, 2.F }),
        AABB2D { .min = { 0.5F, -1.F }, .max = { 1.5F, 3.F } });
}

TEST(AABB2DTest, FromPixelUsesATopLeftOriginAndFlipsY)
{
    expect_near(AABB2D::from_pixel(0.F, 0.F, 200.F, 100.F, 200, 100),
        AABB2D { .min = { -1.F, -1.F }, .max = { 1.F, 1.F } });
    expect_near(AABB2D::from_pixel(50.F, 25.F, 100.F, 50.F, 200, 100),
        AABB2D { .min = { -0.5F, -0.5F }, .max = { 0.5F, 0.5F } });
    expect_near(AABB2D::from_pixel(0.F, 0.F, 100.F, 50.F, 200, 100),
        AABB2D { .min = { -1.F, 0.F }, .max = { 0.F, 1.F } });
}

TEST(AABB2DTest, FromBufferTransformTreatsScaleAsTheFullExtent)
{
    expect_near(AABB2D::from_buffer_transform({ 0.5F, 0.F }, { 1.F, 2.F }),
        AABB2D { .min = { 0.F, -1.F }, .max = { 1.F, 1.F } });
}

// =============================================================================
// AABB3D and BoundingSphere
// =============================================================================

TEST(AABB3DTest, ContainsOverlapsAndMeasures)
{
    const AABB3D box { .min = { 0.F, 0.F, 0.F }, .max = { 2.F, 4.F, 6.F } };

    EXPECT_TRUE(box.contains({ 1.F, 2.F, 3.F }));
    EXPECT_TRUE(box.contains({ 2.F, 4.F, 6.F }));
    EXPECT_FALSE(box.contains({ 1.F, 2.F, 6.1F }));
    EXPECT_TRUE(box.overlaps({ .min = { 2.F, 4.F, 6.F }, .max = { 3.F, 5.F, 7.F } }));
    EXPECT_FALSE(box.overlaps({ .min = { 2.1F, 0.F, 0.F }, .max = { 3.F, 1.F, 1.F } }));
    EXPECT_EQ(box.center(), glm::vec3(1.F, 2.F, 3.F));
    EXPECT_EQ(box.extent(), glm::vec3(2.F, 4.F, 6.F));
    EXPECT_EQ(box.half_extent(), glm::vec3(1.F, 2.F, 3.F));
}

TEST(AABB3DTest, TranslatedExpandedAndFactories)
{
    const AABB3D box = AABB3D::from_center({ 1.F, 1.F, 1.F }, { 0.5F, 1.F, 2.F });

    EXPECT_EQ(box.min, glm::vec3(0.5F, 0.F, -1.F));
    EXPECT_EQ(box.max, glm::vec3(1.5F, 2.F, 3.F));
    EXPECT_EQ(box.translated({ 1.F, 0.F, 0.F }).min, glm::vec3(1.5F, 0.F, -1.F));
    EXPECT_EQ(box.expanded(1.F).max, glm::vec3(2.5F, 3.F, 4.F));

    const AABB3D around = AABB3D::from_sphere({ .center = { 1.F, 2.F, 3.F }, .radius = 2.F });
    EXPECT_EQ(around.min, glm::vec3(-1.F, 0.F, 1.F));
    EXPECT_EQ(around.max, glm::vec3(3.F, 4.F, 5.F));
}

TEST(BoundingSphereTest, ContainsIsInclusiveOnTheSurface)
{
    const BoundingSphere sphere { .center = { 0.F, 0.F, 0.F }, .radius = 1.F };

    EXPECT_TRUE(sphere.contains({ 0.F, 0.F, 0.F }));
    EXPECT_TRUE(sphere.contains({ 1.F, 0.F, 0.F }));
    EXPECT_FALSE(sphere.contains({ 1.01F, 0.F, 0.F }));
}

TEST(BoundingSphereTest, ARayPointingAtTheSphereOverlapsIt)
{
    const BoundingSphere sphere { .center = { 0.F, 0.F, 5.F }, .radius = 1.F };

    EXPECT_TRUE(sphere.overlaps_ray({ .origin = { 0.F, 0.F, 0.F }, .direction = { 0.F, 0.F, 1.F } }));
}

TEST(BoundingSphereTest, ARayPointingAwayFromTheSphereDoesNot)
{
    const BoundingSphere sphere { .center = { 0.F, 0.F, 5.F }, .radius = 1.F };

    EXPECT_FALSE(sphere.overlaps_ray({ .origin = { 0.F, 0.F, 0.F }, .direction = { 0.F, 0.F, -1.F } }));
}

TEST(BoundingSphereTest, ARayStartingInsideAlwaysOverlaps)
{
    const BoundingSphere sphere { .center = { 0.F, 0.F, 0.F }, .radius = 2.F };

    EXPECT_TRUE(sphere.overlaps_ray({ .origin = { 0.5F, 0.F, 0.F }, .direction = { 0.F, 1.F, 0.F } }));
    EXPECT_TRUE(sphere.overlaps_ray({ .origin = { 0.5F, 0.F, 0.F }, .direction = { 0.F, -1.F, 0.F } }));
}

TEST(BoundingSphereTest, AMissAndATangentRay)
{
    const BoundingSphere sphere { .center = { 2.F, 0.F, 5.F }, .radius = 1.F };

    EXPECT_FALSE(sphere.overlaps_ray({ .origin = { 0.F, 0.F, 0.F }, .direction = { 0.F, 0.F, 1.F } }));
    EXPECT_TRUE(sphere.overlaps_ray({ .origin = { 1.F, 0.F, 0.F }, .direction = { 0.F, 0.F, 1.F } }));
}

// =============================================================================
// 2D containment callables
// =============================================================================

TEST(BoundsContainment2DTest, CircleIncludesItsBoundaryAndNothingBeyond)
{
    const auto inside = Kinesis::circular_bounds(glm::vec2(1.F, 1.F), 0.5F);

    EXPECT_TRUE(inside({ 1.F, 1.F }));
    EXPECT_TRUE(inside({ 1.5F, 1.F }));
    EXPECT_FALSE(inside({ 1.6F, 1.F }));
    EXPECT_FALSE(inside({ 1.4F, 1.4F }));
}

TEST(BoundsContainment2DTest, ACircleOfRadiusZeroContainsOnlyItsCentre)
{
    const auto inside = Kinesis::circular_bounds(glm::vec2(1.F, 1.F), 0.F);

    EXPECT_TRUE(inside({ 1.F, 1.F }));
    EXPECT_FALSE(inside({ 1.001F, 1.F }));
}

TEST(BoundsContainment2DTest, PolygonWindingDoesNotMatterForAConvexShape)
{
    const std::array<glm::vec2, 4> ccw { glm::vec2(0.F, 0.F), glm::vec2(1.F, 0.F), glm::vec2(1.F, 1.F), glm::vec2(0.F, 1.F) };
    const std::array<glm::vec2, 4> cw { glm::vec2(0.F, 0.F), glm::vec2(0.F, 1.F), glm::vec2(1.F, 1.F), glm::vec2(1.F, 0.F) };

    for (const auto& verts : { ccw, cw }) {
        const auto inside = Kinesis::polygon_bounds(verts);
        EXPECT_TRUE(inside({ 0.5F, 0.5F }));
        EXPECT_FALSE(inside({ 1.5F, 0.5F }));
        EXPECT_FALSE(inside({ 0.5F, -0.1F }));
        EXPECT_FALSE(inside({ -0.1F, 0.5F }));
    }
}

TEST(BoundsContainment2DTest, ConcavePolygonExcludesItsNotch)
{
    const std::array<glm::vec2, 6> ell {
        glm::vec2(0.F, 0.F), glm::vec2(2.F, 0.F), glm::vec2(2.F, 1.F),
        glm::vec2(1.F, 1.F), glm::vec2(1.F, 2.F), glm::vec2(0.F, 2.F)
    };
    const auto inside = Kinesis::polygon_bounds(ell);

    EXPECT_TRUE(inside({ 0.5F, 1.5F }));
    EXPECT_TRUE(inside({ 1.5F, 0.5F }));
    EXPECT_FALSE(inside({ 1.5F, 1.5F }));
}

TEST(BoundsContainment2DTest, ATriangleAndADegenerateOrEmptyPolygon)
{
    const std::array<glm::vec2, 3> triangle { glm::vec2(0.F, 0.F), glm::vec2(4.F, 0.F), glm::vec2(0.F, 4.F) };
    const auto tri = Kinesis::polygon_bounds(triangle);
    EXPECT_TRUE(tri({ 1.F, 1.F }));
    EXPECT_FALSE(tri({ 3.F, 3.F }));

    const auto empty = Kinesis::polygon_bounds(std::span<const glm::vec2> {});
    EXPECT_FALSE(empty({ 0.F, 0.F }));

    const std::array<glm::vec2, 2> segment { glm::vec2(0.F, 0.F), glm::vec2(1.F, 1.F) };
    EXPECT_FALSE(Kinesis::polygon_bounds(segment)({ 0.2F, 0.8F }));
}

TEST(BoundsContainment2DTest, StrokeCoversAHalfThicknessAroundEachSegmentWithRoundCaps)
{
    const std::array<glm::vec2, 2> path { glm::vec2(0.F, 0.F), glm::vec2(2.F, 0.F) };
    const auto inside = Kinesis::stroke_bounds(path, 0.1F);

    EXPECT_TRUE(inside({ 1.F, 0.05F }));
    EXPECT_TRUE(inside({ 1.F, -0.1F }));
    EXPECT_FALSE(inside({ 1.F, 0.2F }));
    EXPECT_TRUE(inside({ -0.05F, 0.F }));
    EXPECT_TRUE(inside({ 2.05F, 0.F }));
    EXPECT_FALSE(inside({ -0.2F, 0.F }));
    EXPECT_FALSE(inside({ 2.2F, 0.F }));
}

TEST(BoundsContainment2DTest, StrokeFollowsEverySegmentOfABentPath)
{
    const std::array<glm::vec2, 3> path { glm::vec2(0.F, 0.F), glm::vec2(2.F, 0.F), glm::vec2(2.F, 2.F) };
    const auto inside = Kinesis::stroke_bounds(path, 0.1F);

    EXPECT_TRUE(inside({ 1.F, 0.F }));
    EXPECT_TRUE(inside({ 2.F, 1.F }));
    EXPECT_FALSE(inside({ 1.F, 1.F }));
}

TEST(BoundsContainment2DTest, StrokeHandlesZeroThicknessAndDegenerateInput)
{
    const std::array<glm::vec2, 2> path { glm::vec2(0.F, 0.F), glm::vec2(2.F, 0.F) };
    EXPECT_TRUE(Kinesis::stroke_bounds(path, 0.F)({ 1.F, 0.F }));
    EXPECT_FALSE(Kinesis::stroke_bounds(path, 0.F)({ 1.F, 0.001F }));

    const std::array<glm::vec2, 2> dot { glm::vec2(1.F, 1.F), glm::vec2(1.F, 1.F) };
    EXPECT_TRUE(Kinesis::stroke_bounds(dot, 0.1F)({ 1.05F, 1.F }));
    EXPECT_FALSE(Kinesis::stroke_bounds(dot, 0.1F)({ 1.2F, 1.F }));

    const std::array<glm::vec2, 1> single { glm::vec2(1.F, 1.F) };
    EXPECT_FALSE(Kinesis::stroke_bounds(single, 0.5F)({ 1.F, 1.F }));
    EXPECT_FALSE(Kinesis::stroke_bounds(std::span<const glm::vec2> {}, 0.5F)({ 0.F, 0.F }));
}

TEST(BoundsCombinator2DTest, UnionIntersectAndSubtract)
{
    const auto left = Kinesis::circular_bounds(glm::vec2(0.F, 0.F), 1.F);
    const auto right = Kinesis::circular_bounds(glm::vec2(1.F, 0.F), 1.F);
    const glm::vec2 only_left { -0.8F, 0.F };
    const glm::vec2 both { 0.5F, 0.F };
    const glm::vec2 only_right { 1.8F, 0.F };
    const glm::vec2 neither { 5.F, 5.F };

    const auto either = Kinesis::union_bounds(left, right);
    EXPECT_TRUE(either(only_left));
    EXPECT_TRUE(either(both));
    EXPECT_TRUE(either(only_right));
    EXPECT_FALSE(either(neither));

    const auto common = Kinesis::intersect_bounds(left, right);
    EXPECT_FALSE(common(only_left));
    EXPECT_TRUE(common(both));
    EXPECT_FALSE(common(only_right));

    const auto cut = Kinesis::subtract_bounds(left, right);
    EXPECT_TRUE(cut(only_left));
    EXPECT_FALSE(cut(both));
    EXPECT_FALSE(cut(only_right));
}

// =============================================================================
// 3D containment callables
// =============================================================================

TEST(BoundsContainment3DTest, SphereIncludesItsBoundary)
{
    const auto inside = Kinesis::circular_bounds(glm::vec3(1.F, 2.F, 3.F), 2.F);

    EXPECT_TRUE(inside({ 1.F, 2.F, 3.F }));
    EXPECT_TRUE(inside({ 3.F, 2.F, 3.F }));
    EXPECT_FALSE(inside({ 3.1F, 2.F, 3.F }));
}

TEST(BoundsContainment3DTest, ConvexRegionNeedsEveryHalfPlane)
{
    const std::array<std::pair<glm::vec3, float>, 6> unit_box { {
        { { 1.F, 0.F, 0.F }, 0.F },
        { { -1.F, 0.F, 0.F }, -1.F },
        { { 0.F, 1.F, 0.F }, 0.F },
        { { 0.F, -1.F, 0.F }, -1.F },
        { { 0.F, 0.F, 1.F }, 0.F },
        { { 0.F, 0.F, -1.F }, -1.F },
    } };
    const auto inside = Kinesis::convex_region(unit_box);

    EXPECT_TRUE(inside({ 0.5F, 0.5F, 0.5F }));
    EXPECT_TRUE(inside({ 1.F, 1.F, 1.F }));
    EXPECT_FALSE(inside({ 1.5F, 0.5F, 0.5F }));
    EXPECT_FALSE(inside({ 0.5F, -0.1F, 0.5F }));
    EXPECT_FALSE(inside({ 0.5F, 0.5F, 1.1F }));
}

TEST(BoundsContainment3DTest, ExtrudedPolygonUsesTheXzFootprintAndAYRange)
{
    const std::array<glm::vec2, 4> footprint { glm::vec2(0.F, 0.F), glm::vec2(1.F, 0.F), glm::vec2(1.F, 1.F), glm::vec2(0.F, 1.F) };
    const auto inside = Kinesis::extruded_polygon_region(footprint, 0.F, 2.F);

    EXPECT_TRUE(inside({ 0.5F, 1.F, 0.5F }));
    EXPECT_TRUE(inside({ 0.5F, 0.F, 0.5F }));
    EXPECT_TRUE(inside({ 0.5F, 2.F, 0.5F }));
    EXPECT_FALSE(inside({ 0.5F, 2.1F, 0.5F }));
    EXPECT_FALSE(inside({ 0.5F, -0.1F, 0.5F }));
    EXPECT_FALSE(inside({ 1.5F, 1.F, 0.5F }));
    EXPECT_FALSE(inside({ 0.5F, 1.F, 1.5F }));
}

TEST(BoundsCombinator3DTest, UnionIntersectAndSubtract)
{
    const auto a = Kinesis::circular_bounds(glm::vec3(0.F), 1.F);
    const auto b = Kinesis::circular_bounds(glm::vec3(1.F, 0.F, 0.F), 1.F);
    const glm::vec3 only_a { -0.8F, 0.F, 0.F };
    const glm::vec3 both { 0.5F, 0.F, 0.F };
    const glm::vec3 only_b { 1.8F, 0.F, 0.F };

    EXPECT_TRUE(Kinesis::union_bounds(a, b)(only_a));
    EXPECT_TRUE(Kinesis::union_bounds(a, b)(only_b));
    EXPECT_FALSE(Kinesis::intersect_bounds(a, b)(only_a));
    EXPECT_TRUE(Kinesis::intersect_bounds(a, b)(both));
    EXPECT_TRUE(Kinesis::subtract_bounds(a, b)(only_a));
    EXPECT_FALSE(Kinesis::subtract_bounds(a, b)(both));
}

// =============================================================================
// place
// =============================================================================

class LayoutPlaceTest : public ::testing::Test {
protected:
    const AABB2D anchor { .min = { 0.F, 0.F }, .max = { 2.F, 1.F } };
};

TEST_F(LayoutPlaceTest, RightAndLeftSitBesideTheAnchorSeparatedByTheGap)
{
    expect_near(Kinesis::place(anchor, Side::Right, 1.F, 0.5F, 0.1F),
        AABB2D { .min = { 2.1F, 0.25F }, .max = { 3.1F, 0.75F } });
    expect_near(Kinesis::place(anchor, Side::Left, 1.F, 0.5F, 0.1F),
        AABB2D { .min = { -1.1F, 0.25F }, .max = { -0.1F, 0.75F } });
}

TEST_F(LayoutPlaceTest, AboveAndBelowSitOnTheAnchorSeparatedByTheGap)
{
    expect_near(Kinesis::place(anchor, Side::Below, 1.F, 0.5F, 0.1F),
        AABB2D { .min = { 0.5F, -0.6F }, .max = { 1.5F, -0.1F } });
    expect_near(Kinesis::place(anchor, Side::Above, 1.F, 0.5F, 0.1F),
        AABB2D { .min = { 0.5F, 1.1F }, .max = { 1.5F, 1.6F } });
}

TEST_F(LayoutPlaceTest, OverCentresTheBoxOnTheAnchorIgnoringGapAndAlignment)
{
    expect_near(Kinesis::place(anchor, Side::Over, 1.F, 0.5F, 9.F, Align::Start),
        AABB2D { .min = { 0.5F, 0.25F }, .max = { 1.5F, 0.75F } });
}

TEST_F(LayoutPlaceTest, VerticalStacksAlignAlongTheXAxis)
{
    expect_near(Kinesis::place(anchor, Side::Below, 1.F, 0.5F, 0.F, Align::Start),
        AABB2D { .min = { 0.F, -0.5F }, .max = { 1.F, 0.F } });
    expect_near(Kinesis::place(anchor, Side::Below, 1.F, 0.5F, 0.F, Align::End),
        AABB2D { .min = { 1.F, -0.5F }, .max = { 2.F, 0.F } });
    expect_near(Kinesis::place(anchor, Side::Below, 1.F, 0.5F, 0.F, Align::Span),
        AABB2D { .min = { 0.F, -0.5F }, .max = { 2.F, 0.F } });
}

TEST_F(LayoutPlaceTest, HorizontalRowsAlignAlongTheYAxisFromTheTop)
{
    expect_near(Kinesis::place(anchor, Side::Right, 1.F, 0.25F, 0.F, Align::Start),
        AABB2D { .min = { 2.F, 0.75F }, .max = { 3.F, 1.F } });
    expect_near(Kinesis::place(anchor, Side::Right, 1.F, 0.25F, 0.F, Align::End),
        AABB2D { .min = { 2.F, 0.F }, .max = { 3.F, 0.25F } });
    expect_near(Kinesis::place(anchor, Side::Left, 1.F, 0.25F, 0.F, Align::Span),
        AABB2D { .min = { -1.F, 0.F }, .max = { 0.F, 1.F } });
}

TEST_F(LayoutPlaceTest, AnyObjectWithBoundsCanBeTheAnchor)
{
    const Anchor object { anchor };

    expect_near(Kinesis::place(object, Side::Right, 1.F, 0.5F, 0.1F),
        Kinesis::place(anchor, Side::Right, 1.F, 0.5F, 0.1F));
    expect_near(Kinesis::bounds_of(object), anchor);
}

TEST_F(LayoutPlaceTest, PlacingRepeatedlyBelowBuildsAGaplessStackWhenGapIsZero)
{
    AABB2D row = anchor;

    for (int i = 0; i < 3; ++i) {
        const AABB2D next = Kinesis::place(row, Side::Below, 0.F, 0.5F, 0.F, Align::Span);
        EXPECT_NEAR(next.max.y, row.min.y, 1e-6F);
        EXPECT_NEAR(next.height(), 0.5F, 1e-6F);
        row = next;
    }
}

}
