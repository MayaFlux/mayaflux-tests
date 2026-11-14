#include "MayaFlux/Kinesis/Discrete/Transform.hpp"
#include "MayaFlux/Yantra/Transformers/TemporalTransformer.hpp"
#include "transformer_helpers.hpp"

#include <gtest/gtest.h>
#include "../support/test_metadata.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {
namespace {

    using Yantra::TemporalOperation;
    using Yantra::TemporalTransformer;
    using Yantra::TemporalTransformerConfig;
    namespace D = Kinesis::Discrete;

    constexpr std::size_t k_samples = 256;
    constexpr double k_tolerance = 1e-9;

    const char* operation_name(TemporalOperation op)
    {
        switch (op) {
        case TemporalOperation::TIME_REVERSE:
            return "TimeReverse";
        case TemporalOperation::TIME_STRETCH:
            return "TimeStretch";
        case TemporalOperation::DELAY:
            return "Delay";
        case TemporalOperation::FADE_IN_OUT:
            return "FadeInOut";
        case TemporalOperation::SLICE:
            return "Slice";
        case TemporalOperation::INTERPOLATE:
            return "Interpolate";
        }
        return "Unknown";
    }

    TemporalTransformerConfig config_for(TemporalOperation op)
    {
        TemporalTransformerConfig cfg;
        cfg.operation = op;
        cfg.delay_samples = 16;
        cfg.target_size = 128;
        return cfg;
    }

    Yantra::Datum<std::vector<Kakshya::DataVariant>> two_channel_input()
    {
        return to_datum({ sine_samples(k_samples, 440.0), ramp_samples(k_samples) });
    }

    class TemporalOperationTest : public ::testing::TestWithParam<TemporalOperation> {
    };

    INSTANTIATE_TEST_SUITE_P(Operations, TemporalOperationTest,
        ::testing::Values(
            TemporalOperation::TIME_REVERSE,
            TemporalOperation::TIME_STRETCH,
            TemporalOperation::DELAY,
            TemporalOperation::FADE_IN_OUT,
            TemporalOperation::SLICE,
            TemporalOperation::INTERPOLATE),
        [](const ::testing::TestParamInfo<TemporalOperation>& info) {
            return std::string(operation_name(info.param));
        });

    TEST_P(TemporalOperationTest, EveryOutputSampleIsFinite)
    {
        TemporalTransformer<> transformer(config_for(GetParam()));

        const auto result = transformer.apply_operation(two_channel_input());

        ASSERT_EQ(result.data.size(), 2U);
        for (std::size_t ch = 0; ch < 2; ++ch) {
            for (double sample : channel_samples(result, ch)) {
                EXPECT_TRUE(std::isfinite(sample)) << "Channel " << ch;
            }
        }
    }

    TEST_P(TemporalOperationTest, ConfigIsReadBackAsSet)
    {
        TemporalTransformer<> transformer(config_for(GetParam()));

        EXPECT_EQ(static_cast<TemporalOperation>(transformer.config<TemporalTransformerConfig>().operation), GetParam());
    }

    class TemporalTransformerTest : public ::testing::Test {
    };

    TEST_F(TemporalTransformerTest, DefaultReversesTheSignal)
    {
        TemporalTransformer<> transformer;

        const auto input = to_datum({ ramp_samples(k_samples) });
        const auto result = transformer.apply_operation(input);

        const auto out = channel_samples(result, 0);
        const auto in = channel_samples(input, 0);
        for (std::size_t i = 0; i < k_samples; ++i) {
            EXPECT_NEAR(out[i], in[k_samples - 1 - i], k_tolerance) << "Sample " << i;
        }
    }

    TEST_F(TemporalTransformerTest, DelayPrependsFillValue)
    {
        TemporalTransformerConfig cfg;
        cfg.operation = TemporalOperation::DELAY;
        cfg.delay_samples = 3;
        cfg.fill_value = 9.0;
        TemporalTransformer<> transformer(cfg);

        const auto input = to_datum({ { 1.0, 2.0, 3.0, 4.0 } });
        const auto result = transformer.apply_operation(input);

        const auto out = channel_samples(result, 0);
        const std::vector<double> expected { 9.0, 9.0, 9.0, 1.0, 2.0, 3.0, 4.0 };
        EXPECT_EQ(out, expected);
    }

    TEST_F(TemporalTransformerTest, SliceKeepsTheRatioWindow)
    {
        TemporalTransformerConfig cfg;
        cfg.operation = TemporalOperation::SLICE;
        cfg.start_ratio = 0.25;
        cfg.end_ratio = 0.75;
        TemporalTransformer<> transformer(cfg);

        const auto input = to_datum({ { 0.0, 1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0 } });
        const auto result = transformer.apply_operation(input);

        const auto out = channel_samples(result, 0);
        const std::vector<double> expected { 2.0, 3.0, 4.0, 5.0 };
        EXPECT_EQ(out, expected);
    }

    TEST_F(TemporalTransformerTest, SliceResizesTimeWithoutReplacingExplicitAxes)
    {
        using Dimension = Kakshya::DataDimension;
        Yantra::DataIO input(std::vector<Kakshya::DataVariant> {
                                std::vector<double> { 0, 1, 2, 3, 4, 5, 6, 7 },
                                std::vector<double> { 10, 11, 12, 13, 14, 15, 16, 17 } },
            { Dimension::time(8, "source_frames"), Dimension::channel(2) },
            Kakshya::DataModality::AUDIO_MULTICHANNEL);
        TemporalTransformerConfig config;
        config.operation = TemporalOperation::SLICE;
        config.start_ratio = 0.25;
        config.end_ratio = 0.75;
        TemporalTransformer<> transformer(config);
        const auto output = transformer.apply_operation(input);
        ASSERT_EQ(output.dimensions.size(), 2);
        EXPECT_EQ(output.dimensions[0].role, Dimension::Role::TIME);
        EXPECT_EQ(output.dimensions[0].name, "source_frames");
        EXPECT_EQ(output.dimensions[0].size, 4);
        EXPECT_EQ(output.dimensions[1].role, Dimension::Role::CHANNEL);
        EXPECT_EQ(output.dimensions[1].size, 2);
        EXPECT_EQ(output.get_total_elements(), 8);
        EXPECT_EQ(channel_samples(output, 0), (std::vector<double> { 2, 3, 4, 5 }));
        EXPECT_EQ(channel_samples(output, 1), (std::vector<double> { 12, 13, 14, 15 }));
        EXPECT_EQ(input.dimensions[0].size, 8);
        EXPECT_EQ(channel_samples(input, 0), (std::vector<double> { 0, 1, 2, 3, 4, 5, 6, 7 }));
    }

    TEST_F(TemporalTransformerTest, InterpolateToZeroLengthLeavesInputUnchanged)
    {
        TemporalTransformerConfig cfg;
        cfg.operation = TemporalOperation::INTERPOLATE;
        cfg.target_size = 0;
        TemporalTransformer<> transformer(cfg);

        const auto input = to_datum({ ramp_samples(k_samples) });
        const auto result = transformer.apply_operation(input);

        EXPECT_EQ(channel_samples(result, 0), channel_samples(input, 0));
    }

    TEST_F(TemporalTransformerTest, LinearInterpolateMatchesKinesis)
    {
        TemporalTransformerConfig cfg;
        cfg.operation = TemporalOperation::INTERPOLATE;
        cfg.target_size = 512;
        cfg.use_cubic = false;
        TemporalTransformer<> transformer(cfg);

        const auto input = to_datum({ ramp_samples(k_samples) });
        const auto result = transformer.apply_operation(input);

        std::vector<double> expected(512);
        const auto src = channel_samples(input, 0);
        D::interpolate_linear(src, expected);

        const auto out = channel_samples(result, 0);
        ASSERT_EQ(out.size(), 512U);
        for (std::size_t i = 0; i < expected.size(); ++i) {
            EXPECT_NEAR(out[i], expected[i], k_tolerance) << "Sample " << i;
        }
    }

    TEST_F(TemporalTransformerTest, CubicInterpolateMatchesKinesis)
    {
        TemporalTransformerConfig cfg;
        cfg.operation = TemporalOperation::INTERPOLATE;
        cfg.target_size = 512;
        cfg.use_cubic = true;
        TemporalTransformer<> transformer(cfg);

        const auto input = to_datum({ sine_samples(k_samples, 220.0) });
        const auto result = transformer.apply_operation(input);

        std::vector<double> expected(512);
        const auto src = channel_samples(input, 0);
        D::interpolate_cubic(src, expected);

        const auto out = channel_samples(result, 0);
        ASSERT_EQ(out.size(), 512U);
        for (std::size_t i = 0; i < expected.size(); ++i) {
            EXPECT_NEAR(out[i], expected[i], k_tolerance) << "Sample " << i;
        }
    }

    TEST_F(TemporalTransformerTest, TimeStretchMatchesKinesis)
    {
        TemporalTransformerConfig cfg;
        cfg.operation = TemporalOperation::TIME_STRETCH;
        cfg.stretch_factor = 2.0;
        TemporalTransformer<> transformer(cfg);

        const auto input = to_datum({ sine_samples(k_samples, 440.0) });
        const auto result = transformer.apply_operation(input);

        const auto expected = D::time_stretch(channel_samples(input, 0), 2.0);
        const auto out = channel_samples(result, 0);
        ASSERT_EQ(out.size(), expected.size());
        for (std::size_t i = 0; i < expected.size(); ++i) {
            EXPECT_NEAR(out[i], expected[i], k_tolerance) << "Sample " << i;
        }
    }

    TEST_F(TemporalTransformerTest, FadeMatchesKinesis)
    {
        TemporalTransformerConfig cfg;
        cfg.operation = TemporalOperation::FADE_IN_OUT;
        cfg.fade_in_ratio = 0.2;
        cfg.fade_out_ratio = 0.1;
        TemporalTransformer<> transformer(cfg);

        const auto input = to_datum({ sine_samples(k_samples, 440.0) });
        const auto result = transformer.apply_operation(input);

        std::vector<double> expected = channel_samples(input, 0);
        D::fade(expected, 0.2, 0.1);

        const auto out = channel_samples(result, 0);
        for (std::size_t i = 0; i < k_samples; ++i) {
            EXPECT_NEAR(out[i], expected[i], k_tolerance) << "Sample " << i;
        }
    }

    TEST_F(TemporalTransformerTest, SetConfigReplacesTheOperation)
    {
        TemporalTransformer<> transformer;

        TemporalTransformerConfig cfg;
        cfg.operation = TemporalOperation::DELAY;
        cfg.delay_samples = 2;
        cfg.fill_value = 0.0;
        transformer.set_config(cfg);

        const auto input = to_datum({ { 1.0, 2.0 } });
        const auto result = transformer.apply_operation(input);

        const std::vector<double> expected { 0.0, 0.0, 1.0, 2.0 };
        EXPECT_EQ(channel_samples(result, 0), expected);
    }

    TEST_F(TemporalTransformerTest, ConfigReadBackKeepsDelay)
    {
        TemporalTransformerConfig cfg;
        cfg.delay_samples = 77;
        TemporalTransformer<> transformer(cfg);

        EXPECT_EQ(transformer.config<TemporalTransformerConfig>().delay_samples, 77U);
    }

}
}
