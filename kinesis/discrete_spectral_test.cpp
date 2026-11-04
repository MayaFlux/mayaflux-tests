#include "gtest/gtest.h"

#include "MayaFlux/Kinesis/Discrete/Spectral.hpp"

#include <cmath>
#include <complex>
#include <numbers>
#include <vector>

namespace MayaFlux::Test {
namespace {

    namespace D = Kinesis::Discrete;

    constexpr double k_sample_rate = 8000.0;
    constexpr double k_tol = 1e-6;

    std::vector<double> sine(size_t n, double freq_hz, double amplitude = 1.0)
    {
        std::vector<double> data(n);
        for (size_t i = 0; i < n; ++i) {
            data[i] = amplitude * std::sin(2.0 * std::numbers::pi * freq_hz * static_cast<double>(i) / k_sample_rate);
        }
        return data;
    }

    double rms(const std::vector<double>& data, size_t from, size_t to)
    {
        double sum = 0.0;
        for (size_t i = from; i < to; ++i) {
            sum += data[i] * data[i];
        }
        return std::sqrt(sum / static_cast<double>(to - from));
    }

    size_t zero_crossings(const std::vector<double>& data)
    {
        size_t count = 0;
        for (size_t i = 1; i < data.size(); ++i) {
            if ((data[i - 1] < 0.0) != (data[i] < 0.0)) {
                ++count;
            }
        }
        return count;
    }

    TEST(KinesisSpectralEngineTest, ProcessorRunsOncePerFrameWithIndexInOrder)
    {
        constexpr size_t n = 512;
        constexpr uint32_t window = 64;
        constexpr uint32_t hop = 32;
        const auto src = sine(n, 1000.0);

        std::vector<size_t> seen;
        (void)D::apply_spectral(src, window, hop,
            [&seen](std::vector<std::complex<double>>&, size_t f) { seen.push_back(f); });

        const size_t expected_frames = (n - window) / hop + 1;
        ASSERT_EQ(seen.size(), expected_frames);
        for (size_t f = 0; f < seen.size(); ++f) {
            EXPECT_EQ(seen[f], f);
        }
    }

    TEST(KinesisSpectralEngineTest, ShortInputGetsOneFrameAndKeepsItsLength)
    {
        const auto src = sine(10, 1000.0);
        size_t calls = 0;
        const auto out = D::apply_spectral(src, 64, 16,
            [&calls](std::vector<std::complex<double>>&, size_t) { ++calls; });

        EXPECT_EQ(calls, 1U);
        EXPECT_EQ(out.size(), src.size());
    }

    TEST(KinesisSpectralEngineTest, EmptyInputGivesEmptyOutput)
    {
        const auto out = D::apply_spectral(std::span<const double> {}, 64, 16,
            [](std::vector<std::complex<double>>&, size_t) { });
        EXPECT_TRUE(out.empty());
    }

    TEST(KinesisSpectralEngineTest, IdentityProcessorReconstructsTheInterior)
    {
        constexpr uint32_t window = 64;
        const auto src = sine(512, 440.0, 0.8);

        const auto out = D::apply_spectral(src, window, 16,
            [](std::vector<std::complex<double>>&, size_t) { });

        for (size_t i = window; i + window < src.size(); ++i) {
            EXPECT_NEAR(out[i], src[i], k_tol) << "index " << i;
        }
    }

    TEST(KinesisSpectralEngineTest, GateAboveEverythingSilencesOutput)
    {
        const auto src = sine(512, 440.0);
        const auto out = D::spectral_gate(src, 300.0, 64, 16);

        for (double v : out) {
            EXPECT_NEAR(v, 0.0, k_tol);
        }
    }

    TEST(KinesisSpectralTransformTest, FilterRemovesToneOutsideBand)
    {
        constexpr size_t n = 4096;
        constexpr uint32_t window = 512;
        const auto src = sine(n, 1000.0);

        const auto out = D::spectral_filter(src, 2000.0, 4000.0, k_sample_rate, window, 128);

        const double in_rms = rms(src, window, n - window);
        const double out_rms = rms(out, window, n - window);
        EXPECT_LT(out_rms, 0.05 * in_rms);
    }

    TEST(KinesisSpectralTransformTest, FilterKeepsToneInsideBand)
    {
        constexpr size_t n = 4096;
        constexpr uint32_t window = 512;
        const auto src = sine(n, 1000.0);

        const auto out = D::spectral_filter(src, 500.0, 1500.0, k_sample_rate, window, 128);

        const double in_rms = rms(src, window, n - window);
        const double out_rms = rms(out, window, n - window);
        EXPECT_NEAR(out_rms / in_rms, 1.0, 0.05);
    }

    TEST(KinesisSpectralTransformTest, InvertKeepsToneLevel)
    {
        const auto src = sine(4096, 440.0, 0.5);
        const auto out = D::spectral_invert(src, 512, 128);

        const double in_rms = rms(src, 512, src.size() - 512);
        const double out_rms = rms(out, 512, src.size() - 512);
        EXPECT_NEAR(out_rms / in_rms, 1.0, 0.05);
    }

    TEST(KinesisSpectralTransformTest, UnitEnhancementIsIdentityInTheInterior)
    {
        const auto src = sine(512, 440.0, 0.5);
        const auto out = D::harmonic_enhance(src, 1.0, 64, 16);

        for (size_t i = 64; i + 64 < src.size(); ++i) {
            EXPECT_NEAR(out[i], src[i], k_tol) << "index " << i;
        }
    }

    TEST(KinesisSpectralPhaseVocoderTest, UnitStretchIsCopy)
    {
        const auto src = sine(2048, 300.0);
        const auto out = D::phase_vocoder_stretch(src, 1.0, 256, 64);
        EXPECT_EQ(out, src);
    }

    TEST(KinesisSpectralPhaseVocoderTest, OutputLengthFollowsStretchFactor)
    {
        const auto src = sine(2048, 300.0);
        EXPECT_EQ(D::phase_vocoder_stretch(src, 2.0, 256, 64).size(), 4096U);
        EXPECT_EQ(D::phase_vocoder_stretch(src, 0.5, 256, 64).size(), 1024U);
        EXPECT_TRUE(D::phase_vocoder_stretch(src, 0.0, 256, 64).empty());
    }

    TEST(KinesisSpectralPhaseVocoderTest, StretchKeepsConstantSignalConstant)
    {
        constexpr size_t n = 2048;
        constexpr uint32_t window = 256;
        const std::vector<double> src(n, 0.5);

        const auto out = D::phase_vocoder_stretch(src, 2.0, window, 64);

        for (size_t i = window; i + 2 * static_cast<size_t>(window) < out.size(); ++i) {
            EXPECT_NEAR(out[i], 0.5, 1e-3) << "index " << i;
        }
    }

    TEST(KinesisSpectralPhaseVocoderTest, PitchShiftZeroIsCopy)
    {
        const auto src = sine(2048, 300.0);
        EXPECT_EQ(D::pitch_shift(src, 0.0, 256, 64), src);
    }

    TEST(KinesisSpectralPhaseVocoderTest, PitchShiftOctaveDoublesFrequency)
    {
        const auto src = sine(8000, 100.0);
        const auto out = D::pitch_shift(src, 12.0, 512, 128);

        ASSERT_EQ(out.size(), src.size());
        const double ratio = static_cast<double>(zero_crossings(out))
            / static_cast<double>(zero_crossings(src));
        EXPECT_NEAR(ratio, 2.0, 0.1);
    }

} // namespace
} // namespace MayaFlux::Test
