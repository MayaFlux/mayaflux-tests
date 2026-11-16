#include "gtest/gtest.h"
#include "../support/test_metadata.hpp"

#include "MayaFlux/Kinesis/Spatial/Bounds.hpp"
#include "MayaFlux/Kinesis/VertexSampler.hpp"
#include "MayaFlux/Nodes/Generators/Polynomial.hpp"
#include "MayaFlux/Nodes/Network/ModalNetwork.hpp"
#include "network_controls_fixtures.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {

using Nodes::Generator::Polynomial;
using Nodes::Network::Control;
using Nodes::Network::ModalControls;
using Nodes::Network::ModalNetwork;

namespace {

    template <typename Network, typename Controls>
    concept AcceptsControls = requires(Network& network, Controls controls) { network.set(controls); };

    std::shared_ptr<Polynomial> constant_source(double value)
    {
        auto source = std::make_shared<Polynomial>([value](double) { return value; });
        source->process_sample(0.0);
        return source;
    }

    std::vector<double> render(ModalNetwork& network, unsigned int samples)
    {
        network.process_batch(samples);
        return network.get_audio_buffer().value_or(std::vector<double> {});
    }

}

TEST(ControlValueTest, AnEmptyControlHoldsNothingAndIsNeverDue)
{
    Control control;

    EXPECT_FALSE(control.has_value());
    EXPECT_FALSE(control.sourced());
    EXPECT_FALSE(control.per_element());
    EXPECT_FALSE(control.due());
    EXPECT_FALSE(control.pending().has_value());
    EXPECT_FALSE(control.at(0).has_value());
    EXPECT_EQ(control.element_count(), 0U);
}

TEST(ControlValueTest, ANumberIsDueOnceUntilItChanges)
{
    Control control = 0.5;

    EXPECT_TRUE(control.has_value());
    EXPECT_FALSE(control.sourced());
    EXPECT_TRUE(control.due());

    const auto first = control.pending();
    ASSERT_TRUE(first.has_value());
    EXPECT_DOUBLE_EQ(*first, 0.5);

    EXPECT_FALSE(control.due());
    EXPECT_FALSE(control.pending().has_value());
    EXPECT_DOUBLE_EQ(control.value(), 0.5);
    EXPECT_DOUBLE_EQ(static_cast<double>(control), 0.5);
}

TEST(ControlValueTest, ANumberAnswersForEveryElement)
{
    const Control control = 0.5;

    ASSERT_TRUE(control.at(0).has_value());
    ASSERT_TRUE(control.at(41).has_value());
    EXPECT_DOUBLE_EQ(*control.at(41), 0.5);
}

TEST(ControlValueTest, AssigningMakesASettledControlDueAgain)
{
    Control live = 1.0;
    live.settle();
    ASSERT_FALSE(live.due());

    const Control given = 2.0;
    live.assign(given);

    EXPECT_TRUE(live.due());
    EXPECT_DOUBLE_EQ(live.value(), 2.0);
}

TEST(ControlValueTest, ANodeSourceIsDueEveryCallAndReadsTheNodeOnRefresh)
{
    Control control(constant_source(3.0));

    EXPECT_TRUE(control.has_value());
    EXPECT_TRUE(control.sourced());
    EXPECT_FALSE(control.per_element());

    control.refresh();
    const auto first = control.pending();
    const auto second = control.pending();
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());
    EXPECT_DOUBLE_EQ(*first, 3.0);
    EXPECT_DOUBLE_EQ(*second, 3.0);
}

TEST(ControlValueTest, ANodeSourceFollowsTheNodeBetweenRefreshes)
{
    auto level = std::make_shared<double>(1.0);
    auto source = std::make_shared<Polynomial>([level](double) { return *level; });
    source->process_sample(0.0);
    Control control(source);

    control.refresh();
    EXPECT_DOUBLE_EQ(control.value(), 1.0);

    *level = 4.0;
    source->process_sample(0.0);
    control.refresh();
    EXPECT_DOUBLE_EQ(control.value(), 4.0);
}

TEST(ControlValueTest, ANetworkSourceIsPerElementAndNeverScalar)
{
    auto network = std::make_shared<ControlsProbeNetwork>(3);
    network->outputs = { 1.0, 2.0, 3.0 };
    Control control(network);

    EXPECT_TRUE(control.sourced());
    EXPECT_TRUE(control.per_element());
    EXPECT_EQ(control.element_count(), 3U);
    EXPECT_TRUE(control.due());
    EXPECT_FALSE(control.pending().has_value());

    ASSERT_TRUE(control.at(1).has_value());
    EXPECT_DOUBLE_EQ(*control.at(1), 2.0);
    EXPECT_FALSE(control.at(3).has_value());
}

TEST(ControlValueTest, ObservingNeverReplacesASource)
{
    Control sourced(constant_source(3.0));
    sourced.refresh();
    sourced.observe(9.0);
    EXPECT_DOUBLE_EQ(sourced.value(), 3.0);

    Control empty;
    empty.observe(9.0);
    EXPECT_TRUE(empty.has_value());
    EXPECT_DOUBLE_EQ(empty.value(), 9.0);
    EXPECT_FALSE(empty.due());
}

TEST(ControlValueTest, ACopyKeepsTheValueTheFlagsAndTheSource)
{
    Control number = 0.5;
    const Control number_copy = number;
    EXPECT_TRUE(number_copy.has_value());
    EXPECT_TRUE(number_copy.due());
    EXPECT_DOUBLE_EQ(number_copy.value(), 0.5);

    number.settle();
    const Control settled_copy = number;
    EXPECT_TRUE(settled_copy.has_value());
    EXPECT_FALSE(settled_copy.due());

    Control node(constant_source(3.0));
    const Control node_copy = node;
    EXPECT_TRUE(node_copy.sourced());
    EXPECT_FALSE(node_copy.per_element());

    const Control empty_copy = Control();
    EXPECT_FALSE(empty_copy.has_value());
}

TEST(ControlValueTest, AMoveTakesTheSourceAndLeavesTheValueReadable)
{
    auto network = std::make_shared<ControlsProbeNetwork>(3);
    Control from(network);
    Control to = std::move(from);

    EXPECT_TRUE(to.per_element());
    EXPECT_EQ(to.element_count(), 3U);
    EXPECT_FALSE(from.sourced());
}

TEST(ControlValueTest, CopyAssignmentReplacesTheValueAndTheSource)
{
    Control target(constant_source(3.0));
    ASSERT_TRUE(target.sourced());

    target = Control(1.5);

    EXPECT_FALSE(target.sourced());
    EXPECT_DOUBLE_EQ(target.value(), 1.5);
    EXPECT_TRUE(target.due());
}

TEST(ControlValueTest, AssigningANumberOverANumberLeavesNoSource)
{
    Control live = 1.0;
    live.settle();

    live.assign(Control(2.0));

    EXPECT_FALSE(live.sourced());
    EXPECT_TRUE(live.due());
    const auto applied = live.pending();
    ASSERT_TRUE(applied.has_value());
    EXPECT_DOUBLE_EQ(*applied, 2.0);
    EXPECT_FALSE(live.pending().has_value());
}

TEST(ControlValueTest, AssigningANumberOverASourceReplacesIt)
{
    Control live(constant_source(3.0));
    ASSERT_TRUE(live.sourced());

    live.assign(Control(2.0));

    EXPECT_FALSE(live.sourced());
    const auto applied = live.pending();
    ASSERT_TRUE(applied.has_value());
    EXPECT_DOUBLE_EQ(*applied, 2.0);
}

TEST(ControlValueTest, ASourceAssignedOverANumberIsAdopted)
{
    Control live = 1.0;

    live.assign(Control(constant_source(3.0)));

    EXPECT_TRUE(live.sourced());
    live.refresh();
    ASSERT_TRUE(live.pending().has_value());
    EXPECT_DOUBLE_EQ(live.value(), 3.0);
}

TEST(ControlValueTest, ANumberAssignedFromAnotherThreadIsSeenInOrderAndNeverTorn)
{
    Control live = 0.0;
    live.settle();
    constexpr int last = 20000;
    std::atomic<bool> done { false };

    std::thread writer([&] {
        for (int i = 1; i <= last; ++i) {
            live.assign(Control(static_cast<double>(i)));
        }
        done.store(true, std::memory_order_release);
    });

    double previous = 0.0;
    bool ordered = true;
    for (;;) {
        const bool finished = done.load(std::memory_order_acquire);
        if (const auto value = live.pending()) {
            ordered = ordered && *value >= previous && *value == std::floor(*value);
            previous = *value;
        }
        if (finished) {
            break;
        }
    }
    writer.join();

    EXPECT_TRUE(ordered);
    EXPECT_DOUBLE_EQ(previous, static_cast<double>(last));
}

TEST(ControlValueTest, ControlsConvertImplicitlyFromEverySourceKind)
{
    static_assert(std::is_convertible_v<double, Control>);
    static_assert(std::is_convertible_v<std::shared_ptr<Polynomial>, Control>);
    static_assert(std::is_convertible_v<std::shared_ptr<ModalNetwork>, Control>);
    static_assert(std::is_convertible_v<std::shared_ptr<ControlsProbeNetwork>, Control>);
    static_assert(!std::is_convertible_v<std::string, Control>);
}

TEST(NetworkControlsTest, OnlyTheGivenMembersAreApplied)
{
    ControlsProbeNetwork network;

    network.set({ .gain = 2.0 });
    network.process_batch(1);

    ASSERT_TRUE(network.gain_seen.has_value());
    EXPECT_DOUBLE_EQ(*network.gain_seen, 2.0);
    EXPECT_TRUE(network.tilt_seen.empty());
}

TEST(NetworkControlsTest, ANumberIsAppliedOnceAndNotRepeated)
{
    ControlsProbeNetwork network;
    network.set({ .gain = 2.0 });

    network.process_batch(1);
    EXPECT_TRUE(network.gain_seen.has_value());

    network.process_batch(1);
    EXPECT_FALSE(network.gain_seen.has_value());
}

TEST(NetworkControlsTest, ANodeSourceIsAppliedEveryBlockAndFollowsTheNode)
{
    auto level = std::make_shared<double>(1.0);
    auto source = std::make_shared<Polynomial>([level](double) { return *level; });
    source->process_sample(0.0);

    ControlsProbeNetwork network;
    network.set({ .gain = source });

    network.process_batch(1);
    ASSERT_TRUE(network.gain_seen.has_value());
    EXPECT_DOUBLE_EQ(*network.gain_seen, 1.0);

    *level = 5.0;
    source->process_sample(0.0);
    network.process_batch(1);
    ASSERT_TRUE(network.gain_seen.has_value());
    EXPECT_DOUBLE_EQ(*network.gain_seen, 5.0);
}

TEST(NetworkControlsTest, ANetworkSourceIsReadPerElement)
{
    auto source = std::make_shared<ControlsProbeNetwork>(3);
    source->outputs = { 0.25, 0.5, 0.75 };

    ControlsProbeNetwork network(3);
    network.set({ .tilt = source });
    network.process_batch(1);

    ASSERT_EQ(network.tilt_seen.size(), 3U);
    EXPECT_DOUBLE_EQ(network.tilt_seen[0], 0.25);
    EXPECT_DOUBLE_EQ(network.tilt_seen[1], 0.5);
    EXPECT_DOUBLE_EQ(network.tilt_seen[2], 0.75);

    source->outputs = { 1.0, 2.0, 3.0 };
    network.process_batch(1);
    ASSERT_EQ(network.tilt_seen.size(), 3U);
    EXPECT_DOUBLE_EQ(network.tilt_seen[2], 3.0);
}

TEST(NetworkControlsTest, ANumberReplacesTheSourceItFollows)
{
    ControlsProbeNetwork network;
    network.set({ .gain = constant_source(8.0) });
    network.process_batch(1);
    ASSERT_TRUE(network.gain_seen.has_value());
    EXPECT_DOUBLE_EQ(*network.gain_seen, 8.0);

    network.set({ .gain = 1.5 });
    network.process_batch(1);
    ASSERT_TRUE(network.gain_seen.has_value());
    EXPECT_DOUBLE_EQ(*network.gain_seen, 1.5);

    network.process_batch(1);
    EXPECT_FALSE(network.gain_seen.has_value());
}

TEST(NetworkControlsTest, AnEmptyMemberLeavesAnExistingSourceAlone)
{
    ControlsProbeNetwork network;
    network.set({ .gain = constant_source(8.0) });
    network.set({ .tilt = 1.0 });

    network.process_batch(1);
    ASSERT_TRUE(network.gain_seen.has_value());
    EXPECT_DOUBLE_EQ(*network.gain_seen, 8.0);

    network.process_batch(1);
    ASSERT_TRUE(network.gain_seen.has_value());
}

TEST(NetworkControlsTest, TheParentAppliesTheSharedScale)
{
    ControlsProbeNetwork network;
    ASSERT_DOUBLE_EQ(network.get_output_scale(), 1.0);

    network.set({ .scale = 0.25 });
    EXPECT_DOUBLE_EQ(network.get_output_scale(), 1.0);

    network.process_batch(1);
    EXPECT_DOUBLE_EQ(network.get_output_scale(), 0.25);
}

TEST(NetworkControlsTest, TheSharedScaleNeverGoesNegative)
{
    ControlsProbeNetwork network;

    network.set({ .scale = -3.0 });
    network.process_batch(1);

    EXPECT_DOUBLE_EQ(network.get_output_scale(), 0.0);
}

TEST(NetworkControlsTest, TheSharedScaleFollowsANodeSource)
{
    auto level = std::make_shared<double>(0.5);
    auto source = std::make_shared<Polynomial>([level](double) { return *level; });
    source->process_sample(0.0);

    ControlsProbeNetwork network;
    network.set({ .scale = source });

    network.process_batch(1);
    EXPECT_DOUBLE_EQ(network.get_output_scale(), 0.5);

    *level = 0.125;
    source->process_sample(0.0);
    network.process_batch(1);
    EXPECT_DOUBLE_EQ(network.get_output_scale(), 0.125);
}

TEST(NetworkControlsTest, GetBeforeAnySetHoldsNothingExceptWhatTheNetworkReports)
{
    ControlsProbeNetwork network;
    network.actual_gain = 3.0;

    const auto controls = network.get();

    EXPECT_FALSE(controls.tilt.has_value());
    EXPECT_FALSE(controls.tilt.sourced());
    ASSERT_TRUE(controls.gain.has_value());
    EXPECT_DOUBLE_EQ(controls.gain.value(), 3.0);
    ASSERT_TRUE(controls.scale.has_value());
    EXPECT_DOUBLE_EQ(controls.scale.value(), 1.0);
}

TEST(NetworkControlsTest, GetReportsAScaleSetOutsideTheControls)
{
    ControlsProbeNetwork network;
    network.set_output_scale(0.4);

    EXPECT_DOUBLE_EQ(network.get().scale.value(), 0.4);
}

TEST(NetworkControlsTest, GetKeepsAMappedMemberMapped)
{
    ControlsProbeNetwork network;
    network.actual_gain = 3.0;
    network.set({ .gain = constant_source(8.0) });
    network.process_batch(1);

    const auto controls = network.get();

    EXPECT_TRUE(controls.gain.sourced());
    EXPECT_DOUBLE_EQ(controls.gain.value(), 8.0);
}

TEST(NetworkControlsTest, GetReturnsACopyThatDoesNotChangeTheNetwork)
{
    ControlsProbeNetwork network;
    network.set({ .gain = 2.0 });

    auto copy = network.get();
    copy.gain = 99.0;

    network.process_batch(1);
    ASSERT_TRUE(network.gain_seen.has_value());
    EXPECT_DOUBLE_EQ(*network.gain_seen, 2.0);
}

TEST(NetworkControlsTest, SettingTheWholeReadBackStructReappliesEverything)
{
    ControlsProbeNetwork network;
    network.actual_gain = 2.0;
    network.set({ .gain = 2.0 });
    network.process_batch(1);
    ASSERT_TRUE(network.gain_seen.has_value());
    network.process_batch(1);
    ASSERT_FALSE(network.gain_seen.has_value());

    network.set(network.get());
    network.process_batch(1);

    ASSERT_TRUE(network.gain_seen.has_value());
    EXPECT_DOUBLE_EQ(*network.gain_seen, 2.0);
}

TEST(NetworkControlsTest, NetworksOwnTheirControlsIndependently)
{
    ControlsProbeNetwork first;
    ControlsProbeNetwork second;

    first.set({ .gain = 2.0 });
    first.process_batch(1);
    second.process_batch(1);

    EXPECT_TRUE(first.gain_seen.has_value());
    EXPECT_FALSE(second.gain_seen.has_value());
}

TEST(NetworkControlsTest, AReadBackCopyOutlivesTheNetwork)
{
    auto network = std::make_shared<ControlsProbeNetwork>();
    network->actual_gain = 2.0;
    const auto copy = network->get();

    network.reset();

    EXPECT_DOUBLE_EQ(copy.gain.value(), 2.0);
}

TEST(NetworkControlsTest, SetAcceptsOnlyTheNetworksOwnControlsType)
{
    static_assert(AcceptsControls<ControlsProbeNetwork, ControlsProbeControls>);
    static_assert(AcceptsControls<ModalNetwork, ModalControls>);
    static_assert(!AcceptsControls<ControlsProbeNetwork, ModalControls>);
    static_assert(!AcceptsControls<ModalNetwork, ControlsProbeControls>);
    static_assert(std::same_as<decltype(std::declval<ModalNetwork&>().get()), ModalControls>);
}

TEST(ModalControlsTest, ANumberFrequencyRetunesEveryMode)
{
    ModalNetwork network(std::vector<double> { 1.0, 1.5, 2.0 }, 220.0);

    network.set({ .frequency = 330.0 });
    render(network, 1);

    EXPECT_DOUBLE_EQ(network.get_fundamental(), 330.0);
    EXPECT_DOUBLE_EQ(network.get_mode(0).current_frequency, 330.0);
    EXPECT_DOUBLE_EQ(network.get_mode(1).current_frequency, 495.0);
    EXPECT_DOUBLE_EQ(network.get_mode(2).current_frequency, 660.0);
}

TEST(ModalControlsTest, ANodeFrequencyRetunesEveryMode)
{
    ModalNetwork network(std::vector<double> { 1.0, 1.5, 2.0 }, 220.0);

    network.set({ .frequency = constant_source(330.0) });
    render(network, 1);

    EXPECT_DOUBLE_EQ(network.get_fundamental(), 330.0);
    EXPECT_DOUBLE_EQ(network.get_mode(1).current_frequency, 495.0);
}

TEST(ModalControlsTest, AnUnsetFrequencyLeavesTheTuningAlone)
{
    ModalNetwork network(2, 220.0);

    network.set({ .decay = 2.0 });
    render(network, 1);

    EXPECT_DOUBLE_EQ(network.get_fundamental(), 220.0);
}

TEST(ModalControlsTest, ANodeDecayChangesTheRingingEnvelope)
{
    ModalNetwork network(1, 440.0, ModalNetwork::Spectrum::HARMONIC, 0.01);
    network.excite_mode(0);

    network.set({ .decay = constant_source(2.0) });
    render(network, 480);

    EXPECT_NEAR(network.get_mode(0).amplitude, std::exp(-0.5), 1e-10);
}

TEST(ModalControlsTest, ANumberDecayChangesTheMultiplier)
{
    ModalNetwork network(1, 440.0);

    network.set({ .decay = 2.0 });
    render(network, 1);

    EXPECT_DOUBLE_EQ(network.get().decay.value(), 2.0);
    EXPECT_DOUBLE_EQ(network.get_mode(0).decay_coefficient,
        std::exp(-1.0 / (2.0 * network.get_sample_rate())));
}

TEST(ModalControlsTest, ANodeAmplitudeScalesTheCurrentRing)
{
    ModalNetwork network(1, 440.0);
    network.excite_mode(0);

    network.set({ .amplitude = constant_source(0.25) });
    render(network, 1);

    EXPECT_NEAR(network.get_mode(0).amplitude,
        0.25 * network.get_mode(0).decay_coefficient, 1e-12);
}

TEST(ModalControlsTest, ANumberAmplitudeScalesTheRingOnce)
{
    ModalNetwork network(1, 440.0);
    network.excite_mode(0);

    network.set({ .amplitude = 0.25 });
    render(network, 1);
    const double decay = network.get_mode(0).decay_coefficient;
    const double after_first = network.get_mode(0).amplitude;
    EXPECT_NEAR(after_first, 0.25 * decay, 1e-12);

    render(network, 1);
    EXPECT_NEAR(network.get_mode(0).amplitude, after_first * decay, 1e-12);
}

TEST(ModalControlsTest, ANetworkAmplitudeScalesMatchingModesIndependently)
{
    auto source = std::make_shared<ModalNetwork>(2, 6000.0);
    render(*source, 2);
    const double first = source->get_node_output(0).value();
    const double second = source->get_node_output(1).value();
    ASSERT_GT(first, 0.0);
    ASSERT_GT(second, 0.0);

    ModalNetwork network(2, 440.0);
    network.excite_mode(0);
    network.excite_mode(1);
    network.set({ .amplitude = source });
    render(network, 1);

    const double decay = network.get_mode(0).decay_coefficient;
    EXPECT_NEAR(network.get_mode(0).amplitude, first * decay, 1e-12);
    EXPECT_NEAR(network.get_mode(1).amplitude, 0.5 * second * decay, 1e-12);
}

TEST(ModalControlsTest, ANetworkDetuneRetunesMatchingModesIndependently)
{
    auto source = std::make_shared<ModalNetwork>(2, 6000.0);
    render(*source, 2);
    const double first = source->get_node_output(0).value();
    const double second = source->get_node_output(1).value();

    ModalNetwork network(2, 440.0);
    network.set({ .detune = source });
    render(network, 1);

    EXPECT_NEAR(network.get_mode(0).current_frequency,
        440.0 * std::pow(2.0, first / 12.0), 1e-12);
    EXPECT_NEAR(network.get_mode(1).current_frequency,
        880.0 * std::pow(2.0, second / 12.0), 1e-12);
}

TEST(ModalControlsTest, ANodeDetuneRetunesEveryMode)
{
    ModalNetwork network(2, 440.0);

    network.set({ .detune = constant_source(12.0) });
    render(network, 1);

    EXPECT_NEAR(network.get_mode(0).current_frequency, 880.0, 1e-9);
    EXPECT_NEAR(network.get_mode(1).current_frequency, 1760.0, 1e-9);
}

TEST(ModalControlsTest, ANetworkWithTheWrongModeCountIsSkipped)
{
    auto source = std::make_shared<ModalNetwork>(1, 6000.0);
    render(*source, 2);

    ModalNetwork network(2, 440.0);
    network.excite_mode(0);
    network.excite_mode(1);
    network.set({ .amplitude = source });
    render(network, 1);

    const double decay = network.get_mode(0).decay_coefficient;
    EXPECT_NEAR(network.get_mode(0).amplitude, decay, 1e-12);
    EXPECT_NEAR(network.get_mode(1).amplitude, 0.5 * decay, 1e-12);
}

TEST(ModalControlsTest, ANodeScaleChangesTheMix)
{
    ModalNetwork reference(1, 440.0);
    reference.excite_mode(0);
    const auto original = render(reference, 64);

    ModalNetwork scaled(1, 440.0);
    scaled.excite_mode(0);
    scaled.set({ .scale = constant_source(0.25) });
    const auto reduced = render(scaled, 64);

    ASSERT_EQ(reduced.size(), original.size());
    for (size_t i = 0; i < original.size(); ++i) {
        EXPECT_NEAR(reduced[i], 0.25 * original[i], 1e-12) << "sample " << i;
    }
}

TEST(ModalControlsTest, TypedAndStringMappingsRenderTheSameSound)
{
    ModalNetwork by_string(3, 220.0, ModalNetwork::Spectrum::INHARMONIC, 0.5);
    ModalNetwork by_control(3, 220.0, ModalNetwork::Spectrum::INHARMONIC, 0.5);
    by_string.excite_mode(0);
    by_control.excite_mode(0);

    by_string.map_parameter("decay", constant_source(1.5));
    by_string.map_parameter("frequency", constant_source(300.0));
    by_string.map_parameter("scale", constant_source(0.5));
    by_control.set({ .scale = constant_source(0.5),
        .frequency = constant_source(300.0),
        .decay = constant_source(1.5) });

    const auto expected = render(by_string, 256);
    const auto actual = render(by_control, 256);

    ASSERT_EQ(actual.size(), expected.size());
    for (size_t i = 0; i < expected.size(); ++i) {
        EXPECT_NEAR(actual[i], expected[i], 1e-12) << "sample " << i;
    }
}

TEST(ModalControlsTest, GetReportsValuesChangedThroughTheNetworkItself)
{
    ModalNetwork network(1, 220.0);
    network.set_fundamental(330.0);
    network.set_decay_multiplier(2.0);
    network.set_output_scale(0.4);

    const auto controls = network.get();

    EXPECT_DOUBLE_EQ(controls.frequency.value(), 330.0);
    EXPECT_DOUBLE_EQ(controls.decay.value(), 2.0);
    EXPECT_DOUBLE_EQ(controls.scale.value(), 0.4);
}

TEST(ControlStoreTest, ADeclaredStructIsOwnedByTheStoreAndMergesIntoItself)
{
    Nodes::Network::ControlStore store;
    auto& live = store.declare<ControlsProbeControls>();

    store.merge(ControlsProbeControls { .gain = 2.0 });

    EXPECT_TRUE(live.gain.has_value());
    EXPECT_DOUBLE_EQ(live.gain.value(), 2.0);
    EXPECT_FALSE(live.tilt.has_value());
}

TEST(ControlStoreTest, AStructOfAnotherTypeIsIgnored)
{
    Nodes::Network::ControlStore store;
    auto& live = store.declare<ControlsProbeControls>();

    store.merge(ControlsProbeOperatorControls { .gain = 5.0 });

    EXPECT_FALSE(live.gain.has_value());
}

TEST(ControlStoreTest, MergingBeforeAnythingIsDeclaredDoesNothing)
{
    Nodes::Network::ControlStore store;

    EXPECT_NO_THROW(store.merge(ControlsProbeControls { .gain = 2.0 }));
}

TEST(ControlStoreTest, ASnapshotBeforeAnythingIsDeclaredIsAnEmptyStruct)
{
    const Nodes::Network::ControlStore store;

    const auto controls = store.snapshot<ControlsProbeControls>();

    EXPECT_FALSE(controls.gain.has_value());
    EXPECT_FALSE(controls.scale.has_value());
}

TEST(ControlStoreTest, ASnapshotIsACopyThatLeavesTheStoreAlone)
{
    Nodes::Network::ControlStore store;
    auto& live = store.declare<ControlsProbeControls>();
    store.merge(ControlsProbeControls { .gain = 2.0 });

    auto copy = store.snapshot<ControlsProbeControls>();
    copy.gain = 9.0;

    EXPECT_DOUBLE_EQ(live.gain.value(), 2.0);
}

TEST(ControlStoreTest, TheScaleIsWrittenOnlyWhereTheHostPointsAndOnlyWhenDue)
{
    Nodes::Network::ControlStore store;
    auto& live = store.declare<ControlsProbeControls>();
    store.merge(ControlsProbeControls { .scale = 0.25 });
    double target = 1.0;

    store.refresh();
    EXPECT_DOUBLE_EQ(target, 1.0);
    EXPECT_TRUE(live.scale.due());

    store.refresh(&target);
    EXPECT_DOUBLE_EQ(target, 0.25);

    target = 0.9;
    store.refresh(&target);
    EXPECT_DOUBLE_EQ(target, 0.9);
}

TEST(ControlStoreTest, ANegativeScaleIsWrittenAsZero)
{
    Nodes::Network::ControlStore store;
    static_cast<void>(store.declare<ControlsProbeControls>());
    store.merge(ControlsProbeControls { .scale = -2.0 });
    double target = 1.0;

    store.refresh(&target);

    EXPECT_DOUBLE_EQ(target, 0.0);
}

TEST(ControlStoreTest, ASnapshotShowsTheHostsRealScale)
{
    Nodes::Network::ControlStore store;
    static_cast<void>(store.declare<ControlsProbeControls>());
    const double real = 0.4;

    EXPECT_DOUBLE_EQ(store.snapshot<ControlsProbeControls>(&real).scale.value(), 0.4);
    EXPECT_FALSE(store.snapshot<ControlsProbeControls>().scale.has_value());
}

TEST(ControlStoreTest, AStructWithoutAScaleMemberIgnoresTheScaleTarget)
{
    Nodes::Network::ControlStore store;
    static_cast<void>(store.declare<ControlsProbeOperatorControls>());
    store.merge(ControlsProbeOperatorControls { .gain = 2.0 });
    double target = 1.0;

    store.refresh(&target);

    EXPECT_DOUBLE_EQ(target, 1.0);
}

TEST(ControlStoreTest, DeclaringAgainReplacesTheStruct)
{
    Nodes::Network::ControlStore store;
    static_cast<void>(store.declare<ControlsProbeControls>());
    store.merge(ControlsProbeControls { .gain = 2.0 });

    auto& fresh = store.declare<ControlsProbeControls>();

    EXPECT_FALSE(fresh.gain.has_value());
}

TEST(ControlStoreTest, RefreshReadsEveryNodeSource)
{
    Nodes::Network::ControlStore store;
    auto& live = store.declare<ControlsProbeControls>();
    store.merge(ControlsProbeControls { .gain = constant_source(3.0), .tilt = constant_source(4.0) });

    store.refresh();

    EXPECT_DOUBLE_EQ(live.gain.value(), 3.0);
    EXPECT_DOUBLE_EQ(live.tilt.value(), 4.0);
}

TEST(ControlTypeTest, AVectorIsHeldWholeAndReadBackAsItsType)
{
    Control control = glm::vec3(1.0F, 2.0F, 3.0F);

    EXPECT_TRUE(control.has_value());
    EXPECT_EQ(control.value<glm::vec3>(), glm::vec3(1.0F, 2.0F, 3.0F));
    const auto taken = control.pending<glm::vec3>();
    ASSERT_TRUE(taken.has_value());
    EXPECT_EQ(*taken, glm::vec3(1.0F, 2.0F, 3.0F));
    EXPECT_FALSE(control.pending<glm::vec3>().has_value());
}

TEST(ControlTypeTest, EveryGlmVectorWidthRoundTrips)
{
    EXPECT_EQ(Control(glm::vec2(1.0F, 2.0F)).value<glm::vec2>(), glm::vec2(1.0F, 2.0F));
    EXPECT_EQ(Control(glm::vec3(1.0F, 2.0F, 3.0F)).value<glm::vec3>(), glm::vec3(1.0F, 2.0F, 3.0F));
    EXPECT_EQ(Control(glm::vec4(1.0F, 2.0F, 3.0F, 4.0F)).value<glm::vec4>(), glm::vec4(1.0F, 2.0F, 3.0F, 4.0F));
}

TEST(ControlTypeTest, IntegersBoolsAndEnumsRoundTripExactly)
{
    enum class Mode : uint8_t { A,
        B,
        C };

    EXPECT_EQ(Control(7).value<int>(), 7);
    EXPECT_EQ(Control(size_t { 9007199254740992ULL }).value<size_t>(), size_t { 9007199254740992ULL });
    EXPECT_TRUE(Control(true).value<bool>());
    EXPECT_FALSE(Control(false).value<bool>());
    EXPECT_EQ(Control(Mode::C).value<Mode>(), Mode::C);
    EXPECT_FLOAT_EQ(Control(0.25F).value<float>(), 0.25F);
}

TEST(ControlTypeTest, ReadingAnotherTypeThanWasSetFindsNothing)
{
    Control scalar = 2.0;
    Control vector = glm::vec3(1.0F);

    EXPECT_FALSE(scalar.pending<glm::vec3>().has_value());
    EXPECT_FALSE(vector.pending().has_value());
    EXPECT_FALSE(vector.pending<glm::vec2>().has_value());
    EXPECT_TRUE(scalar.pending().has_value());
    EXPECT_TRUE(vector.pending<glm::vec3>().has_value());
}

TEST(ControlTypeTest, ADoubleStillReadsAsADoubleWithNoType)
{
    Control control = 0.75;

    const auto taken = control.pending();

    ASSERT_TRUE(taken.has_value());
    EXPECT_DOUBLE_EQ(*taken, 0.75);
    EXPECT_DOUBLE_EQ(control.value(), 0.75);
    EXPECT_DOUBLE_EQ(static_cast<double>(control), 0.75);
}

TEST(ControlTypeTest, AssigningAVectorOverANumberChangesTheTypeItReads)
{
    Control live = 1.0;

    live.assign(Control(glm::vec3(4.0F, 5.0F, 6.0F)));

    EXPECT_FALSE(live.pending().has_value());
    EXPECT_EQ(live.value<glm::vec3>(), glm::vec3(4.0F, 5.0F, 6.0F));
}

TEST(ControlTypeTest, AVectorMergedWithAnEmptyOneKeepsItsComponents)
{
    Control live = glm::vec3(1.0F, 2.0F, 3.0F);

    if (const Control empty; empty.has_value()) {
        live.assign(empty);
    }

    EXPECT_EQ(live.value<glm::vec3>(), glm::vec3(1.0F, 2.0F, 3.0F));
}

TEST(ControlTypeTest, ObservingAVectorShowsTheOwnersValueWithoutMakingItDue)
{
    Control control;

    control.observe(glm::vec3(0.0F, -9.8F, 0.0F));

    EXPECT_TRUE(control.has_value());
    EXPECT_EQ(control.value<glm::vec3>(), glm::vec3(0.0F, -9.8F, 0.0F));
    EXPECT_FALSE(control.due());
}

TEST(ControlTypeTest, ACopyOfAVectorKeepsEveryComponent)
{
    const Control original = glm::vec4(1.0F, 2.0F, 3.0F, 4.0F);

    const Control copy = original;

    EXPECT_EQ(copy.value<glm::vec4>(), glm::vec4(1.0F, 2.0F, 3.0F, 4.0F));
    EXPECT_TRUE(copy.due());
}

TEST(ControlTypeTest, AVectorSetFromAnotherThreadIsNeverSeenTorn)
{
    Control live = glm::vec3(0.0F);
    static_cast<void>(live.pending<glm::vec3>());
    constexpr int last = 20000;
    std::atomic<bool> done { false };

    std::thread writer([&] {
        for (int i = 1; i <= last; ++i) {
            const auto value = static_cast<float>(i);
            live.assign(Control(glm::vec3(value, value, value)));
        }
        done.store(true, std::memory_order_release);
    });

    bool whole = true;
    float previous = 0.0F;
    for (;;) {
        const bool finished = done.load(std::memory_order_acquire);
        if (const auto taken = live.pending<glm::vec3>()) {
            whole = whole && taken->x == taken->y && taken->y == taken->z && taken->x >= previous;
            previous = taken->x;
        }
        if (finished) {
            break;
        }
    }
    writer.join();

    EXPECT_TRUE(whole);
    EXPECT_FLOAT_EQ(previous, static_cast<float>(last));
}

TEST(ControlTypeTest, ControlsConvertImplicitlyFromEveryValueKind)
{
    static_assert(std::is_convertible_v<double, Control>);
    static_assert(std::is_convertible_v<float, Control>);
    static_assert(std::is_convertible_v<int, Control>);
    static_assert(std::is_convertible_v<size_t, Control>);
    static_assert(std::is_convertible_v<bool, Control>);
    static_assert(std::is_convertible_v<glm::vec2, Control>);
    static_assert(std::is_convertible_v<glm::vec3, Control>);
    static_assert(std::is_convertible_v<glm::vec4, Control>);
    static_assert(!std::is_convertible_v<std::string, Control>);
    static_assert(!std::is_convertible_v<std::vector<double>, Control>);
}

TEST(ControlBoxTest, ControlsConvertFromAnyStructOfTwoCornersAndNothingElse)
{
    struct Scalars {
        float min;
        float max;
    };
    struct ThreeCorners {
        glm::vec3 min;
        glm::vec3 max;
        glm::vec3 mid;
    };
    struct WideCorners {
        glm::vec4 min;
        glm::vec4 max;
    };

    static_assert(std::is_convertible_v<Kinesis::AABB3D, Control>);
    static_assert(std::is_convertible_v<Kinesis::SamplerBounds, Control>);
    static_assert(!std::is_convertible_v<Scalars, Control>);
    static_assert(!std::is_convertible_v<ThreeCorners, Control>);
    static_assert(!std::is_convertible_v<WideCorners, Control>);
    static_assert(!Control::is_box<glm::vec3>);
    static_assert(!Control::is_box<double>);
    SUCCEED();
}

TEST(ControlBoxTest, ABoxIsHeldWholeAndReadBackAsItsType)
{
    Control control = Kinesis::AABB3D { .min = { -1.0F, -2.0F, -3.0F }, .max = { 4.0F, 5.0F, 6.0F } };

    EXPECT_TRUE(control.has_value());
    EXPECT_EQ(control.width(), 6U);
    EXPECT_EQ(control.value<Kinesis::AABB3D>().min, glm::vec3(-1.0F, -2.0F, -3.0F));
    EXPECT_EQ(control.value<Kinesis::AABB3D>().max, glm::vec3(4.0F, 5.0F, 6.0F));

    const auto taken = control.pending<Kinesis::AABB3D>();
    ASSERT_TRUE(taken.has_value());
    EXPECT_EQ(taken->min, glm::vec3(-1.0F, -2.0F, -3.0F));
    EXPECT_EQ(taken->max, glm::vec3(4.0F, 5.0F, 6.0F));
    EXPECT_FALSE(control.pending<Kinesis::AABB3D>().has_value());
}

TEST(ControlBoxTest, SamplerBoundsRoundTripAndKeepTheirDerivedValues)
{
    Control control = Kinesis::SamplerBounds { .min = glm::vec3(-2.0F), .max = glm::vec3(4.0F) };

    const auto bounds = control.pending<Kinesis::SamplerBounds>();

    ASSERT_TRUE(bounds.has_value());
    EXPECT_EQ(bounds->center(), glm::vec3(1.0F));
    EXPECT_EQ(bounds->extent(), glm::vec3(6.0F));
}

TEST(ControlBoxTest, ABoxIsNeverReadAsANumberOrAVector)
{
    Control box = Kinesis::AABB3D { .min = glm::vec3(0.0F), .max = glm::vec3(1.0F) };
    Control wide = glm::vec4(1.0F);

    EXPECT_FALSE(box.pending().has_value());
    EXPECT_FALSE(box.pending<glm::vec3>().has_value());
    EXPECT_FALSE(box.pending<glm::vec4>().has_value());
    EXPECT_FALSE(wide.pending<Kinesis::AABB3D>().has_value());
    EXPECT_TRUE(box.pending<Kinesis::AABB3D>().has_value());
}

TEST(ControlBoxTest, AssigningABoxOverAVectorChangesTheTypeItReads)
{
    Control live = glm::vec3(1.0F);

    live.assign(Control(Kinesis::AABB3D { .min = glm::vec3(-1.0F), .max = glm::vec3(1.0F) }));

    EXPECT_FALSE(live.pending<glm::vec3>().has_value());
    EXPECT_EQ(live.value<Kinesis::AABB3D>().max, glm::vec3(1.0F));
}

TEST(ControlBoxTest, ObservingABoxShowsTheOwnersValueWithoutMakingItDue)
{
    Control control;

    control.observe(Kinesis::SamplerBounds { .min = glm::vec3(-10.0F), .max = glm::vec3(10.0F) });

    EXPECT_TRUE(control.has_value());
    EXPECT_EQ(control.value<Kinesis::SamplerBounds>().max, glm::vec3(10.0F));
    EXPECT_FALSE(control.due());
}

TEST(ControlBoxTest, ACopyOfABoxKeepsBothCorners)
{
    const Control original = Kinesis::AABB3D { .min = { 1.0F, 2.0F, 3.0F }, .max = { 4.0F, 5.0F, 6.0F } };

    const Control copy = original;

    EXPECT_EQ(copy.value<Kinesis::AABB3D>().min, glm::vec3(1.0F, 2.0F, 3.0F));
    EXPECT_EQ(copy.value<Kinesis::AABB3D>().max, glm::vec3(4.0F, 5.0F, 6.0F));
    EXPECT_TRUE(copy.due());
}

TEST(ControlBoxTest, ABoxSetFromAnotherThreadIsNeverSeenWithMismatchedCorners)
{
    Control live = Kinesis::AABB3D { .min = glm::vec3(0.0F), .max = glm::vec3(0.0F) };
    static_cast<void>(live.pending<Kinesis::AABB3D>());
    constexpr int last = 20000;
    std::atomic<bool> done { false };

    std::thread writer([&] {
        for (int i = 1; i <= last; ++i) {
            const auto value = static_cast<float>(i);
            live.assign(Control(Kinesis::AABB3D { .min = glm::vec3(-value), .max = glm::vec3(value) }));
        }
        done.store(true, std::memory_order_release);
    });

    bool whole = true;
    float previous = 0.0F;
    for (;;) {
        const bool finished = done.load(std::memory_order_acquire);
        if (const auto taken = live.pending<Kinesis::AABB3D>()) {
            whole = whole && taken->min == -taken->max && taken->max.x == taken->max.z && taken->max.x >= previous;
            previous = taken->max.x;
        }
        if (finished) {
            break;
        }
    }
    writer.join();

    EXPECT_TRUE(whole);
    EXPECT_FLOAT_EQ(previous, static_cast<float>(last));
}

TEST(ControlStoreTest, AVectorControlMergesAndIsReadBackFromTheStruct)
{
    Nodes::Network::ControlStore store;
    auto& live = store.declare<ControlsProbeOperatorControls>();

    store.merge(ControlsProbeOperatorControls { .direction = glm::vec3(1.0F, 2.0F, 3.0F) });

    const auto taken = live.direction.pending<glm::vec3>();
    ASSERT_TRUE(taken.has_value());
    EXPECT_EQ(*taken, glm::vec3(1.0F, 2.0F, 3.0F));
    EXPECT_FALSE(live.direction.pending<glm::vec3>().has_value());
    EXPECT_EQ(live.direction.value<glm::vec3>(), glm::vec3(1.0F, 2.0F, 3.0F));
}

TEST(ControlStoreTest, AnEmptyVectorMemberLeavesTheHeldVectorAlone)
{
    Nodes::Network::ControlStore store;
    auto& live = store.declare<ControlsProbeOperatorControls>();
    store.merge(ControlsProbeOperatorControls { .direction = glm::vec3(4.0F) });
    static_cast<void>(live.direction.pending<glm::vec3>());

    store.merge(ControlsProbeOperatorControls { .gain = 2.0 });

    EXPECT_FALSE(live.direction.pending<glm::vec3>().has_value());
    EXPECT_EQ(live.direction.value<glm::vec3>(), glm::vec3(4.0F));
}

TEST(OperatorControlsTest, AVectorIsAppliedOnceAndReadsBack)
{
    ControlsProbeOperator op;

    op.set({ .direction = glm::vec3(0.0F, 1.0F, 0.0F) });
    op.process(1.0F);
    ASSERT_TRUE(op.direction_seen.has_value());
    EXPECT_EQ(*op.direction_seen, glm::vec3(0.0F, 1.0F, 0.0F));

    op.process(1.0F);
    EXPECT_FALSE(op.direction_seen.has_value());
    EXPECT_EQ(op.get().direction.value<glm::vec3>(), glm::vec3(0.0F, 1.0F, 0.0F));
}

TEST(ControlTypeTest, AConstexprLocalNeedsNoCaptureInALambda)
{
    constexpr double scatter = 0.5;
    constexpr int neighbours = 3;

    const auto number = [] { return Control(scatter); };
    const auto count = [] { return Control(neighbours); };

    EXPECT_DOUBLE_EQ(number().value(), 0.5);
    EXPECT_EQ(count().value<int>(), 3);
}

TEST(ControlTypeTest, ANodeSourceReadsAsAnyScalarType)
{
    Control control(constant_source(3.0));
    control.refresh();

    ASSERT_TRUE(control.pending<int>().has_value());
    EXPECT_EQ(*control.pending<int>(), 3);
    EXPECT_FLOAT_EQ(*control.pending<float>(), 3.0F);
    EXPECT_TRUE(*control.pending<bool>());
    EXPECT_DOUBLE_EQ(*control.pending(), 3.0);
}

TEST(ControlTypeTest, ANodeSourceIsNeverAVector)
{
    Control control(constant_source(3.0));
    control.refresh();

    EXPECT_FALSE(control.pending<glm::vec3>().has_value());
}

TEST(ControlTypeTest, ANetworkSourceNeverPendsAsAnyType)
{
    auto network = std::make_shared<ControlsProbeNetwork>(3);
    Control control(network);

    EXPECT_FALSE(control.pending().has_value());
    EXPECT_FALSE(control.pending<int>().has_value());
    EXPECT_FALSE(control.pending<glm::vec3>().has_value());
    EXPECT_TRUE(control.due());
}

TEST(ControlTypeTest, AnEnumPendsOnceAsItsOwnType)
{
    enum class Mode : uint8_t { A,
        B,
        C };
    Control control = Mode::B;

    const auto taken = control.pending<Mode>();

    ASSERT_TRUE(taken.has_value());
    EXPECT_EQ(*taken, Mode::B);
    EXPECT_FALSE(control.pending<Mode>().has_value());
}

TEST(ControlTypeTest, AVectorIsDueUntilSettledAndDueAgainWhenAssigned)
{
    Control live = glm::vec3(1.0F);

    EXPECT_TRUE(live.due());
    live.settle();
    EXPECT_FALSE(live.due());

    live.assign(Control(glm::vec3(2.0F)));

    EXPECT_TRUE(live.due());
    EXPECT_EQ(live.value<glm::vec3>(), glm::vec3(2.0F));
}

TEST(ControlTypeTest, AnEmptyControlHoldsNoTypeAndPendsNothing)
{
    Control control;

    EXPECT_FALSE(control.has_value());
    EXPECT_FALSE(control.pending().has_value());
    EXPECT_FALSE(control.pending<glm::vec3>().has_value());
    EXPECT_FALSE(control.pending<int>().has_value());
}

TEST(ControlTypeTest, ObservingAScalarOfAnyTypeShowsItAsThatType)
{
    Control control;

    control.observe(size_t { 12 });

    EXPECT_EQ(control.value<size_t>(), size_t { 12 });
    EXPECT_DOUBLE_EQ(control.value(), 12.0);
    EXPECT_FALSE(control.due());
}

TEST(ControlStoreTest, ANamedSetReachesTheMemberLikeASet)
{
    Nodes::Network::ControlStore store;
    auto& live = store.declare<ControlsProbeOperatorControls>();

    EXPECT_TRUE(store.set_named("gain", 2.0));

    const auto taken = live.gain.pending();
    ASSERT_TRUE(taken.has_value());
    EXPECT_DOUBLE_EQ(*taken, 2.0);
}

TEST(ControlStoreTest, AnUnknownNameSetsNothing)
{
    Nodes::Network::ControlStore store;
    auto& live = store.declare<ControlsProbeOperatorControls>();

    EXPECT_FALSE(store.set_named("nope", 2.0));
    EXPECT_FALSE(store.set_named("Gain", 2.0));
    EXPECT_FALSE(live.gain.has_value());
}

TEST(ControlStoreTest, ANumberCannotReplaceAHeldVector)
{
    Nodes::Network::ControlStore store;
    auto& live = store.declare<ControlsProbeOperatorControls>();
    store.merge(ControlsProbeOperatorControls { .direction = glm::vec3(1.0F, 2.0F, 3.0F) });

    EXPECT_FALSE(store.set_named("direction", 9.0));

    EXPECT_EQ(live.direction.value<glm::vec3>(), glm::vec3(1.0F, 2.0F, 3.0F));
}

TEST(ControlStoreTest, ANamedReadGivesTheNumberASnapshotHolds)
{
    Nodes::Network::ControlStore store;
    static_cast<void>(store.declare<ControlsProbeOperatorControls>());
    store.merge(ControlsProbeOperatorControls { .gain = 2.5, .direction = glm::vec3(1.0F) });
    const auto copy = store.snapshot<ControlsProbeOperatorControls>();

    EXPECT_EQ(Nodes::Network::ControlStore::read_named(copy, "gain"), 2.5);
    EXPECT_FALSE(Nodes::Network::ControlStore::read_named(copy, "tilt").has_value());
    EXPECT_FALSE(Nodes::Network::ControlStore::read_named(copy, "direction").has_value());
    EXPECT_FALSE(Nodes::Network::ControlStore::read_named(copy, "nope").has_value());
}

TEST(ControlStoreTest, AStoreCannotBeCopiedOrMoved)
{
    static_assert(!std::is_copy_constructible_v<Nodes::Network::ControlStore>);
    static_assert(!std::is_copy_assignable_v<Nodes::Network::ControlStore>);
    static_assert(!std::is_move_constructible_v<Nodes::Network::ControlStore>);
    static_assert(!std::is_move_assignable_v<Nodes::Network::ControlStore>);
}

TEST(ControlStoreTest, ThereforeAHostThatOwnsOneCannotBeCopiedEither)
{
    static_assert(!std::is_copy_constructible_v<ControlsProbeNetwork>);
    static_assert(!std::is_copy_constructible_v<ControlsProbeOperator>);
    static_assert(!std::is_move_constructible_v<ControlsProbeOperator>);
}

TEST(OperatorControlsTest, OnlyTheGivenMembersAreApplied)
{
    ControlsProbeOperator op;

    op.set({ .gain = 2.0 });
    op.process(1.0F);

    ASSERT_TRUE(op.gain_seen.has_value());
    EXPECT_DOUBLE_EQ(*op.gain_seen, 2.0);
    EXPECT_TRUE(op.tilt_seen.empty());
}

TEST(OperatorControlsTest, ANumberIsAppliedOnceAndNotRepeated)
{
    ControlsProbeOperator op;
    op.set({ .gain = 2.0 });

    op.process(1.0F);
    EXPECT_TRUE(op.gain_seen.has_value());

    op.process(1.0F);
    EXPECT_FALSE(op.gain_seen.has_value());
}

TEST(OperatorControlsTest, ANodeIsAppliedEveryBlockAndFollowsTheNode)
{
    auto level = std::make_shared<double>(1.0);
    auto source = std::make_shared<Polynomial>([level](double) { return *level; });
    source->process_sample(0.0);

    ControlsProbeOperator op;
    op.set({ .gain = source });

    op.process(1.0F);
    ASSERT_TRUE(op.gain_seen.has_value());
    EXPECT_DOUBLE_EQ(*op.gain_seen, 1.0);

    *level = 5.0;
    source->process_sample(0.0);
    op.process(1.0F);
    ASSERT_TRUE(op.gain_seen.has_value());
    EXPECT_DOUBLE_EQ(*op.gain_seen, 5.0);
}

TEST(OperatorControlsTest, ANetworkSourceIsReadPerElement)
{
    auto source = std::make_shared<ControlsProbeNetwork>(3);
    source->outputs = { 0.25, 0.5, 0.75 };

    ControlsProbeOperator op;
    op.set({ .tilt = source });
    op.process(1.0F);

    ASSERT_EQ(op.tilt_seen.size(), 3U);
    EXPECT_DOUBLE_EQ(op.tilt_seen[0], 0.25);
    EXPECT_DOUBLE_EQ(op.tilt_seen[2], 0.75);
}

TEST(OperatorControlsTest, ANumberReplacesTheSourceItFollows)
{
    ControlsProbeOperator op;
    op.set({ .gain = constant_source(8.0) });
    op.process(1.0F);
    ASSERT_TRUE(op.gain_seen.has_value());

    op.set({ .gain = 1.5 });
    op.process(1.0F);
    ASSERT_TRUE(op.gain_seen.has_value());
    EXPECT_DOUBLE_EQ(*op.gain_seen, 1.5);

    op.process(1.0F);
    EXPECT_FALSE(op.gain_seen.has_value());
}

TEST(OperatorControlsTest, AnEmptyMemberLeavesAnExistingSourceAlone)
{
    ControlsProbeOperator op;
    op.set({ .gain = constant_source(8.0) });
    op.set({ .tilt = 1.0 });

    op.process(1.0F);
    op.process(1.0F);

    ASSERT_TRUE(op.gain_seen.has_value());
    EXPECT_DOUBLE_EQ(*op.gain_seen, 8.0);
}

TEST(OperatorControlsTest, GetReportsWhatTheOperatorHolds)
{
    ControlsProbeOperator op;
    op.actual_gain = 3.0;

    const auto controls = op.get();

    ASSERT_TRUE(controls.gain.has_value());
    EXPECT_DOUBLE_EQ(controls.gain.value(), 3.0);
    EXPECT_FALSE(controls.tilt.has_value());
}

TEST(OperatorControlsTest, GetKeepsAMappedMemberMapped)
{
    ControlsProbeOperator op;
    op.set({ .gain = constant_source(8.0) });
    op.process(1.0F);

    const auto controls = op.get();

    EXPECT_TRUE(controls.gain.sourced());
    EXPECT_DOUBLE_EQ(controls.gain.value(), 8.0);
}

TEST(OperatorControlsTest, OperatorsOwnTheirControlsIndependently)
{
    ControlsProbeOperator first;
    ControlsProbeOperator second;

    first.set({ .gain = 2.0 });
    first.process(1.0F);
    second.process(1.0F);

    EXPECT_TRUE(first.gain_seen.has_value());
    EXPECT_FALSE(second.gain_seen.has_value());
}

TEST(OperatorControlsTest, AnOperatorInAChainRunsItsControlsWithTheChain)
{
    ControlsProbeNetwork network;
    const auto op = network.get_operator_chain()->emplace<ControlsProbeOperator>();

    op->set({ .gain = 4.0 });
    network.get_operator_chain()->process(1.0F);

    ASSERT_TRUE(op->gain_seen.has_value());
    EXPECT_DOUBLE_EQ(*op->gain_seen, 4.0);
    EXPECT_EQ(op->blocks, 1);
}

TEST(OperatorControlsTest, SetAcceptsOnlyTheOperatorsOwnControlsType)
{
    static_assert(AcceptsControls<ControlsProbeOperator, ControlsProbeOperatorControls>);
    static_assert(!AcceptsControls<ControlsProbeOperator, ControlsProbeControls>);
    static_assert(!AcceptsControls<ControlsProbeNetwork, ControlsProbeOperatorControls>);
    static_assert(!AcceptsControls<Nodes::Network::NetworkOperator, ControlsProbeOperatorControls>);
    static_assert(std::same_as<decltype(std::declval<ControlsProbeOperator&>().get()), ControlsProbeOperatorControls>);
}

TEST(OperatorControlsTest, ANumberAssignedFromAnotherThreadIsNeverLost)
{
    ControlsProbeOperator op;
    constexpr int last = 5000;
    std::atomic<bool> done { false };

    std::thread writer([&] {
        for (int i = 1; i <= last; ++i) {
            op.set({ .gain = static_cast<double>(i) });
        }
        done.store(true, std::memory_order_release);
    });

    double previous = 0.0;
    bool ordered = true;
    for (;;) {
        const bool finished = done.load(std::memory_order_acquire);
        op.process(1.0F);
        if (op.gain_seen) {
            ordered = ordered && *op.gain_seen >= previous;
            previous = *op.gain_seen;
        }
        if (finished) {
            break;
        }
    }
    writer.join();

    EXPECT_TRUE(ordered);
    EXPECT_DOUBLE_EQ(previous, static_cast<double>(last));
}

TEST(ModalControlsTest, TheTypedPathWorksThroughASharedPointerAndTheBase)
{
    auto network = std::make_shared<ModalNetwork>(1, 220.0);
    std::shared_ptr<Nodes::Network::NodeNetwork> base = network;

    network->set({ .frequency = 440.0 });
    base->process_batch(1);

    EXPECT_DOUBLE_EQ(network->get_fundamental(), 440.0);
}

}
