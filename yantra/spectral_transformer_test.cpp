#include "MayaFlux/Kinesis/Discrete/Spectral.hpp"
#include "MayaFlux/Yantra/Transformers/SpectralTransformer.hpp"
#include "transformer_helpers.hpp"

#include <gtest/gtest.h>

namespace MayaFlux::Test {
namespace {

    using Yantra::SpectralOperation;
    using Yantra::SpectralTransformer;
    using Yantra::SpectralTransformerConfig;
    namespace D = Kinesis::Discrete;

    constexpr std::size_t k_samples = 4096;
    constexpr double k_sample_rate = 48000.0;

    const char* operation_name(SpectralOperation op)
    {
        switch (op) {
        case SpectralOperation::FREQUENCY_SHIFT:
            return "FrequencyShift";
        case SpectralOperation::PITCH_SHIFT:
            return "PitchShift";
        case SpectralOperation::SPECTRAL_FILTER:
            return "SpectralFilter";
        case SpectralOperation::HARMONIC_ENHANCE:
            return "HarmonicEnhance";
        case SpectralOperation::SPECTRAL_GATE:
            return "SpectralGate";
        }
        return "Unknown";
    }

    SpectralTransformerConfig config_for(SpectralOperation op)
    {
        SpectralTransformerConfig cfg;
        cfg.operation = op;
        cfg.shift_hz = 200.0;
        cfg.pitch_ratio = 2.0;
        cfg.low_freq = 200.0;
        cfg.high_freq = 4000.0;
        cfg.enhancement_factor = 2.0;
        cfg.threshold = -30.0;
        cfg.sample_rate = k_sample_rate;
        return cfg;
    }

    std::vector<double> kinesis_expectation(const SpectralTransformerConfig& cfg, std::span<const double> src)
    {
        const auto window = cfg.window_size;
        const auto hop = cfg.hop_size;
        switch (static_cast<SpectralOperation>(cfg.operation)) {
        case SpectralOperation::FREQUENCY_SHIFT: {
            const double lo = std::max(0.0, cfg.shift_hz);
            const double hi = cfg.sample_rate / 2.0 + cfg.shift_hz;
            return D::spectral_filter(src, lo, hi, cfg.sample_rate, window, hop);
        }
        case SpectralOperation::PITCH_SHIFT:
            return D::pitch_shift(src, 12.0 * std::log2(cfg.pitch_ratio), window, hop);
        case SpectralOperation::SPECTRAL_FILTER:
            return D::spectral_filter(src, cfg.low_freq, cfg.high_freq, cfg.sample_rate, window, hop);
        case SpectralOperation::HARMONIC_ENHANCE:
            return D::harmonic_enhance(src, cfg.enhancement_factor, window, hop);
        case SpectralOperation::SPECTRAL_GATE:
            return D::spectral_gate(src, cfg.threshold, window, hop);
        }
        return {};
    }

    Yantra::Datum<std::vector<Kakshya::DataVariant>> two_channel_input()
    {
        return to_datum({ sine_samples(k_samples, 440.0, k_sample_rate, 0.5), sine_samples(k_samples, 3000.0, k_sample_rate, 0.25) });
    }

    class SpectralOperationTest : public ::testing::TestWithParam<SpectralOperation> {
    };

    INSTANTIATE_TEST_SUITE_P(Operations, SpectralOperationTest,
        ::testing::Values(
            SpectralOperation::FREQUENCY_SHIFT,
            SpectralOperation::PITCH_SHIFT,
            SpectralOperation::SPECTRAL_FILTER,
            SpectralOperation::HARMONIC_ENHANCE,
            SpectralOperation::SPECTRAL_GATE),
        [](const ::testing::TestParamInfo<SpectralOperation>& info) {
            return std::string(operation_name(info.param));
        });

    TEST_P(SpectralOperationTest, PreservesChannelCountAndSampleCount)
    {
        SpectralTransformer<> transformer(config_for(GetParam()));

        const auto result = transformer.apply_operation(two_channel_input());

        ASSERT_EQ(result.data.size(), 2U);
        EXPECT_EQ(channel_samples(result, 0).size(), k_samples);
        EXPECT_EQ(channel_samples(result, 1).size(), k_samples);
    }

    TEST_P(SpectralOperationTest, EveryOutputSampleIsFinite)
    {
        SpectralTransformer<> transformer(config_for(GetParam()));

        const auto result = transformer.apply_operation(two_channel_input());

        for (std::size_t ch = 0; ch < 2; ++ch) {
            for (double sample : channel_samples(result, ch)) {
                EXPECT_TRUE(std::isfinite(sample)) << "Channel " << ch;
            }
        }
    }

    TEST_P(SpectralOperationTest, MatchesTheKinesisCallWithItsSettings)
    {
        const auto cfg = config_for(GetParam());
        SpectralTransformer<> transformer(cfg);

        const auto input = two_channel_input();
        const auto result = transformer.apply_operation(input);

        for (std::size_t ch = 0; ch < 2; ++ch) {
            const auto expected = kinesis_expectation(cfg, channel_samples(input, ch));
            const auto out = channel_samples(result, ch);
            ASSERT_EQ(out.size(), expected.size()) << "Channel " << ch;
            for (std::size_t i = 0; i < expected.size(); ++i) {
                EXPECT_NEAR(out[i], expected[i], 1e-9) << "Channel " << ch << " sample " << i;
            }
        }
    }

    class SpectralTransformerTest : public ::testing::Test {
    };

    TEST_F(SpectralTransformerTest, DefaultsAreTheStandardWindow)
    {
        SpectralTransformer<> transformer;

        EXPECT_EQ(transformer.config<SpectralTransformerConfig>().window_size, 1024U);
        EXPECT_EQ(transformer.config<SpectralTransformerConfig>().hop_size, 512U);
    }

    TEST_F(SpectralTransformerTest, SetConfigReplacesTheOperation)
    {
        SpectralTransformerConfig gate;
        gate.operation = SpectralOperation::SPECTRAL_GATE;
        gate.threshold = -30.0;
        SpectralTransformer<> transformer(gate);

        SpectralTransformerConfig filter;
        filter.operation = SpectralOperation::SPECTRAL_FILTER;
        filter.low_freq = 200.0;
        filter.high_freq = 4000.0;
        filter.sample_rate = k_sample_rate;
        transformer.set_config(filter);

        const auto input = two_channel_input();
        const auto result = transformer.apply_operation(input);

        const auto expected = D::spectral_filter(channel_samples(input, 0), 200.0, 4000.0, k_sample_rate, 1024, 512);
        const auto out = channel_samples(result, 0);
        ASSERT_EQ(out.size(), expected.size());
        for (std::size_t i = 0; i < expected.size(); ++i) {
            EXPECT_NEAR(out[i], expected[i], 1e-9) << "Sample " << i;
        }
    }

    TEST_F(SpectralTransformerTest, ConfigReadBackKeepsPitchRatio)
    {
        SpectralTransformerConfig cfg;
        cfg.pitch_ratio = 1.5;
        SpectralTransformer<> transformer(cfg);

        EXPECT_EQ(transformer.config<SpectralTransformerConfig>().pitch_ratio, 1.5);
    }

}
}
