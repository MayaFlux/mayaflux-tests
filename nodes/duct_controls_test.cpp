#include "gtest/gtest.h"
#include "../support/test_metadata.hpp"

#include "MayaFlux/Nodes/Conduit/Constant.hpp"
#include "MayaFlux/Nodes/Network/DuctNetwork.hpp"
#include "network_controls_fixtures.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {

namespace Acoustics = Kinesis::Acoustics;
using Nodes::Constant;
using Nodes::Network::DuctNetwork;
using Nodes::Network::JunctionOperator;

namespace {

    constexpr double kRadius = 0.0075;
    constexpr double kLength = 0.6;
    constexpr unsigned int kBlock = 512;

    std::shared_ptr<Constant> constant(double value)
    {
        auto node = std::make_shared<Constant>(value);
        static_cast<void>(node->process_sample(0.0));
        return node;
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

}

TEST(DuctControlsTest, NodesDriveTheBlowingPressureThePickupAndTheScale)
{
    DuctNetwork duct(kLength, kRadius);

    duct.set({ .scale = constant(2.0), .pickup = constant(0.25), .pressure = constant(1500.0) });
    duct.process_batch(kBlock);

    EXPECT_DOUBLE_EQ(duct.get_mouth()->query_state("pressure").value(), 1500.0);
    EXPECT_DOUBLE_EQ(duct.get_pickup_position(), 0.25);
    EXPECT_DOUBLE_EQ(duct.get_output_scale(), 2.0);
}

TEST(DuctControlsTest, NumbersReachTheReedAtTheMouth)
{
    DuctNetwork duct(kLength, kRadius);

    duct.set({ .pressure = 900.0, .closing_pressure = 2500.0, .opening = 3.0e-4 });
    duct.process_batch(kBlock);

    EXPECT_DOUBLE_EQ(duct.get_mouth()->query_state("pressure").value(), 900.0);
    EXPECT_DOUBLE_EQ(duct.get_mouth()->query_state("closing_pressure").value(), 2500.0);
    EXPECT_DOUBLE_EQ(duct.get_mouth()->query_state("opening").value(), 3.0e-4);
}

TEST(DuctControlsTest, ANumberIsAppliedOnceAndLaterEditsStay)
{
    DuctNetwork duct(kLength, kRadius);

    duct.set({ .pressure = 900.0 });
    duct.process_batch(kBlock);
    ASSERT_DOUBLE_EQ(duct.get_mouth()->query_state("pressure").value(), 900.0);

    duct.get_mouth()->set_parameter("pressure", 400.0);
    duct.process_batch(kBlock);

    EXPECT_DOUBLE_EQ(duct.get_mouth()->query_state("pressure").value(), 400.0);
}

TEST(DuctControlsTest, ANodeIsAppliedEveryBlock)
{
    DuctNetwork duct(kLength, kRadius);

    duct.set({ .pressure = constant(1500.0) });
    duct.process_batch(kBlock);
    duct.get_mouth()->set_parameter("pressure", 400.0);
    duct.process_batch(kBlock);

    EXPECT_DOUBLE_EQ(duct.get_mouth()->query_state("pressure").value(), 1500.0);
}

TEST(DuctControlsTest, TheBreathNameIsTheOneTheReedAlwaysHad)
{
    DuctNetwork duct(kLength, kRadius);

    duct.set({ .noise = 0.02, .noise_cutoff = 3000.0, .reed_area = 1.46e-4, .lip = 0.5 });
    duct.process_batch(kBlock);

    EXPECT_DOUBLE_EQ(duct.get_mouth()->query_state("noise").value(), 0.02);
    EXPECT_DOUBLE_EQ(duct.get_mouth()->query_state("reed_area").value(), 1.46e-4);
    EXPECT_DOUBLE_EQ(duct.get_mouth()->query_state("lip").value(), 0.5);
}

TEST(DuctControlsTest, AMemberTheTerminationsDoNotKnowIsIgnored)
{
    DuctNetwork duct(kLength, kRadius);

    duct.set({ .driving_pressure = 500.0, .maximum_flow = 2.0 });

    EXPECT_NO_THROW(duct.process_batch(kBlock));
    EXPECT_FALSE(duct.get_mouth()->query_state("driving_pressure").has_value());
}

TEST(DuctControlsTest, JunctionMembersReachAJunctionMouth)
{
    DuctNetwork duct(kLength, kRadius);
    const auto junction = std::make_shared<JunctionOperator>(
        [](double d) -> Acoustics::FlowSample { return { 1.0e-6 * d, 1.0e-6 }; }, 1.0, 1000.0);
    ASSERT_TRUE(duct.set_mouth(junction));

    duct.set({ .driving_pressure = 500.0 });
    duct.process_batch(kBlock);

    EXPECT_DOUBLE_EQ(duct.get_mouth()->query_state("driving_pressure").value(), 500.0);
}

TEST(DuctControlsTest, AMemberLeftOutLeavesTheOthersAlone)
{
    DuctNetwork duct(kLength, kRadius);
    duct.set_pickup_position(0.7);
    duct.get_mouth()->set_parameter("pressure", 800.0);

    duct.set({ .lip = 0.5 });
    duct.process_batch(kBlock);

    EXPECT_DOUBLE_EQ(duct.get_pickup_position(), 0.7);
    EXPECT_DOUBLE_EQ(duct.get_mouth()->query_state("pressure").value(), 800.0);
}

TEST(DuctControlsTest, ANetworkSourceIsNotUsedByAnyDuctControl)
{
    DuctNetwork duct(kLength, kRadius);
    duct.set_pickup_position(0.7);
    auto source = std::make_shared<ControlsProbeNetwork>(3);
    source->outputs = { 0.1, 0.2, 0.3 };

    duct.set({ .pickup = source });
    duct.process_batch(kBlock);

    EXPECT_DOUBLE_EQ(duct.get_pickup_position(), 0.7);
}

TEST(DuctControlsTest, ABlownDuctSoundsThroughTheTypedPressure)
{
    DuctNetwork blown(kLength, kRadius);
    blown.set_output(DuctNetwork::Output::PICKUP);
    blown.set({ .pressure = 1800.0 });
    DuctNetwork quiet(kLength, kRadius);
    quiet.set_output(DuctNetwork::Output::PICKUP);

    const auto sound = render(blown, 100);
    const auto silence = render(quiet, 100);

    const auto [low, high] = std::ranges::minmax(std::span<const double>(sound).last(size_t { 20 } * kBlock));
    EXPECT_GT(high - low, 100.0);
    EXPECT_DOUBLE_EQ(std::ranges::max(silence) - std::ranges::min(silence), 0.0);
}

TEST(DuctControlsTest, TypedAndStringMappingsRenderTheSameSound)
{
    DuctNetwork by_string(kLength, kRadius);
    DuctNetwork by_control(kLength, kRadius);
    by_string.set_output(DuctNetwork::Output::PICKUP);
    by_control.set_output(DuctNetwork::Output::PICKUP);

    by_string.map_parameter("pressure", constant(1800.0));
    by_string.map_parameter("pickup", constant(0.4));
    by_string.map_parameter("scale", constant(0.5));
    by_control.set({ .scale = constant(0.5), .pickup = constant(0.4), .pressure = constant(1800.0) });

    const auto expected = render(by_string, 20);
    const auto actual = render(by_control, 20);

    ASSERT_EQ(actual.size(), expected.size());
    for (size_t i = 0; i < expected.size(); ++i) {
        EXPECT_NEAR(actual[i], expected[i], 1e-9) << "sample " << i;
    }
}

TEST(DuctControlsTest, GetReportsThePickupAndTheScaleTheNetworkHolds)
{
    DuctNetwork duct(kLength, kRadius);
    duct.set_pickup_position(0.25);
    duct.set_output_scale(0.4);

    const auto controls = duct.get();

    EXPECT_DOUBLE_EQ(controls.pickup.value(), 0.25);
    EXPECT_DOUBLE_EQ(controls.scale.value(), 0.4);
    EXPECT_FALSE(controls.pressure.has_value());
}

}
