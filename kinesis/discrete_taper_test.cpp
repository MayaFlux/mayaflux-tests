#include "gtest/gtest.h"
#include "../support/test_metadata.hpp"

#include "MayaFlux/Kinesis/Discrete/Taper.hpp"

#include <functional>
#include <utility>
#include <vector>

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {
namespace {

    namespace D = Kinesis::Discrete;

    constexpr double k_tol = 1e-12;

    void expect_near_all(const std::vector<double>& actual, const std::vector<double>& expected)
    {
        ASSERT_EQ(actual.size(), expected.size());
        for (size_t i = 0; i < expected.size(); ++i) {
            EXPECT_NEAR(actual[i], expected[i], k_tol) << "index " << i;
        }
    }

    TEST(KinesisTaperCoefficientTest, HannKnownValues)
    {
        expect_near_all(D::hann(5), { 0.0, 0.5, 1.0, 0.5, 0.0 });
    }

    TEST(KinesisTaperCoefficientTest, HammingKnownValues)
    {
        expect_near_all(D::hamming(5), { 0.08, 0.54, 1.0, 0.54, 0.08 });
    }

    TEST(KinesisTaperCoefficientTest, BlackmanKnownValues)
    {
        expect_near_all(D::blackman(5), { 0.0, 0.34, 1.0, 0.34, 0.0 });
    }

    TEST(KinesisTaperCoefficientTest, DegenerateLengths)
    {
        EXPECT_TRUE(D::hann(0).empty());
        EXPECT_TRUE(D::hamming(0).empty());
        EXPECT_TRUE(D::blackman(0).empty());
        expect_near_all(D::hann(1), { 1.0 });
        expect_near_all(D::hamming(1), { 1.0 });
        expect_near_all(D::blackman(1), { 1.0 });
    }

    class KinesisTaperSymmetryTest : public ::testing::TestWithParam<size_t> {
    };

    TEST_P(KinesisTaperSymmetryTest, WindowsAreSymmetric)
    {
        const size_t n = GetParam();
        const std::vector<std::vector<double>> windows {
            D::hann(n), D::hamming(n), D::blackman(n), D::trapezoid(n, n / 4)
        };

        for (const auto& w : windows) {
            ASSERT_EQ(w.size(), n);
            for (size_t i = 0; i < n; ++i) {
                EXPECT_NEAR(w[i], w[n - 1 - i], k_tol) << "index " << i;
            }
        }
    }

    INSTANTIATE_TEST_SUITE_P(
        Lengths,
        KinesisTaperSymmetryTest,
        ::testing::Values(2U, 7U, 16U));

    TEST(KinesisTaperCoefficientTest, RectangularSupportAndOffset)
    {
        expect_near_all(D::rectangular(5, 3, 1), { 0.0, 1.0, 1.0, 1.0, 0.0 });
        expect_near_all(D::rectangular(3, 5, 2), { 0.0, 0.0, 1.0 });
        expect_near_all(D::rectangular(4), { 1.0, 1.0, 1.0, 1.0 });
        expect_near_all(D::rectangular(4, 0), { 0.0, 0.0, 0.0, 0.0 });
    }

    TEST(KinesisTaperCoefficientTest, TrapezoidRampsBothEnds)
    {
        expect_near_all(D::trapezoid(5, 2), { 0.0, 0.5, 1.0, 0.5, 0.0 });
        expect_near_all(D::trapezoid(6, 0), { 1.0, 1.0, 1.0, 1.0, 1.0, 1.0 });
        expect_near_all(D::trapezoid(1, 3), { 1.0 });
        EXPECT_TRUE(D::trapezoid(0, 3).empty());
    }

    TEST(KinesisTaperApplyTest, TaperMultipliesElementwise)
    {
        std::vector<double> data(4, 1.0);
        const std::vector<double> taper { 0.5, 2.0, 0.25, 1.0 };
        D::apply_taper(data, taper);
        expect_near_all(data, taper);
    }

    TEST(KinesisTaperApplyTest, ShorterTaperRepeats)
    {
        std::vector<double> data(4, 1.0);
        const std::vector<double> taper { 0.5, 2.0 };
        D::apply_taper(data, taper);
        expect_near_all(data, { 0.5, 2.0, 0.5, 2.0 });
    }

    TEST(KinesisTaperApplyTest, EmptyTaperOrDataIsANoOp)
    {
        std::vector<double> data { 1.0, 2.0 };
        D::apply_taper(data, std::span<const double> {});
        expect_near_all(data, { 1.0, 2.0 });

        D::apply_taper(std::span<double> {}, std::vector<double> { 3.0 });
    }

    TEST(KinesisTaperApplyTest, NamedWindowsMatchTheirCoefficients)
    {
        const std::vector<std::pair<std::function<void(std::span<double>)>, std::function<std::vector<double>(size_t)>>> pairs {
            { [](std::span<double> d) { D::apply_hann(d); }, [](size_t n) { return D::hann(n); } },
            { [](std::span<double> d) { D::apply_hamming(d); }, [](size_t n) { return D::hamming(n); } },
            { [](std::span<double> d) { D::apply_blackman(d); }, [](size_t n) { return D::blackman(n); } },
        };

        for (const auto& [apply, coefficients] : pairs) {
            for (size_t n : { size_t { 1 }, size_t { 9 } }) {
                std::vector<double> data(n, 1.0);
                apply(data);
                expect_near_all(data, coefficients(n));
            }
        }
    }

    TEST(KinesisTaperApplyTest, RectangularMasksOutsideSupport)
    {
        std::vector<double> data(6, 1.0);
        D::apply_rectangular(data, 3, 2);
        expect_near_all(data, { 0.0, 0.0, 1.0, 1.0, 1.0, 0.0 });

        std::vector<double> zeroed(4, 1.0);
        D::apply_rectangular(zeroed, 0);
        expect_near_all(zeroed, { 0.0, 0.0, 0.0, 0.0 });
    }

    TEST(KinesisTaperApplyTest, TrapezoidMatchesCoefficients)
    {
        std::vector<double> data(5, 1.0);
        D::apply_trapezoid(data, 2);
        expect_near_all(data, D::trapezoid(5, 2));
    }

} // namespace
} // namespace MayaFlux::Test
