#include "MayaFlux/Kakshya/SignalSourceContainer.hpp"
#include "MayaFlux/Kinesis/Discrete/Analysis.hpp"
#include "MayaFlux/Kinesis/Discrete/Extract.hpp"
#include "MayaFlux/Yantra/Extractors/FeatureExtractor.hpp"
#include "MayaFlux/Yantra/Transformers/MathematicalTransformer.hpp"
#include "../support/audio_container.hpp"

#include <gtest/gtest.h>
#include "../support/test_metadata.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

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

        EXPECT_EQ(ExtractionMethod(extractor.config<FeatureExtractorConfig>().method), GetParam());
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
        EXPECT_EQ(extractor.config<FeatureExtractorConfig>().scope, ExtractionScope::FULL_DATA);
    }

    enum class FeatureInputPath { Inferred, Explicit, Container };

    class FeatureExtractorPipelineOutputTest : public ::testing::TestWithParam<std::tuple<FeatureInputPath, size_t>> {
    protected:
        using Samples = std::vector<double>;
        using Channels = std::vector<Samples>;
        using Variants = std::vector<Kakshya::DataVariant>;
        using Dimension = Kakshya::DataDimension;

        FeatureExtractorConfig extraction_config()
        {
            auto config = config_for(ExtractionMethod::HIGH_ENERGY_DATA, 1, 1);
            config.energy_threshold = 1.0;
            return config;
        }

        Channels matching_channels(size_t count)
        {
            Channels source;
            for (size_t channel = 0; channel < count; ++channel) {
                const double scale = static_cast<double>(channel + 1);
                source.push_back({ 0, 2 * scale, 3 * scale, 0 });
            }
            return source;
        }

        template <typename Output>
        Datum<Output> run(const Channels& channels,
            Yantra::FeatureExtractor<Variants, Output>& variants,
            Yantra::FeatureExtractor<std::shared_ptr<SignalSourceContainer>, Output>& containers)
        {
            if (std::get<0>(GetParam()) == FeatureInputPath::Container) {
                std::shared_ptr<SignalSourceContainer> source = make_audio_container(channels);
                return containers.apply_operation(Datum<std::shared_ptr<SignalSourceContainer>>(source));
            }
            Variants data;
            for (const auto& channel : channels)
                data.emplace_back(channel);
            if (std::get<0>(GetParam()) == FeatureInputPath::Explicit)
                return variants.apply_operation(Datum<Variants>(std::move(data),
                    { Dimension::time(channels[0].size()), Dimension::channel(channels.size()) },
                    Kakshya::DataModality::AUDIO_MULTICHANNEL));
            return variants.apply_operation(Datum<Variants>(std::move(data)));
        }
    };

    TEST_P(FeatureExtractorPipelineOutputTest, RectangularExtractionProducesMatchingMatrixStructureAndFacet)
    {
        const auto count = std::get<1>(GetParam());
        Yantra::MatrixFeatureExtractor variants(extraction_config());
        Yantra::FeatureExtractor<std::shared_ptr<SignalSourceContainer>, Eigen::MatrixXd> containers(extraction_config());
        const auto source = matching_channels(count);
        const auto output = run(source, variants, containers);
        ASSERT_EQ(output.data.rows(), 2);
        ASSERT_EQ(output.data.cols(), count);
        EXPECT_EQ(output.get_total_elements(), 2 * count);
        const int time = output.find_dimension_by_role(Dimension::Role::TIME);
        ASSERT_GE(time, 0);
        EXPECT_EQ(output.dimensions[time].size, 2);
        if (count > 1) {
            const int channels = output.find_dimension_by_role(Dimension::Role::CHANNEL);
            ASSERT_GE(channels, 0);
            EXPECT_EQ(output.dimensions[channels].size, count);
        }
        const auto* record = output.facets.get<FeatureExtraction>();
        ASSERT_NE(record, nullptr);
        EXPECT_EQ(record->extracted_samples, 2 * count);
        EXPECT_EQ(static_cast<ExtractionMethod>(record->config.method), ExtractionMethod::HIGH_ENERGY_DATA);
        for (size_t channel = 0; channel < count; ++channel) {
            EXPECT_DOUBLE_EQ(output.data(0, channel), 2.0 * (channel + 1));
            EXPECT_DOUBLE_EQ(output.data(1, channel), 3.0 * (channel + 1));
        }
        EXPECT_EQ(source, matching_channels(count));
        Yantra::MathematicalTransformer<Eigen::MatrixXd> gain(
            Yantra::MathematicalTransformerConfig { .gain_factor = 2.0 });
        const auto transformed = gain.apply_operation(output);
        ASSERT_EQ(transformed.data.rows(), 2);
        ASSERT_EQ(transformed.data.cols(), count);
        EXPECT_EQ(transformed.get_total_elements(), 2 * count);
        for (size_t channel = 0; channel < count; ++channel) {
            EXPECT_DOUBLE_EQ(transformed.data(0, channel), 4.0 * (channel + 1));
            EXPECT_DOUBLE_EQ(transformed.data(1, channel), 6.0 * (channel + 1));
        }
    }

    TEST_P(FeatureExtractorPipelineOutputTest, RaggedExtractionRetainsValuesAndCanFeedAVariantTransformer)
    {
        const auto count = std::get<1>(GetParam());
        auto source = matching_channels(count);
        source.push_back({ 0, 4, 0, 0 });
        Yantra::VariantFeatureExtractor variants(extraction_config());
        Yantra::FeatureExtractor<std::shared_ptr<SignalSourceContainer>, Variants> containers(extraction_config());
        const auto output = run(source, variants, containers);
        ASSERT_EQ(output.data.size(), count + 1);
        const auto* record = output.facets.get<FeatureExtraction>();
        ASSERT_NE(record, nullptr);
        EXPECT_EQ(record->extracted_samples, 2 * count + 1);
        for (size_t channel = 0; channel <= count; ++channel) {
            ASSERT_TRUE(std::holds_alternative<Samples>(output.data[channel]));
            const Samples expected = channel == count ? Samples { 4 }
                : Samples { 2.0 * (channel + 1), 3.0 * (channel + 1) };
            EXPECT_EQ(std::get<Samples>(output.data[channel]), expected);
        }
        Yantra::MathematicalTransformer<> gain(Yantra::MathematicalTransformerConfig { .gain_factor = 2.0 });
        const auto transformed = gain.apply_operation(output);
        ASSERT_EQ(transformed.data.size(), count + 1);
        ASSERT_EQ(transformed.dimensions.size(), 1);
        EXPECT_EQ(transformed.dimensions[0].role, Dimension::Role::CHANNEL);
        EXPECT_EQ(transformed.dimensions[0].size, count + 1);
        EXPECT_EQ(transformed.modality, Kakshya::DataModality::TENSOR_ND);
        for (size_t channel = 0; channel <= count; ++channel) {
            ASSERT_TRUE(std::holds_alternative<Samples>(transformed.data[channel]));
            const Samples expected = channel == count ? Samples { 8 }
                : Samples { 4.0 * (channel + 1), 6.0 * (channel + 1) };
            EXPECT_EQ(std::get<Samples>(transformed.data[channel]), expected);
        }
    }

    TEST_P(FeatureExtractorPipelineOutputTest, RaggedMatrixRequestReturnsNoSuccessfulExtraction)
    {
        auto source = matching_channels(std::get<1>(GetParam()));
        source.push_back({ 0, 4, 0, 0 });
        Yantra::MatrixFeatureExtractor variants(extraction_config());
        Yantra::FeatureExtractor<std::shared_ptr<SignalSourceContainer>, Eigen::MatrixXd> containers(extraction_config());
        const auto output = run(source, variants, containers);
        EXPECT_EQ(output.data.size(), 0);
        EXPECT_EQ(output.facets.get<FeatureExtraction>(), nullptr);
    }

    TEST_P(FeatureExtractorPipelineOutputTest, ReuseReplacesThePreviousChannelsAndExtractionFacet)
    {
        const auto count = std::get<1>(GetParam());
        Yantra::VariantFeatureExtractor variants(extraction_config());
        Yantra::FeatureExtractor<std::shared_ptr<SignalSourceContainer>, Variants> containers(extraction_config());
        const auto initial = run(matching_channels(count), variants, containers);
        ASSERT_EQ(initial.data.size(), count);
        const auto replacement = run(Channels { { 0, 7, 0, 0 } }, variants, containers);
        ASSERT_EQ(replacement.data.size(), 1);
        ASSERT_TRUE(std::holds_alternative<Samples>(replacement.data[0]));
        EXPECT_EQ(std::get<Samples>(replacement.data[0]), (Samples { 7 }));
        const auto* record = replacement.facets.get<FeatureExtraction>();
        ASSERT_NE(record, nullptr);
        EXPECT_EQ(record->extracted_samples, 1);
    }

    INSTANTIATE_TEST_SUITE_P(Inputs, FeatureExtractorPipelineOutputTest,
        ::testing::Combine(::testing::Values(FeatureInputPath::Inferred, FeatureInputPath::Explicit, FeatureInputPath::Container),
            ::testing::Values(size_t { 1 }, size_t { 2 }, size_t { 3 })),
        [](const auto& info) {
            const auto path = std::get<0>(info.param);
            return std::string(path == FeatureInputPath::Inferred ? "Inferred" : path == FeatureInputPath::Explicit ? "Explicit" : "Container")
                + std::to_string(std::get<1>(info.param)) + "Channels";
        });

} // namespace
} // namespace MayaFlux::Test
