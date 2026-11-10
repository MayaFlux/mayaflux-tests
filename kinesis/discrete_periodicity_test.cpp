#include "gtest/gtest.h"

#include "MayaFlux/Kinesis/Discrete/Analysis.hpp"
#include "MayaFlux/Kinesis/TemporalMeasures.hpp"

#include <random>

namespace MayaFlux::Test {
namespace {

    namespace D = Kinesis::Discrete;

    std::vector<double> sine_with_period(size_t count, double period)
    {
        std::vector<double> data(count);
        for (size_t i = 0; i < count; ++i) {
            data[i] = std::sin(2.0 * M_PI * static_cast<double>(i) / period);
        }
        return data;
    }

    std::vector<double> noise(size_t count, uint32_t seed)
    {
        std::mt19937 engine(seed);
        std::uniform_real_distribution<double> dist(-1.0, 1.0);
        std::vector<double> data(count);
        for (double& v : data) {
            v = dist(engine);
        }
        return data;
    }

    std::pair<std::vector<double>, std::vector<size_t>> measure(const std::vector<double>& data, uint32_t window, uint32_t hop, uint32_t min_lag, uint32_t max_lag)
    {
        return D::periodicity(data, D::num_windows(data.size(), window, hop), hop, window, min_lag, max_lag);
    }

    TEST(DiscretePeriodicityTest, ASineGivesItsPeriodAndStrengthNearOne)
    {
        const auto [strengths, lags] = measure(sine_with_period(4096, 40.0), 1024, 512, 20, 200);

        ASSERT_EQ(lags.size(), 7U);
        for (size_t w = 0; w < lags.size(); ++w) {
            EXPECT_EQ(lags[w], 40U);
            EXPECT_GT(strengths[w], 0.9);
            EXPECT_LE(strengths[w], 1.0);
        }
    }

    TEST(DiscretePeriodicityTest, NoiseGivesLowStrength)
    {
        const auto [strengths, lags] = measure(noise(4096, 7), 1024, 512, 20, 200);

        ASSERT_FALSE(strengths.empty());
        for (const double strength : strengths) {
            EXPECT_LT(strength, 0.2);
        }
    }

    TEST(DiscretePeriodicityTest, SilenceGivesNothing)
    {
        const auto [strengths, lags] = measure(std::vector<double>(4096, 0.0), 1024, 512, 20, 200);

        ASSERT_FALSE(strengths.empty());
        for (size_t w = 0; w < strengths.size(); ++w) {
            EXPECT_EQ(strengths[w], 0.0);
            EXPECT_EQ(lags[w], 0U);
        }
    }

    TEST(DiscretePeriodicityTest, ALagRangeExcludingThePeriodDoesNotReportIt)
    {
        const auto [strengths, lags] = measure(sine_with_period(4096, 40.0), 1024, 512, 21, 29);

        ASSERT_FALSE(lags.empty());
        for (size_t w = 0; w < lags.size(); ++w) {
            EXPECT_EQ(lags[w], 0U);
            EXPECT_EQ(strengths[w], 0.0);
        }
    }

    TEST(DiscretePeriodicityTest, ARangeBeyondHalfTheWindowGivesNoEstimate)
    {
        const auto [strengths, lags] = measure(sine_with_period(400, 40.0), 100, 100, 60, 80);

        ASSERT_EQ(lags.size(), 4U);
        for (size_t w = 0; w < lags.size(); ++w) {
            EXPECT_EQ(lags[w], 0U);
            EXPECT_EQ(strengths[w], 0.0);
        }
    }

    TEST(DiscretePeriodicityTest, EachWindowFollowsItsOwnSignal)
    {
        std::vector<double> data = sine_with_period(512, 40.0);
        const auto second = sine_with_period(512, 60.0);
        data.insert(data.end(), second.begin(), second.end());

        const auto [strengths, lags] = measure(data, 256, 256, 20, 100);

        ASSERT_EQ(lags.size(), 4U);
        EXPECT_EQ(lags[0], 40U);
        EXPECT_EQ(lags[1], 40U);
        EXPECT_EQ(lags[2], 60U);
        EXPECT_EQ(lags[3], 60U);
    }

    TEST(DiscretePeriodicityTest, EachWindowAgreesWithEstimatePeriod)
    {
        auto data = noise(3000, 11);
        const auto tone = sine_with_period(3000, 57.0);
        for (size_t i = 0; i < data.size(); ++i) {
            data[i] = 0.3 * data[i] + (i % 1500 < 700 ? tone[i] : 0.0);
        }

        constexpr uint32_t window = 512;
        constexpr uint32_t hop = 300;
        const auto [strengths, lags] = measure(data, window, hop, 25, 150);

        ASSERT_EQ(lags.size(), D::num_windows(data.size(), window, hop));
        for (size_t w = 0; w < lags.size(); ++w) {
            const std::span<const double> view(data.data() + w * hop, window);
            const auto expected = Kinesis::estimate_period(view, 1.0, 25.0, 150.0);
            EXPECT_EQ(lags[w], static_cast<size_t>(expected.period));
            EXPECT_EQ(strengths[w], expected.strength);
        }
    }

    TEST(DiscretePeriodicityTest, NoWindowsGiveEmptyVectors)
    {
        const auto [strengths, lags] = measure(sine_with_period(100, 40.0), 1024, 512, 20, 200);

        EXPECT_TRUE(lags.empty());
        EXPECT_TRUE(strengths.empty());
    }

}
}
