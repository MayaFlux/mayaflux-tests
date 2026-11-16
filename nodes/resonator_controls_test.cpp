#include "gtest/gtest.h"
#include "../support/test_metadata.hpp"

#include "MayaFlux/Kinesis/Discrete/Coefficients.hpp"
#include "MayaFlux/Nodes/Generators/Polynomial.hpp"
#include "MayaFlux/Nodes/Network/ResonatorNetwork.hpp"
#include "network_controls_fixtures.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {

using Nodes::Generator::Polynomial;
using Nodes::Network::ResonatorNetwork;

namespace {

    std::shared_ptr<Polynomial> constant_source(double value)
    {
        auto source = std::make_shared<Polynomial>([value](double) { return value; });
        source->process_sample(0.0);
        return source;
    }

    std::shared_ptr<Nodes::Node> pulse()
    {
        auto fired = std::make_shared<bool>(false);
        return std::make_shared<Polynomial>([fired](double) {
            if (*fired) {
                return 0.0;
            }
            *fired = true;
            return 1.0;
        });
    }

    std::shared_ptr<ResonatorNetwork> single(double frequency, double q)
    {
        auto network = std::make_shared<ResonatorNetwork>(
            std::vector<double> { frequency }, std::vector<double> { q });
        network->set_exciter(pulse());
        return network;
    }

    std::shared_ptr<ResonatorNetwork> trio()
    {
        return std::make_shared<ResonatorNetwork>(
            std::vector<double> { 500.0, 1000.0, 1500.0 },
            std::vector<double> { 10.0, 10.0, 10.0 });
    }

    std::vector<double> render(const std::shared_ptr<ResonatorNetwork>& network, unsigned int samples)
    {
        network->process_batch(samples);
        return network->get_audio_buffer().value_or(std::vector<double> {});
    }

}

TEST(ResonatorControlsTest, ANodeRetuneKeepsTheRingWhereANodeFrequencyClearsIt)
{
    auto cleared = single(1200.0, 30.0);
    auto kept = single(1200.0, 30.0);
    render(cleared, 256);
    render(kept, 256);

    cleared->set({ .frequency = constant_source(1250.0) });
    kept->set({ .retune = constant_source(1250.0) });

    const auto after_clear = render(cleared, 1);
    const auto after_retune = render(kept, 1);

    EXPECT_EQ(after_clear.at(0), 0.0);
    EXPECT_GT(std::abs(after_retune.at(0)), 1e-9);
    EXPECT_DOUBLE_EQ(kept->get_resonators().at(0).frequency, 1250.0);
}

TEST(ResonatorControlsTest, ANumberFrequencyRetunesEveryResonator)
{
    auto network = trio();

    network->set({ .frequency = 700.0 });
    network->process_batch(1);

    for (const auto& resonator : network->get_resonators()) {
        EXPECT_DOUBLE_EQ(resonator.frequency, 700.0);
    }
}

TEST(ResonatorControlsTest, ANumberDecaySetsTheRingTime)
{
    auto network = single(1000.0, 30.0);

    network->set({ .decay = 0.25 });
    network->process_batch(1);

    EXPECT_NEAR(network->get_resonators().at(0).q,
        Kinesis::Discrete::q_from_decay(1000.0, 0.25, network->get_sample_rate()), 1e-6);
}

TEST(ResonatorControlsTest, ANodeDecaySetsTheRingTime)
{
    auto network = single(1000.0, 30.0);

    network->set({ .decay = constant_source(0.25) });
    network->process_batch(1);

    EXPECT_NEAR(network->get_resonators().at(0).q,
        Kinesis::Discrete::q_from_decay(1000.0, 0.25, network->get_sample_rate()), 1e-6);
}

TEST(ResonatorControlsTest, FrequencyIsAppliedBeforeDecay)
{
    auto network = single(1000.0, 30.0);

    network->set({ .frequency = constant_source(1400.0), .decay = constant_source(0.25) });
    network->process_batch(1);

    EXPECT_DOUBLE_EQ(network->get_resonators().at(0).frequency, 1400.0);
    EXPECT_NEAR(network->get_resonators().at(0).q,
        Kinesis::Discrete::q_from_decay(1400.0, 0.25, network->get_sample_rate()), 1e-6);
}

TEST(ResonatorControlsTest, ANumberIsAppliedOnceAndLaterEditsStay)
{
    auto network = single(1000.0, 30.0);

    network->set({ .q = 12.0 });
    network->process_batch(1);
    EXPECT_NEAR(network->get_resonators().at(0).q, 12.0, 1e-9);

    network->set_q(0, 50.0);
    network->process_batch(1);
    EXPECT_NEAR(network->get_resonators().at(0).q, 50.0, 1e-9);
}

TEST(ResonatorControlsTest, ANodeIsAppliedEveryBlock)
{
    auto network = single(1000.0, 30.0);

    network->set({ .q = constant_source(12.0) });
    network->process_batch(1);
    network->set_q(0, 50.0);
    network->process_batch(1);

    EXPECT_NEAR(network->get_resonators().at(0).q, 12.0, 1e-9);
}

TEST(ResonatorControlsTest, GainReachesEveryResonator)
{
    auto network = trio();

    network->set({ .gain = 2.0 });
    network->process_batch(1);

    for (const auto& resonator : network->get_resonators()) {
        EXPECT_DOUBLE_EQ(resonator.gain, 2.0);
    }
}

TEST(ResonatorControlsTest, ANetworkSourceWritesOneValuePerResonator)
{
    auto source = std::make_shared<ControlsProbeNetwork>(3);
    source->outputs = { 600.0, 1100.0, 1600.0 };

    auto network = trio();
    network->set({ .frequency = source });
    network->process_batch(1);

    EXPECT_DOUBLE_EQ(network->get_resonators().at(0).frequency, 600.0);
    EXPECT_DOUBLE_EQ(network->get_resonators().at(1).frequency, 1100.0);
    EXPECT_DOUBLE_EQ(network->get_resonators().at(2).frequency, 1600.0);
}

TEST(ResonatorControlsTest, ANetworkSourceWithFewerElementsLeavesTheRestAlone)
{
    auto source = std::make_shared<ControlsProbeNetwork>(2);
    source->outputs = { 600.0, 1100.0 };

    auto network = trio();
    network->set({ .frequency = source });
    network->process_batch(1);

    EXPECT_DOUBLE_EQ(network->get_resonators().at(0).frequency, 600.0);
    EXPECT_DOUBLE_EQ(network->get_resonators().at(1).frequency, 1100.0);
    EXPECT_DOUBLE_EQ(network->get_resonators().at(2).frequency, 1500.0);
}

TEST(ResonatorControlsTest, ANetworkGainScalesEachResonatorIndependently)
{
    auto source = std::make_shared<ControlsProbeNetwork>(3);
    source->outputs = { 0.5, 1.0, 2.0 };

    auto network = trio();
    network->set({ .gain = source });
    network->process_batch(1);

    EXPECT_DOUBLE_EQ(network->get_resonators().at(0).gain, 0.5);
    EXPECT_DOUBLE_EQ(network->get_resonators().at(1).gain, 1.0);
    EXPECT_DOUBLE_EQ(network->get_resonators().at(2).gain, 2.0);
}

TEST(ResonatorControlsTest, AMemberLeftOutLeavesTheOthersAlone)
{
    auto network = trio();

    network->set({ .gain = 2.0 });
    network->process_batch(1);

    EXPECT_DOUBLE_EQ(network->get_resonators().at(1).frequency, 1000.0);
    EXPECT_NEAR(network->get_resonators().at(1).q, 10.0, 1e-9);
}

TEST(ResonatorControlsTest, ANodeScaleChangesTheMix)
{
    auto reference = single(1000.0, 30.0);
    auto scaled = single(1000.0, 30.0);
    scaled->set({ .scale = constant_source(0.25) });

    const auto original = render(reference, 64);
    const auto reduced = render(scaled, 64);

    ASSERT_EQ(reduced.size(), original.size());
    for (size_t i = 0; i < original.size(); ++i) {
        EXPECT_NEAR(reduced[i], 0.25 * original[i], 1e-12) << "sample " << i;
    }
}

TEST(ResonatorControlsTest, TypedAndStringMappingsRenderTheSameSound)
{
    auto by_string = single(1000.0, 30.0);
    auto by_control = single(1000.0, 30.0);

    by_string->map_parameter("retune", constant_source(1300.0));
    by_string->map_parameter("decay", constant_source(0.5));
    by_string->map_parameter("gain", constant_source(1.5));
    by_string->map_parameter("scale", constant_source(0.5));
    by_control->set({ .scale = constant_source(0.5),
        .gain = constant_source(1.5),
        .retune = constant_source(1300.0),
        .decay = constant_source(0.5) });

    const auto expected = render(by_string, 256);
    const auto actual = render(by_control, 256);

    ASSERT_EQ(actual.size(), expected.size());
    for (size_t i = 0; i < expected.size(); ++i) {
        EXPECT_NEAR(actual[i], expected[i], 1e-12) << "sample " << i;
    }
}

TEST(ResonatorControlsTest, GetReportsTheScaleAndWhatWasSet)
{
    auto network = single(1000.0, 30.0);
    network->set_output_scale(0.4);
    network->set({ .q = 12.0 });

    const auto controls = network->get();

    EXPECT_DOUBLE_EQ(controls.scale.value(), 0.4);
    EXPECT_DOUBLE_EQ(controls.q.value(), 12.0);
    EXPECT_FALSE(controls.gain.has_value());
}

}
