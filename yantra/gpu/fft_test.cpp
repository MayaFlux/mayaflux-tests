#include "gtest/gtest.h"

#include <random>

#include "../../support/gpu_dispatch_test_support.hpp"

#include "MayaFlux/Kinesis/Discrete/Spectral.hpp"
#include "MayaFlux/Kinesis/Discrete/Taper.hpp"
#include "MayaFlux/Yantra/Executors/Audio/FftContext.hpp"

namespace MayaFlux::Test {
namespace {

    using namespace GpuDispatch;
    using Portal::Graphics::SpectrumOutput;
    using Yantra::FftContext;
    using Yantra::SegmentGeometry;

    using Complex = std::complex<long double>;

    std::vector<Complex> naive_dft(const std::vector<Complex>& input, bool inverse)
    {
        const size_t n = input.size();
        const long double sign = inverse ? 1.0L : -1.0L;
        std::vector<Complex> out(n);
        for (size_t k = 0; k < n; ++k) {
            Complex sum { 0.0L, 0.0L };
            for (size_t t = 0; t < n; ++t) {
                const long double angle = sign * 2.0L * std::numbers::pi_v<long double> * static_cast<long double>((k * t) % n) / static_cast<long double>(n);
                sum += input[t] * Complex { std::cos(angle), std::sin(angle) };
            }
            out[k] = inverse ? sum / static_cast<long double>(n) : sum;
        }
        return out;
    }

    std::vector<double> random_signal(size_t count, uint64_t seed)
    {
        std::mt19937_64 engine(seed);
        std::uniform_real_distribution<double> distribution(-1.0, 1.0);
        std::vector<double> out(count);
        for (auto& value : out) {
            value = distribution(engine);
        }
        return out;
    }

    std::vector<Complex> padded_window(std::span<const double> interleaved, size_t first, size_t length, size_t points, std::span<const double> window = {})
    {
        std::vector<Complex> out(points, Complex { 0.0L, 0.0L });
        for (size_t i = 0; i < length && i < points; ++i) {
            const long double w = window.empty() ? 1.0L : static_cast<long double>(window[i]);
            out[i] = Complex { w * interleaved[2 * (first + i)], w * interleaved[2 * (first + i) + 1] };
        }
        return out;
    }

    std::vector<double> interleave_real(const std::vector<double>& real)
    {
        std::vector<double> out(real.size() * 2, 0.0);
        for (size_t i = 0; i < real.size(); ++i) {
            out[2 * i] = real[i];
        }
        return out;
    }

    double peak_of(const std::vector<Complex>& spectrum)
    {
        double peak = 0.0;
        for (const auto& bin : spectrum) {
            peak = std::max(peak, static_cast<double>(std::abs(bin)));
        }
        return peak;
    }

    void expect_matches(const std::vector<double>& got, const std::vector<Complex>& want, size_t window_index = 0)
    {
        const size_t points = want.size();
        ASSERT_GE(got.size(), (window_index + 1) * points * 2);
        const double tolerance = 1.0e-12 * (1.0 + peak_of(want));
        for (size_t k = 0; k < points; ++k) {
            const size_t at = (window_index * points + k) * 2;
            EXPECT_NEAR(got[at], static_cast<double>(want[k].real()), tolerance) << "bin " << k << " re";
            EXPECT_NEAR(got[at + 1], static_cast<double>(want[k].imag()), tolerance) << "bin " << k << " im";
        }
    }

    SegmentGeometry one_window(uint32_t length)
    {
        return { .length = length, .stride = length, .offset = 0, .count = 1 };
    }

    template <typename Fixture>
    class FftGpuTest : public Fixture { };

    using HardwareOptions = ::testing::Types<GpuDispatchFixture<true>, GpuDispatchFixture<false>>;
    TYPED_TEST_SUITE(FftGpuTest, HardwareOptions);

    TYPED_TEST(FftGpuTest, AnImpulseGivesAFlatSpectrum)
    {
        FftContext context(16);
        std::vector<double> impulse(16, 0.0);
        impulse[0] = 1.0;

        const auto spectrum = context.transform(impulse, one_window(16));

        ASSERT_EQ(spectrum.size(), 32U);
        for (size_t k = 0; k < 16; ++k) {
            EXPECT_EQ(spectrum[2 * k], 1.0) << "bin " << k;
            EXPECT_EQ(spectrum[2 * k + 1], 0.0) << "bin " << k;
        }
    }

    TYPED_TEST(FftGpuTest, ASingleToneLandsInItsBinAndItsMirror)
    {
        constexpr uint32_t points = 64;
        FftContext context(points);
        std::vector<double> tone(points);
        for (uint32_t n = 0; n < points; ++n) {
            tone[n] = std::cos(2.0 * std::numbers::pi * 3.0 * n / points);
        }

        const auto spectrum = context.transform(tone, one_window(points));

        for (uint32_t k = 0; k < points; ++k) {
            const double expected = (k == 3 || k == points - 3) ? points / 2.0 : 0.0;
            EXPECT_NEAR(spectrum[static_cast<size_t>(2) * k], expected, 1.0e-12) << "bin " << k;
            EXPECT_NEAR(spectrum[static_cast<size_t>(2) * k + 1], 0.0, 1.0e-12) << "bin " << k;
        }
    }

    TYPED_TEST(FftGpuTest, MatchesTheNaiveTransformAcrossSizes)
    {
        for (const uint32_t points : { 2U, 4U, 8U, 16U, 64U, 256U, 1024U, 2048U }) {
            FftContext context(points);
            const auto signal = random_signal(points, points);
            const auto interleaved = interleave_real(signal);

            const auto spectrum = context.transform(signal, one_window(points));

            SCOPED_TRACE(points);
            expect_matches(spectrum, naive_dft(padded_window(interleaved, 0, points, points), false));
        }
    }

    TYPED_TEST(FftGpuTest, ComplexInputTransformsBothParts)
    {
        constexpr uint32_t points = 128;
        FftContext context(points);
        const auto interleaved = random_signal(static_cast<size_t>(points) * 2, 31);

        const auto spectrum = context.transform_complex(interleaved, one_window(points));

        expect_matches(spectrum, naive_dft(padded_window(interleaved, 0, points, points), false));
    }

    TYPED_TEST(FftGpuTest, ThePowerSpectrumIsTheMagnitudeSquaredOfTheNonRedundantBins)
    {
        constexpr uint32_t points = 128;
        FftContext context(points, SpectrumOutput::Power);
        const auto signal = random_signal(points, 5);
        const auto want = naive_dft(padded_window(interleave_real(signal), 0, points, points), false);

        const auto power = context.transform(signal, one_window(points));

        ASSERT_EQ(power.size(), points / 2 + 1);
        const double scale = 1.0 + peak_of(want) * peak_of(want);
        for (size_t k = 0; k <= points / 2; ++k) {
            EXPECT_NEAR(power[k], static_cast<double>(std::norm(want[k])), 1.0e-12 * scale) << "bin " << k;
        }
    }

    TYPED_TEST(FftGpuTest, TheWindowCoefficientsAreApplied)
    {
        constexpr uint32_t points = 64;
        FftContext context(points);
        const auto signal = random_signal(points, 9);
        std::vector<double> hann(points);
        for (uint32_t n = 0; n < points; ++n) {
            hann[n] = 0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * n / points);
        }

        const auto spectrum = context.transform(signal, one_window(points), hann);

        expect_matches(spectrum, naive_dft(padded_window(interleave_real(signal), 0, points, points, hann), false));
    }

    TYPED_TEST(FftGpuTest, AShortWindowIsZeroPaddedToThePointCount)
    {
        constexpr uint32_t points = 64;
        constexpr uint32_t length = 20;
        FftContext context(points);
        const auto signal = random_signal(200, 13);

        const auto spectrum = context.transform(signal, { .length = length, .stride = length, .offset = 7, .count = 1 });

        expect_matches(spectrum, naive_dft(padded_window(interleave_real(signal), 7, length, points), false));
    }

    TYPED_TEST(FftGpuTest, OverlappingWindowsEachGetTheirOwnTransform)
    {
        constexpr uint32_t points = 64;
        FftContext context(points);
        const auto signal = random_signal(300, 17);
        const auto interleaved = interleave_real(signal);
        const SegmentGeometry geometry { .length = points, .stride = 32, .offset = 5, .count = 8 };

        const auto spectra = context.transform(signal, geometry);

        ASSERT_EQ(spectra.size(), 8U * points * 2U);
        for (uint32_t w = 0; w < geometry.count; ++w) {
            SCOPED_TRACE(w);
            expect_matches(spectra, naive_dft(padded_window(interleaved, 5 + static_cast<size_t>(w) * 32, points, points), false), w);
        }
    }

    TYPED_TEST(FftGpuTest, TheInverseOfTheForwardTransformRestoresTheSignal)
    {
        constexpr uint32_t points = 512;
        FftContext forward(points);
        FftContext inverse(points, SpectrumOutput::Complex, true);
        const auto interleaved = random_signal(static_cast<size_t>(points) * 2, 23);

        const auto spectrum = forward.transform_complex(interleaved, one_window(points));
        const auto restored = inverse.transform_complex(spectrum, one_window(points));

        ASSERT_EQ(restored.size(), interleaved.size());
        for (size_t i = 0; i < interleaved.size(); ++i) {
            EXPECT_NEAR(restored[i], interleaved[i], 1.0e-12) << "value " << i;
        }
    }

    TYPED_TEST(FftGpuTest, ParsevalsRelationHolds)
    {
        constexpr uint32_t points = 256;
        FftContext context(points);
        const auto signal = random_signal(points, 41);

        const auto spectrum = context.transform(signal, one_window(points));

        double time_energy = 0.0;
        for (const double x : signal) {
            time_energy += x * x;
        }
        double frequency_energy = 0.0;
        for (size_t k = 0; k < points; ++k) {
            frequency_energy += spectrum[2 * k] * spectrum[2 * k] + spectrum[2 * k + 1] * spectrum[2 * k + 1];
        }
        EXPECT_NEAR(frequency_energy / points, time_energy, 1.0e-11);
    }

    TYPED_TEST(FftGpuTest, AProjectionMatchesTheKinesisProjectionOfTheWindowedPowerSpectrum)
    {
        constexpr uint32_t points = 64;
        constexpr size_t bins = points / 2 + 1;
        constexpr size_t rows = 5;
        const auto signal = random_signal(700, 53);
        const auto weights = random_signal(rows * bins, 59);
        const auto taper = Kinesis::Discrete::hann(points);
        FftContext context(points, weights);

        const auto got = context.transform(signal, { .length = points, .stride = 32, .offset = 0, .count = 20 }, taper);
        const auto want = Kinesis::Discrete::spectral_projection(signal, points, 32, weights, taper);

        ASSERT_EQ(context.descriptors(), rows);
        ASSERT_EQ(want.size(), 20U * rows);
        ASSERT_EQ(got.size(), want.size());
        for (size_t i = 0; i < want.size(); ++i) {
            EXPECT_NEAR(got[i], want[i], 1.0e-11 * (1.0 + std::abs(want[i]))) << "value " << i;
        }
    }

    TYPED_TEST(FftGpuTest, MoreRowsThanInvocationsAreAllComputed)
    {
        constexpr uint32_t points = 64;
        constexpr size_t bins = points / 2 + 1;
        constexpr size_t rows = 70;
        const auto signal = random_signal(256, 61);
        const auto weights = random_signal(rows * bins, 67);
        FftContext context(points, weights);

        const auto got = context.transform(signal, { .length = points, .stride = 64, .offset = 0, .count = 4 });
        const auto want = Kinesis::Discrete::spectral_projection(signal, points, 64, weights);

        ASSERT_EQ(got.size(), want.size());
        for (size_t i = 0; i < want.size(); ++i) {
            EXPECT_NEAR(got[i], want[i], 1.0e-11 * (1.0 + std::abs(want[i]))) << "value " << i;
        }
    }

    TYPED_TEST(FftGpuTest, AProjectionOfAllOnesIsTheSumOfThePowerSpectrum)
    {
        constexpr uint32_t points = 128;
        constexpr size_t bins = points / 2 + 1;
        const auto signal = random_signal(points, 71);
        const std::vector<double> ones(bins, 1.0);
        FftContext projecting(points, ones);
        FftContext power(points, SpectrumOutput::Power);

        const auto total = projecting.transform(signal, one_window(points));
        const auto spectrum = power.transform(signal, one_window(points));

        ASSERT_EQ(total.size(), 1U);
        double sum = 0.0;
        for (const double p : spectrum) {
            sum += p;
        }
        EXPECT_NEAR(total[0], sum, 1.0e-11 * (1.0 + sum));
    }

    TYPED_TEST(FftGpuTest, NoWindowsReturnsNothing)
    {
        FftContext context(16);
        const auto signal = random_signal(16, 3);

        EXPECT_TRUE(context.transform(signal, { .length = 16, .stride = 16, .offset = 0, .count = 0 }).empty());
    }

    TYPED_TEST(FftGpuTest, AStagedTransformRunsThroughTheAsynchronousPathAndCanBeRestaged)
    {
        constexpr uint32_t points = 64;
        FftContext context(points);
        const auto signal = random_signal(400, 29);
        const auto interleaved = interleave_real(signal);

        const auto run = [&context, &interleaved](const SegmentGeometry& geometry) {
            context.stage(interleaved, geometry);
            const auto fence = context.dispatch_async(Yantra::Datum<std::vector<Kakshya::DataVariant>> {});
            EXPECT_NE(fence, Portal::Graphics::INVALID_FENCE);
            foundry().wait_for_fence(fence);
            return context.decode_primary(context.collect_result());
        };

        const SegmentGeometry three { .length = points, .stride = points, .offset = 0, .count = 3 };
        const SegmentGeometry two { .length = points, .stride = 100, .offset = 11, .count = 2 };

        const auto first = run(three);
        const auto second = run(two);

        ASSERT_EQ(first.size(), 3U * points * 2U);
        ASSERT_EQ(second.size(), 2U * points * 2U);
        for (uint32_t w = 0; w < three.count; ++w) {
            expect_matches(first, naive_dft(padded_window(interleaved, static_cast<size_t>(w) * points, points, points), false), w);
        }
        for (uint32_t w = 0; w < two.count; ++w) {
            expect_matches(second, naive_dft(padded_window(interleaved, 11 + static_cast<size_t>(w) * 100, points, points), false), w);
        }
    }

} // namespace
} // namespace MayaFlux::Test
