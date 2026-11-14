#include "gtest/gtest.h"
#include "../support/test_metadata.hpp"

#include "MayaFlux/Kinesis/Morphology.hpp"
#include "MayaFlux/Kinesis/Spatial/Bounds.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {
namespace {

    using Kinesis::AABB2D;

    TEST(AabbMappingTest, FromOriginSizeBuildsTheBoxFromACornerAndAnExtent)
    {
        const AABB2D box = AABB2D::from_origin_size({ -0.8F, 0.3F }, { 1.6F, 0.2F });
        EXPECT_FLOAT_EQ(box.min.x, -0.8F);
        EXPECT_FLOAT_EQ(box.min.y, 0.3F);
        EXPECT_FLOAT_EQ(box.max.x, 0.8F);
        EXPECT_FLOAT_EQ(box.max.y, 0.5F);
    }

    TEST(AabbMappingTest, PointAtMapsUnitSpaceOntoTheBoxAndUnitOfInvertsIt)
    {
        const AABB2D box = AABB2D::from_origin_size({ 1.0F, 2.0F }, { 4.0F, 2.0F });
        EXPECT_EQ(box.point_at({ 0.0F, 0.0F }), glm::vec2(1.0F, 2.0F));
        EXPECT_EQ(box.point_at({ 1.0F, 1.0F }), glm::vec2(5.0F, 4.0F));
        EXPECT_EQ(box.point_at({ 0.5F, 0.25F }), glm::vec2(3.0F, 2.5F));
        EXPECT_EQ(box.point_at({ 2.0F, -1.0F }), glm::vec2(9.0F, 0.0F));

        const glm::vec2 unit = box.unit_of({ 3.0F, 2.5F });
        EXPECT_FLOAT_EQ(unit.x, 0.5F);
        EXPECT_FLOAT_EQ(unit.y, 0.25F);
    }

    TEST(AabbMappingTest, DegenerateExtentMapsToMinAndUnitOfIsNotFinite)
    {
        const AABB2D flat = AABB2D::from_origin_size({ 1.0F, 1.0F }, { 0.0F, 2.0F });
        EXPECT_EQ(flat.point_at({ 0.7F, 0.5F }), glm::vec2(1.0F, 2.0F));
        EXPECT_FALSE(std::isfinite(flat.unit_of({ 2.0F, 1.0F }).x));
    }

    TEST(PlanarAabbTest, BoxesTheSpanAndAnEmptySpanGivesZero)
    {
        const std::vector<glm::vec2> points { { 0.5F, -1.0F }, { -2.0F, 3.0F }, { 1.0F, 0.0F } };
        const AABB2D box = Kinesis::aabb(std::span<const glm::vec2>(points));
        EXPECT_EQ(box.min, glm::vec2(-2.0F, -1.0F));
        EXPECT_EQ(box.max, glm::vec2(1.0F, 3.0F));

        const AABB2D empty = Kinesis::aabb(std::span<const glm::vec2> {});
        EXPECT_EQ(empty.min, glm::vec2(0.0F));
        EXPECT_EQ(empty.max, glm::vec2(0.0F));
    }

    TEST(TriangleBoundsTest, ContainsInsideAndOnTheEdgeWhicheverWayItWinds)
    {
        const glm::vec2 a(0.0F, 0.0F);
        const glm::vec2 b(1.0F, 0.0F);
        const glm::vec2 c(0.0F, 1.0F);
        EXPECT_TRUE(Kinesis::triangle_contains({ 0.25F, 0.25F }, a, b, c));
        EXPECT_TRUE(Kinesis::triangle_contains({ 0.25F, 0.25F }, a, c, b));
        EXPECT_TRUE(Kinesis::triangle_contains({ 0.5F, 0.0F }, a, b, c));
        EXPECT_TRUE(Kinesis::triangle_contains(a, a, b, c));
        EXPECT_FALSE(Kinesis::triangle_contains({ 0.75F, 0.75F }, a, b, c));
        EXPECT_FALSE(Kinesis::triangle_contains({ -0.1F, 0.5F }, a, b, c));
    }

    TEST(TriangleBoundsTest, ListHitsInsideAnyTriangleAndIgnoresATrailingPartialGroup)
    {
        const std::vector<glm::vec2> positions {
            { 0.0F, 0.0F },
            { 1.0F, 0.0F },
            { 0.0F, 1.0F },
            { 2.0F, 0.0F },
            { 3.0F, 0.0F },
            { 2.0F, 1.0F },
            { 9.0F, 9.0F },
            { 9.5F, 9.0F },
        };
        const auto contains = Kinesis::triangle_list_bounds(positions);
        EXPECT_TRUE(contains({ 0.2F, 0.2F }));
        EXPECT_TRUE(contains({ 2.2F, 0.2F }));
        EXPECT_FALSE(contains({ 1.5F, 0.5F }));
        EXPECT_FALSE(contains({ 9.1F, 9.0F }));
    }

    TEST(TriangleBoundsTest, ADegenerateTriangleNeverMakesThePlaneHit)
    {
        const std::vector<glm::vec2> positions {
            { 0.0F, 0.0F },
            { 1.0F, 1.0F },
            { 2.0F, 2.0F },
        };
        const auto contains = Kinesis::triangle_list_bounds(positions);
        EXPECT_FALSE(contains({ 0.5F, 0.5F }));
        EXPECT_FALSE(contains({ 5.0F, -5.0F }));
    }

    TEST(TriangleBoundsTest, NoPositionsNeverHits)
    {
        const auto contains = Kinesis::triangle_list_bounds(std::span<const glm::vec2> {});
        EXPECT_FALSE(contains({ 0.0F, 0.0F }));
    }

    TEST(TriangleBoundsTest, ComposesWithTheOtherBoundsPredicates)
    {
        const std::vector<glm::vec2> square {
            { 0.0F, 0.0F },
            { 1.0F, 0.0F },
            { 1.0F, 1.0F },
            { 0.0F, 0.0F },
            { 1.0F, 1.0F },
            { 0.0F, 1.0F },
        };
        const auto ring = Kinesis::subtract_bounds(
            Kinesis::triangle_list_bounds(square),
            Kinesis::circular_bounds({ 0.5F, 0.5F }, 0.2F));
        EXPECT_TRUE(ring({ 0.1F, 0.1F }));
        EXPECT_FALSE(ring({ 0.5F, 0.5F }));
        EXPECT_FALSE(ring({ 1.5F, 0.5F }));
    }

}
}
