#include "gtest/gtest.h"

#include "MayaFlux/Kinesis/Discrete/Analysis.hpp"

namespace MayaFlux::Test {
namespace {

    namespace D = Kinesis::Discrete;

    using Windowed = std::function<std::vector<double>(std::span<const double>, size_t, uint32_t, uint32_t)>;

    constexpr size_t k_samples = 1024;
    constexpr uint32_t k_window = 256;
    constexpr uint32_t k_hop = 128;

    std::vector<double> varied(size_t count)
    {
        std::vector<double> data(count);
        for (size_t i = 0; i < count; ++i) {
            data[i] = 0.5 * std::sin(0.1 * static_cast<double>(i)) + 0.001 * static_cast<double>(i) + 0.2;
        }
        return data;
    }

    std::vector<double> sine(double amplitude, double cycles)
    {
        std::vector<double> data(k_samples);
        for (size_t i = 0; i < data.size(); ++i) {
            data[i] = amplitude * std::sin(2.0 * M_PI * cycles * static_cast<double>(i) / static_cast<double>(k_samples));
        }
        return data;
    }

    std::vector<double> impulses(double floor, double peak)
    {
        std::vector<double> data(k_samples, floor);
        for (size_t i = 25; i < data.size(); i += 50) {
            data[i] = peak;
        }
        return data;
    }

    std::vector<double> alternating(size_t count)
    {
        std::vector<double> data(count);
        for (size_t i = 0; i < count; ++i) {
            data[i] = (i % 2 == 0) ? 1.0 : -1.0;
        }
        return data;
    }

    std::vector<double> one_to_ten()
    {
        return { 1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 9.0, 10.0 };
    }

    // ------------------------------------------------------------------
    // Window count
    // ------------------------------------------------------------------

    struct CountCase {
        size_t samples;
        uint32_t window;
        uint32_t hop;
        size_t expected;
    };

    class DiscreteWindowCountTest : public ::testing::TestWithParam<CountCase> { };

    INSTANTIATE_TEST_SUITE_P(Geometries, DiscreteWindowCountTest,
        ::testing::Values(
            CountCase { 100, 256, 128, 0 },
            CountCase { 256, 256, 128, 1 },
            CountCase { 1024, 256, 128, 7 },
            CountCase { 1024, 128, 64, 15 },
            CountCase { 1024, 512, 256, 3 },
            CountCase { 10, 10, 5, 1 },
            CountCase { 100, 10, 5, 19 }),
        [](const ::testing::TestParamInfo<CountCase>& info) {
            return "Samples" + std::to_string(info.param.samples) + "Window" + std::to_string(info.param.window)
                + "Hop" + std::to_string(info.param.hop);
        });

    TEST_P(DiscreteWindowCountTest, OnlyCompleteWindowsAreCounted)
    {
        const auto& c = GetParam();
        EXPECT_EQ(D::num_windows(c.samples, c.window, c.hop), c.expected);
    }

    // ------------------------------------------------------------------
    // Properties every windowed function shares
    // ------------------------------------------------------------------

    struct WindowedFunction {
        const char* name;
        Windowed fn;
    };

    const std::vector<WindowedFunction>& every_windowed_function()
    {
        static const std::vector<WindowedFunction> functions {
            { "Rms", [](auto d, auto n, auto h, auto w) { return D::rms(d, n, h, w); } },
            { "Peak", [](auto d, auto n, auto h, auto w) { return D::peak(d, n, h, w); } },
            { "Power", [](auto d, auto n, auto h, auto w) { return D::power(d, n, h, w); } },
            { "DynamicRange", [](auto d, auto n, auto h, auto w) { return D::dynamic_range(d, n, h, w); } },
            { "ZeroCrossingRate", [](auto d, auto n, auto h, auto w) { return D::zero_crossing_rate(d, n, h, w); } },
            { "SpectralEnergy", [](auto d, auto n, auto h, auto w) { return D::spectral_energy(d, n, h, w); } },
            { "LowFrequencyEnergy", [](auto d, auto n, auto h, auto w) { return D::low_frequency_energy(d, n, h, w); } },
            { "Mean", [](auto d, auto n, auto h, auto w) { return D::mean(d, n, h, w); } },
            { "Variance", [](auto d, auto n, auto h, auto w) { return D::variance(d, n, h, w); } },
            { "StdDev", [](auto d, auto n, auto h, auto w) { return D::std_dev(d, n, h, w); } },
            { "Skewness", [](auto d, auto n, auto h, auto w) { return D::skewness(d, n, h, w); } },
            { "Kurtosis", [](auto d, auto n, auto h, auto w) { return D::kurtosis(d, n, h, w); } },
            { "Median", [](auto d, auto n, auto h, auto w) { return D::median(d, n, h, w); } },
            { "Percentile", [](auto d, auto n, auto h, auto w) { return D::percentile(d, n, h, w, 50.0); } },
            { "Entropy", [](auto d, auto n, auto h, auto w) { return D::entropy(d, n, h, w); } },
            { "Min", [](auto d, auto n, auto h, auto w) { return D::min(d, n, h, w); } },
            { "Max", [](auto d, auto n, auto h, auto w) { return D::max(d, n, h, w); } },
            { "Range", [](auto d, auto n, auto h, auto w) { return D::range(d, n, h, w); } },
            { "Sum", [](auto d, auto n, auto h, auto w) { return D::sum(d, n, h, w); } },
            { "Count", [](auto d, auto n, auto h, auto w) { return D::count(d, n, h, w); } },
            { "Mad", [](auto d, auto n, auto h, auto w) { return D::mad(d, n, h, w); } },
            { "CoefficientOfVariation", [](auto d, auto n, auto h, auto w) { return D::coefficient_of_variation(d, n, h, w); } },
            { "Mode", [](auto d, auto n, auto h, auto w) { return D::mode(d, n, h, w); } },
            { "MeanZscore", [](auto d, auto n, auto h, auto w) { return D::mean_zscore(d, n, h, w); } },
        };
        return functions;
    }

    class DiscreteWindowedTest : public ::testing::TestWithParam<WindowedFunction> {
    protected:
        std::vector<double> data = varied(k_samples);
        size_t windows = D::num_windows(k_samples, k_window, k_hop);
    };

    INSTANTIATE_TEST_SUITE_P(Functions, DiscreteWindowedTest,
        ::testing::ValuesIn(every_windowed_function()),
        [](const ::testing::TestParamInfo<WindowedFunction>& info) {
            return std::string(info.param.name);
        });

    TEST_P(DiscreteWindowedTest, ThereIsOneFiniteValuePerWindow)
    {
        const auto out = GetParam().fn(data, windows, k_hop, k_window);

        ASSERT_EQ(out.size(), windows);
        for (double value : out) {
            EXPECT_TRUE(std::isfinite(value));
        }
    }

    TEST_P(DiscreteWindowedTest, NoWindowsGiveAnEmptyResult)
    {
        EXPECT_TRUE(GetParam().fn(data, 0, k_hop, k_window).empty());
    }

    TEST_P(DiscreteWindowedTest, AWindowReadsOnlyItsOwnSamples)
    {
        const auto before = GetParam().fn(data, windows, k_hop, k_window);

        auto changed = data;
        changed[1000] = 5.0;
        const auto after = GetParam().fn(changed, windows, k_hop, k_window);

        ASSERT_EQ(before.size(), after.size());
        for (size_t i = 0; i + 1 < windows; ++i) {
            EXPECT_EQ(before[i], after[i]) << "window " << i;
        }
    }

    TEST_P(DiscreteWindowedTest, RepeatedCallsAgree)
    {
        EXPECT_EQ(GetParam().fn(data, windows, k_hop, k_window), GetParam().fn(data, windows, k_hop, k_window));
    }

    // ------------------------------------------------------------------
    // Windows map to the right slice, for any window and hop
    // ------------------------------------------------------------------

    struct Reference {
        const char* name;
        Windowed fn;
        std::function<double(std::span<const double>)> expected;
    };

    double slice_power(std::span<const double> s)
    {
        return std::accumulate(s.begin(), s.end(), 0.0, [](double acc, double v) { return acc + v * v; });
    }

    const std::vector<Reference>& references()
    {
        static const std::vector<Reference> table {
            { "Mean", [](auto d, auto n, auto h, auto w) { return D::mean(d, n, h, w); },
                [](auto s) { return std::accumulate(s.begin(), s.end(), 0.0) / static_cast<double>(s.size()); } },
            { "Sum", [](auto d, auto n, auto h, auto w) { return D::sum(d, n, h, w); },
                [](auto s) { return std::accumulate(s.begin(), s.end(), 0.0); } },
            { "Min", [](auto d, auto n, auto h, auto w) { return D::min(d, n, h, w); },
                [](auto s) { return *std::ranges::min_element(s); } },
            { "Max", [](auto d, auto n, auto h, auto w) { return D::max(d, n, h, w); },
                [](auto s) { return *std::ranges::max_element(s); } },
            { "Range", [](auto d, auto n, auto h, auto w) { return D::range(d, n, h, w); },
                [](auto s) { return *std::ranges::max_element(s) - *std::ranges::min_element(s); } },
            { "Peak", [](auto d, auto n, auto h, auto w) { return D::peak(d, n, h, w); },
                [](auto s) {
                    double peak = 0.0;
                    for (double v : s) {
                        peak = std::max(peak, std::abs(v));
                    }
                    return peak;
                } },
            { "Power", [](auto d, auto n, auto h, auto w) { return D::power(d, n, h, w); },
                [](auto s) { return slice_power(s); } },
            { "Rms", [](auto d, auto n, auto h, auto w) { return D::rms(d, n, h, w); },
                [](auto s) { return std::sqrt(slice_power(s) / static_cast<double>(s.size())); } },
            { "Count", [](auto d, auto n, auto h, auto w) { return D::count(d, n, h, w); },
                [](auto s) { return static_cast<double>(s.size()); } },
        };
        return table;
    }

    struct Geometry {
        uint32_t window;
        uint32_t hop;
    };

    class DiscreteGeometryTest : public ::testing::TestWithParam<Geometry> { };

    INSTANTIATE_TEST_SUITE_P(Geometries, DiscreteGeometryTest,
        ::testing::Values(
            Geometry { 64, 32 },
            Geometry { 64, 64 },
            Geometry { 32, 64 },
            Geometry { 100, 30 },
            Geometry { 256, 128 }),
        [](const ::testing::TestParamInfo<Geometry>& info) {
            return "Window" + std::to_string(info.param.window) + "Hop" + std::to_string(info.param.hop);
        });

    TEST_P(DiscreteGeometryTest, EachWindowSummarisesTheSliceStartingAtItsHop)
    {
        const auto data = varied(k_samples);
        const auto [window, hop] = GetParam();
        const size_t windows = D::num_windows(data.size(), window, hop);
        ASSERT_GT(windows, 0U);

        for (const auto& reference : references()) {
            SCOPED_TRACE(reference.name);

            const auto out = reference.fn(data, windows, hop, window);
            ASSERT_EQ(out.size(), windows);

            for (size_t i = 0; i < windows; ++i) {
                const std::span<const double> slice(data.data() + i * hop, window);
                EXPECT_NEAR(out[i], reference.expected(slice), 1e-9) << "window " << i;
            }
        }
    }

    // ------------------------------------------------------------------
    // Statistics of a known window
    // ------------------------------------------------------------------

    struct StatisticCase {
        const char* name;
        Windowed fn;
        double expected;
        double tolerance;
    };

    class DiscreteStatisticTest : public ::testing::TestWithParam<StatisticCase> { };

    INSTANTIATE_TEST_SUITE_P(Statistics, DiscreteStatisticTest,
        ::testing::Values(
            StatisticCase { "Mean", [](auto d, auto n, auto h, auto w) { return D::mean(d, n, h, w); }, 5.5, 1e-12 },
            StatisticCase { "SampleVariance", [](auto d, auto n, auto h, auto w) { return D::variance(d, n, h, w, true); }, 82.5 / 9.0, 1e-9 },
            StatisticCase { "PopulationVariance", [](auto d, auto n, auto h, auto w) { return D::variance(d, n, h, w, false); }, 8.25, 1e-9 },
            StatisticCase { "SampleStdDev", [](auto d, auto n, auto h, auto w) { return D::std_dev(d, n, h, w, true); }, std::sqrt(82.5 / 9.0), 1e-9 },
            StatisticCase { "Min", [](auto d, auto n, auto h, auto w) { return D::min(d, n, h, w); }, 1.0, 1e-12 },
            StatisticCase { "Max", [](auto d, auto n, auto h, auto w) { return D::max(d, n, h, w); }, 10.0, 1e-12 },
            StatisticCase { "Range", [](auto d, auto n, auto h, auto w) { return D::range(d, n, h, w); }, 9.0, 1e-12 },
            StatisticCase { "Median", [](auto d, auto n, auto h, auto w) { return D::median(d, n, h, w); }, 5.5, 1e-12 },
            StatisticCase { "FirstQuartile", [](auto d, auto n, auto h, auto w) { return D::percentile(d, n, h, w, 25.0); }, 3.25, 1e-12 },
            StatisticCase { "Sum", [](auto d, auto n, auto h, auto w) { return D::sum(d, n, h, w); }, 55.0, 1e-12 },
            StatisticCase { "Count", [](auto d, auto n, auto h, auto w) { return D::count(d, n, h, w); }, 10.0, 1e-12 },
            StatisticCase { "SymmetricSkewness", [](auto d, auto n, auto h, auto w) { return D::skewness(d, n, h, w); }, 0.0, 1e-9 }),
        [](const ::testing::TestParamInfo<StatisticCase>& info) {
            return std::string(info.param.name);
        });

    TEST_P(DiscreteStatisticTest, AWindowOverOneToTenReportsTheKnownValue)
    {
        const auto data = one_to_ten();
        const auto out = GetParam().fn(data, D::num_windows(data.size(), 10, 5), 5, 10);

        ASSERT_EQ(out.size(), 1U);
        EXPECT_NEAR(out[0], GetParam().expected, GetParam().tolerance);
    }

    // ------------------------------------------------------------------
    // Energy of known signals
    // ------------------------------------------------------------------

    struct EnergyCase {
        const char* name;
        std::vector<double> signal;
        Windowed fn;
        double expected;
        double tolerance;
    };

    class DiscreteEnergyTest : public ::testing::TestWithParam<EnergyCase> { };

    INSTANTIATE_TEST_SUITE_P(Signals, DiscreteEnergyTest,
        ::testing::Values(
            EnergyCase { "SineRms", sine(0.5, 10.0), [](auto d, auto n, auto h, auto w) { return D::rms(d, n, h, w); }, 0.5 / std::numbers::sqrt2, 0.05 },
            EnergyCase { "ImpulsePeak", impulses(0.1, 0.8), [](auto d, auto n, auto h, auto w) { return D::peak(d, n, h, w); }, 0.8, 1e-12 },
            EnergyCase { "SilenceRms", std::vector<double>(k_samples, 0.0), [](auto d, auto n, auto h, auto w) { return D::rms(d, n, h, w); }, 0.0, 1e-12 },
            EnergyCase { "SilenceSpectralEnergy", std::vector<double>(k_samples, 0.0), [](auto d, auto n, auto h, auto w) { return D::spectral_energy(d, n, h, w); }, 0.0, 1e-12 },
            EnergyCase { "SilenceLowFrequencyEnergy", std::vector<double>(k_samples, 0.0), [](auto d, auto n, auto h, auto w) { return D::low_frequency_energy(d, n, h, w); }, 0.0, 1e-12 },
            EnergyCase { "ConstantRms", std::vector<double>(k_samples, 0.3), [](auto d, auto n, auto h, auto w) { return D::rms(d, n, h, w); }, 0.3, 1e-12 },
            EnergyCase { "ConstantPower", std::vector<double>(k_samples, 0.3), [](auto d, auto n, auto h, auto w) { return D::power(d, n, h, w); }, 0.09 * k_window, 1e-9 },
            EnergyCase { "ConstantDynamicRange", std::vector<double>(k_samples, 0.3), [](auto d, auto n, auto h, auto w) { return D::dynamic_range(d, n, h, w); }, 0.0, 1e-9 },
            EnergyCase { "ConstantZeroCrossingRate", std::vector<double>(k_samples, 0.3), [](auto d, auto n, auto h, auto w) { return D::zero_crossing_rate(d, n, h, w); }, 0.0, 1e-12 },
            EnergyCase { "AlternatingZeroCrossingRate", alternating(k_samples), [](auto d, auto n, auto h, auto w) { return D::zero_crossing_rate(d, n, h, w); }, 1.0, 1e-9 }),
        [](const ::testing::TestParamInfo<EnergyCase>& info) {
            return std::string(info.param.name);
        });

    TEST_P(DiscreteEnergyTest, EveryWindowMatchesTheKnownEnergy)
    {
        const auto& c = GetParam();
        const auto out = c.fn(c.signal, D::num_windows(c.signal.size(), k_window, k_hop), k_hop, k_window);

        ASSERT_FALSE(out.empty());
        for (double value : out) {
            EXPECT_NEAR(value, c.expected, c.tolerance);
        }
    }

    // ------------------------------------------------------------------
    // Position finders
    // ------------------------------------------------------------------

    class DiscretePositionsTest : public ::testing::Test { };

    TEST_F(DiscretePositionsTest, AnAlternatingSignalCrossesZeroOnEveryStep)
    {
        const auto positions = D::zero_crossing_positions(alternating(16));

        ASSERT_EQ(positions.size(), 15U);
        EXPECT_EQ(std::ranges::adjacent_find(positions, std::greater_equal<size_t> {}), positions.end());
        EXPECT_LT(positions.back(), 16U);
    }

    TEST_F(DiscretePositionsTest, AConstantSignalNeverCrossesZero)
    {
        EXPECT_TRUE(D::zero_crossing_positions(std::vector<double>(64, 0.3)).empty());
    }

    TEST_F(DiscretePositionsTest, ImpulsesAboveTheThresholdAreEachFoundOnce)
    {
        const auto positions = D::peak_positions(impulses(0.1, 0.8), 0.5, 1);

        std::vector<size_t> expected;
        for (size_t i = 25; i < k_samples; i += 50) {
            expected.push_back(i);
        }
        EXPECT_EQ(positions, expected);
    }

    TEST_F(DiscretePositionsTest, AcceptedPeaksAreAtLeastTheMinimumDistanceApart)
    {
        const auto positions = D::peak_positions(impulses(0.1, 0.8), 0.5, 100);

        ASSERT_FALSE(positions.empty());
        EXPECT_EQ(positions.front(), 25U);
        for (size_t i = 1; i < positions.size(); ++i) {
            EXPECT_GE(positions[i] - positions[i - 1], 100U);
        }
        EXPECT_LT(positions.size(), 20U);
    }

    TEST_F(DiscretePositionsTest, NothingAboveTheThresholdIsNotAPeak)
    {
        EXPECT_TRUE(D::peak_positions(impulses(0.1, 0.8), 0.9, 1).empty());
    }

} // namespace
} // namespace MayaFlux::Test
