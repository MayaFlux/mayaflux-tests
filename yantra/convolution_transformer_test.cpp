#include "MayaFlux/Kinesis/Discrete/Convolution.hpp"
#include "MayaFlux/Yantra/Transformers/ConvolutionTransformer.hpp"
#include "transformer_helpers.hpp"

#include <gtest/gtest.h>
#include "../support/test_metadata.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {
namespace {

    using Yantra::ConvolutionOperation;
    using Yantra::ConvolutionTransformer;
    using Yantra::ConvolutionTransformerConfig;
    using Yantra::TransformationStrategy;
    namespace D = Kinesis::Discrete;

    constexpr std::size_t k_samples = 512;
    constexpr double k_tolerance = 1e-9;

    const char* operation_name(ConvolutionOperation op)
    {
        switch (op) {
        case ConvolutionOperation::DIRECT_CONVOLUTION:
            return "DirectConvolution";
        case ConvolutionOperation::CROSS_CORRELATION:
            return "CrossCorrelation";
        case ConvolutionOperation::MATCHED_FILTER:
            return "MatchedFilter";
        case ConvolutionOperation::DECONVOLUTION:
            return "Deconvolution";
        case ConvolutionOperation::AUTO_CORRELATION:
            return "AutoCorrelation";
        }
        return "Unknown";
    }

    ConvolutionTransformerConfig config_for(ConvolutionOperation op)
    {
        ConvolutionTransformerConfig cfg;
        cfg.operation = op;
        cfg.impulse_response = { 0.25, 0.5, 0.25 };
        cfg.template_signal = { 0.5, 1.0, 0.5, -0.25 };
        cfg.reference_signal = { 1.0, -1.0, 0.5 };
        cfg.normalize = true;
        cfg.regularization = 1e-3;
        return cfg;
    }

    std::vector<double> kinesis_expectation(const ConvolutionTransformerConfig& cfg, std::span<const double> src)
    {
        switch (static_cast<ConvolutionOperation>(cfg.operation)) {
        case ConvolutionOperation::DIRECT_CONVOLUTION:
            return D::convolve(src, cfg.impulse_response);
        case ConvolutionOperation::CROSS_CORRELATION:
            return D::cross_correlate(src, cfg.template_signal, cfg.normalize);
        case ConvolutionOperation::MATCHED_FILTER:
            return D::matched_filter(src, cfg.reference_signal);
        case ConvolutionOperation::DECONVOLUTION:
            return D::deconvolve(src, cfg.impulse_response, cfg.regularization);
        case ConvolutionOperation::AUTO_CORRELATION:
            return D::auto_correlate(src, cfg.normalize);
        }
        return {};
    }

    Yantra::Datum<std::vector<Kakshya::DataVariant>> two_channel_input()
    {
        return to_datum({ sine_samples(k_samples, 440.0), ramp_samples(k_samples) });
    }

    class ConvolutionOperationTest : public ::testing::TestWithParam<ConvolutionOperation> {
    };

    INSTANTIATE_TEST_SUITE_P(Operations, ConvolutionOperationTest,
        ::testing::Values(
            ConvolutionOperation::DIRECT_CONVOLUTION,
            ConvolutionOperation::CROSS_CORRELATION,
            ConvolutionOperation::MATCHED_FILTER,
            ConvolutionOperation::DECONVOLUTION,
            ConvolutionOperation::AUTO_CORRELATION),
        [](const ::testing::TestParamInfo<ConvolutionOperation>& info) {
            return std::string(operation_name(info.param));
        });

    TEST_P(ConvolutionOperationTest, PreservesChannelCountAndSampleCount)
    {
        ConvolutionTransformer<> transformer(config_for(GetParam()));

        const auto result = transformer.apply_operation(two_channel_input());

        ASSERT_EQ(result.data.size(), 2U);
        EXPECT_EQ(channel_samples(result, 0).size(), k_samples);
        EXPECT_EQ(channel_samples(result, 1).size(), k_samples);
    }

    TEST_P(ConvolutionOperationTest, EveryOutputSampleIsFinite)
    {
        ConvolutionTransformer<> transformer(config_for(GetParam()));

        const auto result = transformer.apply_operation(two_channel_input());

        for (std::size_t ch = 0; ch < 2; ++ch) {
            for (double sample : channel_samples(result, ch)) {
                EXPECT_TRUE(std::isfinite(sample)) << "Channel " << ch;
            }
        }
    }

    TEST_P(ConvolutionOperationTest, MatchesTheKinesisCallWithItsSettings)
    {
        const auto cfg = config_for(GetParam());
        ConvolutionTransformer<> transformer(cfg);

        const auto input = two_channel_input();
        const auto result = transformer.apply_operation(input);

        for (std::size_t ch = 0; ch < 2; ++ch) {
            const auto expected = kinesis_expectation(cfg, channel_samples(input, ch));
            const auto out = channel_samples(result, ch);
            ASSERT_EQ(out.size(), expected.size()) << "Channel " << ch;
            for (std::size_t i = 0; i < expected.size(); ++i) {
                EXPECT_NEAR(out[i], expected[i], k_tolerance) << "Channel " << ch << " sample " << i;
            }
        }
    }

    class ConvolutionTransformerTest : public ::testing::Test {
    };

    TEST_F(ConvolutionTransformerTest, DefaultIsIdentityImpulse)
    {
        ConvolutionTransformer<> transformer;

        const auto input = to_datum({ ramp_samples(k_samples) });
        const auto result = transformer.apply_operation(input);

        const auto out = channel_samples(result, 0);
        const auto in = channel_samples(input, 0);
        for (std::size_t i = 0; i < k_samples; ++i) {
            EXPECT_NEAR(out[i], in[i], k_tolerance) << "Sample " << i;
        }
    }

    TEST_F(ConvolutionTransformerTest, SingleTapImpulseScalesTheSignal)
    {
        ConvolutionTransformerConfig cfg;
        cfg.operation = ConvolutionOperation::DIRECT_CONVOLUTION;
        cfg.impulse_response = { 0.5 };
        ConvolutionTransformer<> transformer(cfg);

        const auto input = to_datum({ ramp_samples(k_samples) });
        const auto result = transformer.apply_operation(input);

        const auto out = channel_samples(result, 0);
        const auto in = channel_samples(input, 0);
        for (std::size_t i = 0; i < k_samples; ++i) {
            EXPECT_NEAR(out[i], 0.5 * in[i], k_tolerance) << "Sample " << i;
        }
    }

    TEST_F(ConvolutionTransformerTest, SetConfigReplacesTheOperation)
    {
        ConvolutionTransformer<> transformer(config_for(ConvolutionOperation::AUTO_CORRELATION));

        ConvolutionTransformerConfig cfg;
        cfg.operation = ConvolutionOperation::DIRECT_CONVOLUTION;
        cfg.impulse_response = { 0.5 };
        transformer.set_config(cfg);

        const auto input = to_datum({ ramp_samples(k_samples) });
        const auto result = transformer.apply_operation(input);

        const auto out = channel_samples(result, 0);
        const auto in = channel_samples(input, 0);
        for (std::size_t i = 0; i < k_samples; ++i) {
            EXPECT_NEAR(out[i], 0.5 * in[i], k_tolerance) << "Sample " << i;
        }
    }

    TEST_F(ConvolutionTransformerTest, StrategyDecidesInPlace)
    {
        ConvolutionTransformer<> transformer;
        EXPECT_FALSE(transformer.is_in_place());

        ConvolutionTransformerConfig cfg;
        cfg.strategy = TransformationStrategy::IN_PLACE;
        transformer.set_config(cfg);
        EXPECT_TRUE(transformer.is_in_place());
    }

}
}
