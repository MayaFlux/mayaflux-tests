#include "gtest/gtest.h"

#include "MayaFlux/Kinesis/Spatial/Projection.hpp"

namespace MayaFlux::Test {

using Kinesis::AABB2D;

namespace {

    constexpr float k_pi = 3.14159265358979F;

    glm::vec2 on_circle(glm::vec2 center, float radius, float angle)
    {
        return center + radius * glm::vec2(std::cos(angle), std::sin(angle));
    }

}

// =============================================================================
// axis_fraction
// =============================================================================

TEST(AxisFractionTest, HorizontalMapsTheBoundsOntoZeroToOneAndClamps)
{
    const AABB2D bounds { .min = { 0.F, 0.F }, .max = { 10.F, 2.F } };
    const auto project = Kinesis::axis_fraction(bounds);

    EXPECT_FLOAT_EQ(project({ 0.F, 1.F }), 0.F);
    EXPECT_FLOAT_EQ(project({ 5.F, 1.F }), 0.5F);
    EXPECT_FLOAT_EQ(project({ 10.F, 1.F }), 1.F);
    EXPECT_FLOAT_EQ(project({ -3.F, 1.F }), 0.F);
    EXPECT_FLOAT_EQ(project({ 99.F, 1.F }), 1.F);
}

TEST(AxisFractionTest, AHandleInsetMakesTheHandleCentreReachBothEnds)
{
    const AABB2D bounds { .min = { 0.F, 0.F }, .max = { 10.F, 2.F } };
    const auto project = Kinesis::axis_fraction(bounds, 2.F);

    EXPECT_FLOAT_EQ(project({ 1.F, 1.F }), 0.F);
    EXPECT_FLOAT_EQ(project({ 5.F, 1.F }), 0.5F);
    EXPECT_FLOAT_EQ(project({ 9.F, 1.F }), 1.F);
    EXPECT_FLOAT_EQ(project({ 0.F, 1.F }), 0.F);
    EXPECT_FLOAT_EQ(project({ 10.F, 1.F }), 1.F);
}

TEST(AxisFractionTest, VerticalReadsTheYAxis)
{
    const AABB2D bounds { .min = { 0.F, 0.F }, .max = { 2.F, 2.F } };
    const auto project = Kinesis::axis_fraction(bounds, 0.5F, false);

    EXPECT_FLOAT_EQ(project({ 99.F, 0.25F }), 0.F);
    EXPECT_FLOAT_EQ(project({ -99.F, 1.F }), 0.5F);
    EXPECT_FLOAT_EQ(project({ 0.F, 1.75F }), 1.F);
}

TEST(AxisFractionTest, AHandleAsWideAsTheTrackGivesZero)
{
    const AABB2D bounds { .min = { 0.F, 0.F }, .max = { 2.F, 2.F } };

    EXPECT_FLOAT_EQ(Kinesis::axis_fraction(bounds, 2.F)({ 1.F, 1.F }), 0.F);
    EXPECT_FLOAT_EQ(Kinesis::axis_fraction(bounds, 5.F)({ 2.F, 1.F }), 0.F);
}

TEST(AxisFractionTest, LiveOffsetMovesTheFrameTheCursorIsMeasuredIn)
{
    const AABB2D bounds { .min = { 0.F, 0.F }, .max = { 10.F, 2.F } };
    const auto project = Kinesis::axis_fraction(bounds, 0.F, true, [] { return glm::vec2(3.F, 0.F); });

    EXPECT_FLOAT_EQ(project({ 3.F, 1.F }), 0.F);
    EXPECT_FLOAT_EQ(project({ 8.F, 1.F }), 0.5F);
}

// =============================================================================
// unit_square
// =============================================================================

TEST(UnitSquareTest, MapsCornersAndCentreAndClampsOutside)
{
    const AABB2D bounds { .min = { 2.F, 4.F }, .max = { 4.F, 8.F } };
    const auto project = Kinesis::unit_square(bounds);

    EXPECT_EQ(project({ 2.F, 4.F }), glm::vec2(0.F, 0.F));
    EXPECT_EQ(project({ 4.F, 8.F }), glm::vec2(1.F, 1.F));
    EXPECT_EQ(project({ 3.F, 6.F }), glm::vec2(0.5F, 0.5F));
    EXPECT_EQ(project({ 0.F, 0.F }), glm::vec2(0.F, 0.F));
    EXPECT_EQ(project({ 99.F, 99.F }), glm::vec2(1.F, 1.F));
    EXPECT_EQ(project({ 3.F, 99.F }), glm::vec2(0.5F, 1.F));
}

TEST(UnitSquareTest, AnEmptyRegionGivesTheOrigin)
{
    const AABB2D flat { .min = { 1.F, 1.F }, .max = { 1.F, 5.F } };

    EXPECT_EQ(Kinesis::unit_square(flat)({ 1.F, 3.F }), glm::vec2(0.F));
}

TEST(UnitSquareTest, LiveOffsetMovesTheFrameTheCursorIsMeasuredIn)
{
    const AABB2D bounds { .min = { 0.F, 0.F }, .max = { 2.F, 2.F } };
    const auto project = Kinesis::unit_square(bounds, [] { return glm::vec2(1.F, -1.F); });

    EXPECT_EQ(project({ 2.F, 0.F }), glm::vec2(0.5F, 0.5F));
}

// =============================================================================
// angle_fraction
// =============================================================================

TEST(AngleFractionTest, MapsTheSweepOntoZeroToOne)
{
    const glm::vec2 centre { 1.F, 1.F };
    const auto project = Kinesis::angle_fraction(centre, 0.F, k_pi * 0.5F);

    EXPECT_NEAR(project(on_circle(centre, 0.5F, 0.F)), 0.F, 1e-5F);
    EXPECT_NEAR(project(on_circle(centre, 0.5F, k_pi * 0.25F)), 0.5F, 1e-5F);
    EXPECT_NEAR(project(on_circle(centre, 0.5F, k_pi * 0.5F)), 1.F, 1e-5F);
}

TEST(AngleFractionTest, TheDistanceFromTheCentreIsIgnored)
{
    const glm::vec2 centre { 0.F, 0.F };
    const auto project = Kinesis::angle_fraction(centre, 0.F, k_pi);

    EXPECT_NEAR(project(on_circle(centre, 0.01F, k_pi * 0.5F)), 0.5F, 1e-5F);
    EXPECT_NEAR(project(on_circle(centre, 50.F, k_pi * 0.5F)), 0.5F, 1e-5F);
}

TEST(AngleFractionTest, ADescendingSweepRunsTheOtherWay)
{
    const glm::vec2 centre { 0.F, 0.F };
    const auto project = Kinesis::angle_fraction(centre, 0.F, -k_pi * 0.5F);

    EXPECT_NEAR(project(on_circle(centre, 1.F, -k_pi * 0.25F)), 0.5F, 1e-5F);
    EXPECT_NEAR(project(on_circle(centre, 1.F, -k_pi * 0.5F)), 1.F, 1e-5F);
}

TEST(AngleFractionTest, ASweepCrossingTheAtan2DiscontinuityStaysContinuous)
{
    const glm::vec2 centre { 0.F, 0.F };
    const auto project = Kinesis::angle_fraction(centre, k_pi * 0.75F, k_pi * 1.25F);

    EXPECT_NEAR(project(on_circle(centre, 1.F, k_pi * 0.75F)), 0.F, 1e-4F);
    EXPECT_NEAR(project(on_circle(centre, 1.F, k_pi)), 0.5F, 1e-4F);
    EXPECT_NEAR(project(on_circle(centre, 1.F, -k_pi * 0.75F)), 1.F, 1e-4F);
}

TEST(AngleFractionTest, AnglesJustOutsideTheSweepSnapToTheNearerEnd)
{
    const glm::vec2 centre { 0.F, 0.F };
    const auto project = Kinesis::angle_fraction(centre, 0.F, k_pi * 0.5F);

    EXPECT_FLOAT_EQ(project(on_circle(centre, 1.F, -0.1F)), 0.F);
    EXPECT_FLOAT_EQ(project(on_circle(centre, 1.F, k_pi * 0.5F + 0.1F)), 1.F);
}

TEST(AngleFractionTest, TheDeadZoneSplitsAtItsMiddle)
{
    const glm::vec2 centre { 0.F, 0.F };
    const auto project = Kinesis::angle_fraction(centre, 0.F, k_pi * 0.5F);

    EXPECT_FLOAT_EQ(project(on_circle(centre, 1.F, k_pi)), 1.F);
    EXPECT_FLOAT_EQ(project(on_circle(centre, 1.F, -k_pi * 0.5F)), 0.F);
    EXPECT_FLOAT_EQ(project(on_circle(centre, 1.F, -k_pi * 0.6F)), 0.F);
}

TEST(AngleFractionTest, ADescendingSweepAlsoSnapsToTheNearerEnd)
{
    const glm::vec2 centre { 0.F, 0.F };
    const auto project = Kinesis::angle_fraction(centre, 0.F, -k_pi * 0.5F);

    EXPECT_FLOAT_EQ(project(on_circle(centre, 1.F, 0.1F)), 0.F);
    EXPECT_FLOAT_EQ(project(on_circle(centre, 1.F, -k_pi * 0.5F - 0.1F)), 1.F);
}

TEST(AngleFractionTest, AFullTurnSweepHasNoDeadZone)
{
    const glm::vec2 centre { 0.F, 0.F };
    const auto project = Kinesis::angle_fraction(centre, 0.F, 2.F * k_pi);

    EXPECT_NEAR(project(on_circle(centre, 1.F, k_pi * 0.5F)), 0.25F, 1e-4F);
    EXPECT_NEAR(project(on_circle(centre, 1.F, k_pi)), 0.5F, 1e-4F);
}

TEST(AngleFractionTest, TheCentreAndAZeroSweepGiveZero)
{
    const glm::vec2 centre { 2.F, 2.F };

    EXPECT_FLOAT_EQ(Kinesis::angle_fraction(centre, 0.F, 1.F)(centre), 0.F);
    EXPECT_FLOAT_EQ(Kinesis::angle_fraction(centre, 1.F, 1.F)(on_circle(centre, 1.F, 0.5F)), 0.F);
}

TEST(AngleFractionTest, TheRegionOverloadUsesTheRegionCentre)
{
    const AABB2D region { .min = { 0.F, 0.F }, .max = { 4.F, 2.F } };
    const auto from_region = Kinesis::angle_fraction(region, 0.F, k_pi);
    const auto from_centre = Kinesis::angle_fraction(glm::vec2(2.F, 1.F), 0.F, k_pi);

    EXPECT_FLOAT_EQ(from_region({ 2.F, 3.F }), from_centre({ 2.F, 3.F }));
}

TEST(AngleFractionTest, LiveOffsetMovesTheFrameTheCursorIsMeasuredIn)
{
    const auto project = Kinesis::angle_fraction(glm::vec2(0.F), 0.F, k_pi, [] { return glm::vec2(5.F, 0.F); });

    EXPECT_NEAR(project({ 5.F, 1.F }), 0.5F, 1e-5F);
}

// =============================================================================
// path_fraction and scaled
// =============================================================================

namespace {
    const std::array<glm::vec2, 3> k_path { glm::vec2(0.F, 0.F), glm::vec2(2.F, 0.F), glm::vec2(2.F, 2.F) };
}

TEST(PathFractionTest, ReturnsTheArcLengthOfTheClosestPoint)
{
    const auto project = Kinesis::path_fraction(k_path);

    EXPECT_NEAR(project({ 1.F, 0.F }), 0.25F, 1e-6F);
    EXPECT_NEAR(project({ 2.F, 1.F }), 0.75F, 1e-6F);
    EXPECT_NEAR(project({ 2.F, 2.F }), 1.F, 1e-6F);
    EXPECT_NEAR(project({ 0.F, 0.F }), 0.F, 1e-6F);
}

TEST(PathFractionTest, PointsOffThePathProjectOntoTheNearestSegment)
{
    const auto project = Kinesis::path_fraction(k_path);

    EXPECT_NEAR(project({ 1.F, 0.5F }), 0.25F, 1e-6F);
    EXPECT_NEAR(project({ -5.F, 0.F }), 0.F, 1e-6F);
    EXPECT_NEAR(project({ 2.5F, 5.F }), 1.F, 1e-6F);
}

TEST(PathFractionTest, AJointBelongsToTheFirstSegment)
{
    EXPECT_NEAR(Kinesis::path_fraction(k_path)({ 2.F, 0.F }), 0.5F, 1e-6F);
}

TEST(PathFractionTest, DegeneratePathsGiveZero)
{
    const std::array<glm::vec2, 1> single { glm::vec2(1.F, 1.F) };
    const std::array<glm::vec2, 2> still { glm::vec2(1.F, 1.F), glm::vec2(1.F, 1.F) };

    EXPECT_FLOAT_EQ(Kinesis::path_fraction(single)({ 5.F, 5.F }), 0.F);
    EXPECT_FLOAT_EQ(Kinesis::path_fraction(still)({ 5.F, 5.F }), 0.F);
    EXPECT_FLOAT_EQ(Kinesis::path_fraction(std::span<const glm::vec2> {})({ 5.F, 5.F }), 0.F);
}

TEST(PathFractionTest, LiveOffsetMovesTheFrameTheCursorIsMeasuredIn)
{
    const auto project = Kinesis::path_fraction(k_path, [] { return glm::vec2(10.F, 0.F); });

    EXPECT_NEAR(project({ 11.F, 0.F }), 0.25F, 1e-6F);
}

TEST(ScaledProjectionTest, RemapsAUnitProjectionOntoARange)
{
    const AABB2D bounds { .min = { 0.F, 0.F }, .max = { 10.F, 2.F } };
    const auto project = Kinesis::scaled(Kinesis::axis_fraction(bounds), 10.F, 20.F);

    EXPECT_FLOAT_EQ(project({ 0.F, 0.F }), 10.F);
    EXPECT_FLOAT_EQ(project({ 5.F, 0.F }), 15.F);
    EXPECT_FLOAT_EQ(project({ 10.F, 0.F }), 20.F);
}

TEST(ScaledProjectionTest, AnInvertedRangeFlipsTheMapping)
{
    const AABB2D bounds { .min = { 0.F, 0.F }, .max = { 10.F, 2.F } };
    const auto project = Kinesis::scaled(Kinesis::axis_fraction(bounds), 1.F, 0.F);

    EXPECT_FLOAT_EQ(project({ 0.F, 0.F }), 1.F);
    EXPECT_FLOAT_EQ(project({ 10.F, 0.F }), 0.F);
}

}
