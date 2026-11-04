#include "gtest/gtest.h"

#include "MayaFlux/Kinesis/Discrete/Transform.hpp"

#include <cmath>
#include <numbers>
#include <tuple>

namespace MayaFlux::Test {
namespace {

    namespace D = Kinesis::Discrete;

    constexpr double k_tol = 1e-9;

    void expect_near_all(const std::vector<double>& actual, const std::vector<double>& expected, double tol = k_tol)
    {
        ASSERT_EQ(actual.size(), expected.size());
        for (size_t i = 0; i < expected.size(); ++i) {
            EXPECT_NEAR(actual[i], expected[i], tol) << "index " << i;
        }
    }

    std::vector<double> counting(size_t n)
    {
        std::vector<double> data(n);
        for (size_t i = 0; i < n; ++i) {
            data[i] = static_cast<double>(i);
        }
        return data;
    }

    TEST(KinesisTransformPointwiseTest, LinearAppliesScaleAndOffset)
    {
        std::vector<double> data { 0.0, 1.0, -2.0 };
        D::linear(data, 2.0, 1.0);
        expect_near_all(data, { 1.0, 3.0, -3.0 });
    }

    TEST(KinesisTransformPointwiseTest, PowerRaisesEachSample)
    {
        std::vector<double> data { 2.0, 3.0 };
        D::power(data, 2.0);
        expect_near_all(data, { 4.0, 9.0 });
    }

    TEST(KinesisTransformPointwiseTest, ExponentialDefaultsToNaturalBase)
    {
        std::vector<double> data { 0.0, 1.0 };
        D::exponential(data, 3.0, 1.0);
        expect_near_all(data, { 3.0, 3.0 * std::numbers::e });
    }

    TEST(KinesisTransformPointwiseTest, ExponentialWithBaseTwo)
    {
        std::vector<double> data { 1.0, 2.0 };
        D::exponential(data, 1.0, 1.0, 2.0);
        expect_near_all(data, { 2.0, 4.0 });
    }

    TEST(KinesisTransformPointwiseTest, LogarithmicMapsNonPositiveArgumentsToZero)
    {
        std::vector<double> data { 1.0, std::numbers::e, 0.0, -1.0 };
        D::logarithmic(data, 1.0, 1.0, 0.0);
        expect_near_all(data, { 0.0, 1.0, 0.0, 0.0 });
    }

    TEST(KinesisTransformPointwiseTest, LogarithmicWithBaseTen)
    {
        std::vector<double> data { 1.0, 100.0 };
        D::logarithmic(data, 1.0, 1.0, 0.0, 10.0);
        expect_near_all(data, { 0.0, 2.0 });
    }

    TEST(KinesisTransformPointwiseTest, TrigAppliesAmplitudeToTheFunction)
    {
        std::vector<double> data { 0.0, std::numbers::pi / 2.0 };
        D::apply_trig(data, [](double x) { return std::sin(x); }, 1.0, 2.0, 0.0);
        expect_near_all(data, { 0.0, 2.0 });
    }

    TEST(KinesisTransformPointwiseTest, TrigFrequencyAndPhaseEnterBeforeTheFunction)
    {
        std::vector<double> phased { 0.0 };
        D::apply_trig(phased, [](double x) { return std::sin(x); }, 1.0, 1.0, std::numbers::pi / 2.0);
        expect_near_all(phased, { 1.0 });

        std::vector<double> scaled { std::numbers::pi / 4.0 };
        D::apply_trig(scaled, [](double x) { return std::sin(x); }, 2.0, 1.0, 0.0);
        expect_near_all(scaled, { 1.0 });
    }

    TEST(KinesisTransformPointwiseTest, ClampLimitsToRange)
    {
        std::vector<double> data { -2.0, -0.5, 0.5, 2.0 };
        D::clamp(data, -1.0, 1.0);
        expect_near_all(data, { -1.0, -0.5, 0.5, 1.0 });
    }

    TEST(KinesisTransformPointwiseTest, QuantizeToTwoBitsRoundsToThirds)
    {
        std::vector<double> data { 0.3, -1.0, 2.0, 0.0 };
        D::quantize(data, 2);
        expect_near_all(data, { 1.0 / 3.0, -1.0, 1.0, 0.0 });
    }

    TEST(KinesisTransformPointwiseTest, NormalizeDefaultsToSymmetricRange)
    {
        std::vector<double> data { 2.0, 4.0, 6.0 };
        D::normalize(data);
        expect_near_all(data, { -1.0, 0.0, 1.0 });
    }

    TEST(KinesisTransformPointwiseTest, NormalizeToCustomRange)
    {
        std::vector<double> data { 2.0, 4.0, 6.0 };
        D::normalize(data, 0.0, 10.0);
        expect_near_all(data, { 0.0, 5.0, 10.0 });
    }

    TEST(KinesisTransformPointwiseTest, NormalizeConstantSequenceIsUnchanged)
    {
        std::vector<double> data { 5.0, 5.0, 5.0 };
        D::normalize(data, 0.0, 1.0);
        expect_near_all(data, { 5.0, 5.0, 5.0 });
    }

    TEST(KinesisTransformPointwiseTest, EmptySpanIsANoOp)
    {
        std::span<double> empty;
        EXPECT_NO_THROW(D::linear(empty, 2.0, 1.0));
        EXPECT_NO_THROW(D::normalize(empty));
        EXPECT_NO_THROW(D::reverse(empty));
        EXPECT_NO_THROW(D::fade(empty, 0.5, 0.5));
    }

    class KinesisTransformLinearRoundTripTest
        : public ::testing::TestWithParam<std::tuple<double, double>> {
    };

    TEST_P(KinesisTransformLinearRoundTripTest, InverseMapRestoresInput)
    {
        const auto [a, b] = GetParam();
        const std::vector<double> original { 1.0, -2.0, 0.25 };

        std::vector<double> data = original;
        D::linear(data, a, b);
        D::linear(data, 1.0 / a, -b / a);

        expect_near_all(data, original);
    }

    INSTANTIATE_TEST_SUITE_P(
        Affine,
        KinesisTransformLinearRoundTripTest,
        ::testing::Combine(
            ::testing::Values(2.0, -0.5, 4.0),
            ::testing::Values(0.0, 3.0)));

    TEST(KinesisTransformTemporalTest, ReverseFlipsOrder)
    {
        std::vector<double> data { 1.0, 2.0, 3.0, 4.0 };
        D::reverse(data);
        expect_near_all(data, { 4.0, 3.0, 2.0, 1.0 });
    }

    TEST(KinesisTransformTemporalTest, FadeInStartsSilent)
    {
        std::vector<double> data(5, 1.0);
        D::fade(data, 0.5, 0.0);
        expect_near_all(data, { 0.0, 1.0, 1.0, 1.0, 1.0 });
    }

    TEST(KinesisTransformTemporalTest, FadeOutEndsSilent)
    {
        std::vector<double> data(5, 1.0);
        D::fade(data, 0.0, 0.5);
        expect_near_all(data, { 1.0, 1.0, 1.0, 1.0, 0.0 });
    }

    TEST(KinesisTransformTemporalTest, ZeroFadesLeaveSignalUntouched)
    {
        std::vector<double> data { 0.3, -0.7, 0.9 };
        D::fade(data, 0.0, 0.0);
        expect_near_all(data, { 0.3, -0.7, 0.9 });
    }

    TEST(KinesisTransformTemporalTest, SliceSelectsByRatio)
    {
        const auto data = counting(10);
        expect_near_all(D::slice(data, 0.2, 0.5), { 2.0, 3.0, 4.0 });
    }

    TEST(KinesisTransformTemporalTest, SliceWithDegenerateRangeIsEmpty)
    {
        const auto data = counting(10);
        EXPECT_TRUE(D::slice(data, 0.5, 0.5).empty());
        EXPECT_TRUE(D::slice(data, 0.7, 0.2).empty());
        EXPECT_TRUE(D::slice(std::span<const double> {}, 0.0, 1.0).empty());
    }

    TEST(KinesisTransformTemporalTest, SliceClampsRatiosToUnitInterval)
    {
        const auto data = counting(10);
        expect_near_all(D::slice(data, -1.0, 2.0), data);
    }

    TEST(KinesisTransformTemporalTest, DelayPrependsFillValue)
    {
        const std::vector<double> data { 1.0, 2.0 };
        expect_near_all(D::delay(data, 2, 0.0), { 0.0, 0.0, 1.0, 2.0 });
        expect_near_all(D::delay(data, 2, -1.0), { -1.0, -1.0, 1.0, 2.0 });
    }

    TEST(KinesisTransformTemporalTest, DelayZeroIsCopy)
    {
        const std::vector<double> data { 1.0, 2.0 };
        expect_near_all(D::delay(data, 0), data);
    }

    TEST(KinesisTransformResampleTest, LinearInterpolatesBetweenSamples)
    {
        const std::vector<double> src { 0.0, 1.0, 2.0 };
        std::vector<double> dst(5);
        D::interpolate_linear(src, dst);
        expect_near_all(dst, { 0.0, 0.5, 1.0, 1.5, 2.0 });
    }

    TEST(KinesisTransformResampleTest, LinearKeepsEndpoints)
    {
        const std::vector<double> src { 3.0, -1.0, 4.0, 1.0, 5.0 };
        std::vector<double> dst(11);
        D::interpolate_linear(src, dst);
        EXPECT_NEAR(dst.front(), 3.0, k_tol);
        EXPECT_NEAR(dst.back(), 5.0, k_tol);
    }

    TEST(KinesisTransformResampleTest, CubicIsExactAtSourceSamples)
    {
        const std::vector<double> src { 0.0, 2.0, 1.0, 4.0, 3.0 };
        std::vector<double> dst(src.size());
        D::interpolate_cubic(src, dst);
        expect_near_all(dst, src);
    }

    TEST(KinesisTransformResampleTest, CubicReproducesALinearRampInTheInterior)
    {
        const auto src = counting(7);
        std::vector<double> dst(13);
        D::interpolate_cubic(src, dst);

        for (size_t i = 2; i <= 9; ++i) {
            EXPECT_NEAR(dst[i], static_cast<double>(i) * 0.5, k_tol) << "index " << i;
        }
    }

    TEST(KinesisTransformResampleTest, TimeStretchLengthAndEndpoints)
    {
        const std::vector<double> data { 1.0, 4.0, 2.0, 8.0, 5.0, 7.0 };

        const auto longer = D::time_stretch(data, 2.0);
        ASSERT_EQ(longer.size(), 12U);
        EXPECT_NEAR(longer.front(), 1.0, k_tol);
        EXPECT_NEAR(longer.back(), 7.0, 1e-6);

        const auto shorter = D::time_stretch(data, 0.5);
        ASSERT_EQ(shorter.size(), 3U);
        EXPECT_NEAR(shorter.front(), 1.0, k_tol);
        EXPECT_NEAR(shorter.back(), 7.0, k_tol);
    }

    TEST(KinesisTransformResampleTest, TimeStretchOneIsCopy)
    {
        const std::vector<double> data { 1.0, 4.0, 2.0 };
        expect_near_all(D::time_stretch(data, 1.0), data);
    }

    TEST(KinesisTransformResampleTest, TimeStretchRejectsNonPositiveFactor)
    {
        const std::vector<double> data { 1.0, 2.0 };
        EXPECT_TRUE(D::time_stretch(data, 0.0).empty());
        EXPECT_TRUE(D::time_stretch(data, -1.0).empty());
    }

    TEST(KinesisTransformResampleTest, RampLinearEndpointsAndStep)
    {
        expect_near_all(D::ramp_linear(5, 0.0, 1.0), { 0.0, 0.25, 0.5, 0.75, 1.0 }, 1e-12);
        expect_near_all(D::ramp_linear(1, 2.5, 9.0), { 2.5 }, 1e-12);
        EXPECT_TRUE(D::ramp_linear(0).empty());
    }

    TEST(KinesisTransformResampleTest, RampExponentialIsGeometric)
    {
        expect_near_all(D::ramp_exponential(4, 1.0, 8.0), { 1.0, 2.0, 4.0, 8.0 }, 1e-9);
        expect_near_all(D::ramp_exponential(1, 0.5, 2.0), { 0.5 }, 1e-12);
    }

    TEST(KinesisTransformResampleTest, RampExponentialKeepsNegativeSign)
    {
        expect_near_all(D::ramp_exponential(3, -1.0, -4.0), { -1.0, -2.0, -4.0 }, 1e-9);
    }

    TEST(KinesisTransformResampleTest, RampExponentialClampsZeroStart)
    {
        const auto ramp = D::ramp_exponential(3, 0.0, 1.0);
        ASSERT_EQ(ramp.size(), 3U);
        EXPECT_GT(ramp.front(), 0.0);
        EXPECT_NEAR(ramp.back(), 1.0, 1e-6);
    }

} // namespace
} // namespace MayaFlux::Test
