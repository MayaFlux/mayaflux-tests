#include "gtest/gtest.h"

#include "MayaFlux/Yantra/Analyzers/StatisticalAnalyzer.hpp"
#include "MayaFlux/Yantra/Transformers/MathematicalTransformer.hpp"

#include "audio_container.hpp"

namespace MayaFlux::Test {
namespace {

    using Kakshya::DataVariant;
    using Yantra::Datum;
    using Yantra::StatisticalAnalysis;
    using Yantra::StatisticalAnalyzer;
    using Yantra::StatisticalAnalyzerConfig;
    using Yantra::StatisticalMethod;

    using VariantStatisticalMatrix = StatisticalAnalyzer<std::vector<DataVariant>, Eigen::MatrixXd>;
    using ContainerStatisticalMatrix = StatisticalAnalyzer<std::shared_ptr<Kakshya::SignalSourceContainer>, Eigen::MatrixXd>;

    constexpr uint32_t k_window = 10;
    constexpr uint32_t k_hop = 5;

    StatisticalAnalyzerConfig config_for(StatisticalMethod method, uint32_t window = k_window, uint32_t hop = k_hop)
    {
        return StatisticalAnalyzerConfig { { .window_size = window, .hop_size = hop, .classification_enabled = true }, method, 25.0 };
    }

    std::vector<double> one_to_ten()
    {
        return { 1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 9.0, 10.0 };
    }

    std::vector<DataVariant> single_channel(const std::vector<double>& data)
    {
        return { DataVariant { data } };
    }

    std::vector<double> kinesis_statistic(StatisticalMethod method, std::span<const double> data)
    {
        namespace D = Kinesis::Discrete;
        const size_t windows = D::num_windows(data.size(), k_window, k_hop);

        switch (method) {
        case StatisticalMethod::MEAN:
            return D::mean(data, windows, k_hop, k_window);
        case StatisticalMethod::VARIANCE:
            return D::variance(data, windows, k_hop, k_window, true);
        case StatisticalMethod::STD_DEV:
            return D::std_dev(data, windows, k_hop, k_window, true);
        case StatisticalMethod::SKEWNESS:
            return D::skewness(data, windows, k_hop, k_window);
        case StatisticalMethod::KURTOSIS:
            return D::kurtosis(data, windows, k_hop, k_window);
        case StatisticalMethod::MIN:
            return D::min(data, windows, k_hop, k_window);
        case StatisticalMethod::MAX:
            return D::max(data, windows, k_hop, k_window);
        case StatisticalMethod::MEDIAN:
            return D::median(data, windows, k_hop, k_window);
        case StatisticalMethod::RANGE:
            return D::range(data, windows, k_hop, k_window);
        case StatisticalMethod::PERCENTILE:
            return D::percentile(data, windows, k_hop, k_window, 25.0);
        case StatisticalMethod::MODE:
            return D::mode(data, windows, k_hop, k_window);
        case StatisticalMethod::MAD:
            return D::mad(data, windows, k_hop, k_window);
        case StatisticalMethod::CV:
            return D::coefficient_of_variation(data, windows, k_hop, k_window, true);
        case StatisticalMethod::SUM:
            return D::sum(data, windows, k_hop, k_window);
        case StatisticalMethod::COUNT:
            return D::count(data, windows, k_hop, k_window);
        case StatisticalMethod::ENTROPY:
            return D::entropy(data, windows, k_hop, k_window);
        case StatisticalMethod::ZSCORE:
            return D::mean_zscore(data, windows, k_hop, k_window, true);
        }
        return {};
    }

    const char* method_name(StatisticalMethod method)
    {
        switch (method) {
        case StatisticalMethod::MEAN:
            return "Mean";
        case StatisticalMethod::VARIANCE:
            return "Variance";
        case StatisticalMethod::STD_DEV:
            return "StdDev";
        case StatisticalMethod::SKEWNESS:
            return "Skewness";
        case StatisticalMethod::KURTOSIS:
            return "Kurtosis";
        case StatisticalMethod::MIN:
            return "Min";
        case StatisticalMethod::MAX:
            return "Max";
        case StatisticalMethod::MEDIAN:
            return "Median";
        case StatisticalMethod::RANGE:
            return "Range";
        case StatisticalMethod::PERCENTILE:
            return "Percentile";
        case StatisticalMethod::MODE:
            return "Mode";
        case StatisticalMethod::MAD:
            return "Mad";
        case StatisticalMethod::CV:
            return "CoefficientOfVariation";
        case StatisticalMethod::SUM:
            return "Sum";
        case StatisticalMethod::COUNT:
            return "Count";
        case StatisticalMethod::ENTROPY:
            return "Entropy";
        case StatisticalMethod::ZSCORE:
            return "Zscore";
        }
        return "Unknown";
    }

    class StatisticMethodTest : public ::testing::TestWithParam<StatisticalMethod> {
    protected:
        void SetUp() override
        {
            analyzer = std::make_unique<VariantStatisticalMatrix>(config_for(GetParam()));
            input = single_channel(one_to_ten());
        }

        std::unique_ptr<VariantStatisticalMatrix> analyzer;
        std::vector<DataVariant> input;
    };

    const std::vector<StatisticalMethod>& every_method()
    {
        static const std::vector<StatisticalMethod> methods {
            StatisticalMethod::MEAN,
            StatisticalMethod::VARIANCE,
            StatisticalMethod::STD_DEV,
            StatisticalMethod::SKEWNESS,
            StatisticalMethod::KURTOSIS,
            StatisticalMethod::MIN,
            StatisticalMethod::MAX,
            StatisticalMethod::MEDIAN,
            StatisticalMethod::RANGE,
            StatisticalMethod::PERCENTILE,
            StatisticalMethod::MODE,
            StatisticalMethod::MAD,
            StatisticalMethod::CV,
            StatisticalMethod::SUM,
            StatisticalMethod::COUNT,
            StatisticalMethod::ENTROPY,
            StatisticalMethod::ZSCORE,
        };
        return methods;
    }

    INSTANTIATE_TEST_SUITE_P(Methods, StatisticMethodTest,
        ::testing::ValuesIn(every_method()),
        [](const ::testing::TestParamInfo<StatisticalMethod>& info) {
            return std::string(method_name(info.param));
        });

    TEST_P(StatisticMethodTest, TheWindowValuesAreTheKinesisValuesForTheMethod)
    {
        const auto stats = analyzer->analyze_statistics(input);

        ASSERT_EQ(stats.channel_statistics.size(), 1U);
        EXPECT_EQ(stats.channel_statistics[0].statistical_values, kinesis_statistic(GetParam(), one_to_ten()));
    }

    TEST_P(StatisticMethodTest, TheResultCarriesTheConfigItWasProducedUnder)
    {
        const auto stats = analyzer->analyze_statistics(input);

        EXPECT_EQ(StatisticalMethod(stats.config.method), GetParam());
        EXPECT_EQ(stats.config.window_size, k_window);
        EXPECT_EQ(stats.config.hop_size, k_hop);
        EXPECT_DOUBLE_EQ(stats.config.percentile, 25.0);
    }

    TEST_P(StatisticMethodTest, TheOutputDatumCarriesTheResultInItsFacets)
    {
        const auto output = analyzer->apply_operation(Datum<std::vector<DataVariant>> { input });

        EXPECT_GT(output.data.size(), 0);

        const auto* carried = output.facets.get<StatisticalAnalysis>();
        ASSERT_NE(carried, nullptr);
        EXPECT_EQ(StatisticalMethod(carried->config.method), GetParam());
        EXPECT_FALSE(carried->channel_statistics.empty());
    }

    class StatisticContainerTest : public ::testing::TestWithParam<StatisticalMethod> {
    protected:
        void SetUp() override
        {
            analyzer = std::make_unique<ContainerStatisticalMatrix>(config_for(GetParam()));
            container = make_audio_container(one_to_ten());
        }

        std::unique_ptr<ContainerStatisticalMatrix> analyzer;
        std::shared_ptr<Kakshya::SoundFileContainer> container;
    };

    INSTANTIATE_TEST_SUITE_P(Methods, StatisticContainerTest,
        ::testing::ValuesIn(every_method()),
        [](const ::testing::TestParamInfo<StatisticalMethod>& info) {
            return std::string(method_name(info.param));
        });

    TEST_P(StatisticContainerTest, TheWindowValuesAreTheKinesisValuesForTheMethod)
    {
        const auto stats = analyzer->analyze_statistics(container);

        ASSERT_EQ(stats.channel_statistics.size(), 1U);
        EXPECT_EQ(stats.channel_statistics[0].statistical_values, kinesis_statistic(GetParam(), one_to_ten()));
    }

    TEST_P(StatisticContainerTest, EachChannelIsAnalyzedFromItsOwnSamples)
    {
        const auto forward = one_to_ten();
        const std::vector<double> reversed(forward.rbegin(), forward.rend());
        container = make_audio_container(std::vector<std::vector<double>> { forward, reversed });

        const auto stats = analyzer->analyze_statistics(container);

        ASSERT_EQ(stats.channel_statistics.size(), 2U);
        EXPECT_EQ(stats.channel_statistics[0].statistical_values, kinesis_statistic(GetParam(), forward));
        EXPECT_EQ(stats.channel_statistics[1].statistical_values, kinesis_statistic(GetParam(), reversed));
    }

    struct InvalidCase {
        const char* name;
        StatisticalAnalyzerConfig config;
    };

    class StatisticInvalidConfigTest : public ::testing::TestWithParam<InvalidCase> { };

    INSTANTIATE_TEST_SUITE_P(Configs, StatisticInvalidConfigTest,
        ::testing::Values(
            InvalidCase { "ZeroWindow", StatisticalAnalyzerConfig { { .window_size = 0, .hop_size = 1 } } },
            InvalidCase { "ZeroHop", StatisticalAnalyzerConfig { { .window_size = 8, .hop_size = 0 } } },
            InvalidCase { "HopLargerThanWindow", StatisticalAnalyzerConfig { { .window_size = 8, .hop_size = 16 } } },
            InvalidCase { "PercentileAboveRange", StatisticalAnalyzerConfig { {}, StatisticalMethod::PERCENTILE, 150.0 } },
            InvalidCase { "PercentileBelowRange", StatisticalAnalyzerConfig { {}, StatisticalMethod::PERCENTILE, -1.0 } }),
        [](const ::testing::TestParamInfo<InvalidCase>& info) {
            return std::string(info.param.name);
        });

    TEST_P(StatisticInvalidConfigTest, ConstructionRejectsTheConfig)
    {
        EXPECT_THROW(VariantStatisticalMatrix { GetParam().config }, std::invalid_argument);
    }

    class StatisticAnalysisTest : public ::testing::Test {
    protected:
        VariantStatisticalMatrix analyzer { config_for(StatisticalMethod::MEAN) };
    };

    TEST_F(StatisticAnalysisTest, WindowedAnalysisProducesOneValuePerWindow)
    {
        std::vector<double> data(100);
        std::iota(data.begin(), data.end(), 1.0);

        const auto stats = analyzer.analyze_statistics(single_channel(data));

        ASSERT_EQ(stats.channel_statistics.size(), 1U);
        const auto& values = stats.channel_statistics[0].statistical_values;
        ASSERT_EQ(values.size(), 19U);
        EXPECT_NEAR(values[0], 5.5, 1e-10);
        EXPECT_NEAR(values[1], 10.5, 1e-10);
    }

    TEST_F(StatisticAnalysisTest, EachChannelIsAnalyzedSeparately)
    {
        analyzer.set_config(config_for(StatisticalMethod::MEAN, 5, 5));

        const std::vector<DataVariant> input = {
            DataVariant { std::vector<double> { 1.0, 2.0, 3.0, 4.0, 5.0 } },
            DataVariant { std::vector<double> { 10.0, 20.0, 30.0, 40.0, 50.0 } }
        };
        const auto stats = analyzer.analyze_statistics(input);

        ASSERT_EQ(stats.channel_statistics.size(), 2U);
        EXPECT_NEAR(stats.channel_statistics[0].statistical_values[0], 3.0, 1e-10);
        EXPECT_NEAR(stats.channel_statistics[1].statistical_values[0], 30.0, 1e-10);
        EXPECT_NE(stats.channel_statistics[0].mean_stat, stats.channel_statistics[1].mean_stat);
    }

    TEST_F(StatisticAnalysisTest, EachChannelReportsItsSummaryStructure)
    {
        const auto stats = analyzer.analyze_statistics(single_channel(one_to_ten()));

        ASSERT_FALSE(stats.channel_statistics.empty());
        for (const auto& channel : stats.channel_statistics) {
            EXPECT_FALSE(channel.statistical_values.empty());
            EXPECT_GT(channel.mean_stat, 0.0);
            EXPECT_GE(channel.max_stat, channel.min_stat);
            EXPECT_GE(channel.stat_variance, 0.0);
            EXPECT_GE(channel.stat_std_dev, 0.0);
            EXPECT_EQ(channel.percentiles.size(), 3U);
            EXPECT_FALSE(channel.window_positions.empty());
            EXPECT_GT(std::accumulate(channel.level_counts.begin(), channel.level_counts.end(), 0), 0);
        }
    }

    TEST_F(StatisticAnalysisTest, PopulationVarianceFollowsTheConfig)
    {
        auto config = config_for(StatisticalMethod::VARIANCE);
        config.sample_variance = false;
        analyzer.set_config(config);

        const auto stats = analyzer.analyze_statistics(single_channel(one_to_ten()));

        ASSERT_EQ(stats.channel_statistics.size(), 1U);
        EXPECT_EQ(stats.channel_statistics[0].statistical_values,
            Kinesis::Discrete::variance(one_to_ten(), 1, k_hop, k_window, false));
    }

    TEST_F(StatisticAnalysisTest, EmptyInputThrows)
    {
        EXPECT_THROW(analyzer.analyze_statistics(std::vector<DataVariant> {}), std::runtime_error);
    }

    TEST_F(StatisticAnalysisTest, TheAnalyzerKeepsTheLastAnalysis)
    {
        analyzer.analyze_statistics(single_channel(one_to_ten()));

        const auto cached = analyzer.get_statistical_analysis();
        EXPECT_EQ(StatisticalMethod(cached.config.method), StatisticalMethod::MEAN);
        EXPECT_FALSE(cached.channel_statistics.empty());
    }

    TEST_F(StatisticAnalysisTest, SetConfigReplacesTheSettingsOfLaterAnalyses)
    {
        analyzer.set_config(config_for(StatisticalMethod::MAX));

        const auto stats = analyzer.analyze_statistics(single_channel(one_to_ten()));
        EXPECT_EQ(StatisticalMethod(stats.config.method), StatisticalMethod::MAX);
        EXPECT_NEAR(stats.channel_statistics[0].statistical_values[0], 10.0, 1e-10);
    }

    TEST_F(StatisticAnalysisTest, ADefaultAnalyzerUsesTheLongWindowWithClassification)
    {
        VariantStatisticalMatrix defaults;
        std::vector<double> data(1024);
        std::iota(data.begin(), data.end(), 1.0);

        const auto stats = defaults.analyze_statistics(single_channel(data));
        EXPECT_EQ(StatisticalMethod(stats.config.method), StatisticalMethod::MEAN);
        EXPECT_EQ(stats.config.window_size, 512U);
        EXPECT_EQ(stats.config.hop_size, 256U);
        EXPECT_TRUE(stats.config.classification_enabled);
    }

    enum class StatisticInputPath { Inferred, Explicit, Container };

    class StatisticPipelineOutputTest : public ::testing::TestWithParam<std::tuple<StatisticInputPath, size_t>> {
    protected:
        using Samples = std::vector<double>;
        using Channels = std::vector<Samples>;
        using Variants = std::vector<DataVariant>;
        using Dimension = Kakshya::DataDimension;

        VariantStatisticalMatrix variants { config_for(StatisticalMethod::MEAN, 2, 2) };
        ContainerStatisticalMatrix containers { config_for(StatisticalMethod::MEAN, 2, 2) };

        Datum<Eigen::MatrixXd> run(const Samples& samples, size_t count)
        {
            Channels channels(count, samples);
            for (size_t channel = 0; channel < count; ++channel)
                for (auto& sample : channels[channel])
                    sample += 10.0 * channel;
            if (std::get<0>(GetParam()) == StatisticInputPath::Container) {
                std::shared_ptr<Kakshya::SignalSourceContainer> source = make_audio_container(channels);
                return containers.apply_operation(Datum<std::shared_ptr<Kakshya::SignalSourceContainer>>(source));
            }
            Variants data;
            for (auto& channel : channels)
                data.emplace_back(std::move(channel));
            if (std::get<0>(GetParam()) == StatisticInputPath::Explicit)
                return variants.apply_operation(Datum<Variants>(std::move(data),
                    { Dimension::time(samples.size()), Dimension::channel(count) },
                    Kakshya::DataModality::AUDIO_MULTICHANNEL));
            return variants.apply_operation(Datum<Variants>(std::move(data)));
        }

        void expect_output(const Datum<Eigen::MatrixXd>& output, const Samples& expected, size_t count,
            StatisticalMethod method = StatisticalMethod::MEAN)
        {
            ASSERT_EQ(output.data.rows(), expected.size());
            ASSERT_EQ(output.data.cols(), count);
            EXPECT_EQ(output.get_total_elements(), expected.size() * count);
            const int time = output.find_dimension_by_role(Dimension::Role::TIME);
            ASSERT_GE(time, 0);
            EXPECT_EQ(output.dimensions[time].size, expected.size());
            if (count > 1) {
                const int channels = output.find_dimension_by_role(Dimension::Role::CHANNEL);
                ASSERT_GE(channels, 0);
                EXPECT_EQ(output.dimensions[channels].size, count);
            }
            const auto* record = output.facets.get<StatisticalAnalysis>();
            ASSERT_NE(record, nullptr);
            ASSERT_EQ(record->channel_statistics.size(), count);
            EXPECT_EQ(static_cast<StatisticalMethod>(record->config.method), method);
            EXPECT_EQ(record->config.window_size, 2);
            EXPECT_EQ(record->config.hop_size, 2);
            const double factor = method == StatisticalMethod::SUM ? 2.0 : 1.0;
            for (size_t channel = 0; channel < count; ++channel) {
                SCOPED_TRACE(channel);
                const auto& values = record->channel_statistics[channel].statistical_values;
                ASSERT_EQ(values.size(), expected.size());
                for (size_t row = 0; row < expected.size(); ++row) {
                    EXPECT_NEAR(output.data(row, channel), factor * (expected[row] + 10.0 * channel), 1e-12);
                    EXPECT_NEAR(values[row], output.data(row, channel), 1e-12);
                }
            }
        }
    };

    TEST_P(StatisticPipelineOutputTest, WindowedMatrixAndFacetDescribeTheSameResult)
    {
        const auto count = std::get<1>(GetParam());
        expect_output(run({ 1, 1, 3, 3, 5, 5, 99 }, count), { 1, 3, 5 }, count);
    }

    TEST_P(StatisticPipelineOutputTest, ReuseUpdatesOutputShapeFacetsAndMethod)
    {
        const auto count = std::get<1>(GetParam());
        expect_output(run({ 1, 1, 3, 3, 5, 5 }, count), { 1, 3, 5 }, count);
        expect_output(run({ 7, 7, 9, 9 }, 1), { 7, 9 }, 1);
        variants.set_config(config_for(StatisticalMethod::SUM, 2, 2));
        containers.set_config(config_for(StatisticalMethod::SUM, 2, 2));
        expect_output(run({ 2, 2, 4, 4, 6, 6, 8, 8 }, count + 1), { 2, 4, 6, 8 }, count + 1, StatisticalMethod::SUM);
    }

    TEST_P(StatisticPipelineOutputTest, MatrixCanFeedAnotherNumericalOperation)
    {
        const auto count = std::get<1>(GetParam());
        const auto output = run({ 1, 1, 3, 3, 5, 5 }, count);
        ASSERT_EQ(output.data.rows(), 3);
        ASSERT_EQ(output.data.cols(), count);
        Yantra::MathematicalTransformer<Eigen::MatrixXd> gain(
            Yantra::MathematicalTransformerConfig { .gain_factor = 2.0 });
        const auto transformed = gain.apply_operation(output);
        ASSERT_EQ(transformed.data.rows(), 3);
        ASSERT_EQ(transformed.data.cols(), count);
        EXPECT_EQ(transformed.get_total_elements(), 3 * count);
        const Samples expected { 2, 6, 10 };
        for (size_t channel = 0; channel < count; ++channel)
            for (size_t row = 0; row < expected.size(); ++row)
                EXPECT_NEAR(transformed.data(row, channel), expected[row] + 20.0 * channel, 1e-12);
    }

    INSTANTIATE_TEST_SUITE_P(Inputs, StatisticPipelineOutputTest,
        ::testing::Combine(::testing::Values(StatisticInputPath::Inferred, StatisticInputPath::Explicit, StatisticInputPath::Container),
            ::testing::Values(size_t { 1 }, size_t { 2 }, size_t { 3 })),
        [](const auto& info) {
            const auto path = std::get<0>(info.param);
            return std::string(path == StatisticInputPath::Inferred ? "Inferred" : path == StatisticInputPath::Explicit ? "Explicit" : "Container")
                + std::to_string(std::get<1>(info.param)) + "Channels";
        });

} // namespace
} // namespace MayaFlux::Test
