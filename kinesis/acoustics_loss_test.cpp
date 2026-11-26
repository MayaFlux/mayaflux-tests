#include "gtest/gtest.h"
#include "../support/test_metadata.hpp"

#include "MayaFlux/Kinesis/Acoustics/DuctGrid.hpp"
#include "MayaFlux/Kinesis/Acoustics/DuctLoss.hpp"
#include "MayaFlux/Kinesis/Acoustics/StaggeredLine.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {

namespace Acoustics = Kinesis::Acoustics;

namespace {

    constexpr double kAcousticsLossSound = 343.0;
    constexpr double kAcousticsLossDensity = 1.2;

    Acoustics::DuctGrid cone_grid()
    {
        const Acoustics::DuctSpec spec { .length = 0.6, .sample_rate = 48000.0 };
        return Acoustics::make_duct_grid(spec, [](double x) { return 0.005 + 0.04 * x; }).value();
    }

    std::vector<double> pseudo_random(size_t count, uint32_t seed)
    {
        std::vector<double> values(count);
        uint32_t state = seed;
        for (double& value : values) {
            state = state * 1664525U + 1013904223U;
            value = static_cast<double>(state >> 8) / static_cast<double>(1U << 24) - 0.5;
        }
        return values;
    }

    double inertance_norm(const Acoustics::DuctGrid& grid, std::span<const double> velocity)
    {
        double sum = 0.0;
        for (size_t l = 0; l < velocity.size(); ++l) {
            sum += velocity[l] * velocity[l] / grid.velocity_gain[l];
        }
        return sum;
    }

}

TEST(DuctLossTest, TheAttenuationFollowsTheKirchhoffScalings)
{
    const double base = Acoustics::viscothermal_attenuation(0.0075, 1000.0);

    EXPECT_NEAR(base, 2.984e-5 * std::sqrt(1000.0) / 0.0075, 0.01 * base);
    EXPECT_NEAR(Acoustics::viscothermal_attenuation(0.0075, 4000.0), 2.0 * base, 1e-12);
    EXPECT_NEAR(Acoustics::viscothermal_attenuation(0.015, 1000.0), 0.5 * base, 1e-12);
    EXPECT_DOUBLE_EQ(Acoustics::viscothermal_attenuation(0.0, 1000.0), 0.0);
    EXPECT_DOUBLE_EQ(Acoustics::viscothermal_attenuation(0.0075, -1.0), 0.0);
}

TEST(DuctLossTest, FrictionAndViscosityMatchTheAttenuationAtTheTwoFrequenciesOfTheFit)
{
    constexpr double radius = 0.0075;
    const auto [friction, viscosity] = Acoustics::viscothermal_rates(radius);

    const auto model = [&](double frequency) {
        const double omega = 2.0 * std::numbers::pi * frequency;
        return friction / (2.0 * kAcousticsLossSound) + viscosity * omega * omega / (2.0 * kAcousticsLossSound * kAcousticsLossSound * kAcousticsLossSound);
    };

    EXPECT_GT(friction, 0.0);
    EXPECT_GT(viscosity, 0.0);
    EXPECT_NEAR(model(150.0), Acoustics::viscothermal_attenuation(radius, 150.0), 1e-9);
    EXPECT_NEAR(model(3000.0), Acoustics::viscothermal_attenuation(radius, 3000.0), 1e-9);

    const auto [wide_friction, wide_viscosity] = Acoustics::viscothermal_rates(2.0 * radius);
    EXPECT_NEAR(wide_friction, 0.5 * friction, 1e-9 * friction);
    EXPECT_NEAR(wide_viscosity, 0.5 * viscosity, 1e-9 * viscosity);

    EXPECT_EQ(Acoustics::viscothermal_rates(radius, kAcousticsLossSound, kAcousticsLossDensity, 3000.0, 150.0), std::make_pair(0.0, 0.0));
    EXPECT_EQ(Acoustics::viscothermal_rates(-1.0), std::make_pair(0.0, 0.0));
}

TEST(DuctLossTest, ConfiguringNeedsAGridAndRatesThatAreNotNegative)
{
    const Acoustics::DuctGrid grid = cone_grid();
    Acoustics::DuctLoss loss;

    EXPECT_FALSE(loss.configure(Acoustics::DuctGrid {}, 1.0, 1.0));
    EXPECT_FALSE(loss.configure(grid, -1.0, 0.0));
    EXPECT_FALSE(loss.configure(grid, 0.0, std::numeric_limits<double>::quiet_NaN()));
    EXPECT_FALSE(loss.active());

    EXPECT_TRUE(loss.configure(grid, 0.0, 0.0));
    EXPECT_FALSE(loss.active());
    EXPECT_TRUE(loss.configure(grid, 10.0, 0.02));
    EXPECT_TRUE(loss.active());
    EXPECT_DOUBLE_EQ(loss.friction(), 10.0);
    EXPECT_DOUBLE_EQ(loss.viscosity(), 0.02);
}

TEST(DuctLossTest, WithNoRatesTheVelocitiesAreLeftAlone)
{
    const Acoustics::DuctGrid grid = cone_grid();
    Acoustics::DuctLoss loss;
    ASSERT_TRUE(loss.configure(grid, 0.0, 0.0));

    std::vector<double> velocity = pseudo_random(grid.intervals, 3);
    const std::vector<double> before = velocity;
    loss.apply(velocity);

    EXPECT_EQ(velocity, before);
}

TEST(DuctLossTest, FrictionAloneScalesEveryVelocityByTheSameFactor)
{
    const Acoustics::DuctGrid grid = cone_grid();
    constexpr double friction = 400.0;
    Acoustics::DuctLoss loss;
    ASSERT_TRUE(loss.configure(grid, friction, 0.0));

    std::vector<double> velocity = pseudo_random(grid.intervals, 5);
    const std::vector<double> before = velocity;
    loss.apply(velocity);

    for (size_t l = 0; l < velocity.size(); ++l) {
        EXPECT_NEAR(velocity[l], before[l] / (1.0 + grid.time_step * friction), 1e-12);
    }
}

TEST(DuctLossTest, ViscosityNeverRaisesTheInertanceWeightedEnergyAndSmoothsTheVelocities)
{
    const Acoustics::DuctGrid grid = cone_grid();
    Acoustics::DuctLoss loss;
    ASSERT_TRUE(loss.configure(grid, 20.0, 5.0));

    for (const uint32_t seed : { 1U, 2U, 3U, 4U }) {
        std::vector<double> velocity = pseudo_random(grid.intervals, seed);
        const double before = inertance_norm(grid, velocity);

        double roughness_before = 0.0;
        double roughness_after = 0.0;
        for (size_t l = 0; l + 1 < velocity.size(); ++l) {
            roughness_before += std::pow(velocity[l + 1] - velocity[l], 2.0);
        }

        loss.apply(velocity);
        for (size_t l = 0; l + 1 < velocity.size(); ++l) {
            roughness_after += std::pow(velocity[l + 1] - velocity[l], 2.0);
        }

        EXPECT_LT(inertance_norm(grid, velocity), before);
        EXPECT_LT(roughness_after, roughness_before);
    }
}

TEST(DuctLossTest, ASizeThatDoesNotMatchTheGridIsLeftAlone)
{
    const Acoustics::DuctGrid grid = cone_grid();
    Acoustics::DuctLoss loss;
    ASSERT_TRUE(loss.configure(grid, 400.0, 5.0));

    std::vector<double> velocity(grid.intervals - 1, 1.0);
    loss.apply(velocity);

    for (const double value : velocity) {
        EXPECT_DOUBLE_EQ(value, 1.0);
    }
}

TEST(DuctLossTest, ASealedDuctLosesEnergyAtTheRateOfItsMode)
{
    const Acoustics::DuctSpec spec { .length = 3.0, .sample_rate = 1000.0, .sound_speed = 300.0 };
    const Acoustics::DuctGrid grid = Acoustics::make_duct_grid(spec, [](double) { return 0.01; }).value();
    ASSERT_EQ(grid.intervals, 10U);

    constexpr double friction = 1.25;
    constexpr double viscosity = 0.5;
    Acoustics::DuctLoss loss;
    ASSERT_TRUE(loss.configure(grid, friction, viscosity));

    std::vector<double> pressure(grid.intervals + 1);
    std::vector<double> velocity(grid.intervals, 0.0);
    for (size_t l = 0; l <= grid.intervals; ++l) {
        pressure[l] = std::cos(std::numbers::pi * static_cast<double>(l) / static_cast<double>(grid.intervals));
    }

    constexpr size_t steps = 4000;
    double first = 0.0;
    double last = 0.0;
    for (size_t n = 0; n <= steps; ++n) {
        const std::vector<double> previous = velocity;
        Acoustics::advance_velocity(velocity, pressure, grid.velocity_gain);
        loss.apply(velocity);

        const double energy = Acoustics::discrete_energy(
            pressure, previous, velocity, grid.pressure_gain, grid.velocity_gain, grid.time_step);
        if (n == 0) {
            first = energy;
        }
        last = energy;

        Acoustics::advance_pressure(pressure, velocity, grid.pressure_gain);
        pressure.front() = Acoustics::end_cell_pressure(pressure.front(), grid.pressure_gain.front(), -velocity.front());
        pressure.back() = Acoustics::end_cell_pressure(pressure.back(), grid.pressure_gain.back(), velocity.back());
    }

    const double theta = std::numbers::pi / static_cast<double>(grid.intervals);
    const double eigenvalue = (2.0 - 2.0 * std::cos(theta)) / (grid.spacing * grid.spacing);
    const double expected = std::exp(-(friction + viscosity * eigenvalue) * static_cast<double>(steps) * grid.time_step);

    ASSERT_GT(first, 0.0);
    EXPECT_NEAR(last / first, expected, 0.03 * expected);
}

TEST(DuctLossTest, ASealedConeWithLossesDoesNotGrow)
{
    const Acoustics::DuctGrid grid = cone_grid();
    Acoustics::DuctLoss loss;
    ASSERT_TRUE(loss.configure(grid, 30.0, 0.04));

    std::vector<double> pressure = pseudo_random(grid.intervals + 1, 9);
    std::vector<double> velocity(grid.intervals, 0.0);

    double first = 0.0;
    double worst = 0.0;
    double last = 0.0;
    for (size_t n = 0; n < 4000; ++n) {
        const std::vector<double> previous = velocity;
        Acoustics::advance_velocity(velocity, pressure, grid.velocity_gain);
        loss.apply(velocity);

        last = Acoustics::discrete_energy(pressure, previous, velocity, grid.pressure_gain, grid.velocity_gain, grid.time_step);
        if (n == 0) {
            first = last;
        }
        worst = std::max(worst, last);

        Acoustics::advance_pressure(pressure, velocity, grid.pressure_gain);
        pressure.front() = Acoustics::end_cell_pressure(pressure.front(), grid.pressure_gain.front(), -velocity.front());
        pressure.back() = Acoustics::end_cell_pressure(pressure.back(), grid.pressure_gain.back(), velocity.back());
    }

    ASSERT_GT(first, 0.0);
    EXPECT_LE(worst, 1.05 * first);
    EXPECT_LT(last, 0.5 * first);
}

} // namespace MayaFlux::Test
