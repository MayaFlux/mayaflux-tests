#include "gtest/gtest.h"
#include "../support/test_metadata.hpp"

#include "MayaFlux/Kinesis/Acoustics/DuctGrid.hpp"
#include "MayaFlux/Kinesis/Acoustics/PressureFlowSolve.hpp"
#include "MayaFlux/Kinesis/Acoustics/Radiation.hpp"
#include "MayaFlux/Kinesis/Acoustics/Reed.hpp"
#include "MayaFlux/Kinesis/Acoustics/StaggeredLine.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {

namespace Acoustics = Kinesis::Acoustics;
using Acoustics::FlowSample;
using Acoustics::ReedChannel;
using Kinesis::RootStatus;

namespace {

    constexpr double kRate = 48000.0;
    constexpr double kSound = 343.0;
    constexpr double kDensity = 1.2;
    constexpr double kRadius = 0.0075;
    constexpr double kLength = 0.6;

    ReedChannel clarinet_reed()
    {
        return { .width = 0.012, .rest_opening = 4.0e-4, .closing_pressure = 3000.0, .air_density = kDensity };
    }

    double characteristic_impedance()
    {
        return kDensity * kSound / (std::numbers::pi * kRadius * kRadius);
    }

    double coupling_gain()
    {
        return characteristic_impedance() * kSound / kRate / (kLength / 83.0);
    }

    FlowSample reed_law(const ReedChannel& reed, double difference)
    {
        const double coefficient = Acoustics::reed_flow_coefficient(reed);
        const double closing = reed.closing_pressure;
        if (difference >= closing) {
            return { 0.0, 0.0 };
        }
        if (difference == 0.0) {
            return { 0.0, std::numeric_limits<double>::infinity() };
        }
        const double root = std::sqrt(std::abs(difference));
        const double flow = Acoustics::reed_flow(reed, difference);
        if (difference > 0.0) {
            return { flow, coefficient * ((1.0 - difference / closing) / (2.0 * root) - root / closing) };
        }
        return { flow, coefficient * ((1.0 + root * root / closing) / (2.0 * root) + root / closing) };
    }

    double dominant_frequency(std::span<const double> signal, double low_hz, double high_hz)
    {
        const size_t window = std::min(signal.size(), static_cast<size_t>(0.5 * kRate));
        const std::span<const double> tail = signal.last(window);
        const double mean = std::accumulate(tail.begin(), tail.end(), 0.0) / static_cast<double>(window);

        std::vector<double> centred(window);
        std::ranges::transform(tail, centred.begin(), [mean](double x) { return x - mean; });

        const double energy = std::inner_product(centred.begin(), centred.end(), centred.begin(), 0.0);
        const auto lag_min = static_cast<size_t>(kRate / high_hz);
        const auto lag_max = static_cast<size_t>(kRate / low_hz);
        if (energy <= 0.0 || lag_max + 2 >= window) {
            return 0.0;
        }

        std::vector<double> correlation(lag_max + 2, 0.0);
        for (size_t lag = lag_min - 1; lag <= lag_max + 1; ++lag) {
            correlation[lag] = std::inner_product(centred.begin(), centred.end() - static_cast<std::ptrdiff_t>(lag),
                                   centred.begin() + static_cast<std::ptrdiff_t>(lag), 0.0)
                / energy;
        }

        const double best = *std::max_element(correlation.begin() + static_cast<std::ptrdiff_t>(lag_min),
            correlation.begin() + static_cast<std::ptrdiff_t>(lag_max) + 1);
        for (size_t lag = lag_min; lag <= lag_max; ++lag) {
            const bool peak = correlation[lag] >= correlation[lag - 1] && correlation[lag] >= correlation[lag + 1];
            if (peak && correlation[lag] >= 0.9 * best) {
                const double curvature = correlation[lag - 1] - 2.0 * correlation[lag] + correlation[lag + 1];
                const double offset = curvature != 0.0 ? 0.5 * (correlation[lag - 1] - correlation[lag + 1]) / curvature : 0.0;
                return kRate / (static_cast<double>(lag) + offset);
            }
        }
        return 0.0;
    }

    struct Blow {
        double frequency {};
        double peak_to_peak {};
        bool finite { true };
    };

    Blow blow(double mouth_pressure, double seconds, double settle_seconds)
    {
        const Acoustics::DuctSpec spec { .length = kLength, .sample_rate = kRate };
        const auto grid = Acoustics::make_duct_grid(spec, [](double) { return kRadius; });
        const double area = std::numbers::pi * kRadius * kRadius;
        const auto radiation = Acoustics::make_radiation_coefficients(
            { .model = Acoustics::unflanged_radiation, .radius = kRadius, .time_step = 1.0 / kRate },
            area, grid->pressure_gain.back());
        const ReedChannel reed = clarinet_reed();

        std::vector<double> pressure(grid->intervals + 1, 0.0);
        std::vector<double> velocity(grid->intervals, 0.0);
        pressure[grid->intervals / 2] = 1.0;

        Acoustics::RadiationState state;
        double warm_start = 0.0;

        const auto total = static_cast<size_t>(seconds * kRate);
        const auto settle = static_cast<size_t>(settle_seconds * kRate);
        const auto ramp = static_cast<size_t>(0.05 * kRate);

        std::vector<double> pickup;
        Blow result;
        for (size_t n = 0; n < total; ++n) {
            const double mouth = mouth_pressure * std::min(1.0, (static_cast<double>(n) + 0.5) / static_cast<double>(ramp));

            Acoustics::advance_velocity(velocity, pressure, grid->velocity_gain);
            const auto [mouth_pressure_after, flow, difference, status] = Acoustics::reed_mouth_step(
                reed, warm_start, grid->pressure_gain.front(), mouth, pressure.front(), velocity.front());
            Acoustics::advance_pressure(pressure, velocity, grid->pressure_gain);
            pressure.front() = mouth_pressure_after;
            const auto [end_pressure, radiated] = Acoustics::radiation_step(
                *radiation, state, pressure.back(), velocity.back());
            pressure.back() = end_pressure;

            const double sample = Acoustics::sample_pressure(pressure, 0.5);
            result.finite = result.finite && std::isfinite(sample) && status != RootStatus::NOT_FINITE;
            if (n >= settle) {
                pickup.push_back(sample);
            }
        }

        const auto [low, high] = std::ranges::minmax(pickup);
        result.peak_to_peak = high - low;
        result.frequency = dominant_frequency(pickup, 60.0, 1000.0);
        return result;
    }

}

TEST(ReedCouplingTest, TheFlowRisesToAThirdOfTheClosingPressureThenFallsToZero)
{
    const ReedChannel reed = clarinet_reed();
    const double closing = reed.closing_pressure;
    const double coefficient = Acoustics::reed_flow_coefficient(reed);

    EXPECT_DOUBLE_EQ(Acoustics::reed_flow(reed, 0.0), 0.0);
    EXPECT_NEAR(Acoustics::reed_flow(reed, closing / 3.0), coefficient * (2.0 / 3.0) * std::sqrt(closing / 3.0), 1e-15);
    EXPECT_GT(Acoustics::reed_flow(reed, closing / 3.0), Acoustics::reed_flow(reed, 0.1 * closing));
    EXPECT_GT(Acoustics::reed_flow(reed, closing / 3.0), Acoustics::reed_flow(reed, 0.9 * closing));
    EXPECT_DOUBLE_EQ(Acoustics::reed_flow(reed, closing), 0.0);
    EXPECT_DOUBLE_EQ(Acoustics::reed_flow(reed, 2.0 * closing), 0.0);
}

TEST(ReedCouplingTest, ReverseFlowIsLargerBecauseTheChannelOpensFurther)
{
    const ReedChannel reed = clarinet_reed();

    EXPECT_LT(Acoustics::reed_flow(reed, -500.0), 0.0);
    EXPECT_GT(std::abs(Acoustics::reed_flow(reed, -500.0)), Acoustics::reed_flow(reed, 500.0));
}

TEST(ReedCouplingTest, AChannelWithAFieldThatIsNotPositiveHasNoFlow)
{
    ReedChannel reed = clarinet_reed();
    reed.closing_pressure = 0.0;
    EXPECT_DOUBLE_EQ(Acoustics::reed_flow(reed, 100.0), 0.0);

    reed = clarinet_reed();
    reed.width = std::numeric_limits<double>::quiet_NaN();
    EXPECT_DOUBLE_EQ(Acoustics::reed_flow(reed, 100.0), 0.0);
}

TEST(ReedCouplingTest, UniquenessFollowsTheReedParameter)
{
    const ReedChannel reed = clarinet_reed();
    const double gain = Acoustics::flow_coupling_gain(2.0 * coupling_gain());
    const double zeta = characteristic_impedance() * Acoustics::reed_flow_coefficient(reed) / std::sqrt(reed.closing_pressure);

    EXPECT_NEAR(zeta, 0.264, 5e-3);
    EXPECT_TRUE(Acoustics::reed_coupling_is_unique(reed, gain));
    EXPECT_FALSE(Acoustics::reed_coupling_is_unique(reed, 5.0 * gain));

    ReedChannel broken = reed;
    broken.rest_opening = -1.0;
    EXPECT_FALSE(Acoustics::reed_coupling_is_unique(broken, gain));
}

TEST(ReedCouplingTest, TheSolutionSatisfiesTheCouplingAndTheCellUpdateForEverySign)
{
    const ReedChannel reed = clarinet_reed();
    const double end_gain = 2.0 * coupling_gain();
    const double gain = Acoustics::flow_coupling_gain(end_gain);

    for (const double mouth : { 0.0, 500.0, 1500.0, 2600.0 }) {
        for (const double end : { -800.0, 0.0, 500.0, 2000.0 }) {
            for (const double downstream : { -1e-4, 0.0, 2e-4 }) {
                double warm_start = 0.0;
                const auto [pressure, flow, difference, status] = Acoustics::reed_mouth_step(
                    reed, warm_start, end_gain, mouth, end, downstream);

                const double target = Acoustics::flow_coupling_target(mouth, end, gain, downstream);
                const double scale = std::max({ 1.0, std::abs(target), std::abs(difference) });

                EXPECT_EQ(status, RootStatus::CONVERGED);
                EXPECT_NEAR(difference + gain * flow, target, 1e-9 * scale)
                    << mouth << " " << end << " " << downstream;
                EXPECT_NEAR(flow, Acoustics::reed_flow(reed, difference), 1e-12 * std::max(1.0, std::abs(flow)));
                EXPECT_NEAR(pressure, 2.0 * (mouth - difference) - end, 1e-9 * scale);
                EXPECT_NEAR(pressure, end + end_gain * (flow - downstream), 1e-9 * scale);
            }
        }
    }
}

TEST(ReedCouplingTest, AShutReedIsAWall)
{
    const ReedChannel reed = clarinet_reed();
    double warm_start = 7.0;

    const auto [pressure, flow, difference, status] = Acoustics::reed_mouth_step(
        reed, warm_start, 4.0e6, 4000.0, 0.0, 0.0);

    EXPECT_EQ(status, RootStatus::CONVERGED);
    EXPECT_DOUBLE_EQ(flow, 0.0);
    EXPECT_DOUBLE_EQ(difference, 4000.0);
    EXPECT_DOUBLE_EQ(pressure, 0.0);
    EXPECT_DOUBLE_EQ(warm_start, 0.0);
}

TEST(ReedCouplingTest, ANullDifferenceMeansNoFlow)
{
    const ReedChannel reed = clarinet_reed();
    double warm_start = 3.0;

    const auto [pressure, flow, difference, status] = Acoustics::reed_mouth_step(
        reed, warm_start, 4.0e6, 900.0, 900.0, 0.0);

    EXPECT_EQ(status, RootStatus::CONVERGED);
    EXPECT_DOUBLE_EQ(flow, 0.0);
    EXPECT_DOUBLE_EQ(difference, 0.0);
    EXPECT_DOUBLE_EQ(pressure, 900.0);
}

TEST(ReedCouplingTest, TheSolutionIsSmoothAcrossZeroAndIndependentOfTheWarmStart)
{
    const ReedChannel reed = clarinet_reed();
    const double end_gain = 2.0 * coupling_gain();

    for (const double target : { 1e-6, -1e-6, 1e-12, -1e-12 }) {
        double warm_start = 0.0;
        const auto [pressure, flow, difference, status] = Acoustics::reed_mouth_step(
            reed, warm_start, end_gain, target, 0.0, 0.0);

        EXPECT_EQ(status, RootStatus::CONVERGED);
        EXPECT_LT(std::abs(difference), 1e-8);
        EXPECT_EQ(std::signbit(flow), target < 0.0);
    }

    double cold = 0.0;
    double hot = 40.0;
    const auto first = Acoustics::reed_mouth_step(reed, cold, end_gain, 1800.0, 200.0, 5e-5);
    const auto second = Acoustics::reed_mouth_step(reed, hot, end_gain, 1800.0, 200.0, 5e-5);
    EXPECT_NEAR(std::get<2>(first), std::get<2>(second), 1e-8);
    EXPECT_NEAR(std::get<1>(first), std::get<1>(second), 1e-14);
}

TEST(ReedCouplingTest, AChannelWithAFieldThatIsNotPositiveActsAsAWall)
{
    ReedChannel reed = clarinet_reed();
    reed.width = 0.0;
    double warm_start = 1.0;

    const auto [pressure, flow, difference, status] = Acoustics::reed_mouth_step(
        reed, warm_start, 4.0e6, 1000.0, 0.0, 0.0);

    EXPECT_EQ(status, RootStatus::NOT_FINITE);
    EXPECT_DOUBLE_EQ(flow, 0.0);
    EXPECT_DOUBLE_EQ(difference, 1000.0);
}

TEST(ReedCouplingTest, ADynamicReedSettlesToTheQuasiStaticSolution)
{
    const ReedChannel reed = clarinet_reed();
    const double end_gain = 2.0 * coupling_gain();

    double warm_start = 0.0;
    const auto [expected_pressure, expected_flow, expected_difference, status] = Acoustics::reed_mouth_step(
        reed, warm_start, end_gain, 1000.0, 0.0, 0.0);
    ASSERT_EQ(status, RootStatus::CONVERGED);

    std::array<double, 2> motion {};
    double difference = 0.0;
    double flow = 0.0;
    for (size_t n = 0; n < 3000; ++n) {
        const auto step = Acoustics::dynamic_reed_mouth_step(reed, 20000.0, 20000.0, 0.0, 1.0 / kRate, motion, end_gain, 1000.0, 0.0, 0.0);
        flow = std::get<1>(step);
        difference = std::get<2>(step);
    }

    EXPECT_NEAR(difference, expected_difference, 1e-6 * expected_difference);
    EXPECT_NEAR(flow, expected_flow, 1e-6 * expected_flow);
    EXPECT_NEAR(motion[0], -reed.rest_opening * expected_difference / reed.closing_pressure, 1e-9);
}

TEST(ReedCouplingTest, ADynamicReedResonatesWhereTheQuasiStaticOneCannot)
{
    const ReedChannel reed = clarinet_reed();

    const auto swing = [&](double frequency) {
        std::array<double, 2> motion {};
        double largest = 0.0;
        for (size_t n = 0; n < 24000; ++n) {
            const double mouth = 100.0 * std::sin(2.0 * std::numbers::pi * frequency * static_cast<double>(n) / kRate);
            (void)Acoustics::dynamic_reed_mouth_step(reed, 2500.0, 3000.0, 0.0, 1.0 / kRate, motion, 1.0, mouth, 0.0, 0.0);
            if (n >= 19200) {
                largest = std::max(largest, std::abs(motion[0]));
            }
        }
        return largest;
    };

    const double low = swing(300.0);
    const double at_resonance = swing(2500.0);
    const double above = swing(9000.0);

    EXPECT_GT(low, 0.0);
    EXPECT_GT(at_resonance, 2.5 * low);
    EXPECT_LT(above, low);
}

TEST(ReedCouplingTest, ADynamicReedPressedToTheLayStaysThereAndPassesNoFlow)
{
    const ReedChannel reed = clarinet_reed();
    std::array<double, 2> motion {};

    double flow = 1.0;
    for (size_t n = 0; n < 200; ++n) {
        const auto step = Acoustics::dynamic_reed_mouth_step(reed, 2500.0, 3000.0, 0.0, 1.0 / kRate, motion, 4.0e6, 5000.0, 0.0, 0.0);
        flow = std::get<1>(step);
        EXPECT_GE(motion[0], -reed.rest_opening);
    }

    EXPECT_DOUBLE_EQ(flow, 0.0);
    EXPECT_DOUBLE_EQ(motion[0], -reed.rest_opening);
}

TEST(ReedCouplingTest, ADynamicReedWithAnInvalidResonanceActsAsAWall)
{
    const ReedChannel reed = clarinet_reed();
    std::array<double, 2> motion { 1.0e-5, 2.0e-5 };

    const auto [pressure, flow, difference] = Acoustics::dynamic_reed_mouth_step(
        reed, 0.0, 3000.0, 0.0, 1.0 / kRate, motion, 4.0e6, 1000.0, 0.0, 0.0);

    EXPECT_DOUBLE_EQ(flow, 0.0);
    EXPECT_DOUBLE_EQ(difference, 1000.0);
    EXPECT_DOUBLE_EQ(motion[0], 0.0);
    EXPECT_DOUBLE_EQ(motion[1], 0.0);
}

TEST(ReedCouplingTest, AMovingReedAddsItsOwnFlowAndTheCellStaysConsistent)
{
    const ReedChannel reed = clarinet_reed();
    const double end_gain = 2.0 * coupling_gain();
    constexpr double area = 1.46e-4;
    constexpr double downstream = 2.0e-5;

    std::array<double, 2> still {};
    std::array<double, 2> pushing {};
    double flow_still = 0.0;
    double flow_pushing = 0.0;
    double pressure_pushing = 0.0;
    for (size_t n = 0; n < 40; ++n) {
        const double mouth = 1500.0 + 300.0 * std::sin(2.0 * std::numbers::pi * 2000.0 * static_cast<double>(n) / kRate);
        flow_still = std::get<1>(Acoustics::dynamic_reed_mouth_step(reed, 2500.0, 8000.0, 0.0, 1.0 / kRate, still, end_gain, mouth, 100.0, downstream));
        const auto step = Acoustics::dynamic_reed_mouth_step(reed, 2500.0, 8000.0, area, 1.0 / kRate, pushing, end_gain, mouth, 100.0, downstream);
        pressure_pushing = std::get<0>(step);
        flow_pushing = std::get<1>(step);
    }

    EXPECT_NE(flow_pushing, flow_still);
    EXPECT_NEAR(pressure_pushing, 100.0 + end_gain * (flow_pushing - downstream), 1e-6 * std::abs(pressure_pushing));
}

TEST(ReedCouplingTest, ANegativeReedAreaMakesTheReedAWall)
{
    const ReedChannel reed = clarinet_reed();
    std::array<double, 2> motion {};

    const auto [pressure, flow, difference] = Acoustics::dynamic_reed_mouth_step(
        reed, 2500.0, 8000.0, -1.0, 1.0 / kRate, motion, 4.0e6, 1000.0, 0.0, 0.0);

    EXPECT_DOUBLE_EQ(flow, 0.0);
    EXPECT_DOUBLE_EQ(difference, 1000.0);
}

TEST(PressureFlowSolveTest, ALinearLawHasTheOhmicSolution)
{
    constexpr double conductance = 3.0e-3;
    const auto [difference, flow, status] = Acoustics::solve_pressure_flow(
        [conductance](double d) -> FlowSample { return { conductance * d, conductance }; },
        2.0e3, 3.0, -10.0, 10.0, 0.0);

    EXPECT_EQ(status, RootStatus::CONVERGED);
    EXPECT_NEAR(difference, 3.0 / (1.0 + 2.0e3 * conductance), 1e-10);
    EXPECT_NEAR(flow, conductance * difference, 1e-15);
}

TEST(PressureFlowSolveTest, ASaturatingLawIsBracketedByItsLargestFlow)
{
    constexpr double largest = 2.0e-3;
    constexpr double knee = 40.0;
    constexpr double gain = 1.5e4;
    constexpr double target = 25.0;

    const auto law = [](double d) -> FlowSample {
        const double t = std::tanh(d / knee);
        return { largest * t, largest * (1.0 - t * t) / knee };
    };

    const auto [difference, flow, status] = Acoustics::solve_pressure_flow(
        law, gain, target, target - gain * largest, target + gain * largest, 0.0);

    EXPECT_EQ(status, RootStatus::CONVERGED);
    EXPECT_NEAR(difference + gain * flow, target, 1e-9);
}

TEST(PressureFlowSolveTest, ABracketWithoutASignChangeIsReported)
{
    const auto [difference, flow, status] = Acoustics::solve_pressure_flow(
        [](double d) -> FlowSample { return { 1e-3 * d, 1e-3 }; }, 1.0, 500.0, -1.0, 1.0, 0.0);

    EXPECT_EQ(status, RootStatus::NO_BRACKET);
}

TEST(PressureFlowSolveTest, TheReedLawGivesTheSameSolutionAsTheRegularisedSolver)
{
    const ReedChannel reed = clarinet_reed();
    const double end_gain = 2.0 * coupling_gain();
    const double gain = Acoustics::flow_coupling_gain(end_gain);
    constexpr double mouth = 1500.0;
    constexpr double end = 1100.0;

    double warm_start = 0.0;
    const auto [pressure, reference_flow, reference_difference, reference_status] = Acoustics::reed_mouth_step(
        reed, warm_start, end_gain, mouth, end, 0.0);
    ASSERT_EQ(reference_status, RootStatus::CONVERGED);

    const double target = Acoustics::flow_coupling_target(mouth, end, gain, 0.0);
    const auto [difference, flow, status] = Acoustics::solve_pressure_flow(
        [&reed](double d) { return reed_law(reed, d); }, gain, target, -5000.0, 3000.0, 0.0);

    EXPECT_EQ(status, RootStatus::CONVERGED);
    EXPECT_NEAR(difference, reference_difference, 1e-6 * std::max(1.0, std::abs(reference_difference)));
    EXPECT_NEAR(flow, reference_flow, 1e-6 * std::abs(reference_flow));
    EXPECT_NEAR(Acoustics::centred_end_pressure(mouth, difference, end), pressure, 1e-5);
}

TEST(AerophoneSystemTest, ABlownCylinderSoundsAtItsQuarterWaveFrequency)
{
    const double quarter_wave = kSound / (4.0 * (kLength + 0.6133 * kRadius));

    for (const double mouth : { 1500.0, 2200.0 }) {
        const Blow result = blow(mouth, 3.0, 1.0);

        EXPECT_TRUE(result.finite);
        EXPECT_GT(result.peak_to_peak, 100.0) << mouth;
        EXPECT_NEAR(result.frequency, quarter_wave, 0.01 * quarter_wave) << mouth;
    }
}

TEST(AerophoneSystemTest, BelowAThirdOfTheClosingPressureTheNoteDiesAway)
{
    const Blow result = blow(800.0, 3.0, 2.5);

    EXPECT_TRUE(result.finite);
    EXPECT_LT(result.peak_to_peak, 1.0);
}

TEST(AerophoneSystemTest, TheOutputStaysFiniteAcrossTheBlowingRange)
{
    for (const double mouth : { 0.0, 300.0, 1000.0, 2000.0, 2850.0 }) {
        const Blow result = blow(mouth, 0.5, 0.25);

        EXPECT_TRUE(result.finite) << mouth;
        EXPECT_TRUE(std::isfinite(result.peak_to_peak)) << mouth;
    }
}

} // namespace MayaFlux::Test
