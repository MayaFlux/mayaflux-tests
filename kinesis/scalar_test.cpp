#include "gtest/gtest.h"
#include "../support/test_metadata.hpp"

#include "MayaFlux/Kinesis/Scalar.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {
namespace {

    TEST(KinesisScalarMapTest, MapsLinearlyBetweenTheRanges)
    {
        EXPECT_DOUBLE_EQ(Kinesis::map(0.0, 0.0, 10.0, 0.0, 100.0), 0.0);
        EXPECT_DOUBLE_EQ(Kinesis::map(5.0, 0.0, 10.0, 0.0, 100.0), 50.0);
        EXPECT_DOUBLE_EQ(Kinesis::map(10.0, 0.0, 10.0, 0.0, 100.0), 100.0);
    }

    TEST(KinesisScalarMapTest, ExtrapolatesBeyondTheInputRange)
    {
        EXPECT_DOUBLE_EQ(Kinesis::map(15.0, 0.0, 10.0, 0.0, 100.0), 150.0);
        EXPECT_DOUBLE_EQ(Kinesis::map(-5.0, 0.0, 10.0, 0.0, 100.0), -50.0);
    }

    TEST(KinesisScalarMapTest, ReversedOutputRangeInverts)
    {
        EXPECT_DOUBLE_EQ(Kinesis::map(2.5, 0.0, 10.0, 10.0, 0.0), 7.5);
    }

    TEST(KinesisScalarMapTest, AnEmptyInputRangeGivesTheOutputStart)
    {
        EXPECT_DOUBLE_EQ(Kinesis::map(3.0, 5.0, 5.0, 2.0, 9.0), 2.0);
    }

    TEST(KinesisScalarMapTest, ClampedMappingStaysInsideTheOutputRange)
    {
        EXPECT_DOUBLE_EQ(Kinesis::map_clamped(5.0, 0.0, 10.0, 0.0, 100.0), 50.0);
        EXPECT_DOUBLE_EQ(Kinesis::map_clamped(15.0, 0.0, 10.0, 0.0, 100.0), 100.0);
        EXPECT_DOUBLE_EQ(Kinesis::map_clamped(-5.0, 0.0, 10.0, 0.0, 100.0), 0.0);
    }

    TEST(KinesisScalarMapTest, ClampedMappingToAReversedRangeClampsToItsBounds)
    {
        EXPECT_DOUBLE_EQ(Kinesis::map_clamped(15.0, 0.0, 10.0, 10.0, 0.0), 0.0);
        EXPECT_DOUBLE_EQ(Kinesis::map_clamped(-5.0, 0.0, 10.0, 10.0, 0.0), 10.0);
    }

    TEST(KinesisScalarWrapTest, ValuesInsideTheRangeAreUnchanged)
    {
        EXPECT_DOUBLE_EQ(Kinesis::wrap(5.0, 0.0, 10.0), 5.0);
        EXPECT_DOUBLE_EQ(Kinesis::wrap(0.0, 0.0, 10.0), 0.0);
    }

    TEST(KinesisScalarWrapTest, TheUpperBoundIsExclusive)
    {
        EXPECT_DOUBLE_EQ(Kinesis::wrap(10.0, 0.0, 10.0), 0.0);
    }

    TEST(KinesisScalarWrapTest, WrapsAboveAndBelowTheRange)
    {
        EXPECT_DOUBLE_EQ(Kinesis::wrap(12.0, 0.0, 10.0), 2.0);
        EXPECT_DOUBLE_EQ(Kinesis::wrap(35.0, 0.0, 10.0), 5.0);
        EXPECT_DOUBLE_EQ(Kinesis::wrap(-1.0, 0.0, 10.0), 9.0);
        EXPECT_DOUBLE_EQ(Kinesis::wrap(-23.0, 0.0, 10.0), 7.0);
    }

    TEST(KinesisScalarWrapTest, WrapsRelativeToAnOffsetRange)
    {
        EXPECT_DOUBLE_EQ(Kinesis::wrap(25.0, 10.0, 20.0), 15.0);
        EXPECT_DOUBLE_EQ(Kinesis::wrap(5.0, 10.0, 20.0), 15.0);
    }

    TEST(KinesisScalarWrapTest, ANonPositiveRangeGivesTheLowerBound)
    {
        EXPECT_DOUBLE_EQ(Kinesis::wrap(3.0, 5.0, 5.0), 5.0);
        EXPECT_DOUBLE_EQ(Kinesis::wrap(3.0, 5.0, 2.0), 5.0);
    }

    TEST(KinesisScalarPingPongTest, ValuesInsideTheRangeAreUnchanged)
    {
        EXPECT_DOUBLE_EQ(Kinesis::ping_pong(0.0, 0.0, 10.0), 0.0);
        EXPECT_DOUBLE_EQ(Kinesis::ping_pong(5.0, 0.0, 10.0), 5.0);
        EXPECT_DOUBLE_EQ(Kinesis::ping_pong(10.0, 0.0, 10.0), 10.0);
    }

    TEST(KinesisScalarPingPongTest, ReflectsAtBothBounds)
    {
        EXPECT_DOUBLE_EQ(Kinesis::ping_pong(12.0, 0.0, 10.0), 8.0);
        EXPECT_DOUBLE_EQ(Kinesis::ping_pong(20.0, 0.0, 10.0), 0.0);
        EXPECT_DOUBLE_EQ(Kinesis::ping_pong(25.0, 0.0, 10.0), 5.0);
        EXPECT_DOUBLE_EQ(Kinesis::ping_pong(-3.0, 0.0, 10.0), 3.0);
        EXPECT_DOUBLE_EQ(Kinesis::ping_pong(-10.0, 0.0, 10.0), 10.0);
    }

    TEST(KinesisScalarPingPongTest, ReflectsWithinAnOffsetRange)
    {
        EXPECT_DOUBLE_EQ(Kinesis::ping_pong(13.0, 10.0, 20.0), 13.0);
        EXPECT_DOUBLE_EQ(Kinesis::ping_pong(23.0, 10.0, 20.0), 17.0);
        EXPECT_DOUBLE_EQ(Kinesis::ping_pong(5.0, 10.0, 20.0), 15.0);
    }

    TEST(KinesisScalarPingPongTest, ANonPositiveRangeGivesTheLowerBound)
    {
        EXPECT_DOUBLE_EQ(Kinesis::ping_pong(3.0, 5.0, 5.0), 5.0);
        EXPECT_DOUBLE_EQ(Kinesis::ping_pong(3.0, 5.0, 2.0), 5.0);
    }

    TEST(KinesisScalarSoftLimitTest, IsTheIdentityUpToTheThreshold)
    {
        EXPECT_DOUBLE_EQ(Kinesis::soft_limit(0.0), 0.0);
        EXPECT_DOUBLE_EQ(Kinesis::soft_limit(0.5), 0.5);
        EXPECT_DOUBLE_EQ(Kinesis::soft_limit(-0.5), -0.5);
        EXPECT_DOUBLE_EQ(Kinesis::soft_limit(0.95), 0.95);
    }

    TEST(KinesisScalarSoftLimitTest, KeepsTheSignAndIsOddInput)
    {
        EXPECT_DOUBLE_EQ(Kinesis::soft_limit(-2.0), -Kinesis::soft_limit(2.0));
        EXPECT_LT(Kinesis::soft_limit(-2.0), 0.0);
    }

    TEST(KinesisScalarSoftLimitTest, IsMonotonicAndNeverExceedsTheCeiling)
    {
        double previous = Kinesis::soft_limit(0.0);
        for (int step = 1; step <= 400; ++step) {
            const double x = step * 0.05;
            const double y = Kinesis::soft_limit(x);
            EXPECT_GE(y, previous) << "input " << x;
            EXPECT_LE(y, 1.0) << "input " << x;
            previous = y;
        }
    }

    TEST(KinesisScalarSoftLimitTest, HonoursAnExplicitThresholdAndCeiling)
    {
        const double limited = Kinesis::soft_limit(0.4, 0.3, 0.5);
        EXPECT_GT(limited, 0.3);
        EXPECT_LT(limited, 0.5);
        EXPECT_DOUBLE_EQ(Kinesis::soft_limit(0.2, 0.3, 0.5), 0.2);
    }

} // namespace
} // namespace MayaFlux::Test
