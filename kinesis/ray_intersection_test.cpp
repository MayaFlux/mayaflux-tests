#include "gtest/gtest.h"
#include "../support/test_metadata.hpp"

#include "MayaFlux/Kinesis/Spatial/HitTest.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {
namespace {

    using Kinesis::Ray;

    const glm::vec3 k_ray_a { -1.F, -1.F, 0.F };
    const glm::vec3 k_ray_b { 1.F, -1.F, 0.F };
    const glm::vec3 k_ray_c { 1.F, 1.F, 0.F };
    const glm::vec3 k_ray_d { -1.F, 1.F, 0.F };

    Ray down_from(float x, float y, float z = 4.F)
    {
        return { .origin = { x, y, z }, .direction = { 0.F, 0.F, -1.F } };
    }

    Kinesis::ViewTransform ray_camera()
    {
        return Kinesis::look_at_perspective({ 0.F, 0.F, 5.F }, { 0.F, 0.F, 0.F }, 1.5707964F, 1.F, 0.1F, 100.F);
    }

}

TEST(RayThroughTest, TheRayPassesThroughWhatTheScreenPositionShows)
{
    const Kinesis::ViewTransform vt = ray_camera();
    const Kinesis::Ray ray = Kinesis::ray_through({ 0.5F, -0.5F }, vt);

    const auto t = Kinesis::ray_plane_parameter(ray, glm::vec3(0.F), { 0.F, 0.F, 1.F });

    ASSERT_TRUE(t.has_value());
    const glm::vec3 met = ray.origin + ray.direction * *t;
    EXPECT_NEAR(met.x, 2.5F, 1e-3F);
    EXPECT_NEAR(met.y, -2.5F, 1e-3F);
    EXPECT_NEAR(glm::length(ray.direction), 1.F, 1e-5F);
}

TEST(RayThroughTest, AnOrthographicViewGivesParallelRays)
{
    const Kinesis::ViewTransform vt = Kinesis::ortho(-2.F, 2.F, -2.F, 2.F);

    const Ray left = Kinesis::ray_through({ -1.F, 0.F }, vt);
    const Ray right = Kinesis::ray_through({ 1.F, 0.F }, vt);

    EXPECT_NEAR(glm::dot(left.direction, right.direction), 1.F, 1e-5F);
    EXPECT_NEAR(right.origin.x - left.origin.x, 4.F, 1e-4F);
}

TEST(RayThroughTest, ThePlanePointBehindAPerspectiveEyeIsNotSeen)
{
    const Kinesis::ViewTransform away = Kinesis::look_at_perspective(
        { 0.F, 0.F, 5.F }, { 0.F, 0.F, 10.F }, 1.5707964F, 1.F, 0.1F, 100.F);

    EXPECT_FALSE(Kinesis::view_plane_point({ 0.F, 0.F }, away).has_value());
}

TEST(RayPlaneParameterTest, TheParameterIsInUnitsOfTheDirection)
{
    const Ray ray { .origin = { 0.F, 0.F, 6.F }, .direction = { 0.F, 0.F, -3.F } };

    const auto t = Kinesis::ray_plane_parameter(ray, glm::vec3(0.F), { 0.F, 0.F, 10.F });

    ASSERT_TRUE(t.has_value());
    EXPECT_FLOAT_EQ(*t, 2.F);
}

TEST(RayPlaneParameterTest, ParallelBehindAndDegenerateInputsMiss)
{
    const glm::vec3 normal { 0.F, 0.F, 1.F };

    EXPECT_FALSE(Kinesis::ray_plane_parameter(
        { .origin = { 0.F, 0.F, 1.F }, .direction = { 1.F, 0.F, 0.F } }, glm::vec3(0.F), normal));
    EXPECT_FALSE(Kinesis::ray_plane_parameter(
        { .origin = { 0.F, 0.F, 1.F }, .direction = { 0.F, 0.F, 1.F } }, glm::vec3(0.F), normal));
    EXPECT_FALSE(Kinesis::ray_plane_parameter(down_from(0.F, 0.F), glm::vec3(0.F), glm::vec3(0.F)));
    EXPECT_FALSE(Kinesis::ray_plane_parameter(
        { .origin = { 0.F, 0.F, 1.F }, .direction = glm::vec3(0.F) }, glm::vec3(0.F), normal));
}

TEST(RayPlaneParameterTest, AnOriginOnThePlaneIsAtParameterZero)
{
    const auto t = Kinesis::ray_plane_parameter(down_from(0.F, 0.F, 0.F), glm::vec3(0.F), { 0.F, 0.F, 1.F });

    ASSERT_TRUE(t.has_value());
    EXPECT_FLOAT_EQ(std::abs(*t), 0.F);
}

TEST(RayTriangleParameterTest, TheBarycentricsRebuildTheMetPoint)
{
    const auto hit = Kinesis::ray_triangle_parameter(down_from(0.5F, -0.25F), k_ray_a, k_ray_b, k_ray_c);

    ASSERT_TRUE(hit.has_value());
    EXPECT_FLOAT_EQ(hit->x, 4.F);
    const glm::vec3 met = k_ray_a * (1.F - hit->y - hit->z) + k_ray_b * hit->y + k_ray_c * hit->z;
    EXPECT_NEAR(met.x, 0.5F, 1e-5F);
    EXPECT_NEAR(met.y, -0.25F, 1e-5F);
}

TEST(RayTriangleParameterTest, EitherWindingIsMet)
{
    EXPECT_TRUE(Kinesis::ray_triangle_parameter(down_from(0.5F, -0.25F), k_ray_a, k_ray_b, k_ray_c));
    EXPECT_TRUE(Kinesis::ray_triangle_parameter(down_from(0.5F, -0.25F), k_ray_a, k_ray_c, k_ray_b));
}

TEST(RayTriangleParameterTest, OutsideBehindEdgeOnAndCollinearMiss)
{
    EXPECT_FALSE(Kinesis::ray_triangle_parameter(down_from(-0.5F, 0.5F), k_ray_a, k_ray_b, k_ray_c));
    EXPECT_FALSE(Kinesis::ray_triangle_parameter(down_from(0.5F, -0.25F, -4.F), k_ray_a, k_ray_b, k_ray_c));
    EXPECT_FALSE(Kinesis::ray_triangle_parameter(
        { .origin = { -5.F, -0.5F, 0.F }, .direction = { 1.F, 0.F, 0.F } }, k_ray_a, k_ray_b, k_ray_c));
    EXPECT_FALSE(Kinesis::ray_triangle_parameter(
        down_from(0.F, 0.F), k_ray_a, glm::vec3(0.F), k_ray_c));
    EXPECT_FALSE(Kinesis::ray_triangle_parameter(down_from(-1.F, -1.F), k_ray_a, k_ray_a, k_ray_a));
}

TEST(RayTriangleParameterTest, ATriangleFarSmallerThanAUnitIsStillMet)
{
    constexpr float s = 1.0e-4F;

    const auto hit = Kinesis::ray_triangle_parameter(
        down_from(0.5F * s, -0.25F * s, 1.F), k_ray_a * s, k_ray_b * s, k_ray_c * s);

    EXPECT_TRUE(hit.has_value());
}

TEST(RayTrianglesParameterTest, TheNearestTriangleWinsWhateverItsPlaceInTheList)
{
    const std::vector<glm::vec3> vertices {
        k_ray_a, k_ray_b, k_ray_c,
        k_ray_a + glm::vec3(0.F, 0.F, 1.F), k_ray_b + glm::vec3(0.F, 0.F, 1.F), k_ray_c + glm::vec3(0.F, 0.F, 1.F)
    };
    const std::vector<uint32_t> far_first { 0, 1, 2, 3, 4, 5 };
    const std::vector<uint32_t> near_first { 3, 4, 5, 0, 1, 2 };
    const Ray ray = down_from(0.5F, -0.25F);

    const auto later = Kinesis::ray_triangles_parameter(ray, vertices, far_first);
    const auto earlier = Kinesis::ray_triangles_parameter(ray, vertices, near_first);

    ASSERT_TRUE(later.has_value());
    ASSERT_TRUE(earlier.has_value());
    EXPECT_EQ(later->first, 1U);
    EXPECT_EQ(earlier->first, 0U);
    EXPECT_FLOAT_EQ(later->second.x, 3.F);
}

TEST(RayTrianglesParameterTest, AHitOnASharedDiagonalIsNotLost)
{
    const std::vector<glm::vec3> vertices { k_ray_a, k_ray_b, k_ray_c, k_ray_d };
    const std::vector<uint32_t> indices { 0, 1, 2, 0, 2, 3 };

    EXPECT_TRUE(Kinesis::ray_triangles_parameter(down_from(0.F, 0.F), vertices, indices));
}

TEST(RayTrianglesParameterTest, OutOfRangeAndIncompleteTrianglesAreSkipped)
{
    const std::vector<glm::vec3> vertices { k_ray_a, k_ray_b, k_ray_c };
    const std::vector<uint32_t> indices { 0, 1, 9, 0, 1, 2, 0, 1 };

    const auto hit = Kinesis::ray_triangles_parameter(down_from(0.5F, -0.25F), vertices, indices);

    ASSERT_TRUE(hit.has_value());
    EXPECT_EQ(hit->first, 1U);
    EXPECT_FALSE(Kinesis::ray_triangles_parameter(down_from(0.5F, -0.25F), std::vector<glm::vec3> {}, indices));
    EXPECT_FALSE(Kinesis::ray_triangles_parameter(down_from(0.5F, -0.25F), vertices, std::vector<uint32_t> {}));
}

TEST(NearestSegmentParameterTest, ItIsWhereTheSegmentPassesClosestToTheLine)
{
    const auto s = Kinesis::nearest_segment_parameter(
        down_from(0.5F, 3.F), { -1.F, 0.F, 0.F }, { 1.F, 0.F, 0.F });

    ASSERT_TRUE(s.has_value());
    EXPECT_NEAR(*s, 0.75F, 1e-5F);
}

TEST(NearestSegmentParameterTest, ItIsClampedToTheEnds)
{
    const glm::vec3 a { -1.F, 0.F, 0.F };
    const glm::vec3 b { 1.F, 0.F, 0.F };

    EXPECT_FLOAT_EQ(Kinesis::nearest_segment_parameter(down_from(7.F, 0.F), a, b).value(), 1.F);
    EXPECT_FLOAT_EQ(Kinesis::nearest_segment_parameter(down_from(-7.F, 0.F), a, b).value(), 0.F);
}

TEST(NearestSegmentParameterTest, TheWholeLineOfTheRayCountsNotOnlyAheadOfIt)
{
    const auto s = Kinesis::nearest_segment_parameter(
        down_from(0.F, 0.F, -4.F), { -1.F, 0.F, 0.F }, { 1.F, 0.F, 0.F });

    ASSERT_TRUE(s.has_value());
    EXPECT_NEAR(*s, 0.5F, 1e-5F);
}

TEST(NearestSegmentParameterTest, ParallelAndEmptySegmentsHaveNoParameter)
{
    EXPECT_FALSE(Kinesis::nearest_segment_parameter(down_from(1.F, 0.F), { 0.F, 0.F, 0.F }, { 0.F, 0.F, 2.F }));
    EXPECT_FALSE(Kinesis::nearest_segment_parameter(down_from(1.F, 0.F), glm::vec3(0.F), glm::vec3(0.F)));
}

}
