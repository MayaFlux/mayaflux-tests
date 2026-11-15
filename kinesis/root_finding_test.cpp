#include "gtest/gtest.h"
#include "../support/test_metadata.hpp"

#include "MayaFlux/Kinesis/RootFinding.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {

using Kinesis::RootOptions;
using Kinesis::RootSample;
using Kinesis::RootStatus;
using Kinesis::bracketed_newton;

TEST(RootFindingTest, FindsTheCubeRootOfTwo)
{
    const auto [root, status] = bracketed_newton(
        [](double x) -> RootSample { return { x * x * x - 2.0, 3.0 * x * x }; }, 0.0, 2.0, 1.0);

    EXPECT_EQ(status, RootStatus::CONVERGED);
    EXPECT_NEAR(root, std::cbrt(2.0), 1e-10);
}

TEST(RootFindingTest, AZeroSlopeAtTheGuessFallsBackToBisection)
{
    const auto [root, status] = bracketed_newton(
        [](double x) -> RootSample { return { x * x * x - 8.0, 3.0 * x * x }; }, 0.0, 4.0, 0.0);

    EXPECT_EQ(status, RootStatus::CONVERGED);
    EXPECT_NEAR(root, 2.0, 1e-9);
}

TEST(RootFindingTest, AStepOutOfTheBracketIsReplacedByBisection)
{
    const double expected = std::tan(1.3);
    const auto [root, status] = bracketed_newton(
        [](double x) -> RootSample { return { std::atan(x) - 1.3, 1.0 / (1.0 + x * x) }; }, 0.0, 10.0, 10.0);

    EXPECT_EQ(status, RootStatus::CONVERGED);
    EXPECT_NEAR(root, expected, 1e-8);
}

TEST(RootFindingTest, AnInfiniteSlopeDoesNotEndTheSearch)
{
    const auto [root, status] = bracketed_newton(
        [](double x) -> RootSample {
            const double value = std::cbrt(x) - 0.5;
            return { value, std::numeric_limits<double>::infinity() };
        },
        -1.0, 1.0, 0.0, RootOptions { .max_iterations = 100 });

    EXPECT_EQ(status, RootStatus::CONVERGED);
    EXPECT_NEAR(root, 0.125, 1e-9);
}

TEST(RootFindingTest, ABracketWithoutASignChangeIsReported)
{
    const auto function = [](double x) -> RootSample { return { x - 5.0, 1.0 }; };

    EXPECT_EQ(bracketed_newton(function, 0.0, 1.0, 0.5).second, RootStatus::NO_BRACKET);
    EXPECT_EQ(bracketed_newton(function, 1.0, 0.0, 0.5).second, RootStatus::NO_BRACKET);
    EXPECT_EQ(bracketed_newton(function, 0.0, std::numeric_limits<double>::infinity(), 0.5).second,
        RootStatus::NO_BRACKET);
}

TEST(RootFindingTest, ABracketEndThatIsARootIsReturnedWithoutIterating)
{
    int calls = 0;
    const auto [root, status] = bracketed_newton(
        [&calls](double x) -> RootSample {
            ++calls;
            return { x - 1.0, 1.0 };
        },
        -2.0, 1.0, 0.0);

    EXPECT_EQ(status, RootStatus::CONVERGED);
    EXPECT_DOUBLE_EQ(root, 1.0);
    EXPECT_EQ(calls, 2);
}

TEST(RootFindingTest, ANonFiniteValueIsReported)
{
    const auto [root, status] = bracketed_newton(
        [](double x) -> RootSample {
            return { x < 0.5 ? -1.0 : std::numeric_limits<double>::quiet_NaN(), 1.0 };
        },
        0.0, 1.0, 0.75, RootOptions { .check_bracket = false });

    EXPECT_EQ(status, RootStatus::NOT_FINITE);
}

TEST(RootFindingTest, RunningOutOfIterationsReturnsTheLastIterate)
{
    const auto [root, status] = bracketed_newton(
        [](double x) -> RootSample { return { std::atan(x) - 1.3, 1.0 / (1.0 + x * x) }; },
        0.0, 10.0, 10.0, RootOptions { .max_iterations = 1 });

    EXPECT_EQ(status, RootStatus::MAX_ITERATIONS);
    EXPECT_DOUBLE_EQ(root, 5.0);
}

TEST(RootFindingTest, AGuessNearTheRootNeedsFewEvaluations)
{
    int calls = 0;
    const auto [root, status] = bracketed_newton(
        [&calls](double x) -> RootSample {
            ++calls;
            return { x * x - 2.0, 2.0 * x };
        },
        1.0, 2.0, std::sqrt(2.0) + 1e-6, RootOptions { .check_bracket = false });

    EXPECT_EQ(status, RootStatus::CONVERGED);
    EXPECT_NEAR(root, std::sqrt(2.0), 1e-12);
    EXPECT_LE(calls, 3);
}

TEST(RootFindingTest, ADegenerateBracketReturnsItsOnlyPoint)
{
    const auto [root, status] = bracketed_newton(
        [](double x) -> RootSample { return { x - 1.0, 1.0 }; }, 1.0, 1.0, 1.0);

    EXPECT_EQ(status, RootStatus::CONVERGED);
    EXPECT_DOUBLE_EQ(root, 1.0);
}

} // namespace MayaFlux::Test
