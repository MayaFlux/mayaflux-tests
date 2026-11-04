#include "MayaFlux/Kakshya/SignalSourceContainer.hpp"
#include "MayaFlux/Yantra/Analyzers/EnergyAnalyzer.hpp"
#include "audio_container.hpp"

#include <gtest/gtest.h>

namespace MayaFlux::Test {
namespace {

    using Kakshya::SignalSourceContainer;
    using Yantra::Datum;
    using Yantra::EnergyAnalysis;
    using Yantra::EnergyAnalyzer;
    using Yantra::EnergyAnalyzerConfig;
    using Yantra::EnergyLevel;
    using Yantra::EnergyMethod;

    using ContainerEnergyMatrix = EnergyAnalyzer<std::shared_ptr<SignalSourceContainer>, Eigen::MatrixXd>;

    constexpr uint32_t k_window = 256;
    constexpr uint32_t k_hop = 128;
    constexpr size_t k_samples = 1024;

    EnergyAnalyzerConfig config_for(EnergyMethod method, uint32_t window = k_window, uint32_t hop = k_hop)
    {
        return EnergyAnalyzerConfig { { .window_size = window, .hop_size = hop }, method };
    }

    std::vector<double> ramp()
    {
        std::vector<double> data(k_samples);
        for (size_t i = 0; i < data.size(); ++i) {
            data[i] = static_cast<double>(i) / static_cast<double>(k_samples - 1);
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

    std::shared_ptr<Kakshya::SoundFileContainer> container_of(const std::vector<double>& data)
    {
        return make_audio_container(data);
    }

    std::vector<double> kinesis_energy(EnergyMethod method, std::span<const double> data)
    {
        namespace D = Kinesis::Discrete;
        const size_t windows = D::num_windows(data.size(), k_window, k_hop);

        switch (method) {
        case EnergyMethod::RMS:
            return D::rms(data, windows, k_hop, k_window);
        case EnergyMethod::PEAK:
            return D::peak(data, windows, k_hop, k_window);
        case EnergyMethod::SPECTRAL:
            return D::spectral_energy(data, windows, k_hop, k_window);
        case EnergyMethod::ZERO_CROSSING:
            return D::zero_crossing_rate(data, windows, k_hop, k_window);
        case EnergyMethod::HARMONIC:
            return D::low_frequency_energy(data, windows, k_hop, k_window);
        case EnergyMethod::POWER:
            return D::power(data, windows, k_hop, k_window);
        case EnergyMethod::DYNAMIC_RANGE:
            return D::dynamic_range(data, windows, k_hop, k_window);
        }
        return {};
    }

    const char* method_name(EnergyMethod method)
    {
        switch (method) {
        case EnergyMethod::RMS:
            return "Rms";
        case EnergyMethod::PEAK:
            return "Peak";
        case EnergyMethod::SPECTRAL:
            return "Spectral";
        case EnergyMethod::ZERO_CROSSING:
            return "ZeroCrossing";
        case EnergyMethod::HARMONIC:
            return "Harmonic";
        case EnergyMethod::POWER:
            return "Power";
        case EnergyMethod::DYNAMIC_RANGE:
            return "DynamicRange";
        }
        return "Unknown";
    }

    class EnergyMethodTest : public ::testing::TestWithParam<EnergyMethod> {
    protected:
        void SetUp() override
        {
            container = container_of(ramp());
            analyzer = std::make_unique<ContainerEnergyMatrix>(config_for(GetParam()));
        }

        std::shared_ptr<Kakshya::SoundFileContainer> container;
        std::unique_ptr<ContainerEnergyMatrix> analyzer;
    };

    INSTANTIATE_TEST_SUITE_P(Methods, EnergyMethodTest,
        ::testing::Values(
            EnergyMethod::RMS,
            EnergyMethod::PEAK,
            EnergyMethod::SPECTRAL,
            EnergyMethod::ZERO_CROSSING,
            EnergyMethod::HARMONIC,
            EnergyMethod::POWER,
            EnergyMethod::DYNAMIC_RANGE),
        [](const ::testing::TestParamInfo<EnergyMethod>& info) {
            return std::string(method_name(info.param));
        });

    TEST_P(EnergyMethodTest, TheResultCarriesTheConfigItWasProducedUnder)
    {
        const auto result = analyzer->analyze_energy(container);

        EXPECT_EQ(EnergyMethod(result.config.method), GetParam());
        EXPECT_EQ(result.config.window_size, k_window);
        EXPECT_EQ(result.config.hop_size, k_hop);
        EXPECT_FALSE(result.channels.empty());
    }

    TEST_P(EnergyMethodTest, EveryWindowEnergyIsNonNegative)
    {
        const auto result = analyzer->analyze_energy(container);

        ASSERT_FALSE(result.channels.empty());
        for (double value : result.channels[0].energy_values) {
            EXPECT_GE(value, 0.0);
        }
    }

    TEST_P(EnergyMethodTest, TheWindowValuesAreTheKinesisValuesForTheMethod)
    {
        const auto result = analyzer->analyze_energy(container);

        ASSERT_FALSE(result.channels.empty());
        EXPECT_EQ(result.channels[0].energy_values, kinesis_energy(GetParam(), ramp()));
    }

    TEST_P(EnergyMethodTest, WindowPositionsDescribeEveryWindow)
    {
        const auto result = analyzer->analyze_energy(container);

        ASSERT_FALSE(result.channels.empty());
        const auto& channel = result.channels[0];
        EXPECT_EQ(channel.window_positions.size(), channel.energy_values.size());
        EXPECT_GE(channel.max_energy, channel.min_energy);

        for (const auto& [start, end] : channel.window_positions) {
            EXPECT_LT(start, end);
            EXPECT_LE(end, k_samples);
        }
    }

    TEST_P(EnergyMethodTest, TheOutputDatumCarriesTheResultInItsFacets)
    {
        const auto output = analyzer->apply_operation(Datum<std::shared_ptr<SignalSourceContainer>>(container));

        EXPECT_GT(output.data.size(), 0);

        const auto* carried = output.facets.get<EnergyAnalysis>();
        ASSERT_NE(carried, nullptr);
        EXPECT_EQ(EnergyMethod(carried->config.method), GetParam());
        EXPECT_EQ(carried->config.window_size, k_window);
        EXPECT_EQ(carried->config.hop_size, k_hop);
        EXPECT_FALSE(carried->channels.empty());
    }

    TEST_P(EnergyMethodTest, TheAnalyzerKeepsTheLastAnalysis)
    {
        analyzer->apply_operation(Datum<std::shared_ptr<SignalSourceContainer>>(container));

        const auto cached = analyzer->get_energy_analysis();
        EXPECT_EQ(EnergyMethod(cached.config.method), GetParam());
        EXPECT_FALSE(cached.channels.empty());

        const auto generic = safe_any_cast_or_throw<EnergyAnalysis>(analyzer->get_current_analysis());
        EXPECT_EQ(EnergyMethod(generic.config.method), GetParam());
    }

    struct WindowCase {
        uint32_t window;
        uint32_t hop;
    };

    class EnergyWindowTest : public ::testing::TestWithParam<WindowCase> {
    protected:
        void SetUp() override
        {
            container = container_of(ramp());
            analyzer = std::make_unique<ContainerEnergyMatrix>(
                config_for(EnergyMethod::RMS, GetParam().window, GetParam().hop));
        }

        std::shared_ptr<Kakshya::SoundFileContainer> container;
        std::unique_ptr<ContainerEnergyMatrix> analyzer;
    };

    INSTANTIATE_TEST_SUITE_P(Geometries, EnergyWindowTest,
        ::testing::Values(
            WindowCase { 128, 64 },
            WindowCase { 256, 128 },
            WindowCase { 512, 256 }),
        [](const ::testing::TestParamInfo<WindowCase>& info) {
            return "Window" + std::to_string(info.param.window) + "Hop" + std::to_string(info.param.hop);
        });

    TEST_P(EnergyWindowTest, TheWindowCountFollowsTheWindowAndHop)
    {
        const auto result = analyzer->analyze_energy(container);

        ASSERT_FALSE(result.channels.empty());
        const size_t expected = (k_samples - GetParam().window) / GetParam().hop + 1;
        EXPECT_EQ(result.channels[0].energy_values.size(), expected);
        EXPECT_EQ(result.config.window_size, GetParam().window);
        EXPECT_EQ(result.config.hop_size, GetParam().hop);
    }

    struct InvalidCase {
        const char* name;
        EnergyAnalyzerConfig config;
    };

    class EnergyInvalidConfigTest : public ::testing::TestWithParam<InvalidCase> { };

    INSTANTIATE_TEST_SUITE_P(Configs, EnergyInvalidConfigTest,
        ::testing::Values(
            InvalidCase { "ZeroWindow", EnergyAnalyzerConfig { { .window_size = 0, .hop_size = 1 } } },
            InvalidCase { "ZeroHop", EnergyAnalyzerConfig { { .window_size = 8, .hop_size = 0 } } },
            InvalidCase { "HopLargerThanWindow", EnergyAnalyzerConfig { { .window_size = 8, .hop_size = 16 } } },
            InvalidCase { "DescendingThresholds", EnergyAnalyzerConfig { {}, EnergyMethod::RMS, { 0.5, 0.1, 0.05, 0.01 } } }),
        [](const ::testing::TestParamInfo<InvalidCase>& info) {
            return std::string(info.param.name);
        });

    TEST_P(EnergyInvalidConfigTest, ConstructionRejectsTheConfig)
    {
        EXPECT_THROW(ContainerEnergyMatrix { GetParam().config }, std::invalid_argument);
    }

    class EnergyClassificationTest : public ::testing::Test {
    protected:
        static EnergyAnalyzerConfig classified()
        {
            return EnergyAnalyzerConfig { { .window_size = k_window, .hop_size = k_hop, .classification_enabled = true }, EnergyMethod::RMS, { 0.01, 0.05, 0.1, 0.5 } };
        }
    };

    TEST_F(EnergyClassificationTest, EveryWindowReceivesALevel)
    {
        ContainerEnergyMatrix analyzer(classified());
        const auto result = analyzer.analyze_energy(container_of(ramp()));

        ASSERT_FALSE(result.channels.empty());
        for (const auto& channel : result.channels) {
            EXPECT_EQ(channel.classifications.size(), channel.energy_values.size());
            EXPECT_GT(std::accumulate(channel.level_counts.begin(), channel.level_counts.end(), 0), 0);

            for (const auto& level : channel.classifications) {
                EXPECT_TRUE(level >= EnergyLevel::SILENT && level <= EnergyLevel::PEAK);
            }
        }
    }

    TEST_F(EnergyClassificationTest, QuarterwiseLevelsAreEachRepresented)
    {
        std::vector<double> signal(k_samples);
        for (size_t i = 0; i < signal.size(); ++i) {
            signal[i] = i < 256 ? 0.005 : (i < 512 ? 0.03 : (i < 768 ? 0.07 : 0.3));
        }

        ContainerEnergyMatrix analyzer(classified());
        const auto result = analyzer.analyze_energy(container_of(signal));

        ASSERT_FALSE(result.channels.empty());
        for (const auto& channel : result.channels) {
            EXPECT_GT(channel.level_counts[static_cast<size_t>(EnergyLevel::SILENT)], 0);
            EXPECT_GT(channel.level_counts[static_cast<size_t>(EnergyLevel::QUIET)], 0);
            EXPECT_GT(channel.level_counts[static_cast<size_t>(EnergyLevel::MODERATE)], 0);
            EXPECT_GT(channel.level_counts[static_cast<size_t>(EnergyLevel::LOUD)], 0);
        }
    }

    class EnergyEventTest : public ::testing::Test { };

    TEST_F(EnergyEventTest, ZeroCrossingEventsAreTheKinesisZeroCrossings)
    {
        const auto signal = sine(1.0, 10.0);
        ContainerEnergyMatrix analyzer(config_for(EnergyMethod::ZERO_CROSSING));

        const auto result = analyzer.analyze_energy(container_of(signal));

        ASSERT_FALSE(result.channels.empty());
        EXPECT_EQ(result.channels[0].event_positions, Kinesis::Discrete::zero_crossing_positions(signal, 0.0));
    }

    TEST_F(EnergyEventTest, PeakEventsAreTheKinesisPeaksAtAQuarterOfTheHop)
    {
        const auto signal = sine(1.0, 10.0);
        ContainerEnergyMatrix analyzer(config_for(EnergyMethod::PEAK));

        const auto result = analyzer.analyze_energy(container_of(signal));

        ASSERT_FALSE(result.channels.empty());
        EXPECT_EQ(result.channels[0].event_positions, Kinesis::Discrete::peak_positions(signal, 0.01, k_hop / 4));
    }

    class EnergyChannelTest : public ::testing::Test { };

    TEST_F(EnergyChannelTest, EachChannelIsAnalyzedSeparately)
    {
        auto container = make_audio_container(std::vector<std::vector<double>> { std::vector<double>(k_samples, 0.3), std::vector<double>(k_samples, 0.7) });

        ContainerEnergyMatrix analyzer(config_for(EnergyMethod::RMS));
        const auto result = analyzer.analyze_energy(container);

        ASSERT_EQ(result.channels.size(), 2U);
        EXPECT_NEAR(result.channels[0].mean_energy, 0.3, 0.01);
        EXPECT_NEAR(result.channels[1].mean_energy, 0.7, 0.01);
    }

    TEST_F(EnergyChannelTest, BatchAnalysisReturnsOneResultPerInput)
    {
        std::vector<std::shared_ptr<SignalSourceContainer>> containers;
        for (int i = 0; i < 3; ++i) {
            containers.push_back(container_of(ramp()));
        }

        ContainerEnergyMatrix analyzer;
        const auto batch = analyzer.analyze_batch(containers);

        ASSERT_EQ(batch.size(), 3U);
        for (const auto& entry : batch) {
            EXPECT_FALSE(safe_any_cast_or_throw<EnergyAnalysis>(entry).channels.empty());
        }
    }

    TEST_F(EnergyChannelTest, ADefaultAnalyzerUsesTheConfigDefaults)
    {
        ContainerEnergyMatrix analyzer;
        const auto result = analyzer.analyze_energy(container_of(ramp()));

        EXPECT_EQ(EnergyMethod(result.config.method), EnergyMethod::RMS);
        EXPECT_EQ(result.config.window_size, k_window);
        EXPECT_EQ(result.config.hop_size, k_hop);
        EXPECT_FALSE(result.config.classification_enabled);
    }

    TEST_F(EnergyChannelTest, SetConfigReplacesTheSettingsOfLaterAnalyses)
    {
        ContainerEnergyMatrix analyzer;
        analyzer.set_config(config_for(EnergyMethod::PEAK, 128, 64));

        const auto result = analyzer.analyze_energy(container_of(ramp()));
        EXPECT_EQ(EnergyMethod(result.config.method), EnergyMethod::PEAK);
        EXPECT_EQ(result.config.window_size, 128U);
        EXPECT_EQ(result.config.hop_size, 64U);
    }

} // namespace
} // namespace MayaFlux::Test
