#include "gtest/gtest.h"
#include "../support/test_metadata.hpp"

#include "MayaFlux/Nodes/Generators/Polynomial.hpp"
#include "MayaFlux/Nodes/Network/WaveguideNetwork.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {

using Nodes::Generator::Polynomial;
using Nodes::Network::WaveguideNetwork;

namespace {

    using GuideType = WaveguideNetwork::WaveguideType;

    constexpr double k_pass_latency = 1.5;

    std::shared_ptr<Polynomial> constant_source(double value)
    {
        auto source = std::make_shared<Polynomial>([value](double) { return value; });
        source->process_sample(0.0);
        return source;
    }

    std::vector<double> render(WaveguideNetwork& network, unsigned int samples)
    {
        network.process_batch(samples);
        return network.get_audio_buffer().value_or(std::vector<double> {});
    }

    double peak(std::span<const double> samples)
    {
        double result = 0.0;
        for (double sample : samples) {
            result = std::max(result, std::abs(sample));
        }
        return result;
    }

}

TEST(WaveguideControlsTest, ANodeFrequencyRecomputesTheDelay)
{
    WaveguideNetwork network(GuideType::STRING, 480.0);

    network.set({ .frequency = constant_source(120.0) });
    render(network, 1);

    EXPECT_DOUBLE_EQ(network.get_fundamental(), 120.0);
    EXPECT_EQ(network.get_segments().at(0).p_plus.capacity(),
        static_cast<size_t>(48000.0 / 120.0 - k_pass_latency) + 2);
}

TEST(WaveguideControlsTest, ANumberFrequencyRecomputesTheDelay)
{
    WaveguideNetwork network(GuideType::STRING, 480.0);

    network.set({ .frequency = 120.0 });
    render(network, 1);

    EXPECT_DOUBLE_EQ(network.get().frequency.value(), 120.0);
}

TEST(WaveguideControlsTest, ANetworkFrequencyDoesNotChangeTheGuide)
{
    WaveguideNetwork network(GuideType::STRING, 480.0);
    auto source = std::make_shared<WaveguideNetwork>(GuideType::STRING, 120.0);

    network.set({ .frequency = source });
    render(network, 1);

    EXPECT_DOUBLE_EQ(network.get_fundamental(), 480.0);
}

TEST(WaveguideControlsTest, ANodeDampingSetsTheLossFactor)
{
    WaveguideNetwork network(GuideType::STRING, 480.0);

    network.set({ .damping = constant_source(0.7) });
    render(network, 1);

    EXPECT_DOUBLE_EQ(network.get_loss_factor(), 0.7);
}

TEST(WaveguideControlsTest, ANumberDampingSetsTheLossFactorOnce)
{
    WaveguideNetwork network(GuideType::STRING, 480.0);

    network.set({ .damping = 0.7 });
    render(network, 1);
    ASSERT_DOUBLE_EQ(network.get().damping.value(), 0.7);

    network.set_loss_factor(0.9);
    render(network, 1);
    EXPECT_DOUBLE_EQ(network.get().damping.value(), 0.9);
}

TEST(WaveguideControlsTest, ADampingOutsideTheUnitIntervalIsClampedLikeTheSetter)
{
    WaveguideNetwork network(GuideType::STRING, 480.0);

    network.set({ .damping = 5.0 });
    render(network, 1);

    EXPECT_DOUBLE_EQ(network.get().damping.value(), 1.0);
}

TEST(WaveguideControlsTest, APositionMovesThePickup)
{
    WaveguideNetwork network(GuideType::STRING, 480.0);

    network.set({ .position = constant_source(0.25) });
    render(network, 1);

    EXPECT_NEAR(network.get_pickup_position(), 0.25, 0.01);
}

TEST(WaveguideControlsTest, AMemberLeftOutLeavesTheOthersAlone)
{
    WaveguideNetwork network(GuideType::STRING, 480.0);
    network.set_loss_factor(0.8);

    network.set({ .position = 0.25 });
    render(network, 1);

    EXPECT_DOUBLE_EQ(network.get().frequency.value(), 480.0);
    EXPECT_DOUBLE_EQ(network.get().damping.value(), 0.8);
}

TEST(WaveguideControlsTest, ANodeScaleChangesTheAudioMix)
{
    WaveguideNetwork reference(GuideType::STRING, 480.0);
    WaveguideNetwork scaled(GuideType::STRING, 480.0);
    reference.pluck(0.5);
    scaled.pluck(0.5);
    scaled.set({ .scale = constant_source(0.25) });

    const auto original = render(reference, 256);
    const auto reduced = render(scaled, 256);

    ASSERT_EQ(original.size(), reduced.size());
    for (size_t i = 0; i < original.size(); ++i) {
        EXPECT_NEAR(reduced[i], 0.25 * original[i], 1e-12) << "sample " << i;
    }
}

TEST(WaveguideControlsTest, ANegativeScaleMutesTheAudioMix)
{
    WaveguideNetwork network(GuideType::STRING, 480.0);
    network.pluck(0.5);

    network.set({ .scale = constant_source(-1.0) });

    EXPECT_DOUBLE_EQ(peak(render(network, 256)), 0.0);
}

TEST(WaveguideControlsTest, TypedAndStringMappingsRenderTheSameSound)
{
    WaveguideNetwork by_string(GuideType::STRING, 480.0);
    WaveguideNetwork by_control(GuideType::STRING, 480.0);
    by_string.pluck(0.5);
    by_control.pluck(0.5);

    by_string.map_parameter("frequency", constant_source(240.0));
    by_string.map_parameter("damping", constant_source(0.9));
    by_string.map_parameter("position", constant_source(0.3));
    by_string.map_parameter("scale", constant_source(0.5));
    by_control.set({ .scale = constant_source(0.5),
        .frequency = constant_source(240.0),
        .damping = constant_source(0.9),
        .position = constant_source(0.3) });

    const auto expected = render(by_string, 256);
    const auto actual = render(by_control, 256);

    ASSERT_EQ(actual.size(), expected.size());
    for (size_t i = 0; i < expected.size(); ++i) {
        EXPECT_NEAR(actual[i], expected[i], 1e-12) << "sample " << i;
    }
}

TEST(WaveguideControlsTest, GetReportsValuesChangedThroughTheNetworkItself)
{
    WaveguideNetwork network(GuideType::STRING, 480.0);
    network.set_fundamental(240.0);
    network.set_loss_factor(0.5);
    network.set_pickup_position(0.25);
    network.set_output_scale(0.4);

    const auto controls = network.get();

    EXPECT_DOUBLE_EQ(controls.frequency.value(), 240.0);
    EXPECT_DOUBLE_EQ(controls.damping.value(), 0.5);
    EXPECT_DOUBLE_EQ(controls.position.value(), network.get_pickup_position());
    EXPECT_DOUBLE_EQ(controls.scale.value(), 0.4);
}

TEST(WaveguideControlsTest, GetKeepsAMappedMemberMapped)
{
    WaveguideNetwork network(GuideType::STRING, 480.0);
    network.set({ .damping = constant_source(0.7) });
    render(network, 1);

    const auto controls = network.get();

    EXPECT_TRUE(controls.damping.sourced());
    EXPECT_DOUBLE_EQ(controls.damping.value(), 0.7);
}

TEST(WaveguideControlsTest, AnExciterDurationIsFlooredLikeTheSetter)
{
    WaveguideNetwork network(GuideType::STRING, 480.0);

    network.set({ .exciter_duration = 0.0001 });
    render(network, 1);
    EXPECT_DOUBLE_EQ(network.get().exciter_duration.value(), 0.001);

    network.set({ .exciter_duration = 0.02 });
    render(network, 1);
    EXPECT_DOUBLE_EQ(network.get().exciter_duration.value(), 0.02);
}

TEST(WaveguideControlsTest, VelocityMeasurementReadsADifferentSignalThanPressure)
{
    WaveguideNetwork pressure(GuideType::TUBE, 480.0);
    WaveguideNetwork velocity(GuideType::TUBE, 480.0);
    pressure.pluck(0.3);
    velocity.pluck(0.3);

    velocity.set({ .measurement_mode = WaveguideNetwork::MeasurementMode::VELOCITY });

    const auto expected = render(pressure, 256);
    const auto actual = render(velocity, 256);

    double difference = 0.0;
    for (size_t i = 0; i < expected.size(); ++i) {
        difference = std::max(difference, std::abs(actual[i] - expected[i]));
    }
    EXPECT_GT(difference, 1e-9);
    EXPECT_DOUBLE_EQ(velocity.get().measurement_mode.value(), 1.0);
    EXPECT_DOUBLE_EQ(pressure.get().measurement_mode.value(), 0.0);
}

TEST(WaveguideControlsTest, TheDeprecatedSettersRenderTheSameSoundAsTheControls)
{
    WaveguideNetwork by_setter(GuideType::TUBE, 480.0);
    WaveguideNetwork by_control(GuideType::TUBE, 480.0);
    by_setter.pluck(0.5);
    by_control.pluck(0.5);

    by_setter.set_fundamental(240.0);
    by_setter.set_loss_factor(0.9);
    by_setter.set_pickup_position(0.3);
    by_setter.set_measurement_mode(WaveguideNetwork::MeasurementMode::VELOCITY);
    by_control.set({ .frequency = 240.0,
        .damping = 0.9,
        .position = 0.3,
        .measurement_mode = WaveguideNetwork::MeasurementMode::VELOCITY });

    const auto expected = render(by_setter, 256);
    const auto actual = render(by_control, 256);

    ASSERT_EQ(actual.size(), expected.size());
    for (size_t i = 0; i < expected.size(); ++i) {
        EXPECT_NEAR(actual[i], expected[i], 1e-12) << "sample " << i;
    }
}

TEST(WaveguideControlsTest, AnImmediateSetterAfterATypedSetWinsAndBeforeItLoses)
{
    WaveguideNetwork later(GuideType::STRING, 480.0);
    later.set({ .damping = 0.9, .position = 0.2 });
    later.set_loss_factor(0.8);
    later.set_pickup_position(0.7);
    render(later, 1);
    EXPECT_DOUBLE_EQ(later.get().damping.value(), 0.8);
    EXPECT_NEAR(later.get().position.value(), 0.7, 0.02);

    WaveguideNetwork earlier(GuideType::STRING, 480.0);
    earlier.set_loss_factor(0.8);
    earlier.set({ .damping = 0.9 });
    render(earlier, 1);
    EXPECT_DOUBLE_EQ(earlier.get().damping.value(), 0.9);
}

TEST(WaveguideControlsTest, GetReportsTheExciterDurationAndMeasurementModeTheNetworkHolds)
{
    WaveguideNetwork network(GuideType::STRING, 480.0);

    const auto controls = network.get();

    EXPECT_DOUBLE_EQ(controls.exciter_duration.value(), 0.005);
    EXPECT_DOUBLE_EQ(controls.measurement_mode.value(), 0.0);
}

TEST(WaveguideControlsTest, ANodeAboveHalfSelectsVelocityMeasurement)
{
    WaveguideNetwork by_node(GuideType::TUBE, 480.0);
    WaveguideNetwork by_enum(GuideType::TUBE, 480.0);
    WaveguideNetwork pressure(GuideType::TUBE, 480.0);
    by_node.pluck(0.3);
    by_enum.pluck(0.3);
    pressure.pluck(0.3);

    by_node.set({ .measurement_mode = constant_source(1.0) });
    by_enum.set({ .measurement_mode = WaveguideNetwork::MeasurementMode::VELOCITY });

    const auto expected = render(by_enum, 256);
    const auto actual = render(by_node, 256);
    const auto unchanged = render(pressure, 256);

    ASSERT_EQ(actual.size(), expected.size());
    double difference = 0.0;
    for (size_t i = 0; i < expected.size(); ++i) {
        EXPECT_NEAR(actual[i], expected[i], 1e-12) << "sample " << i;
        difference = std::max(difference, std::abs(actual[i] - unchanged[i]));
    }
    EXPECT_GT(difference, 1e-9);
}

TEST(WaveguideControlsTest, TheDeprecatedGettersAgreeWithGet)
{
    WaveguideNetwork network(GuideType::STRING, 480.0);

    network.set({ .frequency = 240.0,
        .damping = 0.9,
        .position = 0.3,
        .measurement_mode = WaveguideNetwork::MeasurementMode::VELOCITY });
    render(network, 1);

    EXPECT_DOUBLE_EQ(network.get_fundamental(), network.get().frequency.value());
    EXPECT_DOUBLE_EQ(network.get_loss_factor(), network.get().damping.value());
    EXPECT_DOUBLE_EQ(network.get_pickup_position(), network.get().position.value());
    EXPECT_EQ(network.get_measurement_mode(), WaveguideNetwork::MeasurementMode::VELOCITY);
}

}
