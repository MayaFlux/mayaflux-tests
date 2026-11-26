#include "gtest/gtest.h"
#include "../support/test_metadata.hpp"

#include "MayaFlux/Kinesis/Acoustics/DuctGrid.hpp"
#include "MayaFlux/Kinesis/Acoustics/Radiation.hpp"
#include "MayaFlux/Kinesis/Acoustics/StaggeredLine.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {

namespace Acoustics = Kinesis::Acoustics;
using Acoustics::RadiationModel;
using Acoustics::RadiationSpec;
using Acoustics::RadiationState;
using Complex = std::complex<double>;

namespace {

    constexpr double kAcousticsRadiationRate = 48000.0;
    constexpr double kAcousticsRadiationSound = 343.0;
    constexpr double kAcousticsRadiationDensity = 1.2;
    constexpr double kAcousticsRadiationRadius = 0.0075;

    Complex load_impedance(const RadiationModel& model, Complex s)
    {
        const auto [gamma, lambda, theta] = model;
        const Complex numerator = lambda * s * (1.0 + gamma + gamma * theta * s);
        const Complex denominator = 1.0 + gamma + (lambda + gamma * theta) * s + gamma * lambda * theta * s * s;
        return numerator / denominator;
    }

    Complex silva_reflection(double n1, double d1, double d2, double ka)
    {
        const Complex s(0.0, ka);
        return -(1.0 - n1 * s) / (1.0 - d1 * s + d2 * s * s);
    }

    Complex model_reflection(const RadiationModel& model, double ka)
    {
        const Complex z = load_impedance(model, Complex(0.0, ka));
        return (z - 1.0) / (z + 1.0);
    }

    RadiationSpec spec_for(const RadiationModel& model)
    {
        return { .model = model, .radius = kAcousticsRadiationRadius, .sound_speed = kAcousticsRadiationSound, .air_density = kAcousticsRadiationDensity, .time_step = 1.0 / kAcousticsRadiationRate };
    }

}

TEST(RadiationLoadTest, TheCircuitConstantsMatchTheUnflangedFitOfBilbaoAndChick)
{
    const auto [gamma, lambda, theta] = Acoustics::unflanged_radiation;

    EXPECT_NEAR(gamma, 0.505, 2e-3);
    EXPECT_NEAR(lambda, 0.613, 1e-3);
    EXPECT_NEAR(theta, 1.111, 2e-3);
}

TEST(RadiationLoadTest, TheFlangedEndHasTheLargerEndCorrection)
{
    const auto [gamma, lambda, theta] = Acoustics::flanged_radiation;

    EXPECT_NEAR(lambda, 0.8216, 1e-3);
    EXPECT_NEAR(gamma, 0.350, 2e-3);
    EXPECT_NEAR(theta, 1.523, 5e-3);
    EXPECT_GT(lambda, std::get<1>(Acoustics::unflanged_radiation));
}

TEST(RadiationLoadTest, TheNetworkIsTheSilvaPadeReflectionAtEveryFrequency)
{
    for (const double ka : { 0.0, 0.01, 0.1, 0.5, 1.0, 2.0, 3.0, 10.0 }) {
        EXPECT_NEAR(std::abs(model_reflection(Acoustics::unflanged_radiation, ka)
                        - silva_reflection(0.167, 1.393, 0.457, -ka)),
            0.0, 1e-12)
            << "unflanged ka " << ka;
        EXPECT_NEAR(std::abs(model_reflection(Acoustics::flanged_radiation, ka)
                        - silva_reflection(0.182, 1.825, 0.649, -ka)),
            0.0, 1e-12)
            << "flanged ka " << ka;
    }
}

TEST(RadiationLoadTest, LowFrequencyLimitsAreTheEndCorrectionAndTheRadiationResistance)
{
    constexpr double ka = 1e-3;
    const Complex z = load_impedance(Acoustics::unflanged_radiation, Complex(0.0, ka));

    EXPECT_NEAR(z.imag() / ka, 0.6133, 1e-3);
    EXPECT_NEAR(z.real() / (ka * ka), 0.25, 5e-3);
    EXPECT_NEAR(1.0 - std::abs(model_reflection(Acoustics::unflanged_radiation, ka)), 0.5 * ka * ka, 1e-8);
}

TEST(RadiationLoadTest, TheLoadPassesHighFrequenciesAndReflectsLowOnes)
{
    EXPECT_GT(std::abs(model_reflection(Acoustics::unflanged_radiation, 0.05)), 0.99);
    EXPECT_LT(std::abs(model_reflection(Acoustics::unflanged_radiation, 50.0)), 0.1);
}

TEST(RadiationLoadTest, CoefficientsNeedPositiveFiniteInputs)
{
    const RadiationSpec spec = spec_for(Acoustics::unflanged_radiation);
    const double area = std::numbers::pi * kAcousticsRadiationRadius * kAcousticsRadiationRadius;

    EXPECT_TRUE(Acoustics::make_radiation_coefficients(spec, area, 1.0e6).has_value());

    EXPECT_FALSE(Acoustics::make_radiation_coefficients(spec, 0.0, 1.0e6).has_value());
    EXPECT_FALSE(Acoustics::make_radiation_coefficients(spec, area, 0.0).has_value());
    EXPECT_FALSE(Acoustics::make_radiation_coefficients(spec, std::numeric_limits<double>::quiet_NaN(), 1.0e6).has_value());

    RadiationSpec broken = spec;
    broken.radius = 0.0;
    EXPECT_FALSE(Acoustics::make_radiation_coefficients(broken, area, 1.0e6).has_value());
    broken = spec;
    broken.time_step = -1.0;
    EXPECT_FALSE(Acoustics::make_radiation_coefficients(broken, area, 1.0e6).has_value());
    broken = spec;
    broken.model = { 0.0, 0.6, 1.1 };
    EXPECT_FALSE(Acoustics::make_radiation_coefficients(broken, area, 1.0e6).has_value());
}

TEST(RadiationLoadTest, SteadyStateAdmittanceIsTheTrapezoidalImageOfTheNetwork)
{
    const double area = std::numbers::pi * kAcousticsRadiationRadius * kAcousticsRadiationRadius;
    const double impedance = kAcousticsRadiationDensity * kAcousticsRadiationSound / area;
    const double k = 1.0 / kAcousticsRadiationRate;

    for (const RadiationModel& model : { Acoustics::unflanged_radiation, Acoustics::flanged_radiation }) {
        const auto coefficients = Acoustics::make_radiation_coefficients(spec_for(model), area, 1e-30);
        ASSERT_TRUE(coefficients.has_value());

        for (const size_t period : { size_t { 96 }, size_t { 24 }, size_t { 8 } }) {
            const double omega = 2.0 * std::numbers::pi * kAcousticsRadiationRate / static_cast<double>(period);
            RadiationState state;

            Complex pressure_sum;
            Complex flow_sum;
            const size_t settle = 40 * period;
            const size_t measured = 20 * period;
            for (size_t n = 0; n < settle + measured; ++n) {
                const double half_time = (static_cast<double>(n) + 0.5) * k;
                const double pressure = std::sin(omega * half_time);
                const auto [unused, flow] = Acoustics::radiation_step(*coefficients, state, pressure, 0.0);

                if (n >= settle) {
                    const Complex phasor = std::polar(1.0, -omega * half_time);
                    pressure_sum += pressure * phasor;
                    flow_sum += flow * phasor;
                }
            }

            const double warped = 2.0 / k * std::tan(omega * k / 2.0);
            const Complex expected = 1.0 / (impedance * load_impedance(model, Complex(0.0, warped * kAcousticsRadiationRadius / kAcousticsRadiationSound)));
            const Complex measured_admittance = flow_sum / pressure_sum;

            EXPECT_NEAR(std::abs(measured_admittance - expected), 0.0, 1e-6 * std::abs(expected))
                << "period " << period;
        }
    }
}

TEST(RadiationLoadTest, ABoreWithAnOpenEndOnlyEverLosesEnergy)
{
    Acoustics::DuctSpec bore { .length = 0.6, .sample_rate = kAcousticsRadiationRate };
    const auto grid = Acoustics::make_duct_grid(bore, [](double) { return kAcousticsRadiationRadius; });
    ASSERT_TRUE(grid.has_value());

    const double area = std::numbers::pi * kAcousticsRadiationRadius * kAcousticsRadiationRadius;
    const auto coefficients = Acoustics::make_radiation_coefficients(
        spec_for(Acoustics::unflanged_radiation), area, grid->pressure_gain.back());
    ASSERT_TRUE(coefficients.has_value());

    const auto [gamma, lambda, theta] = Acoustics::unflanged_radiation;
    const double impedance = kAcousticsRadiationDensity * kAcousticsRadiationSound / area;
    const double inductance = lambda * (kAcousticsRadiationRadius / kAcousticsRadiationSound) * impedance;
    const double capacitance = theta * (kAcousticsRadiationRadius / kAcousticsRadiationSound) / impedance;

    std::vector<double> pressure(grid->intervals + 1, 0.0);
    std::vector<double> velocity(grid->intervals, 0.0);
    for (size_t l = 10; l < 14; ++l) {
        pressure[l] = 100.0;
    }

    RadiationState state;
    std::vector<double> total;
    for (size_t n = 0; n < 6000; ++n) {
        const std::vector<double> previous = velocity;
        Acoustics::advance_velocity(velocity, pressure, grid->velocity_gain);

        const double bore_energy = Acoustics::discrete_energy(
            pressure, previous, velocity, grid->pressure_gain, grid->velocity_gain, grid->time_step);
        const double load_energy = 0.5 * inductance * state.inductor_flow * state.inductor_flow
            + 0.5 * capacitance * state.branch_pressure * state.branch_pressure;
        total.push_back(bore_energy + load_energy);

        Acoustics::advance_pressure(pressure, velocity, grid->pressure_gain);
        pressure.front() = Acoustics::end_cell_pressure(pressure.front(), grid->pressure_gain.front(), -velocity.front());
        const auto [end_pressure, flow] = Acoustics::radiation_step(*coefficients, state, pressure.back(), velocity.back());
        pressure.back() = end_pressure;
    }

    ASSERT_GT(total.front(), 0.0);
    for (size_t n = 1; n < total.size(); ++n) {
        EXPECT_LE(total[n], total[n - 1] * (1.0 + 1e-12)) << "step " << n;
    }
    EXPECT_LT(total.back(), 0.99 * total.front());
}

TEST(RadiationLoadTest, RadiatedPressureIsTheRateOfChangeOfFlowOverFourPiR)
{
    const double pressure = Acoustics::radiated_pressure(3.0, 1.0, 1.0 / kAcousticsRadiationRate, kAcousticsRadiationDensity);

    EXPECT_NEAR(pressure, kAcousticsRadiationDensity / (4.0 * std::numbers::pi) * 2.0 * kAcousticsRadiationRate, 1e-9);
    EXPECT_NEAR(Acoustics::radiated_pressure(3.0, 1.0, 1.0 / kAcousticsRadiationRate, kAcousticsRadiationDensity, 2.0), 0.5 * pressure, 1e-9);
    EXPECT_DOUBLE_EQ(Acoustics::radiated_pressure(1.0, 1.0, 1.0 / kAcousticsRadiationRate, kAcousticsRadiationDensity), 0.0);
}

} // namespace MayaFlux::Test
