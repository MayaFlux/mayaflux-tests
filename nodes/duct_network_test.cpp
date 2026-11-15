#include "gtest/gtest.h"
#include "../support/test_metadata.hpp"

#include "MayaFlux/Nodes/Conduit/Constant.hpp"
#include "MayaFlux/Nodes/Network/DuctNetwork.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {

namespace Acoustics = Kinesis::Acoustics;
using Nodes::Constant;
using Nodes::Network::DuctEnd;
using Nodes::Network::DuctNetwork;
using Nodes::Network::JunctionOperator;
using Nodes::Network::RadiationOperator;
using Nodes::Network::ReedOperator;
using Nodes::Network::WallOperator;

namespace {

    constexpr double kRate = 48000.0;
    constexpr double kRadius = 0.0075;
    constexpr double kLength = 0.6;
    constexpr unsigned int kBlock = 512;

    Acoustics::DuctSpec spec()
    {
        return { .length = kLength, .sample_rate = kRate };
    }

    Acoustics::DuctGrid grid()
    {
        return Acoustics::make_duct_grid(spec(), [](double) { return kRadius; }).value();
    }

    std::vector<double> render(DuctNetwork& network, unsigned int blocks)
    {
        std::vector<double> out;
        for (unsigned int block = 0; block < blocks; ++block) {
            network.process_batch(kBlock);
            const auto buffer = network.get_audio_buffer().value_or(std::vector<double> {});
            out.insert(out.end(), buffer.begin(), buffer.end());
        }
        return out;
    }

    double peak_to_peak(std::span<const double> samples)
    {
        const auto [low, high] = std::ranges::minmax(samples);
        return high - low;
    }

}

TEST(TerminationOperatorTest, AWallPassesNoFlowAndMovesTheCellByTheInteriorFlow)
{
    const Acoustics::DuctGrid duct = grid();
    WallOperator wall;
    ASSERT_TRUE(wall.accepts(duct, spec(), DuctEnd::MOUTH));
    wall.prepare(duct, spec(), DuctEnd::MOUTH);

    const auto [pressure, inflow] = wall.couple(120.0, 2.0e-5);

    EXPECT_DOUBLE_EQ(inflow, 0.0);
    EXPECT_DOUBLE_EQ(pressure, Acoustics::end_cell_pressure(120.0, duct.pressure_gain.front(), -2.0e-5));
}

TEST(TerminationOperatorTest, NoTerminationAcceptsAnEmptyGrid)
{
    const Acoustics::DuctGrid empty;
    const Acoustics::DuctSpec none;

    EXPECT_FALSE(WallOperator {}.accepts(empty, none, DuctEnd::MOUTH));
    EXPECT_FALSE(ReedOperator {}.accepts(empty, none, DuctEnd::MOUTH));
    EXPECT_FALSE(RadiationOperator {}.accepts(empty, none, DuctEnd::BELL));
    EXPECT_FALSE(JunctionOperator([](double) -> Acoustics::FlowSample { return { 0.0, 0.0 }; }, 1.0).accepts(empty, none, DuctEnd::MOUTH));
}

TEST(TerminationOperatorTest, TheReedGivesTheSamplesOfTheKinesisCoupling)
{
    const Acoustics::DuctGrid duct = grid();
    ReedOperator reed;
    reed.set_parameter("smoothing", 0.0);
    reed.set_parameter("pressure", 1500.0);
    ASSERT_TRUE(reed.accepts(duct, spec(), DuctEnd::MOUTH));
    reed.prepare(duct, spec(), DuctEnd::MOUTH);

    const Acoustics::ReedChannel channel { .width = 0.012, .rest_opening = 4.0e-4, .closing_pressure = 3000.0, .air_density = 1.2 };
    double warm_start = 0.0;

    double pressure = 0.0;
    for (const double interior : { 0.0, 1.0e-4, 3.0e-5, -2.0e-5, 8.0e-5 }) {
        const auto [expected_pressure, expected_flow, difference, status] = Acoustics::reed_mouth_step(
            channel, warm_start, duct.pressure_gain.front(), 1500.0, pressure, interior);
        const auto [actual_pressure, actual_flow] = reed.couple(pressure, interior);

        EXPECT_DOUBLE_EQ(actual_pressure, expected_pressure);
        EXPECT_DOUBLE_EQ(actual_flow, expected_flow);
        pressure = expected_pressure;
    }
}

TEST(TerminationOperatorTest, TheReedSmoothsTheBlowingPressureAndValidatesItsParameters)
{
    const Acoustics::DuctGrid duct = grid();

    ReedOperator instant;
    instant.set_parameter("smoothing", 0.0);
    instant.set_parameter("pressure", 1500.0);
    instant.prepare(duct, spec(), DuctEnd::MOUTH);

    ReedOperator slow;
    slow.set_parameter("smoothing", 0.1);
    slow.set_parameter("pressure", 1500.0);
    slow.prepare(duct, spec(), DuctEnd::MOUTH);

    EXPECT_LT(slow.couple(0.0, 0.0).second, instant.couple(0.0, 0.0).second);

    slow.set_parameter("closing_pressure", -5.0);
    slow.set_parameter("opening", std::numeric_limits<double>::quiet_NaN());
    slow.set_parameter("unknown", 1.0);
    EXPECT_DOUBLE_EQ(slow.query_state("closing_pressure").value(), 3000.0);
    EXPECT_DOUBLE_EQ(slow.query_state("opening").value(), 4.0e-4);
    EXPECT_FALSE(slow.query_state("unknown").has_value());
    EXPECT_DOUBLE_EQ(slow.query_state("pressure").value(), 1500.0);
}

TEST(TerminationOperatorTest, TheReedReportsWhenTheCouplingStopsBeingUnique)
{
    const Acoustics::DuctGrid duct = grid();
    ReedOperator reed;
    reed.prepare(duct, spec(), DuctEnd::MOUTH);
    EXPECT_DOUBLE_EQ(reed.query_state("unique").value(), 1.0);

    reed.set_parameter("opening", 4.0e-3);
    reed.process(static_cast<float>(kBlock));
    EXPECT_DOUBLE_EQ(reed.query_state("unique").value(), 0.0);
}

TEST(TerminationOperatorTest, RadiationGivesTheSamplesOfTheKinesisLoadInTheInwardFrame)
{
    const Acoustics::DuctGrid duct = grid();
    RadiationOperator radiation;
    ASSERT_TRUE(radiation.accepts(duct, spec(), DuctEnd::BELL));
    radiation.prepare(duct, spec(), DuctEnd::BELL);

    const auto coefficients = Acoustics::make_radiation_coefficients(
        { .model = Acoustics::unflanged_radiation,
            .radius = kRadius,
            .sound_speed = 343.0,
            .air_density = 1.2,
            .time_step = 1.0 / kRate },
        std::numbers::pi * kRadius * kRadius, duct.pressure_gain.back());
    ASSERT_TRUE(coefficients.has_value());
    Acoustics::RadiationState state;

    double pressure = 40.0;
    for (const double arriving : { 1.0e-4, 2.0e-4, -5.0e-5, 0.0, 3.0e-4 }) {
        const auto [expected_pressure, expected_outflow] = Acoustics::radiation_step(*coefficients, state, pressure, arriving);
        const auto [actual_pressure, inflow] = radiation.couple(pressure, -arriving);

        EXPECT_DOUBLE_EQ(actual_pressure, expected_pressure);
        EXPECT_DOUBLE_EQ(inflow, -expected_outflow);
        pressure = expected_pressure;
    }
}

TEST(TerminationOperatorTest, ADifferentRadiationModelChangesTheFlow)
{
    const Acoustics::DuctGrid duct = grid();
    RadiationOperator open;
    RadiationOperator flanged(Acoustics::flanged_radiation);
    open.prepare(duct, spec(), DuctEnd::BELL);
    flanged.prepare(duct, spec(), DuctEnd::BELL);

    EXPECT_NE(open.couple(100.0, 0.0).second, flanged.couple(100.0, 0.0).second);
}

TEST(TerminationOperatorTest, AJunctionSolvesAnOhmicLawInClosedForm)
{
    const Acoustics::DuctGrid duct = grid();
    constexpr double conductance = 2.0e-6;
    JunctionOperator junction(
        [conductance](double d) -> Acoustics::FlowSample { return { conductance * d, conductance }; }, 1.0);
    junction.set_parameter("driving_pressure", 100.0);
    ASSERT_TRUE(junction.accepts(duct, spec(), DuctEnd::MOUTH));
    junction.prepare(duct, spec(), DuctEnd::MOUTH);

    constexpr double end_pressure = 20.0;
    constexpr double interior = 1.0e-5;
    const double gain = Acoustics::flow_coupling_gain(duct.pressure_gain.front());
    const double target = Acoustics::flow_coupling_target(100.0, end_pressure, gain, interior);
    const double difference = target / (1.0 + gain * conductance);

    const auto [pressure, inflow] = junction.couple(end_pressure, interior);

    EXPECT_NEAR(inflow, conductance * difference, 1e-12);
    EXPECT_NEAR(pressure, Acoustics::centred_end_pressure(100.0, difference, end_pressure), 1e-6);
    EXPECT_DOUBLE_EQ(junction.query_state("status").value(), 0.0);
}

TEST(TerminationOperatorTest, AJunctionWithoutALawIsNotAccepted)
{
    const Acoustics::DuctGrid duct = grid();

    EXPECT_FALSE(JunctionOperator({}, 1.0).accepts(duct, spec(), DuctEnd::MOUTH));
    EXPECT_FALSE(JunctionOperator([](double) -> Acoustics::FlowSample { return { 0.0, 0.0 }; }, 0.0)
                     .accepts(duct, spec(), DuctEnd::MOUTH));
}

TEST(TerminationOperatorTest, ADynamicReedGivesTheSamplesOfTheKinesisCoupling)
{
    const Acoustics::DuctGrid duct = grid();
    ReedOperator reed;
    reed.set_parameter("smoothing", 0.0);
    reed.set_parameter("pressure", 1500.0);
    reed.set_parameter("resonance", 2500.0);
    reed.set_parameter("reed_damping", 3000.0);
    reed.prepare(duct, spec(), DuctEnd::MOUTH);

    const Acoustics::ReedChannel channel { .width = 0.012, .rest_opening = 4.0e-4, .closing_pressure = 3000.0, .air_density = 1.2 };
    std::array<double, 2> motion {};

    double pressure = 0.0;
    for (const double interior : { 0.0, 1.0e-4, 3.0e-5, -2.0e-5, 8.0e-5, 4.0e-5 }) {
        const auto [expected_pressure, expected_flow, difference] = Acoustics::dynamic_reed_mouth_step(
            channel, 2500.0, 3000.0, 0.0, duct.time_step, motion, duct.pressure_gain.front(), 1500.0, pressure, interior);
        const auto [actual_pressure, actual_flow] = reed.couple(pressure, interior);

        EXPECT_DOUBLE_EQ(actual_pressure, expected_pressure);
        EXPECT_DOUBLE_EQ(actual_flow, expected_flow);
        pressure = expected_pressure;
    }
}

TEST(TerminationOperatorTest, TheDynamicReedIsAlwaysUniqueAndItsNewParametersAreReadBack)
{
    const Acoustics::DuctGrid duct = grid();
    ReedOperator reed;
    reed.set_parameter("opening", 4.0e-3);
    reed.process(static_cast<float>(kBlock));
    reed.prepare(duct, spec(), DuctEnd::MOUTH);
    EXPECT_DOUBLE_EQ(reed.query_state("unique").value(), 0.0);

    reed.set_parameter("resonance", 2500.0);
    reed.set_parameter("reed_damping", 2000.0);
    reed.set_parameter("noise", 0.05);
    reed.set_parameter("noise_cutoff", 1500.0);
    reed.process(static_cast<float>(kBlock));

    EXPECT_DOUBLE_EQ(reed.query_state("noise_cutoff").value(), 1500.0);
    EXPECT_DOUBLE_EQ(reed.query_state("unique").value(), 1.0);
    EXPECT_DOUBLE_EQ(reed.query_state("resonance").value(), 2500.0);
    EXPECT_DOUBLE_EQ(reed.query_state("reed_damping").value(), 2000.0);
    EXPECT_DOUBLE_EQ(reed.query_state("noise").value(), 0.05);
}

TEST(TerminationOperatorTest, BreathNoiseIsDeterministicAndOnlyWhenAsked)
{
    const Acoustics::DuctGrid duct = grid();

    const auto make = [&](double noise) {
        auto reed = std::make_unique<ReedOperator>();
        reed->set_parameter("smoothing", 0.0);
        reed->set_parameter("pressure", 1500.0);
        reed->set_parameter("noise", noise);
        reed->prepare(duct, spec(), DuctEnd::MOUTH);
        return reed;
    };

    const auto clean = make(0.0);
    const auto first = make(0.1);
    const auto second = make(0.1);

    bool differs = false;
    for (size_t n = 0; n < 8; ++n) {
        const double a = clean->couple(0.0, 0.0).second;
        const double b = first->couple(0.0, 0.0).second;
        const double c = second->couple(0.0, 0.0).second;

        EXPECT_DOUBLE_EQ(b, c);
        differs = differs || a != b;
    }
    EXPECT_TRUE(differs);
}

TEST(TerminationOperatorTest, BreathFollowsTheReedFlowSoAShutReedHasNone)
{
    const Acoustics::DuctGrid duct = grid();

    const auto make = [&](double pressure, double noise) {
        auto reed = std::make_unique<ReedOperator>();
        reed->set_parameter("smoothing", 0.0);
        reed->set_parameter("pressure", pressure);
        reed->set_parameter("noise", noise);
        reed->prepare(duct, spec(), DuctEnd::MOUTH);
        return reed;
    };

    const auto clean = make(5000.0, 0.0);
    const auto breathy = make(5000.0, 0.5);
    for (size_t n = 0; n < 8; ++n) {
        EXPECT_EQ(clean->couple(0.0, 0.0), breathy->couple(0.0, 0.0));
    }
}

TEST(TerminationOperatorTest, BreathAddsToTheFlowIntoTheDuctAndKeepsTheCellConsistent)
{
    const Acoustics::DuctGrid duct = grid();

    ReedOperator reed;
    reed.set_parameter("smoothing", 0.0);
    reed.set_parameter("pressure", 1500.0);
    reed.set_parameter("noise", 0.3);
    reed.prepare(duct, spec(), DuctEnd::MOUTH);

    ReedOperator clean;
    clean.set_parameter("smoothing", 0.0);
    clean.set_parameter("pressure", 1500.0);
    clean.prepare(duct, spec(), DuctEnd::MOUTH);

    static_cast<void>(reed.couple(0.0, 0.0));
    static_cast<void>(clean.couple(0.0, 0.0));

    const auto [breathy_pressure, breathy_flow] = reed.couple(100.0, 2.0e-5);
    const auto [clean_pressure, clean_flow] = clean.couple(100.0, 2.0e-5);

    EXPECT_NE(breathy_flow, clean_flow);
    EXPECT_NEAR(breathy_pressure, 100.0 + duct.pressure_gain.front() * (breathy_flow - 2.0e-5), 1e-6 * std::abs(breathy_pressure));
    EXPECT_NEAR(clean_pressure, 100.0 + duct.pressure_gain.front() * (clean_flow - 2.0e-5), 1e-6 * std::abs(clean_pressure));
}

TEST(TerminationOperatorTest, TheLipNarrowsTheOpeningAndTheReedAreaAddsTheReedsOwnFlow)
{
    const Acoustics::DuctGrid duct = grid();

    const auto make = [&](double lip, double area) {
        auto reed = std::make_unique<ReedOperator>();
        reed->set_parameter("smoothing", 0.0);
        reed->set_parameter("pressure", 1500.0);
        reed->set_parameter("resonance", 2500.0);
        reed->set_parameter("reed_damping", 8000.0);
        reed->set_parameter("lip", lip);
        reed->set_parameter("reed_area", area);
        reed->prepare(duct, spec(), DuctEnd::MOUTH);
        return reed;
    };

    const auto open = make(0.0, 0.0);
    const auto pressed = make(1.0, 0.0);
    const auto pushing = make(0.0, 1.46e-4);

    EXPECT_LT(pressed->couple(0.0, 0.0).second, open->couple(0.0, 0.0).second);

    bool differs = false;
    open->reset();
    open->prepare(duct, spec(), DuctEnd::MOUTH);
    for (size_t n = 0; n < 30; ++n) {
        differs = differs || open->couple(10.0, 1.0e-5).second != pushing->couple(10.0, 1.0e-5).second;
    }
    EXPECT_TRUE(differs);

    pressed->set_parameter("lip", 3.0);
    pressed->set_parameter("reed_area", -1.0);
    EXPECT_DOUBLE_EQ(pressed->query_state("lip").value(), 1.0);
    EXPECT_DOUBLE_EQ(pressed->query_state("reed_area").value(), 0.0);
}

TEST(DuctNetworkTest, ABlownDuctWithADynamicReedSounds)
{
    DuctNetwork duct(kLength, kRadius);
    duct.set_output(DuctNetwork::Output::PICKUP);
    duct.get_mouth()->set_parameter("pressure", 1800.0);
    duct.get_mouth()->set_parameter("resonance", 2500.0);

    const auto sound = render(duct, 100);

    const std::span<const double> tail = std::span<const double>(sound).last(size_t { 20 } * kBlock);
    EXPECT_GT(peak_to_peak(tail), 50.0);
    for (const double sample : sound) {
        ASSERT_TRUE(std::isfinite(sample));
    }
}

TEST(DuctNetworkTest, ADefaultDuctIsAReedAtTheMouthAndARadiatingBell)
{
    DuctNetwork duct(kLength, kRadius);

    EXPECT_EQ(duct.get_node_count(), 84U);
    EXPECT_EQ(duct.get_topology(), Nodes::Network::Topology::CHAIN);
    EXPECT_EQ(duct.get_output_mode(), Nodes::Network::OutputMode::AUDIO_SINK);
    EXPECT_EQ(duct.get_mouth()->get_type_name(), "Reed");
    EXPECT_EQ(duct.get_bell()->get_type_name(), "Radiation");

    const auto metadata = duct.get_metadata();
    EXPECT_EQ(metadata.at("intervals"), "83");
    EXPECT_EQ(metadata.at("mouth"), "Reed");
    EXPECT_EQ(metadata.at("bell"), "Radiation");
    EXPECT_NE(duct.get_operator_chain()->find<ReedOperator>(), nullptr);
}

TEST(DuctNetworkTest, AMouthVolumeIsInstalledOrRefused)
{
    DuctNetwork duct(kLength, kRadius);
    duct.process_batch(kBlock);

    EXPECT_FALSE(duct.set_mouth_volume(-1.0e-6));
    EXPECT_FALSE(duct.set_mouth_volume(std::numeric_limits<double>::infinity()));
    ASSERT_TRUE(duct.set_mouth_volume(2.0e-6));
    duct.process_batch(kBlock);

    EXPECT_EQ(duct.get_node_count(), 84U);
    for (const double sample : render(duct, 4)) {
        EXPECT_TRUE(std::isfinite(sample));
    }
}

TEST(DuctNetworkTest, ALengthThatSlidesKeepsTheNoteSoundingWhileAResetRestartsIt)
{
    const auto rms = [](std::span<const double> samples) {
        double sum = 0.0;
        for (const double sample : samples) {
            sum += sample * sample;
        }
        return std::sqrt(sum / static_cast<double>(samples.size()));
    };

    const auto blown = [] {
        auto duct = std::make_unique<DuctNetwork>(kLength, kRadius);
        duct->set_output(DuctNetwork::Output::PICKUP);
        duct->get_mouth()->set_parameter("pressure", 1800.0);
        duct->get_mouth()->set_parameter("resonance", 2500.0);
        return duct;
    };

    auto sliding = blown();
    const auto settled = render(*sliding, 80);
    const double before = rms(std::span<const double>(settled).last(size_t { 8 } * kBlock));

    double length = kLength;
    std::vector<double> slide;
    for (size_t step = 0; step < 40; ++step) {
        length -= 0.0005;
        ASSERT_TRUE(sliding->set_length(length));
        const auto block = render(*sliding, 1);
        slide.insert(slide.end(), block.begin(), block.end());
    }
    const double during = rms(std::span<const double>(slide).last(size_t { 20 } * kBlock));

    auto restarted = blown();
    static_cast<void>(render(*restarted, 80));
    double restart_length = kLength;
    std::vector<double> restart_slide;
    for (size_t step = 0; step < 40; ++step) {
        restart_length -= 0.0005;
        ASSERT_TRUE(restarted->set_length(restart_length));
        restarted->reset();
        const auto block = render(*restarted, 1);
        restart_slide.insert(restart_slide.end(), block.begin(), block.end());
    }
    const double restart_during = rms(std::span<const double>(restart_slide).last(size_t { 20 } * kBlock));

    ASSERT_GT(before, 0.0);
    EXPECT_GT(during, 0.5 * before);
    EXPECT_LT(restart_during, 0.25 * during);
    for (const double sample : slide) {
        ASSERT_TRUE(std::isfinite(sample));
    }
}

TEST(DuctNetworkTest, ChangingTheProfileOrTheMouthVolumeKeepsTheSoundToo)
{
    DuctNetwork duct(kLength, kRadius);
    duct.set_output(DuctNetwork::Output::PICKUP);
    duct.get_mouth()->set_parameter("pressure", 1800.0);
    duct.get_mouth()->set_parameter("resonance", 2500.0);
    static_cast<void>(render(duct, 80));

    ASSERT_TRUE(duct.set_radius(0.0076));
    ASSERT_TRUE(duct.set_mouth_volume(1.0e-7));
    const auto after = render(duct, 4);

    EXPECT_GT(peak_to_peak(after), 50.0);
}

TEST(TerminationOperatorTest, RetargetingAReedKeepsItsStateWhilePreparingResetsIt)
{
    const Acoustics::DuctGrid duct = grid();

    const auto run = [&](bool keep) {
        ReedOperator reed;
        reed.set_parameter("smoothing", 0.0);
        reed.set_parameter("pressure", 1500.0);
        reed.set_parameter("resonance", 2500.0);
        reed.set_parameter("reed_damping", 8000.0);
        reed.prepare(duct, spec(), DuctEnd::MOUTH);
        for (size_t n = 0; n < 200; ++n) {
            static_cast<void>(reed.couple(50.0, 1.0e-5));
        }
        if (keep) {
            reed.retarget(duct, spec(), DuctEnd::MOUTH);
        } else {
            reed.prepare(duct, spec(), DuctEnd::MOUTH);
        }
        return reed.couple(50.0, 1.0e-5).second;
    };

    ReedOperator continuing;
    continuing.set_parameter("smoothing", 0.0);
    continuing.set_parameter("pressure", 1500.0);
    continuing.set_parameter("resonance", 2500.0);
    continuing.set_parameter("reed_damping", 8000.0);
    continuing.prepare(duct, spec(), DuctEnd::MOUTH);
    for (size_t n = 0; n < 200; ++n) {
        static_cast<void>(continuing.couple(50.0, 1.0e-5));
    }
    const double expected = continuing.couple(50.0, 1.0e-5).second;

    EXPECT_DOUBLE_EQ(run(true), expected);
    EXPECT_NE(run(false), expected);
}

TEST(DuctNetworkTest, ADuctAtRestIsSilent)
{
    DuctNetwork duct(kLength, kRadius);

    const auto out = render(duct, 4);

    ASSERT_EQ(out.size(), size_t { 4 } * kBlock);
    for (const double sample : out) {
        EXPECT_DOUBLE_EQ(sample, 0.0);
    }
    for (const double pressure : duct.pressure_field()) {
        EXPECT_DOUBLE_EQ(pressure, 0.0);
    }
}

TEST(DuctNetworkTest, TheNetworkGivesTheSamplesOfTheKinesisCallsItIsBuiltFrom)
{
    DuctNetwork duct(kLength, kRadius);
    ASSERT_TRUE(duct.set_wall_losses(0.0, 0.0));
    duct.set_output(DuctNetwork::Output::PICKUP);
    duct.set_pickup_position(0.5);
    duct.get_mouth()->set_parameter("smoothing", 0.0);
    duct.get_mouth()->set_parameter("pressure", 1500.0);

    const Acoustics::DuctGrid line = grid();
    const auto radiation = Acoustics::make_radiation_coefficients(
        { .model = Acoustics::unflanged_radiation, .radius = kRadius, .time_step = 1.0 / kRate },
        std::numbers::pi * kRadius * kRadius, line.pressure_gain.back());
    ASSERT_TRUE(radiation.has_value());

    const Acoustics::ReedChannel channel { .width = 0.012, .rest_opening = 4.0e-4, .closing_pressure = 3000.0, .air_density = 1.2 };
    Acoustics::RadiationState state;
    double warm_start = 0.0;
    std::vector<double> pressure(line.intervals + 1, 0.0);
    std::vector<double> velocity(line.intervals, 0.0);
    std::vector<double> expected;
    for (unsigned int n = 0; n < 4 * kBlock; ++n) {
        Acoustics::advance_velocity(velocity, pressure, line.velocity_gain);
        const auto [mouth_pressure, flow, difference, status] = Acoustics::reed_mouth_step(
            channel, warm_start, line.pressure_gain.front(), 1500.0, pressure.front(), velocity.front());
        Acoustics::advance_pressure(pressure, velocity, line.pressure_gain);
        pressure.front() = mouth_pressure;
        const auto [bell_pressure, outflow] = Acoustics::radiation_step(*radiation, state, pressure.back(), velocity.back());
        pressure.back() = bell_pressure;
        expected.push_back(Acoustics::sample_pressure(pressure, 0.5));
    }

    const auto actual = render(duct, 4);

    ASSERT_EQ(actual.size(), expected.size());
    EXPECT_GT(peak_to_peak(actual), 1.0);
    for (size_t n = 0; n < actual.size(); ++n) {
        ASSERT_NEAR(actual[n], expected[n], 1e-9 * std::max(1.0, std::abs(expected[n]))) << "sample " << n;
    }
}

TEST(DuctNetworkTest, RadiatedOutputIsTheRateOfChangeOfTheBellFlow)
{
    DuctNetwork radiated(kLength, kRadius);
    DuctNetwork pickup(kLength, kRadius);
    for (DuctNetwork* duct : { &radiated, &pickup }) {
        duct->get_mouth()->set_parameter("smoothing", 0.0);
        duct->get_mouth()->set_parameter("pressure", 1500.0);
    }
    pickup.set_output(DuctNetwork::Output::PICKUP);

    const auto from_flow = render(radiated, 2);
    const auto from_pressure = render(pickup, 2);

    EXPECT_GT(peak_to_peak(from_flow), 0.0);
    EXPECT_GT(peak_to_peak(from_pressure), peak_to_peak(from_flow));
}

TEST(DuctNetworkTest, ABlownDuctSoundsAndAnUnblownOneDoesNot)
{
    DuctNetwork blown(kLength, kRadius);
    blown.set_output(DuctNetwork::Output::PICKUP);
    blown.get_mouth()->set_parameter("pressure", 1800.0);
    DuctNetwork quiet(kLength, kRadius);
    quiet.set_output(DuctNetwork::Output::PICKUP);

    const auto sound = render(blown, 100);
    const auto silence = render(quiet, 100);

    const std::span<const double> tail = std::span<const double>(sound).last(size_t { 20 } * kBlock);
    EXPECT_GT(peak_to_peak(tail), 100.0);
    EXPECT_DOUBLE_EQ(peak_to_peak(silence), 0.0);
}

TEST(DuctNetworkTest, AMappedNodeDrivesTheBlowingPressureThePickupAndTheScale)
{
    DuctNetwork duct(kLength, kRadius);
    const auto breath = std::make_shared<Constant>(1500.0);
    const auto position = std::make_shared<Constant>(0.25);
    const auto scale = std::make_shared<Constant>(2.0);
    for (const auto& node : { breath, position, scale }) {
        static_cast<void>(node->process_sample(0.0));
    }

    duct.map_parameter("pressure", breath);
    duct.map_parameter("pickup", position);
    duct.map_parameter("scale", scale);
    duct.process_batch(kBlock);

    EXPECT_DOUBLE_EQ(duct.get_mouth()->query_state("pressure").value(), 1500.0);
    EXPECT_DOUBLE_EQ(duct.get_pickup_position(), 0.25);
    EXPECT_DOUBLE_EQ(duct.get_output_scale(), 2.0);

    duct.unmap_parameter("pressure");
    static_cast<void>(breath->process_sample(0.0));
    EXPECT_DOUBLE_EQ(duct.get_mouth()->query_state("pressure").value(), 1500.0);
}

TEST(DuctNetworkTest, AChangeOfLengthIsInstalledAtTheNextBlockOrRefused)
{
    DuctNetwork duct(kLength, kRadius);
    duct.process_batch(kBlock);
    ASSERT_EQ(duct.get_node_count(), 84U);

    EXPECT_FALSE(duct.set_length(-1.0));
    EXPECT_FALSE(duct.set_length(0.001));
    EXPECT_FALSE(duct.set_radius(0.0));
    duct.process_batch(kBlock);
    EXPECT_EQ(duct.get_node_count(), 84U);

    ASSERT_TRUE(duct.set_length(0.3));
    EXPECT_EQ(duct.get_node_count(), 84U);
    duct.process_batch(kBlock);
    EXPECT_EQ(duct.get_node_count(), 42U);
    EXPECT_EQ(duct.get_metadata().at("intervals"), "41");
}

TEST(DuctNetworkTest, SeveralChangesBeforeABlockInstallTheLast)
{
    DuctNetwork duct(kLength, kRadius);
    duct.process_batch(kBlock);

    ASSERT_TRUE(duct.set_length(0.3));
    ASSERT_TRUE(duct.set_length(0.9));
    ASSERT_TRUE(duct.set_profile([](double x) { return 0.005 + 0.01 * x; }));
    duct.process_batch(kBlock);

    EXPECT_EQ(duct.get_node_count(), 126U);
    ASSERT_TRUE(duct.set_profile(std::vector<double> { 0.01, 0.02 }));
    duct.process_batch(kBlock);
    EXPECT_EQ(duct.get_node_count(), 126U);
}

TEST(DuctNetworkTest, ATerminationCanBeReplacedAndOneThatCannotTerminateIsRefused)
{
    DuctNetwork duct(kLength, kRadius);

    EXPECT_FALSE(duct.set_mouth(nullptr));
    EXPECT_FALSE(duct.set_mouth(std::make_shared<JunctionOperator>(JunctionOperator::FlowLaw {}, 1.0)));
    EXPECT_EQ(duct.get_mouth()->get_type_name(), "Reed");

    const auto source = std::make_shared<JunctionOperator>(
        [](double d) -> Acoustics::FlowSample { return { 1.0e-6 * d, 1.0e-6 }; }, 1.0, 1000.0);
    ASSERT_TRUE(duct.set_mouth(source));
    EXPECT_EQ(duct.get_mouth()->get_type_name(), "Junction");
    EXPECT_EQ(duct.get_operator_chain()->find<ReedOperator>(), nullptr);
    EXPECT_EQ(duct.get_operator_chain()->find<JunctionOperator>(), source);

    duct.set_output(DuctNetwork::Output::PICKUP);
    const auto out = render(duct, 8);
    EXPECT_GT(peak_to_peak(out), 0.0);
    for (const double sample : out) {
        EXPECT_TRUE(std::isfinite(sample));
    }

    ASSERT_TRUE(duct.set_bell(std::make_shared<WallOperator>()));
    EXPECT_EQ(duct.get_bell()->get_type_name(), "Wall");
}

TEST(DuctNetworkTest, ADuctDrivenThroughAnOhmicSourceThenReleasedDoesNotGrow)
{
    DuctNetwork duct(kLength, kRadius);
    ASSERT_TRUE(duct.set_bell(std::make_shared<WallOperator>()));
    const auto pulse = std::make_shared<JunctionOperator>(
        [](double d) -> Acoustics::FlowSample { return { 2.0e-6 * d, 2.0e-6 }; }, 1.0, 500.0);
    ASSERT_TRUE(duct.set_mouth(pulse));
    duct.set_output(DuctNetwork::Output::PICKUP);

    render(duct, 4);
    pulse->set_parameter("driving_pressure", 0.0);
    const auto after = render(duct, 40);

    const std::span<const double> early = std::span<const double>(after).first(kBlock);
    const std::span<const double> late = std::span<const double>(after).last(kBlock);
    for (const double sample : after) {
        ASSERT_TRUE(std::isfinite(sample));
    }
    EXPECT_GT(peak_to_peak(early), 0.0);
    EXPECT_LE(peak_to_peak(late), 1.05 * peak_to_peak(early));
}

TEST(DuctNetworkTest, ThePressureFieldIsReadThroughTheNodeOutputsToo)
{
    DuctNetwork duct(kLength, kRadius);
    duct.get_mouth()->set_parameter("pressure", 1800.0);
    render(duct, 20);

    const std::vector<double> field = duct.pressure_field();

    ASSERT_EQ(field.size(), duct.get_node_count());
    EXPECT_GT(peak_to_peak(field), 0.0);
    for (size_t l = 0; l < field.size(); ++l) {
        EXPECT_DOUBLE_EQ(duct.get_node_output(l).value(), field[l]);
    }
    EXPECT_FALSE(duct.get_node_output(field.size()).has_value());
}

TEST(DuctNetworkTest, ResetReturnsTheDuctToRest)
{
    DuctNetwork duct(kLength, kRadius);
    duct.get_mouth()->set_parameter("pressure", 1800.0);
    render(duct, 20);
    ASSERT_GT(peak_to_peak(duct.pressure_field()), 0.0);

    duct.get_mouth()->set_parameter("pressure", 0.0);
    duct.reset();
    duct.process_batch(kBlock);

    for (const double pressure : duct.pressure_field()) {
        EXPECT_DOUBLE_EQ(pressure, 0.0);
    }
}

TEST(DuctNetworkTest, ADisabledDuctIsSilentAndKeepsItsState)
{
    DuctNetwork duct(kLength, kRadius);
    duct.get_mouth()->set_parameter("pressure", 1800.0);
    render(duct, 10);
    const std::vector<double> before = duct.pressure_field();

    duct.set_enabled(false);
    const auto out = render(duct, 2);

    for (const double sample : out) {
        EXPECT_DOUBLE_EQ(sample, 0.0);
    }
    EXPECT_EQ(duct.pressure_field().size(), before.size());
}

TEST(DuctNetworkTest, TheWallLossesDefaultToTheViscothermalFitAndCanBeSetOrRefused)
{
    DuctNetwork duct(kLength, kRadius);

    const auto [friction, viscosity] = Acoustics::viscothermal_rates(kRadius);
    EXPECT_EQ(duct.get_metadata().at("friction"), std::to_string(friction) + " 1/s");
    EXPECT_EQ(duct.get_metadata().at("viscosity"), std::to_string(viscosity) + " m^2/s");

    EXPECT_FALSE(duct.set_wall_losses(-1.0, 0.0));
    EXPECT_FALSE(duct.set_wall_losses(0.0, std::numeric_limits<double>::quiet_NaN()));
    EXPECT_EQ(duct.get_metadata().at("friction"), std::to_string(friction) + " 1/s");

    ASSERT_TRUE(duct.set_wall_losses(12.0, 0.5));
    EXPECT_EQ(duct.get_metadata().at("friction"), std::to_string(12.0) + " 1/s");
    EXPECT_EQ(duct.get_metadata().at("viscosity"), std::to_string(0.5) + " m^2/s");

    ASSERT_TRUE(duct.use_viscothermal_losses());
    EXPECT_EQ(duct.get_metadata().at("friction"), std::to_string(friction) + " 1/s");
}

TEST(DuctNetworkTest, WallLossesLetASealedDuctRingDownWhereALosslessOneKeepsRinging)
{
    const auto ring = [](bool lossless) {
        DuctNetwork duct(kLength, kRadius);
        EXPECT_TRUE(duct.set_bell(std::make_shared<WallOperator>()));
        if (lossless) {
            EXPECT_TRUE(duct.set_wall_losses(0.0, 0.0));
        }

        const auto pulse = std::make_shared<JunctionOperator>(
            [](double d) -> Acoustics::FlowSample { return { 1.0e-12 * d, 1.0e-12 }; }, 1.0, 1.0e8);
        EXPECT_TRUE(duct.set_mouth(pulse));
        duct.set_output(DuctNetwork::Output::PICKUP);

        render(duct, 2);
        pulse->set_parameter("driving_pressure", 0.0);
        const auto after = render(duct, 120);

        return std::make_pair(
            peak_to_peak(std::span<const double>(after).first(size_t { 4 } * kBlock)),
            peak_to_peak(std::span<const double>(after).last(size_t { 4 } * kBlock)));
    };

    const auto [lossless_early, lossless_late] = ring(true);
    const auto [lossy_early, lossy_late] = ring(false);

    ASSERT_GT(lossless_early, 0.0);
    ASSERT_GT(lossy_early, 0.0);
    EXPECT_GT(lossless_late, 0.3 * lossless_early);
    EXPECT_LT(lossy_late, 0.05 * lossless_late);
}

} // namespace MayaFlux::Test
