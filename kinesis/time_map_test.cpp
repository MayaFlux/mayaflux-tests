#include "gtest/gtest.h"

#include "MayaFlux/Kinesis/Tendency/TimeMap.hpp"

namespace MayaFlux::Test {
namespace {

    namespace TimeMaps = Kinesis::TimeMaps;

    TEST(TimeMapLinearTest, TravelsStraightFromStartToEndOverTheDuration)
    {
        const auto path = TimeMaps::linear(0.0, 10.0, 2.0);

        EXPECT_DOUBLE_EQ(path(0.0), 0.0);
        EXPECT_DOUBLE_EQ(path(0.5), 2.5);
        EXPECT_DOUBLE_EQ(path(1.0), 5.0);
        EXPECT_DOUBLE_EQ(path(2.0), 10.0);
    }

    TEST(TimeMapLinearTest, HoldsItsEndsOutsideTheDuration)
    {
        const auto path = TimeMaps::linear(0.0, 10.0, 2.0);

        EXPECT_DOUBLE_EQ(path(-1.0), 0.0);
        EXPECT_DOUBLE_EQ(path(5.0), 10.0);
    }

    TEST(TimeMapLinearTest, ADescendingPathHoldsBothEndsToo)
    {
        const auto path = TimeMaps::linear(10.0, 0.0, 2.0);

        EXPECT_DOUBLE_EQ(path(-1.0), 10.0);
        EXPECT_DOUBLE_EQ(path(1.0), 5.0);
        EXPECT_DOUBLE_EQ(path(5.0), 0.0);
    }

    TEST(TimeMapLinearTest, ANonPositiveDurationIsAlwaysTheEnd)
    {
        EXPECT_DOUBLE_EQ(TimeMaps::linear(3.0, 9.0, 0.0)(0.0), 9.0);
        EXPECT_DOUBLE_EQ(TimeMaps::linear(3.0, 9.0, 0.0)(4.0), 9.0);
        EXPECT_DOUBLE_EQ(TimeMaps::linear(3.0, 9.0, -1.0)(0.5), 9.0);
    }

    TEST(TimeMapQuadraticTest, FollowsPositionVelocityAndAcceleration)
    {
        const auto path = TimeMaps::quadratic(1.0, 2.0, 4.0);

        EXPECT_DOUBLE_EQ(path(0.0), 1.0);
        EXPECT_DOUBLE_EQ(path(1.0), 5.0);
        EXPECT_DOUBLE_EQ(path(2.0), 13.0);
    }

    TEST(TimeMapQuadraticTest, ZeroAccelerationIsConstantVelocityAndNegativeReverses)
    {
        EXPECT_DOUBLE_EQ(TimeMaps::quadratic(100.0, 48.0, 0.0)(0.5), 124.0);
        EXPECT_DOUBLE_EQ(TimeMaps::quadratic(100.0, -48.0, 0.0)(0.5), 76.0);
    }

    TEST(TimeMapQuadraticTest, ZeroVelocityAndAccelerationHoldsThePosition)
    {
        const auto path = TimeMaps::quadratic(700.0, 0.0, 0.0);

        EXPECT_DOUBLE_EQ(path(0.0), 700.0);
        EXPECT_DOUBLE_EQ(path(123.0), 700.0);
    }

    TEST(TimeMapQuadraticTest, TimeBeforeZeroIsTreatedAsZero)
    {
        const auto path = TimeMaps::quadratic(1.0, 2.0, 4.0);

        EXPECT_DOUBLE_EQ(path(-3.0), 1.0);
    }

    TEST(TimeMapExponentialTest, EachEqualStepMultipliesByTheSameFactor)
    {
        const auto path = TimeMaps::exponential(1.0, 8.0, 3.0);

        EXPECT_NEAR(path(0.0), 1.0, 1e-12);
        EXPECT_NEAR(path(1.0), 2.0, 1e-12);
        EXPECT_NEAR(path(2.0), 4.0, 1e-12);
        EXPECT_NEAR(path(3.0), 8.0, 1e-12);
    }

    TEST(TimeMapExponentialTest, ShrinkingPathsFallGeometrically)
    {
        const auto path = TimeMaps::exponential(8.0, 1.0, 3.0);

        EXPECT_NEAR(path(1.0), 4.0, 1e-12);
        EXPECT_NEAR(path(2.0), 2.0, 1e-12);
    }

    TEST(TimeMapExponentialTest, HoldsItsEndsOutsideTheDuration)
    {
        const auto path = TimeMaps::exponential(1.0, 8.0, 3.0);

        EXPECT_DOUBLE_EQ(path(-1.0), 1.0);
        EXPECT_DOUBLE_EQ(path(10.0), 8.0);
    }

    TEST(TimeMapExponentialTest, InvalidArgumentsGiveTheEndEverywhere)
    {
        EXPECT_DOUBLE_EQ(TimeMaps::exponential(1.0, 8.0, 0.0)(0.0), 8.0);
        EXPECT_DOUBLE_EQ(TimeMaps::exponential(0.0, 8.0, 3.0)(1.0), 8.0);
        EXPECT_DOUBLE_EQ(TimeMaps::exponential(-1.0, 8.0, 3.0)(1.0), 8.0);
        EXPECT_DOUBLE_EQ(TimeMaps::exponential(1.0, -2.0, 3.0)(0.0), -2.0);
    }

    TEST(TimeMapTriangleTest, BouncesBetweenItsBounds)
    {
        const auto path = TimeMaps::triangle(0.0, 10.0, 5.0, 0.0);

        EXPECT_DOUBLE_EQ(path(0.0), 0.0);
        EXPECT_DOUBLE_EQ(path(1.0), 5.0);
        EXPECT_DOUBLE_EQ(path(2.0), 10.0);
        EXPECT_DOUBLE_EQ(path(3.0), 5.0);
        EXPECT_DOUBLE_EQ(path(4.0), 0.0);
        EXPECT_DOUBLE_EQ(path(5.0), 5.0);
    }

    TEST(TimeMapTriangleTest, StartsFromTheGivenPositionAndNegativeVelocityGoesDownFirst)
    {
        EXPECT_DOUBLE_EQ(TimeMaps::triangle(0.0, 10.0, 0.0, 7.0)(3.0), 7.0);

        const auto down = TimeMaps::triangle(0.0, 10.0, -5.0, 5.0);
        EXPECT_DOUBLE_EQ(down(0.0), 5.0);
        EXPECT_DOUBLE_EQ(down(1.0), 0.0);
        EXPECT_DOUBLE_EQ(down(2.0), 5.0);
    }

    TEST(TimeMapLaggedTest, TrailsTheOriginAdvancingAtTheRateByTheLag)
    {
        const auto path = TimeMaps::lagged(100.0, 10.0, TimeMaps::linear(4.0, 4.0, 1.0));

        EXPECT_DOUBLE_EQ(path(0.0), 96.0);
        EXPECT_DOUBLE_EQ(path(2.0), 116.0);
    }

    TEST(TimeMapLaggedTest, AMovingLagIsSubtractedAtEachTime)
    {
        const auto path = TimeMaps::lagged(100.0, 10.0, TimeMaps::linear(0.0, 6.0, 3.0));

        EXPECT_DOUBLE_EQ(path(0.0), 100.0);
        EXPECT_DOUBLE_EQ(path(3.0), 124.0);
        EXPECT_DOUBLE_EQ(path(6.0), 154.0);
    }

    TEST(TimeMapIntegratedTest, AccumulatesVelocityFromTheStart)
    {
        const auto velocity = std::make_shared<double>(2.0);
        const auto path = TimeMaps::integrated(10.0, velocity);

        EXPECT_DOUBLE_EQ(path(0.0), 10.0);
        EXPECT_DOUBLE_EQ(path(1.0), 12.0);
        EXPECT_DOUBLE_EQ(path(2.5), 15.0);
    }

    TEST(TimeMapIntegratedTest, ChangingTheVelocityNeverJumpsThePosition)
    {
        const auto velocity = std::make_shared<double>(2.0);
        const auto path = TimeMaps::integrated(10.0, velocity);

        EXPECT_DOUBLE_EQ(path(1.0), 12.0);
        *velocity = 100.0;
        EXPECT_DOUBLE_EQ(path(1.0), 12.0);
        EXPECT_DOUBLE_EQ(path(2.0), 112.0);
    }

    TEST(TimeMapIntegratedTest, ANegativeVelocityRunsBackwardsAndZeroHolds)
    {
        const auto velocity = std::make_shared<double>(2.0);
        const auto path = TimeMaps::integrated(10.0, velocity);

        EXPECT_DOUBLE_EQ(path(1.0), 12.0);
        *velocity = -1.0;
        EXPECT_DOUBLE_EQ(path(3.0), 10.0);
        *velocity = 0.0;
        EXPECT_DOUBLE_EQ(path(5.0), 10.0);
        EXPECT_DOUBLE_EQ(path(9.0), 10.0);
    }

    TEST(TimeMapIntegratedTest, AskingAgainForTheSameTimeDoesNotAdvance)
    {
        const auto velocity = std::make_shared<double>(3.0);
        const auto path = TimeMaps::integrated(0.0, velocity);

        EXPECT_DOUBLE_EQ(path(2.0), 6.0);
        EXPECT_DOUBLE_EQ(path(2.0), 6.0);
        EXPECT_DOUBLE_EQ(path(2.0), 6.0);
    }

    TEST(TimeMapIntegratedTest, TimeGoingBackwardsRestartsFromTheStart)
    {
        const auto velocity = std::make_shared<double>(2.0);
        const auto path = TimeMaps::integrated(10.0, velocity);

        EXPECT_DOUBLE_EQ(path(6.0), 22.0);
        EXPECT_DOUBLE_EQ(path(1.0), 12.0);
        EXPECT_DOUBLE_EQ(path(2.0), 14.0);
    }

    TEST(TimeMapIntegratedTest, TwoInstancesKeepSeparatePositions)
    {
        const auto velocity = std::make_shared<double>(1.0);
        const auto first = TimeMaps::integrated(0.0, velocity);
        const auto second = TimeMaps::integrated(0.0, velocity);

        EXPECT_DOUBLE_EQ(first(4.0), 4.0);
        EXPECT_DOUBLE_EQ(second(1.0), 1.0);
        EXPECT_DOUBLE_EQ(first(5.0), 5.0);
    }

} // namespace
} // namespace MayaFlux::Test
