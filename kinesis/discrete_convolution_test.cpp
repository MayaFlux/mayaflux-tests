#include "gtest/gtest.h"

#include "MayaFlux/Kinesis/Discrete/Convolution.hpp"

#include <algorithm>
#include <vector>

namespace MayaFlux::Test {
namespace {

    namespace D = Kinesis::Discrete;

    constexpr double k_tol = 1e-6;

    std::vector<double> naive_convolution(const std::vector<double>& src, const std::vector<double>& ir)
    {
        std::vector<double> out(src.size() + ir.size() - 1, 0.0);
        for (size_t i = 0; i < src.size(); ++i) {
            for (size_t j = 0; j < ir.size(); ++j) {
                out[i + j] += src[i] * ir[j];
            }
        }
        return out;
    }

    std::vector<double> naive_cross_correlation(const std::vector<double>& src, const std::vector<double>& tmpl)
    {
        std::vector<double> out(src.size(), 0.0);
        for (size_t m = 0; m < src.size(); ++m) {
            for (size_t n = 0; n < tmpl.size() && m + n < src.size(); ++n) {
                out[m] += src[m + n] * tmpl[n];
            }
        }
        return out;
    }

    std::vector<double> naive_autocorrelation(const std::vector<double>& x)
    {
        std::vector<double> out(x.size(), 0.0);
        for (size_t m = 0; m < x.size(); ++m) {
            for (size_t n = 0; n + m < x.size(); ++n) {
                out[m] += x[n] * x[n + m];
            }
        }
        return out;
    }

    void expect_near_all(const std::vector<double>& actual, const std::vector<double>& expected, double tol = k_tol)
    {
        ASSERT_EQ(actual.size(), expected.size());
        for (size_t i = 0; i < expected.size(); ++i) {
            EXPECT_NEAR(actual[i], expected[i], tol) << "index " << i;
        }
    }

    const std::vector<double> signal { 1.0, -2.0, 0.5, 3.0, 0.25, -1.0, 2.0, 0.0 };
    const std::vector<double> kernel { 1.0, 0.5, -0.25 };

    TEST(KinesisConvolutionEngineTest, MultiplyingSpectraGivesLinearConvolution)
    {
        const D::ConvolutionProcessor multiply = [](const auto& s, const auto& k, auto& r) {
            for (size_t b = 0; b < s.size(); ++b) {
                r[b] = s[b] * k[b];
            }
        };

        const auto full = D::apply_convolution(signal, kernel, multiply, true);
        expect_near_all(full, naive_convolution(signal, kernel));
    }

    TEST(KinesisConvolutionEngineTest, DefaultOutputIsSourceLength)
    {
        const D::ConvolutionProcessor multiply = [](const auto& s, const auto& k, auto& r) {
            for (size_t b = 0; b < s.size(); ++b) {
                r[b] = s[b] * k[b];
            }
        };

        const auto out = D::apply_convolution(signal, kernel, multiply);
        EXPECT_EQ(out.size(), signal.size());
    }

    TEST(KinesisConvolutionEngineTest, EmptyKernelGivesZerosOfSourceLength)
    {
        const D::ConvolutionProcessor multiply = [](const auto& s, const auto& k, auto& r) {
            for (size_t b = 0; b < s.size(); ++b) {
                r[b] = s[b] * k[b];
            }
        };

        const auto out = D::apply_convolution(signal, std::span<const double> {}, multiply);
        ASSERT_EQ(out.size(), signal.size());
        for (double v : out) {
            EXPECT_DOUBLE_EQ(v, 0.0);
        }
    }

    TEST(KinesisConvolutionTest, SingleTapKernelScalesTheSignal)
    {
        const std::vector<double> gain { 2.0 };
        const auto out = D::convolve(signal, gain);

        std::vector<double> expected = signal;
        for (double& v : expected) {
            v *= 2.0;
        }
        expect_near_all(out, expected);
    }

    TEST(KinesisConvolutionTest, UnitImpulseKernelIsIdentity)
    {
        const std::vector<double> impulse { 1.0, 0.0, 0.0 };
        expect_near_all(D::convolve(signal, impulse), signal);
    }

    TEST(KinesisConvolutionTest, MatchesNaiveConvolutionTruncatedToSourceLength)
    {
        const auto out = D::convolve(signal, kernel);
        const auto full = naive_convolution(signal, kernel);

        ASSERT_EQ(out.size(), signal.size());
        expect_near_all(out, std::vector<double>(full.begin(), full.begin() + static_cast<std::ptrdiff_t>(signal.size())));
    }

    TEST(KinesisConvolutionTest, EmptySourceGivesEmptyOutput)
    {
        EXPECT_TRUE(D::convolve(std::span<const double> {}, kernel).empty());
    }

    TEST(KinesisConvolutionCorrelationTest, CrossCorrelateMatchesSlidingDotProduct)
    {
        const std::vector<double> tmpl { 1.0, 2.0, -1.0 };
        const auto out = D::cross_correlate(signal, tmpl, false);
        expect_near_all(out, naive_cross_correlation(signal, tmpl));
    }

    TEST(KinesisConvolutionCorrelationTest, CrossCorrelatePeaksWhereTemplateStarts)
    {
        std::vector<double> src(20, 0.0);
        src[7] = 1.0;
        src[8] = 2.0;
        src[9] = 3.0;
        const std::vector<double> tmpl { 1.0, 2.0, 3.0 };

        const auto out = D::cross_correlate(src, tmpl, false);
        const auto peak = static_cast<size_t>(std::distance(out.begin(), std::ranges::max_element(out)));
        EXPECT_EQ(peak, 7U);
    }

    TEST(KinesisConvolutionCorrelationTest, CrossCorrelateNormalizedPeakIsOne)
    {
        const std::vector<double> tmpl { 1.0, 2.0, -1.0 };
        const auto out = D::cross_correlate(signal, tmpl, true);

        const auto raw = naive_cross_correlation(signal, tmpl);
        const double peak = std::max(std::abs(*std::ranges::min_element(raw)), std::abs(*std::ranges::max_element(raw)));
        std::vector<double> expected = raw;
        for (double& v : expected) {
            v /= peak;
        }
        expect_near_all(out, expected);
    }

    TEST(KinesisConvolutionCorrelationTest, MatchedFilterPeaksAtReferenceStart)
    {
        std::vector<double> src(20, 0.0);
        src[7] = 1.0;
        src[8] = 2.0;
        src[9] = 3.0;
        const std::vector<double> reference { 1.0, 2.0, 3.0 };

        const auto out = D::matched_filter(src, reference);
        ASSERT_EQ(out.size(), src.size());

        const auto peak = static_cast<size_t>(std::distance(out.begin(), std::ranges::max_element(out)));
        EXPECT_EQ(peak, 7U);
        EXPECT_NEAR(out[7], 1.0, k_tol);
        for (double v : out) {
            EXPECT_LE(std::abs(v), 1.0 + k_tol);
        }
    }

    TEST(KinesisConvolutionCorrelationTest, AutoCorrelateMatchesNaiveSums)
    {
        const std::vector<double> x { 1.0, 2.0, 0.0, -1.0, 3.0, 1.0, -2.0, 0.5 };
        expect_near_all(D::auto_correlate(x, false), naive_autocorrelation(x));
    }

    TEST(KinesisConvolutionCorrelationTest, AutoCorrelateNormalizesToZeroLag)
    {
        const std::vector<double> x { 1.0, 2.0, 0.0, -1.0, 3.0, 1.0, -2.0, 0.5 };
        const auto raw = naive_autocorrelation(x);

        std::vector<double> expected = raw;
        for (double& v : expected) {
            v /= raw.front();
        }
        const auto out = D::auto_correlate(x, true);
        expect_near_all(out, expected);
        EXPECT_NEAR(out.front(), 1.0, k_tol);
    }

    TEST(KinesisConvolutionDeconvolveTest, InvertsTheFullLinearConvolution)
    {
        const std::vector<double> x { 1.0, -2.0, 0.5, 3.0, 0.25, -1.0 };
        const std::vector<double> ir { 1.0, 0.5 };

        const auto y = naive_convolution(x, ir);
        std::vector<double> expected = x;
        expected.resize(y.size(), 0.0);

        expect_near_all(D::deconvolve(y, ir, 1e-12), expected);
    }

} // namespace
} // namespace MayaFlux::Test
