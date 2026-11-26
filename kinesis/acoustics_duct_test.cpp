#include "gtest/gtest.h"
#include "../support/test_metadata.hpp"

#include "MayaFlux/Kinesis/Acoustics/DuctGrid.hpp"
#include "MayaFlux/Kinesis/Acoustics/StaggeredLine.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {

namespace Acoustics = Kinesis::Acoustics;
using Acoustics::DuctError;
using Acoustics::DuctGrid;
using Acoustics::DuctSpec;

namespace {

    constexpr double kAcousticsDuctRadius = 0.0075;

    DuctSpec flute_spec()
    {
        return { .length = 0.6, .sample_rate = 48000.0 };
    }

    DuctGrid cylinder(const DuctSpec& spec, double radius = kAcousticsDuctRadius)
    {
        const auto grid = Acoustics::make_duct_grid(spec, [radius](double) { return radius; });
        return grid.value();
    }

    std::vector<double> uneven_pressure(size_t points)
    {
        std::vector<double> pressure(points);
        for (size_t l = 0; l < points; ++l) {
            const double x = static_cast<double>(l);
            pressure[l] = std::sin(0.7 * x) + 0.2 * std::cos(2.9 * x) + 0.05 * x;
        }
        return pressure;
    }

    struct Line {
        DuctGrid grid;
        std::vector<double> pressure;
        std::vector<double> velocity;

        explicit Line(DuctGrid prepared)
            : grid(std::move(prepared))
            , pressure(uneven_pressure(grid.intervals + 1))
            , velocity(grid.intervals, 0.0)
        {
        }

        void step()
        {
            Acoustics::advance_velocity(velocity, pressure, grid.velocity_gain);
            Acoustics::advance_pressure(pressure, velocity, grid.pressure_gain);
            pressure.front() = Acoustics::end_cell_pressure(pressure.front(), grid.pressure_gain.front(), -velocity.front());
            pressure.back() = Acoustics::end_cell_pressure(pressure.back(), grid.pressure_gain.back(), velocity.back());
        }

        double energy_then_step()
        {
            const std::vector<double> previous = velocity;
            Acoustics::advance_velocity(velocity, pressure, grid.velocity_gain);
            const double energy = Acoustics::discrete_energy(
                pressure, previous, velocity, grid.pressure_gain, grid.velocity_gain, grid.time_step);
            Acoustics::advance_pressure(pressure, velocity, grid.pressure_gain);
            pressure.front() = Acoustics::end_cell_pressure(pressure.front(), grid.pressure_gain.front(), -velocity.front());
            pressure.back() = Acoustics::end_cell_pressure(pressure.back(), grid.pressure_gain.back(), velocity.back());
            return energy;
        }
    };

}

TEST(DuctGridTest, ACylinderGetsTheLargestGridThatKeepsTheCourantNumberUnderOne)
{
    const DuctSpec spec = flute_spec();
    const DuctGrid grid = cylinder(spec);

    EXPECT_EQ(Acoustics::duct_intervals(spec), 83U);
    EXPECT_EQ(grid.intervals, 83U);
    EXPECT_NEAR(grid.spacing, 0.6 / 83.0, 1e-15);
    EXPECT_NEAR(grid.courant, 343.0 / 48000.0 / (0.6 / 83.0), 1e-12);
    EXPECT_NEAR(grid.courant, 0.9885, 1e-4);
    EXPECT_LE(grid.courant, 1.0);
    EXPECT_NEAR(grid.bandwidth, 48000.0 * std::asin(grid.courant) / std::numbers::pi, 1e-9);
    EXPECT_DOUBLE_EQ(grid.start_radius, kAcousticsDuctRadius);
    EXPECT_DOUBLE_EQ(grid.end_radius, kAcousticsDuctRadius);
}

TEST(DuctGridTest, GainsFollowCellVolumesAndEndCellsAreHalfCells)
{
    const DuctSpec spec = flute_spec();
    const DuctGrid grid = cylinder(spec);

    const double area = std::numbers::pi * kAcousticsDuctRadius * kAcousticsDuctRadius;
    const double velocity_gain = grid.time_step * area / (spec.air_density * grid.spacing);
    const double interior = spec.air_density * spec.sound_speed * spec.sound_speed * grid.time_step
        / (grid.spacing * area);

    ASSERT_EQ(grid.velocity_gain.size(), grid.intervals);
    ASSERT_EQ(grid.pressure_gain.size(), grid.intervals + 1);
    for (const double gain : grid.velocity_gain) {
        EXPECT_NEAR(gain, velocity_gain, 1e-12 * velocity_gain);
    }
    for (size_t l = 1; l < grid.intervals; ++l) {
        EXPECT_NEAR(grid.pressure_gain[l], interior, 1e-12 * interior);
    }
    EXPECT_NEAR(grid.pressure_gain.front(), 2.0 * interior, 1e-12 * interior);
    EXPECT_NEAR(grid.pressure_gain.back(), 2.0 * interior, 1e-12 * interior);
}

TEST(DuctGridTest, ACallableAndASampledProfileOfACone)
{
    const DuctSpec spec = flute_spec();
    const auto from_function = Acoustics::make_duct_grid(spec, [](double x) { return 0.005 + 0.01 * x; });
    const std::array<double, 2> ends { 0.005, 0.011 };
    const auto from_samples = Acoustics::make_duct_grid(spec, ends);

    ASSERT_TRUE(from_function.has_value());
    ASSERT_TRUE(from_samples.has_value());
    ASSERT_EQ(from_function->area.size(), from_samples->area.size());
    for (size_t l = 0; l < from_function->area.size(); ++l) {
        EXPECT_NEAR(from_function->area[l], from_samples->area[l], 1e-12 * from_samples->area[l]);
    }
    EXPECT_GT(from_function->area.back(), from_function->area.front());
    EXPECT_NEAR(from_function->end_radius, 0.011, 1e-15);
    EXPECT_GT(from_function->pressure_gain.front(), from_function->pressure_gain.back());
}

TEST(DuctGridTest, SpecsAndProfilesThatCannotBuildAGridSayWhy)
{
    const auto unit = [](double) { return kAcousticsDuctRadius; };

    DuctSpec spec = flute_spec();
    spec.length = 0.0;
    EXPECT_EQ(Acoustics::make_duct_grid(spec, unit).error(), DuctError::INVALID_SPEC);

    spec = flute_spec();
    spec.sound_speed = -343.0;
    EXPECT_EQ(Acoustics::make_duct_grid(spec, unit).error(), DuctError::INVALID_SPEC);

    spec = flute_spec();
    spec.sample_rate = std::numeric_limits<double>::quiet_NaN();
    EXPECT_EQ(Acoustics::make_duct_grid(spec, unit).error(), DuctError::INVALID_SPEC);
    EXPECT_EQ(Acoustics::duct_intervals(spec), 0U);

    spec = flute_spec();
    spec.length = 0.01;
    EXPECT_EQ(Acoustics::make_duct_grid(spec, unit).error(), DuctError::TOO_SHORT);

    spec = flute_spec();
    spec.length = 0.001;
    EXPECT_EQ(Acoustics::make_duct_grid(spec, unit).error(), DuctError::TOO_SHORT);

    EXPECT_EQ(Acoustics::make_duct_grid(flute_spec(), [](double) { return 0.0; }).error(), DuctError::INVALID_PROFILE);
    EXPECT_EQ(Acoustics::make_duct_grid(flute_spec(), [](double) { return std::numeric_limits<double>::quiet_NaN(); }).error(),
        DuctError::INVALID_PROFILE);

    const std::array<double, 1> one { 0.01 };
    const std::array<double, 2> flat { 0.01, -0.01 };
    EXPECT_EQ(Acoustics::make_duct_grid(flute_spec(), one).error(), DuctError::INVALID_PROFILE);
    EXPECT_EQ(Acoustics::make_duct_grid(flute_spec(), flat).error(), DuctError::INVALID_PROFILE);
}

TEST(DuctGridTest, TheCourantNumberStaysInTheTopOfItsRangeAcrossLengthsAndRates)
{
    for (const double rate : { 44100.0, 48000.0, 96000.0 }) {
        for (double length = 0.1; length < 2.0; length += 0.0731) {
            const DuctSpec spec { .length = length, .sample_rate = rate };
            const auto grid = Acoustics::make_duct_grid(spec, [](double) { return kAcousticsDuctRadius; });
            ASSERT_TRUE(grid.has_value());

            const auto n = static_cast<double>(grid->intervals);
            EXPECT_LE(grid->courant, 1.0);
            EXPECT_GE(grid->courant, n / (n + 1.0) - 1e-12);
        }
    }
}

TEST(DuctGridTest, AMouthVolumeAddsComplianceToTheFirstCellOnly)
{
    const DuctSpec plain = flute_spec();
    DuctSpec with_volume = flute_spec();
    with_volume.mouth_volume = 2.0e-6;

    const DuctGrid a = Acoustics::make_duct_grid(plain, [](double) { return kAcousticsDuctRadius; }).value();
    const DuctGrid b = Acoustics::make_duct_grid(with_volume, [](double) { return kAcousticsDuctRadius; }).value();

    const double area = std::numbers::pi * kAcousticsDuctRadius * kAcousticsDuctRadius;
    const double stiffness = plain.air_density * plain.sound_speed * plain.sound_speed * a.time_step;

    EXPECT_NEAR(b.pressure_gain.front(), stiffness / (0.5 * a.spacing * area + 2.0e-6), 1e-12 * b.pressure_gain.front());
    EXPECT_LT(b.pressure_gain.front(), a.pressure_gain.front());
    for (size_t l = 1; l <= a.intervals; ++l) {
        EXPECT_DOUBLE_EQ(b.pressure_gain[l], a.pressure_gain[l]);
    }
    EXPECT_EQ(b.velocity_gain, a.velocity_gain);
}

TEST(DuctGridTest, ANegativeOrNonFiniteMouthVolumeIsAnInvalidSpec)
{
    DuctSpec spec = flute_spec();

    spec.mouth_volume = -1.0e-6;
    EXPECT_EQ(Acoustics::make_duct_grid(spec, [](double) { return kAcousticsDuctRadius; }).error(), DuctError::INVALID_SPEC);

    spec.mouth_volume = std::numeric_limits<double>::infinity();
    EXPECT_EQ(Acoustics::make_duct_grid(spec, [](double) { return kAcousticsDuctRadius; }).error(), DuctError::INVALID_SPEC);
}

TEST(StaggeredLineTest, ASealedCylinderConservesItsDiscreteEnergy)
{
    Line line(cylinder(flute_spec()));

    std::vector<double> energy;
    for (size_t n = 0; n < 2000; ++n) {
        energy.push_back(line.energy_then_step());
    }

    ASSERT_GT(energy.front(), 0.0);
    for (const double value : energy) {
        EXPECT_NEAR(value, energy.front(), 1e-9 * energy.front());
    }
}

TEST(StaggeredLineTest, ASealedConeConservesItsDiscreteEnergy)
{
    const auto cone = Acoustics::make_duct_grid(flute_spec(), [](double x) { return 0.01 + 0.04 * x; });
    ASSERT_TRUE(cone.has_value());
    Line line(*cone);

    std::vector<double> energy;
    for (size_t n = 0; n < 2000; ++n) {
        energy.push_back(line.energy_then_step());
    }

    ASSERT_GT(energy.front(), 0.0);
    for (const double value : energy) {
        EXPECT_NEAR(value, energy.front(), 1e-9 * energy.front());
    }
}

TEST(StaggeredLineTest, WithACourantNumberOfOneASealedCylinderRepeatsExactly)
{
    const DuctSpec spec { .length = 3.0, .sample_rate = 1000.0, .sound_speed = 300.0 };
    Line line(cylinder(spec, 0.01));
    ASSERT_EQ(line.grid.intervals, 10U);
    ASSERT_NEAR(line.grid.courant, 1.0, 1e-12);

    const size_t n = line.grid.intervals;
    for (size_t l = 0; l <= n; ++l) {
        const double phase = std::numbers::pi * static_cast<double>(l) / static_cast<double>(n);
        line.pressure[l] = std::cos(phase) + 0.5 * std::cos(2.0 * phase) + 0.25 * std::cos(3.0 * phase);
    }
    const std::vector<double> start = line.pressure;

    for (size_t step = 0; step < n; ++step) {
        line.step();
    }
    for (size_t l = 0; l <= n; ++l) {
        EXPECT_NEAR(line.pressure[l], start[n - l], 1e-9) << "mirror at point " << l;
    }

    for (size_t step = 0; step < n; ++step) {
        line.step();
    }
    for (size_t l = 0; l <= n; ++l) {
        EXPECT_NEAR(line.pressure[l], start[l], 1e-9) << "period at point " << l;
    }
}

TEST(StaggeredLineTest, KernelsIgnoreSpansThatDoNotMatch)
{
    std::vector<double> none;
    std::vector<double> pressure { 1.0, 2.0, 3.0 };
    std::vector<double> velocity { 0.0, 0.0 };
    const std::vector<double> gain { 0.5, 0.5, 0.5 };

    Acoustics::advance_velocity(none, none, none);
    Acoustics::advance_pressure(none, none, none);
    Acoustics::advance_velocity(velocity, pressure, std::span<const double>(gain).first(1));
    EXPECT_DOUBLE_EQ(velocity[0], -0.5);
    EXPECT_DOUBLE_EQ(velocity[1], 0.0);

    Acoustics::advance_pressure(pressure, velocity, std::span<const double>(gain).first(2));
    EXPECT_DOUBLE_EQ(pressure[0], 1.0);
    EXPECT_DOUBLE_EQ(pressure[2], 3.0);
    EXPECT_DOUBLE_EQ(Acoustics::discrete_energy({}, {}, {}, {}, {}, 1.0), 0.0);
}

TEST(StaggeredLineTest, ThePickupInterpolatesAndClamps)
{
    const std::vector<double> pressure { 0.0, 10.0, 20.0 };

    EXPECT_DOUBLE_EQ(Acoustics::sample_pressure(pressure, 0.0), 0.0);
    EXPECT_DOUBLE_EQ(Acoustics::sample_pressure(pressure, 0.25), 5.0);
    EXPECT_DOUBLE_EQ(Acoustics::sample_pressure(pressure, 0.5), 10.0);
    EXPECT_DOUBLE_EQ(Acoustics::sample_pressure(pressure, 1.0), 20.0);
    EXPECT_DOUBLE_EQ(Acoustics::sample_pressure(pressure, 3.0), 20.0);
    EXPECT_DOUBLE_EQ(Acoustics::sample_pressure(pressure, -1.0), 0.0);
    EXPECT_DOUBLE_EQ(Acoustics::sample_pressure({}, 0.5), 0.0);
    EXPECT_DOUBLE_EQ(Acoustics::sample_pressure(std::span<const double>(pressure).first(1), 0.5), 0.0);
}

TEST(StaggeredLineTest, AnEndCellMovesByItsGainTimesTheNetInflow)
{
    EXPECT_DOUBLE_EQ(Acoustics::end_cell_pressure(100.0, 2.0, 3.0), 106.0);
    EXPECT_DOUBLE_EQ(Acoustics::end_cell_pressure(100.0, 2.0, -3.0), 94.0);
}

TEST(StaggeredLineTest, ResamplingKeepsALinearRampAndTheEndsOnBothKindsOfLine)
{
    std::vector<double> points(11);
    std::vector<double> halves(10);
    for (size_t i = 0; i < points.size(); ++i) {
        points[i] = 2.0 * static_cast<double>(i) / 10.0 + 1.0;
    }
    for (size_t i = 0; i < halves.size(); ++i) {
        halves[i] = 2.0 * (static_cast<double>(i) + 0.5) / 10.0 + 1.0;
    }

    std::vector<double> more_points(26);
    std::vector<double> more_halves(25);
    Acoustics::resample_line(points, more_points, 0.0);
    Acoustics::resample_line(halves, more_halves, 0.5);

    EXPECT_DOUBLE_EQ(more_points.front(), 1.0);
    EXPECT_DOUBLE_EQ(more_points.back(), 3.0);
    for (size_t i = 0; i < more_points.size(); ++i) {
        EXPECT_NEAR(more_points[i], 2.0 * static_cast<double>(i) / 25.0 + 1.0, 1e-12);
    }
    EXPECT_DOUBLE_EQ(more_halves.front(), halves.front());
    EXPECT_DOUBLE_EQ(more_halves.back(), halves.back());
    for (size_t i = 3; i + 3 < more_halves.size(); ++i) {
        EXPECT_NEAR(more_halves[i], 2.0 * (static_cast<double>(i) + 0.5) / 25.0 + 1.0, 1e-12);
    }
}

TEST(StaggeredLineTest, ResamplingToTheSameSizeIsTheIdentityAndAnEmptySourceGivesZeros)
{
    const std::vector<double> source { 3.0, -1.0, 4.0, 1.0, -5.0, 9.0 };
    std::vector<double> same(source.size());
    Acoustics::resample_line(source, same, 0.0);
    for (size_t i = 0; i < source.size(); ++i) {
        EXPECT_NEAR(same[i], source[i], 1e-12);
    }

    std::vector<double> target(4, 7.0);
    Acoustics::resample_line({}, target, 0.5);
    for (const double value : target) {
        EXPECT_DOUBLE_EQ(value, 0.0);
    }

    std::vector<double> single(1, 0.0);
    Acoustics::resample_line(source, single, 0.0);
    EXPECT_DOUBLE_EQ(single.front(), source.front());
}

} // namespace MayaFlux::Test
