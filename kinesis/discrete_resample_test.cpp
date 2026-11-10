#include "gtest/gtest.h"

#include "MayaFlux/Kinesis/Discrete/Resample.hpp"

#include <random>

namespace MayaFlux::Test {
namespace {

    namespace D = Kinesis::Discrete;

    double bessel_i0(double x)
    {
        double term = 1.0;
        double sum = 1.0;
        for (int k = 1; k < 200; ++k) {
            term *= (x * x / 4.0) / (static_cast<double>(k) * static_cast<double>(k));
            sum += term;
        }
        return sum;
    }

    std::vector<double> kaiser_sinc_lowpass(uint32_t up, uint32_t down, const D::ResampleConfig& config)
    {
        const uint32_t wider = std::max(up, down);
        const size_t half = static_cast<size_t>(config.zero_crossings) * wider;
        const size_t taps = 2 * half + 1;
        const double cutoff = 1.0 / static_cast<double>(wider);

        std::vector<double> h(taps);
        double total = 0.0;
        for (size_t n = 0; n < taps; ++n) {
            const double offset = static_cast<double>(n) - static_cast<double>(half);
            const double x = cutoff * offset;
            const double sinc = offset == 0.0 ? 1.0 : std::sin(std::numbers::pi * x) / (std::numbers::pi * x);
            const double r = 2.0 * static_cast<double>(n) / static_cast<double>(taps - 1) - 1.0;
            const double window = bessel_i0(config.kaiser_beta * std::sqrt(1.0 - r * r)) / bessel_i0(config.kaiser_beta);
            h[n] = sinc * window;
            total += h[n];
        }
        for (double& tap : h) {
            tap *= static_cast<double>(up) / total;
        }
        return h;
    }

    /**
     * Output sample k is the filtered upsampled signal at position k * down, with the
     * filter delay of half taps removed. Each input sample j sits at j * up, so the
     * sum runs over every input sample whose tap lies inside the filter.
     */
    std::vector<double> direct_sum(std::span<const double> src, uint32_t from, uint32_t to, const D::ResampleConfig& config)
    {
        const uint32_t divisor = std::gcd(from, to);
        const uint32_t up = to / divisor;
        const uint32_t down = from / divisor;
        const auto h = kaiser_sinc_lowpass(up, down, config);
        const long half = static_cast<long>((h.size() - 1) / 2);

        std::vector<double> out(D::resampled_length(src.size(), from, to), 0.0);
        for (size_t k = 0; k < out.size(); ++k) {
            const long centre = static_cast<long>(k) * down + half;
            double acc = 0.0;
            for (size_t j = 0; j < src.size(); ++j) {
                const long tap = centre - static_cast<long>(j) * up;
                if (tap >= 0 && tap < static_cast<long>(h.size())) {
                    acc += h[static_cast<size_t>(tap)] * src[j];
                }
            }
            out[k] = acc;
        }
        return out;
    }

    std::vector<double> noise(size_t count, uint32_t seed)
    {
        std::mt19937 engine(seed);
        std::uniform_real_distribution<double> dist(-1.0, 1.0);
        std::vector<double> out(count);
        for (double& v : out) {
            v = dist(engine);
        }
        return out;
    }

    std::vector<double> tone(double frequency, double rate, size_t count)
    {
        std::vector<double> out(count);
        for (size_t i = 0; i < count; ++i) {
            out[i] = std::sin(2.0 * std::numbers::pi * frequency * static_cast<double>(i) / rate);
        }
        return out;
    }

    double rms_inside(std::span<const double> values, size_t margin)
    {
        double sum = 0.0;
        for (size_t i = margin; i < values.size() - margin; ++i) {
            sum += values[i] * values[i];
        }
        return std::sqrt(sum / static_cast<double>(values.size() - 2 * margin));
    }

    struct Ratio {
        const char* name;
        uint32_t from;
        uint32_t to;
        size_t count;
        D::ResampleConfig config;
    };

    class DiscreteResampleRatioTest : public ::testing::TestWithParam<Ratio> { };

    TEST_P(DiscreteResampleRatioTest, MatchesTheDirectSumOverEveryInputSample)
    {
        const auto& p = GetParam();
        const auto input = noise(p.count, 5);

        const auto got = D::resample(input, p.from, p.to, p.config);
        const auto want = direct_sum(input, p.from, p.to, p.config);

        ASSERT_EQ(got.size(), want.size());
        for (size_t i = 0; i < got.size(); ++i) {
            ASSERT_NEAR(got[i], want[i], 1e-12) << i;
        }
    }

    TEST_P(DiscreteResampleRatioTest, TheLengthIsTheCeilingOfTheRatio)
    {
        const auto& p = GetParam();
        const auto input = noise(p.count, 6);

        const uint64_t scaled = static_cast<uint64_t>(p.count) * p.to;
        const size_t ceiling = static_cast<size_t>((scaled + p.from - 1) / p.from);

        EXPECT_EQ(D::resampled_length(p.count, p.from, p.to), ceiling);
        EXPECT_EQ(D::resample(input, p.from, p.to, p.config).size(), ceiling);
    }

    INSTANTIATE_TEST_SUITE_P(Ratios, DiscreteResampleRatioTest,
        ::testing::Values(
            Ratio { "decimate_three", 48000, 16000, 1500, {} },
            Ratio { "decimate_three_across_blocks", 48000, 16000, 9000, {} },
            Ratio { "interpolate_three", 16000, 48000, 500, {} },
            Ratio { "double", 22050, 44100, 600, {} },
            Ratio { "half", 44100, 22050, 700, {} },
            Ratio { "cd_to_speech", 44100, 16000, 1200, {} },
            Ratio { "studio_to_cd", 48000, 44100, 1500, {} },
            Ratio { "cd_to_studio_wide_soft", 44100, 48000, 900, { 20, 3.0 } },
            Ratio { "narrow_hard", 48000, 16000, 800, { 4, 8.0 } },
            Ratio { "short_input", 48000, 16000, 5, {} },
            Ratio { "one_sample", 48000, 16000, 1, {} }),
        [](const ::testing::TestParamInfo<Ratio>& info) { return std::string(info.param.name); });

    struct Tone {
        const char* name;
        uint32_t from;
        uint32_t to;
        double frequency;
        double gain_low;
        double gain_high;
    };

    class DiscreteResampleToneTest : public ::testing::TestWithParam<Tone> { };

    TEST_P(DiscreteResampleToneTest, ALevelIsKeptInTheBandAndRemovedOutsideIt)
    {
        const auto& p = GetParam();
        const auto input = tone(p.frequency, p.from, 8000);

        const auto out = D::resample(input, p.from, p.to);
        const double gain = rms_inside(out, 200) / (std::numbers::sqrt2 / 2.0);

        EXPECT_GE(gain, p.gain_low);
        EXPECT_LE(gain, p.gain_high);
    }

    INSTANTIATE_TEST_SUITE_P(Response, DiscreteResampleToneTest,
        ::testing::Values(
            Tone { "decimate_500", 48000, 16000, 500.0, 0.99, 1.01 },
            Tone { "decimate_2000", 48000, 16000, 2000.0, 0.99, 1.01 },
            Tone { "decimate_5000", 48000, 16000, 5000.0, 0.99, 1.01 },
            Tone { "decimate_12000_removed", 48000, 16000, 12000.0, 0.0, 0.01 },
            Tone { "decimate_20000_removed", 48000, 16000, 20000.0, 0.0, 0.01 },
            Tone { "interpolate_1000", 16000, 48000, 1000.0, 0.99, 1.01 },
            Tone { "interpolate_6000_without_images", 16000, 48000, 6000.0, 0.99, 1.01 },
            Tone { "cd_to_speech_3000", 44100, 16000, 3000.0, 0.99, 1.01 },
            Tone { "cd_to_speech_11000_removed", 44100, 16000, 11000.0, 0.0, 0.01 }),
        [](const ::testing::TestParamInfo<Tone>& info) { return std::string(info.param.name); });

    TEST(DiscreteResampleTest, EqualRatesReturnACopy)
    {
        const std::vector<double> input { 0.1, -0.2, 0.3, 0.4 };

        EXPECT_EQ(D::resample(input, 48000, 48000), input);
    }

    TEST(DiscreteResampleTest, EmptyInputAndZeroRatesGiveNothing)
    {
        const std::vector<double> input(100, 1.0);

        EXPECT_TRUE(D::resample({}, 48000, 16000).empty());
        EXPECT_TRUE(D::resample(input, 0, 16000).empty());
        EXPECT_TRUE(D::resample(input, 48000, 0).empty());
        EXPECT_EQ(D::resampled_length(100, 0, 16000), 0U);
        EXPECT_EQ(D::resampled_length(100, 16000, 0), 0U);
        EXPECT_EQ(D::resampled_length(0, 48000, 16000), 0U);
    }

    TEST(DiscreteResampleTest, AnImpulseKeepsItsPositionInTime)
    {
        std::vector<double> input(300, 0.0);
        input[30] = 1.0;

        const auto out = D::resample(input, 48000, 16000);

        const auto peak = std::ranges::max_element(out, [](double a, double b) { return std::abs(a) < std::abs(b); });
        EXPECT_EQ(std::distance(out.begin(), peak), 10);
    }

    TEST(DiscreteResampleTest, ADirectCurrentLevelPassesUnchangedAwayFromTheEdges)
    {
        const std::vector<double> input(3000, 0.75);

        const auto down = D::resample(input, 48000, 16000);
        const auto up = D::resample(input, 16000, 48000);

        for (size_t i = 100; i < down.size() - 100; ++i) {
            ASSERT_NEAR(down[i], 0.75, 1e-9) << i;
        }
        for (size_t i = 400; i < up.size() - 400; ++i) {
            ASSERT_NEAR(up[i], 0.75, 1e-2) << i;
        }
    }

    TEST(DiscreteResampleTest, ADeeperWindowRemovesMoreOfAToneAboveTheNewNyquist)
    {
        const auto input = tone(12000.0, 48000.0, 6000);
        D::ResampleConfig deep;
        deep.kaiser_beta = 9.0;

        const auto shallow_out = D::resample(input, 48000, 16000);
        const auto deep_out = D::resample(input, 48000, 16000, deep);

        EXPECT_LT(rms_inside(deep_out, 100), rms_inside(shallow_out, 100));
    }

    TEST(DiscreteResampleTest, MoreZeroCrossingsNarrowTheTransitionBand)
    {
        const auto input = tone(8600.0, 48000.0, 6000);
        D::ResampleConfig sharp;
        sharp.zero_crossings = 30;

        const auto soft_out = D::resample(input, 48000, 16000);
        const auto sharp_out = D::resample(input, 48000, 16000, sharp);

        EXPECT_LT(rms_inside(sharp_out, 200), rms_inside(soft_out, 200));
    }

    TEST(DiscreteResampleTest, ResamplingThereAndBackRecoversABandLimitedSignal)
    {
        const auto input = tone(1500.0, 48000.0, 6000);

        const auto there = D::resample(input, 48000, 16000);
        const auto back = D::resample(there, 16000, 48000);

        ASSERT_EQ(back.size(), input.size());
        for (size_t i = 600; i < input.size() - 600; ++i) {
            ASSERT_NEAR(back[i], input[i], 1e-2) << i;
        }
    }

}
}
