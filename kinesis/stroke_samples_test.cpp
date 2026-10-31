#include "gtest/gtest.h"

#include "MayaFlux/Kinesis/Spatial/Stroke.hpp"

namespace MayaFlux::Test {

using Kinesis::stroke_samples;

TEST(StrokeSamplesTest, EmptyArrayIsUntouchedAndKeepsThePreviousIndex)
{
    std::vector<float> samples;
    std::optional<size_t> previous = 3;

    stroke_samples(samples, previous, 0.5F, 0.5F);

    EXPECT_TRUE(samples.empty());
    EXPECT_EQ(previous, std::optional<size_t> { 3 });
}

TEST(StrokeSamplesTest, FirstWriteSetsOneSampleAndRecordsItsIndex)
{
    std::vector<float> samples(5, 0.F);
    std::optional<size_t> previous;

    stroke_samples(samples, previous, 0.5F, 0.8F);

    EXPECT_EQ(samples, (std::vector<float> { 0.F, 0.F, 0.8F, 0.F, 0.F }));
    EXPECT_EQ(previous, std::optional<size_t> { 2 });
}

TEST(StrokeSamplesTest, PositionMapsOntoTheFirstAndLastSamples)
{
    std::vector<float> samples(5, 0.F);
    std::optional<size_t> previous;

    stroke_samples(samples, previous, 0.F, 1.F);
    EXPECT_EQ(previous, std::optional<size_t> { 0 });
    EXPECT_FLOAT_EQ(samples.front(), 1.F);

    previous.reset();
    stroke_samples(samples, previous, 1.F, 0.5F);
    EXPECT_EQ(previous, std::optional<size_t> { 4 });
    EXPECT_FLOAT_EQ(samples.back(), 0.5F);
}

TEST(StrokeSamplesTest, PositionAndLevelAreClamped)
{
    std::vector<float> samples(4, 0.5F);
    std::optional<size_t> previous;

    stroke_samples(samples, previous, -3.F, 9.F);
    EXPECT_EQ(previous, std::optional<size_t> { 0 });
    EXPECT_FLOAT_EQ(samples[0], 1.F);

    previous.reset();
    stroke_samples(samples, previous, 7.F, -2.F);
    EXPECT_EQ(previous, std::optional<size_t> { 3 });
    EXPECT_FLOAT_EQ(samples[3], 0.F);
}

TEST(StrokeSamplesTest, NanSelectsTheFirstSampleAndInfinityClampsToTheEnds)
{
    std::vector<float> samples(4, 0.F);
    std::optional<size_t> previous;

    stroke_samples(samples, previous, std::numeric_limits<float>::quiet_NaN(), 1.F);
    EXPECT_EQ(previous, std::optional<size_t> { 0 });

    previous.reset();
    stroke_samples(samples, previous, std::numeric_limits<float>::infinity(), 1.F);
    EXPECT_EQ(previous, std::optional<size_t> { 3 });

    previous.reset();
    stroke_samples(samples, previous, -std::numeric_limits<float>::infinity(), 1.F);
    EXPECT_EQ(previous, std::optional<size_t> { 0 });
}

TEST(StrokeSamplesTest, SingleSampleArrayAlwaysWritesIndexZero)
{
    std::vector<float> samples(1, 0.F);
    std::optional<size_t> previous;

    stroke_samples(samples, previous, 0.9F, 0.7F);
    stroke_samples(samples, previous, 0.1F, 0.2F);

    EXPECT_FLOAT_EQ(samples[0], 0.2F);
    EXPECT_EQ(previous, std::optional<size_t> { 0 });
}

TEST(StrokeSamplesTest, RepeatingTheSameIndexOverwritesOnlyThatSample)
{
    std::vector<float> samples { 0.1F, 0.2F, 0.3F, 0.4F, 0.5F };
    std::optional<size_t> previous;

    stroke_samples(samples, previous, 0.5F, 0.9F);
    stroke_samples(samples, previous, 0.5F, 0.6F);

    EXPECT_EQ(samples, (std::vector<float> { 0.1F, 0.2F, 0.6F, 0.4F, 0.5F }));
}

TEST(StrokeSamplesTest, OutOfRangePreviousIndexIsTreatedAsAFreshGesture)
{
    std::vector<float> samples(4, 0.F);
    std::optional<size_t> previous = 99;

    stroke_samples(samples, previous, 1.F, 1.F);

    EXPECT_EQ(samples, (std::vector<float> { 0.F, 0.F, 0.F, 1.F }));
    EXPECT_EQ(previous, std::optional<size_t> { 3 });
}

struct StrokeRun {
    size_t from;
    size_t to;
};

class StrokeSamplesRunTest : public ::testing::TestWithParam<StrokeRun> { };

INSTANTIATE_TEST_SUITE_P(Runs, StrokeSamplesRunTest,
    ::testing::Values(
        StrokeRun { 0, 4 },
        StrokeRun { 4, 0 },
        StrokeRun { 1, 3 },
        StrokeRun { 3, 1 },
        StrokeRun { 2, 3 },
        StrokeRun { 3, 2 }));

TEST_P(StrokeSamplesRunTest, RampRunsFromTheOldValueToTheNewLevelInEitherDirection)
{
    const auto [from, to] = GetParam();
    std::vector<float> samples(5, 0.F);
    std::optional<size_t> previous;
    const float start = 0.2F;
    const float target = 0.9F;
    const float last = static_cast<float>(samples.size() - 1);

    stroke_samples(samples, previous, static_cast<float>(from) / last, start);
    stroke_samples(samples, previous, static_cast<float>(to) / last, target);

    const size_t lo = std::min(from, to);
    const size_t hi = std::max(from, to);
    EXPECT_FLOAT_EQ(samples[from], start);
    EXPECT_FLOAT_EQ(samples[to], target);
    for (size_t i = lo; i <= hi; ++i) {
        const float travelled = static_cast<float>(i > from ? i - from : from - i);
        const float span = static_cast<float>(hi - lo);
        EXPECT_NEAR(samples[i], start + (target - start) * travelled / span, 1e-6F) << "index " << i;
    }
    EXPECT_EQ(previous, std::optional<size_t> { to });
}

TEST_P(StrokeSamplesRunTest, SamplesOutsideTheRunAreUntouched)
{
    const auto [from, to] = GetParam();
    std::vector<float> samples(5, 0.33F);
    std::optional<size_t> previous;
    const float last = static_cast<float>(samples.size() - 1);

    stroke_samples(samples, previous, static_cast<float>(from) / last, 0.F);
    stroke_samples(samples, previous, static_cast<float>(to) / last, 1.F);

    for (size_t i = 0; i < samples.size(); ++i) {
        if (i < std::min(from, to) || i > std::max(from, to))
            EXPECT_FLOAT_EQ(samples[i], 0.33F) << "index " << i;
    }
}

TEST(StrokeSamplesTest, ResettingPreviousStartsANewGestureWithoutFilling)
{
    std::vector<float> samples(5, 0.F);
    std::optional<size_t> previous;

    stroke_samples(samples, previous, 0.F, 1.F);
    previous.reset();
    stroke_samples(samples, previous, 1.F, 1.F);

    EXPECT_EQ(samples, (std::vector<float> { 1.F, 0.F, 0.F, 0.F, 1.F }));
}

}
