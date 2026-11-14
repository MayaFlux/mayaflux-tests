#include "MayaFlux/Kinesis/Discrete/Transform.hpp"
#include "MayaFlux/Yantra/Transformers/MathematicalTransformer.hpp"
#include "transformer_helpers.hpp"

#include <gtest/gtest.h>
#include "../support/test_metadata.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {
namespace {

    using Yantra::MathematicalOperation;
    using Yantra::MathematicalTransformer;
    using Yantra::MathematicalTransformerConfig;
    using Yantra::TrigFunction;
    using Yantra::TransformationStrategy;
    namespace D = Kinesis::Discrete;

    constexpr std::size_t k_samples = 256;
    constexpr double k_tolerance = 1e-9;

    const char* operation_name(MathematicalOperation op)
    {
        switch (op) {
        case MathematicalOperation::GAIN:
            return "Gain";
        case MathematicalOperation::OFFSET:
            return "Offset";
        case MathematicalOperation::POWER:
            return "Power";
        case MathematicalOperation::LOGARITHMIC:
            return "Logarithmic";
        case MathematicalOperation::EXPONENTIAL:
            return "Exponential";
        case MathematicalOperation::TRIGONOMETRIC:
            return "Trigonometric";
        case MathematicalOperation::QUANTIZE:
            return "Quantize";
        case MathematicalOperation::NORMALIZE:
            return "Normalize";
        case MathematicalOperation::POLYNOMIAL:
            return "Polynomial";
        }
        return "Unknown";
    }

    MathematicalTransformerConfig config_for(MathematicalOperation op)
    {
        MathematicalTransformerConfig cfg;
        cfg.operation = op;
        cfg.target_peak = 0.8;
        cfg.coefficients = { 2.0, 1.0 };
        return cfg;
    }

    Yantra::Datum<std::vector<Kakshya::DataVariant>> two_channel_input()
    {
        return to_datum({ sine_samples(k_samples, 440.0, 48000.0, 0.5), ramp_samples(k_samples) });
    }

    class MathOperationTest : public ::testing::TestWithParam<MathematicalOperation> {
    };

    INSTANTIATE_TEST_SUITE_P(Operations, MathOperationTest,
        ::testing::Values(
            MathematicalOperation::GAIN,
            MathematicalOperation::OFFSET,
            MathematicalOperation::POWER,
            MathematicalOperation::LOGARITHMIC,
            MathematicalOperation::EXPONENTIAL,
            MathematicalOperation::TRIGONOMETRIC,
            MathematicalOperation::QUANTIZE,
            MathematicalOperation::NORMALIZE,
            MathematicalOperation::POLYNOMIAL),
        [](const ::testing::TestParamInfo<MathematicalOperation>& info) {
            return std::string(operation_name(info.param));
        });

    TEST_P(MathOperationTest, PreservesChannelCountAndSampleCount)
    {
        MathematicalTransformer<> transformer(config_for(GetParam()));

        const auto result = transformer.apply_operation(two_channel_input());

        ASSERT_EQ(result.data.size(), 2U);
        EXPECT_EQ(channel_samples(result, 0).size(), k_samples);
        EXPECT_EQ(channel_samples(result, 1).size(), k_samples);
    }

    TEST_P(MathOperationTest, EveryOutputSampleIsFinite)
    {
        MathematicalTransformer<> transformer(config_for(GetParam()));

        const auto result = transformer.apply_operation(two_channel_input());

        for (std::size_t ch = 0; ch < 2; ++ch) {
            for (double sample : channel_samples(result, ch)) {
                EXPECT_TRUE(std::isfinite(sample)) << "Channel " << ch;
            }
        }
    }

    TEST_P(MathOperationTest, ConfigIsReadBackAsSet)
    {
        const auto cfg = config_for(GetParam());
        MathematicalTransformer<> transformer(cfg);

        EXPECT_EQ(static_cast<MathematicalOperation>(transformer.config<MathematicalTransformerConfig>().operation), GetParam());
    }

    class MathTransformerTest : public ::testing::Test {
    protected:
        Yantra::Datum<std::vector<Kakshya::DataVariant>> ramp_input()
        {
            return to_datum({ ramp_samples(k_samples) });
        }
    };

    TEST_F(MathTransformerTest, DefaultsAreGainOfOne)
    {
        MathematicalTransformer<> transformer;

        const auto input = ramp_input();
        const auto result = transformer.apply_operation(input);

        const auto out = channel_samples(result, 0);
        const auto in = channel_samples(input, 0);
        for (std::size_t i = 0; i < k_samples; ++i) {
            EXPECT_NEAR(out[i], in[i], k_tolerance) << "Sample " << i;
        }
    }

    TEST_F(MathTransformerTest, GainScalesEverySample)
    {
        MathematicalTransformerConfig cfg;
        cfg.operation = MathematicalOperation::GAIN;
        cfg.gain_factor = 2.0;
        MathematicalTransformer<> transformer(cfg);

        const auto input = ramp_input();
        const auto result = transformer.apply_operation(input);

        const auto out = channel_samples(result, 0);
        const auto in = channel_samples(input, 0);
        for (std::size_t i = 0; i < k_samples; ++i) {
            EXPECT_NEAR(out[i], 2.0 * in[i], k_tolerance) << "Sample " << i;
        }
    }

    TEST_F(MathTransformerTest, GainPreservesExplicitFrequencyAxesAndNames)
    {
        using Dimension = Kakshya::DataDimension;
        auto bins = Dimension::frequency(3, "selected_bins");
        Yantra::DataIO input(std::vector<Kakshya::DataVariant> {
                                std::vector<double> { 1, 2, 3 }, std::vector<double> { 11, 12, 13 } },
            { Dimension::channel(2), bins }, Kakshya::DataModality::SPECTRAL_2D);
        MathematicalTransformer<> transformer(MathematicalTransformerConfig { .gain_factor = 2.0 });
        const auto output = transformer.apply_operation(input);
        ASSERT_EQ(output.dimensions.size(), 2);
        EXPECT_EQ(output.modality, Kakshya::DataModality::SPECTRAL_2D);
        EXPECT_EQ(output.dimensions[0].role, Dimension::Role::CHANNEL);
        EXPECT_EQ(output.dimensions[0].size, 2);
        EXPECT_EQ(output.dimensions[1].role, Dimension::Role::FREQUENCY);
        EXPECT_EQ(output.dimensions[1].name, "selected_bins");
        EXPECT_EQ(output.dimensions[1].size, 3);
        EXPECT_EQ(channel_samples(output, 0), (std::vector<double> { 2, 4, 6 }));
        EXPECT_EQ(channel_samples(output, 1), (std::vector<double> { 22, 24, 26 }));
    }

    TEST_F(MathTransformerTest, OffsetAddsConstant)
    {
        MathematicalTransformerConfig cfg;
        cfg.operation = MathematicalOperation::OFFSET;
        cfg.offset_value = 0.5;
        MathematicalTransformer<> transformer(cfg);

        const auto input = ramp_input();
        const auto result = transformer.apply_operation(input);

        const auto out = channel_samples(result, 0);
        const auto in = channel_samples(input, 0);
        for (std::size_t i = 0; i < k_samples; ++i) {
            EXPECT_NEAR(out[i], in[i] + 0.5, k_tolerance) << "Sample " << i;
        }
    }

    TEST_F(MathTransformerTest, PowerRaisesToExponent)
    {
        MathematicalTransformerConfig cfg;
        cfg.operation = MathematicalOperation::POWER;
        cfg.exponent = 3.0;
        MathematicalTransformer<> transformer(cfg);

        const auto input = ramp_input();
        const auto result = transformer.apply_operation(input);

        const auto out = channel_samples(result, 0);
        const auto in = channel_samples(input, 0);
        for (std::size_t i = 0; i < k_samples; ++i) {
            EXPECT_NEAR(out[i], std::pow(in[i], 3.0), k_tolerance) << "Sample " << i;
        }
    }

    TEST_F(MathTransformerTest, LogarithmicDefaultIsNaturalLogOfOnePlusInput)
    {
        MathematicalTransformerConfig cfg;
        cfg.operation = MathematicalOperation::LOGARITHMIC;
        MathematicalTransformer<> transformer(cfg);

        const auto input = to_datum({ sine_samples(k_samples, 440.0, 48000.0, 0.5) });
        const auto result = transformer.apply_operation(input);

        const auto out = channel_samples(result, 0);
        const auto in = channel_samples(input, 0);
        for (std::size_t i = 0; i < k_samples; ++i) {
            const double arg = in[i] + 1.0;
            const double expected = arg > 0.0 ? std::log(arg) : 0.0;
            EXPECT_NEAR(out[i], expected, k_tolerance) << "Sample " << i;
        }
    }

    TEST_F(MathTransformerTest, ExponentialDefaultIsExpOfInput)
    {
        MathematicalTransformerConfig cfg;
        cfg.operation = MathematicalOperation::EXPONENTIAL;
        MathematicalTransformer<> transformer(cfg);

        const auto input = ramp_input();
        const auto result = transformer.apply_operation(input);

        const auto out = channel_samples(result, 0);
        const auto in = channel_samples(input, 0);
        for (std::size_t i = 0; i < k_samples; ++i) {
            EXPECT_NEAR(out[i], std::exp(in[i]), k_tolerance) << "Sample " << i;
        }
    }

    TEST_F(MathTransformerTest, TrigonometricMatchesKinesisSine)
    {
        MathematicalTransformerConfig cfg;
        cfg.operation = MathematicalOperation::TRIGONOMETRIC;
        cfg.trig_function = TrigFunction::SIN;
        cfg.frequency = 2.0;
        cfg.amplitude = 0.7;
        cfg.phase = 0.25;
        MathematicalTransformer<> transformer(cfg);

        const auto input = ramp_input();
        const auto result = transformer.apply_operation(input);

        std::vector<double> expected = channel_samples(input, 0);
        D::apply_trig(expected, [](double x) { return std::sin(x); }, 2.0, 0.7, 0.25);

        const auto out = channel_samples(result, 0);
        for (std::size_t i = 0; i < k_samples; ++i) {
            EXPECT_NEAR(out[i], expected[i], k_tolerance) << "Sample " << i;
        }
    }

    TEST_F(MathTransformerTest, TrigonometricMatchesKinesisCosine)
    {
        MathematicalTransformerConfig cfg;
        cfg.operation = MathematicalOperation::TRIGONOMETRIC;
        cfg.trig_function = TrigFunction::COS;
        MathematicalTransformer<> transformer(cfg);

        const auto input = ramp_input();
        const auto result = transformer.apply_operation(input);

        std::vector<double> expected = channel_samples(input, 0);
        D::apply_trig(expected, [](double x) { return std::cos(x); }, 1.0, 1.0, 0.0);

        const auto out = channel_samples(result, 0);
        for (std::size_t i = 0; i < k_samples; ++i) {
            EXPECT_NEAR(out[i], expected[i], k_tolerance) << "Sample " << i;
        }
    }

    TEST_F(MathTransformerTest, QuantizeRoundsToBitLevels)
    {
        MathematicalTransformerConfig cfg;
        cfg.operation = MathematicalOperation::QUANTIZE;
        cfg.bits = 2;
        MathematicalTransformer<> transformer(cfg);

        const auto result = transformer.apply_operation(to_datum({ { 0.4, 0.9, -0.9 } }));

        const auto out = channel_samples(result, 0);
        ASSERT_EQ(out.size(), 3U);
        EXPECT_NEAR(out[0], 1.0 / 3.0, k_tolerance);
        EXPECT_NEAR(out[1], 1.0, k_tolerance);
        EXPECT_NEAR(out[2], -1.0, k_tolerance);
    }

    TEST_F(MathTransformerTest, NormalizeMapsRangeToTargetPeak)
    {
        MathematicalTransformerConfig cfg;
        cfg.operation = MathematicalOperation::NORMALIZE;
        cfg.target_peak = 0.8;
        MathematicalTransformer<> transformer(cfg);

        const auto input = ramp_input();
        const auto result = transformer.apply_operation(input);

        const auto out = channel_samples(result, 0);
        const auto in = channel_samples(input, 0);
        EXPECT_NEAR(out.front(), -0.8, k_tolerance);
        EXPECT_NEAR(out.back(), 0.8, k_tolerance);
        for (std::size_t i = 0; i < k_samples; ++i) {
            EXPECT_NEAR(out[i], 0.8 * in[i], k_tolerance) << "Sample " << i;
        }
    }

    TEST_F(MathTransformerTest, PolynomialHighestOrderFirst)
    {
        MathematicalTransformerConfig cfg;
        cfg.operation = MathematicalOperation::POLYNOMIAL;
        cfg.coefficients = { 2.0, 1.0 };
        MathematicalTransformer<> transformer(cfg);

        const auto input = ramp_input();
        const auto result = transformer.apply_operation(input);

        const auto out = channel_samples(result, 0);
        const auto in = channel_samples(input, 0);
        for (std::size_t i = 0; i < k_samples; ++i) {
            EXPECT_NEAR(out[i], 2.0 * in[i] + 1.0, 1e-6) << "Sample " << i;
        }
    }

    TEST_F(MathTransformerTest, SetConfigReplacesTheOperation)
    {
        MathematicalTransformerConfig gain;
        gain.operation = MathematicalOperation::GAIN;
        gain.gain_factor = 3.0;
        MathematicalTransformer<> transformer(gain);

        MathematicalTransformerConfig offset;
        offset.operation = MathematicalOperation::OFFSET;
        offset.offset_value = 1.0;
        transformer.set_config(offset);

        const auto input = ramp_input();
        const auto result = transformer.apply_operation(input);

        const auto out = channel_samples(result, 0);
        const auto in = channel_samples(input, 0);
        for (std::size_t i = 0; i < k_samples; ++i) {
            EXPECT_NEAR(out[i], in[i] + 1.0, k_tolerance) << "Sample " << i;
        }
    }

    TEST_F(MathTransformerTest, StrategyDecidesInPlace)
    {
        MathematicalTransformer<> transformer;
        EXPECT_FALSE(transformer.is_in_place()) << "Default strategy is BUFFERED";

        MathematicalTransformerConfig cfg;
        cfg.strategy = TransformationStrategy::IN_PLACE;
        transformer.set_config(cfg);
        EXPECT_TRUE(transformer.is_in_place());
    }

    TEST_F(MathTransformerTest, ConfigReadBackKeepsGainFactor)
    {
        MathematicalTransformerConfig cfg;
        cfg.gain_factor = 2.5;
        MathematicalTransformer<> transformer(cfg);

        EXPECT_EQ(transformer.config<MathematicalTransformerConfig>().gain_factor, 2.5);
    }

}
}
