#include "MayaFlux/Kakshya/SignalSourceContainer.hpp"
#include "MayaFlux/Kinesis/Discrete/Analysis.hpp"
#include "MayaFlux/Kinesis/Discrete/Extract.hpp"
#include "MayaFlux/Yantra/Extractors/FeatureExtractor.hpp"
#include "audio_container.hpp"

#include <gtest/gtest.h>

namespace MayaFlux::Test {
namespace {

    using Kakshya::SignalSourceContainer;
    using Yantra::ContainerFeatureExtractor;
    using Yantra::Datum;
    using Yantra::ExtractionMethod;
    using Yantra::ExtractionScope;
    using Yantra::FeatureExtraction;
    using Yantra::FeatureExtractorConfig;

    namespace D = Kinesis::Discrete;

    constexpr uint32_t k_window = 256;
    constexpr uint32_t k_hop = 128;
    constexpr size_t k_samples = 1024;

    FeatureExtractorConfig config_for(ExtractionMethod method, uint32_t window = k_window, uint32_t hop = k_hop)
    {
        FeatureExtractorConfig config;
        config.method = method;
        config.window_size = window;
        config.hop_size = hop;
        return config;
    }

    std::vector<double> tone(size_t count, double period)
    {
        std::vector<double> data(count);
        for (size_t i = 0; i < count; ++i) {
            data[i] = std::sin(2.0 * std::numbers::pi * static_cast<double>(i) / period);
        }
        return data;
    }

    std::vector<double> burst()
    {
        std::vector<double> data(k_samples, 0.0);
        for (size_t i = 512; i < 768; ++i) {
            data[i] = 0.5 * std::sin(2.0 * std::numbers::pi * static_cast<double>(i) / 64.0);
        }
        return data;
    }

    std::vector<std::vector<double>> sources()
    {
        return { tone(k_samples, 64.0), burst() };
    }

    std::shared_ptr<SignalSourceContainer> two_channels()
    {
        return make_audio_container(sources());
    }

    std::shared_ptr<SignalSourceContainer> one_channel(const std::vector<double>& samples)
    {
        return make_audio_container(samples);
    }

    const char* method_name(ExtractionMethod method)
    {
        switch (method) {
        case ExtractionMethod::HIGH_ENERGY_DATA:
            return "HighEnergy";
        case ExtractionMethod::PEAK_DATA:
            return "Peak";
        case ExtractionMethod::OUTLIER_DATA:
            return "Outlier";
        case ExtractionMethod::HIGH_SPECTRAL_DATA:
            return "HighSpectral";
        case ExtractionMethod::ABOVE_MEAN_DATA:
            return "AboveMean";
        case ExtractionMethod::OVERLAPPING_WINDOWS:
            return "OverlappingWindows";
        case ExtractionMethod::ZERO_CROSSING_DATA:
            return "ZeroCrossing";
        case ExtractionMethod::SILENCE_DATA:
            return "Silence";
        case ExtractionMethod::ONSET_DATA:
            return "Onset";
        }
        return "Unknown";
    }

    // ------------------------------------------------------------------
    // Every method: machinery shared by all extraction methods
    // ------------------------------------------------------------------

    class FeatureExtractorMethodTest : public ::testing::TestWithParam<ExtractionMethod> { };

    INSTANTIATE_TEST_SUITE_P(Methods, FeatureExtractorMethodTest,
        ::testing::Values(
            ExtractionMethod::HIGH_ENERGY_DATA,
            ExtractionMethod::PEAK_DATA,
            ExtractionMethod::OUTLIER_DATA,
            ExtractionMethod::HIGH_SPECTRAL_DATA,
            ExtractionMethod::ABOVE_MEAN_DATA,
            ExtractionMethod::OVERLAPPING_WINDOWS,
            ExtractionMethod::ZERO_CROSSING_DATA,
            ExtractionMethod::SILENCE_DATA,
            ExtractionMethod::ONSET_DATA),
        [](const ::testing::TestParamInfo<ExtractionMethod>& info) {
            return std::string(method_name(info.param));
        });

    TEST_P(FeatureExtractorMethodTest, OutputHasOneChannelPerInputChannel)
    {
        ContainerFeatureExtractor extractor(config_for(GetParam()));
        const auto channels = extractor.extract_data(two_channels());

        EXPECT_EQ(channels.size(), 2U);
    }

    TEST_P(FeatureExtractorMethodTest, OutputFacetRecordsTheConfigAndSampleCount)
    {
        ContainerFeatureExtractor extractor(config_for(GetParam()));
        const auto output = extractor.apply_operation(Datum<std::shared_ptr<SignalSourceContainer>>(two_channels()));

        const auto* record = output.facets.get<FeatureExtraction>();
        ASSERT_NE(record, nullptr);
        EXPECT_EQ(ExtractionMethod(record->config.method), GetParam());
        EXPECT_EQ(record->config.window_size, k_window);
        EXPECT_EQ(record->config.hop_size, k_hop);

        size_t total = 0;
        for (const auto& channel : output.data) {
            total += channel.size();
        }
        EXPECT_EQ(record->extracted_samples, total);
    }

    TEST_P(FeatureExtractorMethodTest, ExtractionIsRepeatable)
    {
        ContainerFeatureExtractor extractor(config_for(GetParam()));
        const auto source = two_channels();

        EXPECT_EQ(extractor.extract_data(source), extractor.extract_data(source));
    }

    TEST_P(FeatureExtractorMethodTest, ExtractionLeavesTheStoredMethodUnchanged)
    {
        ContainerFeatureExtractor extractor(config_for(GetParam()));
        extractor.extract_data(two_channels());

        EXPECT_EQ(ExtractionMethod(extractor.config().method), GetParam());
    }

    TEST_P(FeatureExtractorMethodTest, SetConfigSelectsTheMethodUsed)
    {
        ContainerFeatureExtractor extractor;
        extractor.set_config(config_for(GetParam()));

        const auto output = extractor.apply_operation(Datum<std::shared_ptr<SignalSourceContainer>>(two_channels()));
        const auto* record = output.facets.get<FeatureExtraction>();
        ASSERT_NE(record, nullptr);
        EXPECT_EQ(ExtractionMethod(record->config.method), GetParam());
    }

    // ------------------------------------------------------------------
    // Overlapping windows agree with Kinesis, channel by channel
    // ------------------------------------------------------------------

    struct OverlapCase {
        double overlap;
    };

    class FeatureExtractorOverlapTest : public ::testing::TestWithParam<OverlapCase> { };

    INSTANTIATE_TEST_SUITE_P(Overlaps, FeatureExtractorOverlapTest,
        ::testing::Values(
            OverlapCase { 0.0 },
            OverlapCase { 0.5 },
            OverlapCase { 0.75 }),
        [](const ::testing::TestParamInfo<OverlapCase>& info) {
            return "Overlap" + std::to_string(static_cast<int>(info.param.overlap * 100.0));
        });

    TEST_P(FeatureExtractorOverlapTest, EachChannelIsTheKinesisWindowing)
    {
        auto config = config_for(ExtractionMethod::OVERLAPPING_WINDOWS);
        config.overlap = GetParam().overlap;
        ContainerFeatureExtractor extractor(config);

        const auto channels = extractor.extract_data(two_channels());
        const auto signals = sources();

        ASSERT_EQ(channels.size(), signals.size());
        for (size_t c = 0; c < signals.size(); ++c) {
            EXPECT_EQ(channels[c], D::overlapping_windows(signals[c], k_window, GetParam().overlap))
                << "channel " << c;
        }
    }

    // ------------------------------------------------------------------
    // High energy: the output is the span covered by qualifying windows
    // ------------------------------------------------------------------

    struct EnergyCase {
        double threshold;
        size_t expected_samples;
    };

    class FeatureExtractorHighEnergyTest : public ::testing::TestWithParam<EnergyCase> { };

    INSTANTIATE_TEST_SUITE_P(Thresholds, FeatureExtractorHighEnergyTest,
        ::testing::Values(
            EnergyCase { 0.1, 512 },
            EnergyCase { 0.3, 256 },
            EnergyCase { 0.5, 0 }),
        [](const ::testing::TestParamInfo<EnergyCase>& info) {
            return "Threshold" + std::to_string(static_cast<int>(info.param.threshold * 10.0));
        });

    TEST_P(FeatureExtractorHighEnergyTest, OnlyWindowsAboveTheThresholdAreKept)
    {
        auto config = config_for(ExtractionMethod::HIGH_ENERGY_DATA);
        config.energy_threshold = GetParam().threshold;
        ContainerFeatureExtractor extractor(config);

        const auto channels = extractor.extract_data(one_channel(burst()));

        ASSERT_EQ(channels.size(), 1U);
        EXPECT_EQ(channels[0].size(), GetParam().expected_samples);
    }

    // ------------------------------------------------------------------
    // Zero crossings agree with the Kinesis crossings and regions
    // ------------------------------------------------------------------

    class FeatureExtractorZeroCrossingTest : public ::testing::TestWithParam<uint32_t> { };

    INSTANTIATE_TEST_SUITE_P(RegionSizes, FeatureExtractorZeroCrossingTest,
        ::testing::Values(16U, 32U),
        [](const ::testing::TestParamInfo<uint32_t>& info) {
            return "Region" + std::to_string(info.param);
        });

    TEST_P(FeatureExtractorZeroCrossingTest, RegionsAroundEachKinesisCrossing)
    {
        const auto signal = tone(k_samples, 64.0);
        const uint32_t region = GetParam();

        auto config = config_for(ExtractionMethod::ZERO_CROSSING_DATA);
        config.zero_crossing_region_size = region;
        ContainerFeatureExtractor extractor(config);

        const auto channels = extractor.extract_data(one_channel(signal));

        const auto crossings = D::zero_crossing_positions(signal, 0.0);
        const auto expected = D::slice_intervals(signal,
            D::merge_intervals(D::regions_around_positions(crossings, region / 2, signal.size())));

        ASSERT_EQ(channels.size(), 1U);
        EXPECT_EQ(channels[0], expected);
    }

    // ------------------------------------------------------------------
    // Invalid settings are rejected at construction
    // ------------------------------------------------------------------

    struct InvalidCase {
        const char* name;
        uint32_t window;
        uint32_t hop;
    };

    class FeatureExtractorInvalidConfigTest : public ::testing::TestWithParam<InvalidCase> { };

    INSTANTIATE_TEST_SUITE_P(Configs, FeatureExtractorInvalidConfigTest,
        ::testing::Values(
            InvalidCase { "ZeroWindow", 0, 128 },
            InvalidCase { "ZeroHop", 256, 0 },
            InvalidCase { "HopLargerThanWindow", 128, 256 }),
        [](const ::testing::TestParamInfo<InvalidCase>& info) {
            return std::string(info.param.name);
        });

    TEST_P(FeatureExtractorInvalidConfigTest, ConstructionRejectsTheConfig)
    {
        const auto& c = GetParam();
        EXPECT_THROW(ContainerFeatureExtractor { config_for(ExtractionMethod::HIGH_ENERGY_DATA, c.window, c.hop) }, std::invalid_argument);
    }

    // ------------------------------------------------------------------
    // Scope is a setting: asking for a scope never rewrites the stored one
    // ------------------------------------------------------------------

    class FeatureExtractorScopeTest : public ::testing::TestWithParam<ExtractionScope> { };

    INSTANTIATE_TEST_SUITE_P(Scopes, FeatureExtractorScopeTest,
        ::testing::Values(
            ExtractionScope::FULL_DATA,
            ExtractionScope::TARGETED_REGIONS,
            ExtractionScope::FILTERED_CONTENT,
            ExtractionScope::SAMPLED_DATA),
        [](const ::testing::TestParamInfo<ExtractionScope>& info) {
            return "Scope" + std::to_string(static_cast<int>(info.param));
        });

    TEST_P(FeatureExtractorScopeTest, ExtractWithScopeKeepsTheStoredScope)
    {
        ContainerFeatureExtractor extractor(config_for(ExtractionMethod::OVERLAPPING_WINDOWS));
        const auto channels = extractor.extract_with_scope(two_channels(), GetParam());

        EXPECT_EQ(channels.size(), 2U);
        EXPECT_EQ(extractor.config().scope, ExtractionScope::FULL_DATA);
    }

} // namespace
} // namespace MayaFlux::Test
