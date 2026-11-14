#include "gtest/gtest.h"
#include "../support/test_metadata.hpp"

#include "MayaFlux/Kinesis/Discrete/Analysis.hpp"
#include "MayaFlux/Kinesis/Discrete/Extract.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {
namespace {

    namespace D = Kinesis::Discrete;

    using Intervals = std::vector<std::pair<size_t, size_t>>;

    std::vector<double> one_to_ten()
    {
        return { 1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 9.0, 10.0 };
    }

    std::vector<double> silence(size_t count)
    {
        return std::vector<double>(count, 0.0);
    }

    std::vector<double> step_at(size_t count, size_t onset)
    {
        std::vector<double> data(count, 0.0);
        for (size_t i = onset; i < count; ++i) {
            data[i] = 1.0;
        }
        return data;
    }

    std::vector<double> sine_wave(size_t count, double period_samples)
    {
        std::vector<double> data(count);
        for (size_t i = 0; i < count; ++i) {
            data[i] = std::sin(2.0 * std::numbers::pi * static_cast<double>(i) / period_samples);
        }
        return data;
    }

    // ------------------------------------------------------------------
    // Window parameter validation
    // ------------------------------------------------------------------

    struct ValidateCase {
        const char* name;
        uint32_t window;
        uint32_t hop;
        size_t data_size;
        bool expected;
    };

    class KinesisExtractValidateWindowTest : public ::testing::TestWithParam<ValidateCase> { };

    INSTANTIATE_TEST_SUITE_P(Parameters, KinesisExtractValidateWindowTest,
        ::testing::Values(
            ValidateCase { "ZeroWindow", 0, 128, 1024, false },
            ValidateCase { "ZeroHop", 256, 0, 1024, false },
            ValidateCase { "ZeroWindowAndHop", 0, 0, 1024, false },
            ValidateCase { "UnknownLengthAccepted", 256, 128, 0, true },
            ValidateCase { "ValidGeometry", 256, 128, 1024, true }),
        [](const ::testing::TestParamInfo<ValidateCase>& info) {
            return std::string(info.param.name);
        });

    TEST_P(KinesisExtractValidateWindowTest, ReportsWhetherTheGeometryIsUsable)
    {
        const auto& c = GetParam();
        EXPECT_EQ(D::validate_window_parameters(c.window, c.hop, c.data_size), c.expected);
    }

    // ------------------------------------------------------------------
    // Interval merging
    // ------------------------------------------------------------------

    struct MergeCase {
        const char* name;
        Intervals input;
        Intervals expected;
    };

    class KinesisExtractMergeTest : public ::testing::TestWithParam<MergeCase> { };

    INSTANTIATE_TEST_SUITE_P(Intervals, KinesisExtractMergeTest,
        ::testing::Values(
            MergeCase { "Empty", {}, {} },
            MergeCase { "Single", { { 4, 6 } }, { { 4, 6 } } },
            MergeCase { "Unsorted", { { 5, 8 }, { 1, 3 }, { 2, 4 }, { 8, 9 }, { 10, 12 } }, { { 1, 4 }, { 5, 9 }, { 10, 12 } } },
            MergeCase { "Adjacent", { { 0, 3 }, { 3, 6 } }, { { 0, 6 } } },
            MergeCase { "Contained", { { 0, 10 }, { 2, 3 } }, { { 0, 10 } } },
            MergeCase { "Disjoint", { { 0, 2 }, { 5, 7 } }, { { 0, 2 }, { 5, 7 } } }),
        [](const ::testing::TestParamInfo<MergeCase>& info) {
            return std::string(info.param.name);
        });

    TEST_P(KinesisExtractMergeTest, ProducesSortedNonOverlappingIntervals)
    {
        const auto& c = GetParam();
        EXPECT_EQ(D::merge_intervals(c.input), c.expected);
    }

    // ------------------------------------------------------------------
    // Interval slicing
    // ------------------------------------------------------------------

    struct SliceCase {
        const char* name;
        Intervals intervals;
        std::vector<double> expected;
    };

    class KinesisExtractSliceTest : public ::testing::TestWithParam<SliceCase> { };

    INSTANTIATE_TEST_SUITE_P(Intervals, KinesisExtractSliceTest,
        ::testing::Values(
            SliceCase { "NoIntervals", {}, {} },
            SliceCase { "TwoRanges", { { 0, 2 }, { 4, 6 } }, { 1.0, 2.0, 5.0, 6.0 } },
            SliceCase { "EndOfData", { { 9, 10 } }, { 10.0 } },
            SliceCase { "EmptyRangeSkipped", { { 3, 3 } }, {} },
            SliceCase { "OutOfRangeSkipped", { { 8, 12 } }, {} },
            SliceCase { "ValidBesideInvalid", { { 8, 12 }, { 1, 3 } }, { 2.0, 3.0 } }),
        [](const ::testing::TestParamInfo<SliceCase>& info) {
            return std::string(info.param.name);
        });

    TEST_P(KinesisExtractSliceTest, CopiesOnlyTheRequestedSamples)
    {
        const auto& c = GetParam();
        const auto data = one_to_ten();
        EXPECT_EQ(D::slice_intervals(data, c.intervals), c.expected);
    }

    // ------------------------------------------------------------------
    // Intervals around positions
    // ------------------------------------------------------------------

    struct RegionCase {
        const char* name;
        std::vector<size_t> positions;
        size_t half_region;
        size_t data_size;
        Intervals expected;
    };

    class KinesisExtractRegionsTest : public ::testing::TestWithParam<RegionCase> { };

    INSTANTIATE_TEST_SUITE_P(Positions, KinesisExtractRegionsTest,
        ::testing::Values(
            RegionCase { "NoPositions", {}, 3, 10, {} },
            RegionCase { "CentredAndClamped", { 2, 9 }, 3, 10, { { 0, 5 }, { 6, 10 } } },
            RegionCase { "ClampedAtStart", { 0 }, 3, 10, { { 0, 3 } } },
            RegionCase { "ZeroRadius", { 4 }, 0, 10, {} },
            RegionCase { "PositionBeyondData", { 20 }, 3, 10, {} }),
        [](const ::testing::TestParamInfo<RegionCase>& info) {
            return std::string(info.param.name);
        });

    TEST_P(KinesisExtractRegionsTest, BuildsOneClampedIntervalPerPosition)
    {
        const auto& c = GetParam();
        EXPECT_EQ(D::regions_around_positions(c.positions, c.half_region, c.data_size), c.expected);
    }

    // ------------------------------------------------------------------
    // Intervals from window starts
    // ------------------------------------------------------------------

    struct WindowStartCase {
        const char* name;
        std::vector<size_t> starts;
        uint32_t window;
        size_t data_size;
        Intervals expected;
    };

    class KinesisExtractWindowStartIntervalsTest : public ::testing::TestWithParam<WindowStartCase> { };

    INSTANTIATE_TEST_SUITE_P(WindowStarts, KinesisExtractWindowStartIntervalsTest,
        ::testing::Values(
            WindowStartCase { "NoStarts", {}, 4, 10, {} },
            WindowStartCase { "FullAndClampedWindows", { 0, 3, 8 }, 4, 10, { { 0, 4 }, { 3, 7 }, { 8, 10 } } },
            WindowStartCase { "StartBeyondData", { 12 }, 4, 10, {} },
            WindowStartCase { "ZeroWindow", { 2 }, 0, 10, {} }),
        [](const ::testing::TestParamInfo<WindowStartCase>& info) {
            return std::string(info.param.name);
        });

    TEST_P(KinesisExtractWindowStartIntervalsTest, ClampsEachWindowToTheData)
    {
        const auto& c = GetParam();
        EXPECT_EQ(D::intervals_from_window_starts(c.starts, c.window, c.data_size), c.expected);
    }

    // ------------------------------------------------------------------
    // Overlapping windows
    // ------------------------------------------------------------------

    struct OverlapSizeCase {
        const char* name;
        uint32_t window;
        double overlap;
        size_t data_size;
        size_t expected_windows;
    };

    class KinesisExtractOverlapSizeTest : public ::testing::TestWithParam<OverlapSizeCase> { };

    INSTANTIATE_TEST_SUITE_P(Geometries, KinesisExtractOverlapSizeTest,
        ::testing::Values(
            OverlapSizeCase { "NoOverlap", 4, 0.0, 10, 2 },
            OverlapSizeCase { "HalfOverlap", 4, 0.5, 10, 4 },
            OverlapSizeCase { "HopBelowOneSampleClampedToOne", 4, 0.9, 10, 7 },
            OverlapSizeCase { "ExactlyOneWindow", 4, 0.5, 4, 1 },
            OverlapSizeCase { "DataShorterThanWindow", 4, 0.5, 3, 0 },
            OverlapSizeCase { "OverlapAtOneRejected", 4, 1.0, 10, 0 },
            OverlapSizeCase { "NegativeOverlapRejected", 4, -0.1, 10, 0 },
            OverlapSizeCase { "ZeroWindowRejected", 0, 0.5, 10, 0 }),
        [](const ::testing::TestParamInfo<OverlapSizeCase>& info) {
            return std::string(info.param.name);
        });

    TEST_P(KinesisExtractOverlapSizeTest, CountsEveryCompleteWindow)
    {
        const auto& c = GetParam();
        std::vector<double> data(c.data_size);
        std::iota(data.begin(), data.end(), 1.0);
        const auto out = D::overlapping_windows(data, c.window, c.overlap);
        EXPECT_EQ(out.size(), c.expected_windows * c.window);
    }

    struct OverlapValueCase {
        const char* name;
        uint32_t window;
        double overlap;
        std::vector<double> expected;
    };

    class KinesisExtractOverlapValueTest : public ::testing::TestWithParam<OverlapValueCase> { };

    INSTANTIATE_TEST_SUITE_P(Contents, KinesisExtractOverlapValueTest,
        ::testing::Values(
            OverlapValueCase { "NoOverlap", 4, 0.0, { 1, 2, 3, 4, 5, 6, 7, 8 } },
            OverlapValueCase { "HalfOverlap", 4, 0.5,
                { 1, 2, 3, 4, 3, 4, 5, 6, 5, 6, 7, 8, 7, 8, 9, 10 } }),
        [](const ::testing::TestParamInfo<OverlapValueCase>& info) {
            return std::string(info.param.name);
        });

    TEST_P(KinesisExtractOverlapValueTest, WindowsAdvanceByTheHop)
    {
        const auto& c = GetParam();
        EXPECT_EQ(D::overlapping_windows(one_to_ten(), c.window, c.overlap), c.expected);
    }

    // ------------------------------------------------------------------
    // Windows at explicit indices
    // ------------------------------------------------------------------

    struct IndexedCase {
        const char* name;
        std::vector<size_t> starts;
        uint32_t window;
        std::vector<double> expected;
    };

    class KinesisExtractIndexedWindowsTest : public ::testing::TestWithParam<IndexedCase> { };

    INSTANTIATE_TEST_SUITE_P(Starts, KinesisExtractIndexedWindowsTest,
        ::testing::Values(
            IndexedCase { "NoStarts", {}, 4, {} },
            IndexedCase { "InteriorAndOverflowSkipped", { 0, 3, 8 }, 4, { 1, 2, 3, 4, 4, 5, 6, 7 } },
            IndexedCase { "LastFullWindow", { 6 }, 4, { 7, 8, 9, 10 } },
            IndexedCase { "OverflowOnly", { 7 }, 4, {} },
            IndexedCase { "RepeatedStartRepeats", { 0, 0 }, 4, { 1, 2, 3, 4, 1, 2, 3, 4 } },
            IndexedCase { "ZeroWindow", { 0 }, 0, {} }),
        [](const ::testing::TestParamInfo<IndexedCase>& info) {
            return std::string(info.param.name);
        });

    TEST_P(KinesisExtractIndexedWindowsTest, ReturnsOnlyWindowsThatFit)
    {
        const auto& c = GetParam();
        EXPECT_EQ(D::windowed_by_indices(one_to_ten(), c.starts, c.window), c.expected);
    }

    // ------------------------------------------------------------------
    // Onset positions
    // ------------------------------------------------------------------

    struct OnsetEmptyCase {
        const char* name;
        std::vector<double> signal;
        uint32_t window;
        uint32_t hop;
        double threshold;
    };

    class KinesisExtractOnsetEmptyTest : public ::testing::TestWithParam<OnsetEmptyCase> { };

    INSTANTIATE_TEST_SUITE_P(NoOnset, KinesisExtractOnsetEmptyTest,
        ::testing::Values(
            OnsetEmptyCase { "Silence", silence(1024), 256, 128, 0.1 },
            OnsetEmptyCase { "TooShortForTwoWindows", step_at(200, 100), 256, 128, 0.1 },
            OnsetEmptyCase { "ThresholdAboveNormalisedMaximum", step_at(1024, 512), 256, 128, 1.0 }),
        [](const ::testing::TestParamInfo<OnsetEmptyCase>& info) {
            return std::string(info.param.name);
        });

    TEST_P(KinesisExtractOnsetEmptyTest, ReportsNoOnsets)
    {
        const auto& c = GetParam();
        EXPECT_TRUE(D::onset_positions(c.signal, c.window, c.hop, c.threshold).empty());
    }

    struct OnsetStepCase {
        const char* name;
        double threshold;
    };

    class KinesisExtractOnsetStepTest : public ::testing::TestWithParam<OnsetStepCase> { };

    INSTANTIATE_TEST_SUITE_P(Thresholds, KinesisExtractOnsetStepTest,
        ::testing::Values(
            OnsetStepCase { "LowThreshold", 0.1 },
            OnsetStepCase { "HighThreshold", 0.5 }),
        [](const ::testing::TestParamInfo<OnsetStepCase>& info) {
            return std::string(info.param.name);
        });

    TEST_P(KinesisExtractOnsetStepTest, FindsAnOnsetNearTheStep)
    {
        const auto& c = GetParam();
        constexpr uint32_t window = 256;
        constexpr uint32_t hop = 128;
        constexpr size_t onset = 512;
        const auto signal = step_at(1024, onset);

        const auto positions = D::onset_positions(signal, window, hop, c.threshold);

        ASSERT_FALSE(positions.empty());
        EXPECT_TRUE(std::is_sorted(positions.begin(), positions.end()));
        for (size_t p : positions) {
            EXPECT_EQ(p % hop, 0U);
            EXPECT_LT(p, signal.size());
        }

        const bool near_step = std::ranges::any_of(positions, [&](size_t p) {
            const size_t distance = p > onset ? p - onset : onset - p;
            return distance <= window;
        });
        EXPECT_TRUE(near_step);
    }

    // ------------------------------------------------------------------
    // Frequency estimation from zero crossings
    // ------------------------------------------------------------------

    struct FrequencyCase {
        const char* name;
        std::vector<double> signal;
        double sample_rate;
        double expected_hz;
        double tolerance;
    };

    class KinesisExtractFrequencyTest : public ::testing::TestWithParam<FrequencyCase> { };

    constexpr double k_fallback = -1.0;

    INSTANTIATE_TEST_SUITE_P(Signals, KinesisExtractFrequencyTest,
        ::testing::Values(
            FrequencyCase { "HundredHertz", sine_wave(800, 80.0), 8000.0, 100.0, 1.0 },
            FrequencyCase { "TwoHundredHertz", sine_wave(800, 40.0), 8000.0, 200.0, 2.0 },
            FrequencyCase { "ConstantHasNoCrossings", std::vector<double>(800, 1.0), 8000.0, k_fallback, 0.0 },
            FrequencyCase { "SingleCrossingFallsBack", step_at(100, 50), 8000.0, k_fallback, 0.0 },
            FrequencyCase { "SingleSampleFallsBack", std::vector<double> { 1.0 }, 8000.0, k_fallback, 0.0 }),
        [](const ::testing::TestParamInfo<FrequencyCase>& info) {
            return std::string(info.param.name);
        });

    TEST_P(KinesisExtractFrequencyTest, EstimatesOrFallsBack)
    {
        const auto& c = GetParam();
        const double estimate = D::estimate_frequency(c.signal, c.sample_rate, k_fallback, 0.0);
        if (c.expected_hz == k_fallback) {
            EXPECT_EQ(estimate, k_fallback);
        } else {
            EXPECT_NEAR(estimate, c.expected_hz, c.tolerance);
        }
    }

} // namespace
} // namespace MayaFlux::Test
