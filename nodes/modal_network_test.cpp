#include "../test_config.h"

#include "MayaFlux/Nodes/Filters/FIR.hpp"
#include "MayaFlux/Nodes/Generators/Polynomial.hpp"
#include "MayaFlux/Nodes/Network/ModalNetwork.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <ranges>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace MayaFlux::Test {

using Nodes::Generator::Polynomial;
using Nodes::Network::ModalNetwork;

namespace {

    std::vector<double> render(ModalNetwork& network, unsigned int samples)
    {
        network.process_batch(samples);
        return network.get_audio_buffer().value_or(std::vector<double> {});
    }

    double peak(std::span<const double> samples)
    {
        double value = 0.0;
        for (double sample : samples) {
            value = std::max(value, std::abs(sample));
        }
        return value;
    }

    std::shared_ptr<Nodes::Node> pulse_train(size_t period)
    {
        auto position = std::make_shared<size_t>(0);
        return std::make_shared<Polynomial>([position, period](double) {
            const size_t sample = (*position)++;
            return sample % period == 1 ? 1.0 : 0.0;
        });
    }

    struct SpectrumCase {
        const char* name;
        ModalNetwork::Spectrum spectrum;
        std::array<double, 4> ratios;
    };

    struct CouplingCase {
        const char* name;
        size_t first;
        size_t second;
        double strength;
    };

    struct NoiseCase {
        const char* name;
        ModalNetwork::ExciterType type;
    };

    struct SpatialCase {
        const char* name;
        double position;
        std::array<double, 4> factors;
    };

    struct OneShotCase {
        const char* name;
        ModalNetwork::ExciterType type;
        std::vector<double> sample;
    };

    struct ExciterLabelCase {
        const char* name;
        ModalNetwork::ExciterType type;
        const char* label;
    };

    struct SpectrumLabelCase {
        const char* name;
        ModalNetwork::Spectrum spectrum;
        const char* label;
    };

    struct FloorCase {
        const char* name;
        double strength;
        bool silenced;
    };

class ModalSpectrumTest : public ::testing::TestWithParam<SpectrumCase> { };

TEST_P(ModalSpectrumTest, ConstructorAssignsFrequenciesAndModeLevels)
{
    const auto& config = GetParam();
    ModalNetwork network(4, 100.0, config.spectrum, 0.4);

    ASSERT_EQ(network.get_node_count(), 4U);
    for (size_t i = 0; i < config.ratios.size(); ++i) {
        const auto& mode = network.get_mode(i);
        EXPECT_NEAR(mode.frequency_ratio, config.ratios[i], 1e-5) << "mode " << i;
        EXPECT_NEAR(mode.base_frequency, 100.0 * config.ratios[i], 1e-3) << "mode " << i;
        EXPECT_DOUBLE_EQ(mode.initial_amplitude, 1.0 / static_cast<double>(i + 1)) << "mode " << i;
        EXPECT_DOUBLE_EQ(mode.amplitude, 0.0) << "mode " << i;
    }
}

INSTANTIATE_TEST_SUITE_P(
    Spectra,
    ModalSpectrumTest,
    ::testing::Values(
        SpectrumCase { "Harmonic", ModalNetwork::Spectrum::HARMONIC, { 1.0, 2.0, 3.0, 4.0 } },
        SpectrumCase { "Inharmonic", ModalNetwork::Spectrum::INHARMONIC, { 1.0, 2.756, 5.404, 8.933 } },
        SpectrumCase { "Stretched", ModalNetwork::Spectrum::STRETCHED, { 1.00005, 2.00039996, 3.0013497, 4.00319872 } }),
    [](const ::testing::TestParamInfo<SpectrumCase>& info) { return std::string(info.param.name); });

TEST(ModalSpectrumExtraTest, CustomRatiosTrackFundamentalChanges)
{
    ModalNetwork network(std::vector<double> { 1.0, 1.5, 2.25 }, 200.0);

    network.set_fundamental(300.0);

    EXPECT_DOUBLE_EQ(network.get_mode(0).base_frequency, 300.0);
    EXPECT_DOUBLE_EQ(network.get_mode(1).base_frequency, 450.0);
    EXPECT_DOUBLE_EQ(network.get_mode(2).base_frequency, 675.0);
    EXPECT_DOUBLE_EQ(network.get_mode(2).current_frequency, 675.0);
}

class ModalIndividualExcitationTest : public ::testing::TestWithParam<size_t> { };

class ModalPitchTest : public ::testing::TestWithParam<size_t> { };

TEST_P(ModalPitchTest, RingOscillatesAtItsSelectedFrequency)
{
    const size_t selected = GetParam();
    const double frequency = 250.0 * static_cast<double>(selected + 1);
    ModalNetwork network(3, 250.0, ModalNetwork::Spectrum::HARMONIC, 2.0);
    network.excite_mode(selected);

    const auto audio = render(network, static_cast<unsigned int>(network.get_sample_rate()));
    std::vector<size_t> rising_crossings;
    for (size_t i = 1; i < audio.size(); ++i) {
        if (audio[i - 1] <= 0.0 && audio[i] > 0.0) {
            rising_crossings.push_back(i);
        }
    }

    ASSERT_GE(rising_crossings.size(), 3U);
    const double measured = static_cast<double>(rising_crossings.size() - 1)
        * network.get_sample_rate()
        / static_cast<double>(rising_crossings.back() - rising_crossings.front());
    EXPECT_NEAR(measured, frequency, 2.0);
}

INSTANTIATE_TEST_SUITE_P(
    Frequencies,
    ModalPitchTest,
    ::testing::Values(size_t { 0 }, size_t { 1 }, size_t { 2 }));

TEST_P(ModalIndividualExcitationTest, ExciteModeSoundsOnlyTheSelectedMode)
{
    const size_t selected = GetParam();
    ModalNetwork network(4, 440.0);
    network.excite_mode(selected);

    const auto mixed = render(network, 256);

    ASSERT_EQ(mixed.size(), 256U);
    for (size_t i = 0; i < network.get_node_count(); ++i) {
        const auto voice = network.get_node_audio_buffer(i);
        ASSERT_TRUE(voice.has_value()) << "mode " << i;
        ASSERT_EQ(voice->size(), mixed.size()) << "mode " << i;
        if (i == selected) {
            EXPECT_GT(peak(*voice), 1e-4);
            EXPECT_GT(network.get_mode(i).amplitude, 0.0);
            for (size_t sample = 0; sample < mixed.size(); ++sample) {
                EXPECT_NEAR(mixed[sample], (*voice)[sample], 1e-12) << "sample " << sample;
            }
        } else {
            EXPECT_DOUBLE_EQ(peak(*voice), 0.0) << "mode " << i;
            EXPECT_DOUBLE_EQ(network.get_mode(i).amplitude, 0.0) << "mode " << i;
        }
    }
}

INSTANTIATE_TEST_SUITE_P(
    Modes,
    ModalIndividualExcitationTest,
    ::testing::Values(size_t { 0 }, size_t { 1 }, size_t { 3 }));

TEST(ModalExcitationTest, SilentUntilExcitedAndResetClearsTheRing)
{
    ModalNetwork network(3, 440.0);

    const auto silent = render(network, 128);
    ASSERT_EQ(silent.size(), 128U);
    EXPECT_DOUBLE_EQ(peak(silent), 0.0);

    network.excite_mode(0);
    EXPECT_GT(peak(render(network, 128)), 1e-4);

    network.reset();
    EXPECT_DOUBLE_EQ(peak(render(network, 128)), 0.0);
}

class ModalStrengthTest : public ::testing::TestWithParam<double> { };

TEST_P(ModalStrengthTest, ExciteScalesEveryMode)
{
    ModalNetwork network(3, 440.0);
    network.excite(GetParam());

    for (size_t i = 0; i < network.get_node_count(); ++i) {
        EXPECT_DOUBLE_EQ(network.get_mode(i).amplitude,
            GetParam() / static_cast<double>(i + 1));
    }
}

INSTANTIATE_TEST_SUITE_P(
    Strengths,
    ModalStrengthTest,
    ::testing::Values(0.0, 0.25, 1.5));

class ModalDampingTest : public ::testing::TestWithParam<double> { };

TEST_P(ModalDampingTest, DampScalesTheCurrentRing)
{
    ModalNetwork network(2, 440.0);
    network.excite(0.8);
    network.damp(GetParam());

    EXPECT_DOUBLE_EQ(network.get_mode(0).amplitude, 0.8 * GetParam());
    EXPECT_DOUBLE_EQ(network.get_mode(1).amplitude, 0.4 * GetParam());
}

INSTANTIATE_TEST_SUITE_P(
    Factors,
    ModalDampingTest,
    ::testing::Values(0.0, 0.25, 1.0));

class ModalSpatialExcitationTest : public ::testing::TestWithParam<SpatialCase> { };

TEST_P(ModalSpatialExcitationTest, AStrikeFollowsTheModeShapeAtItsPosition)
{
    const auto& strike = GetParam();
    ModalNetwork network(4, 440.0);

    network.excite_at_position(strike.position);

    for (size_t i = 0; i < strike.factors.size(); ++i) {
        EXPECT_NEAR(network.get_mode(i).amplitude,
            strike.factors[i] / static_cast<double>(i + 1), 1e-12)
            << "mode " << i;
    }
}

INSTANTIATE_TEST_SUITE_P(
    StrikePositions,
    ModalSpatialExcitationTest,
    ::testing::Values(
        SpatialCase { "FirstNode", 0.0, { 0.0, 0.0, 0.0, 0.0 } },
        SpatialCase { "Quarter", 0.25, { 0.7071067811865476, 1.0, 0.7071067811865476, 0.0 } },
        SpatialCase { "Centre", 0.5, { 1.0, 0.0, 1.0, 0.0 } },
        SpatialCase { "LastNode", 1.0, { 0.0, 0.0, 0.0, 0.0 } },
        SpatialCase { "ClampedBelow", -0.5, { 0.0, 0.0, 0.0, 0.0 } },
        SpatialCase { "ClampedAbove", 1.5, { 0.0, 0.0, 0.0, 0.0 } }),
    [](const ::testing::TestParamInfo<SpatialCase>& info) { return std::string(info.param.name); });

class ModalCouplingTest : public ::testing::TestWithParam<CouplingCase> { };

TEST_P(ModalCouplingTest, CouplingTransfersTheSpecifiedFractionOfAmplitude)
{
    const auto& config = GetParam();
    ModalNetwork network(2, 440.0);
    network.excite_mode(0);
    network.set_mode_coupling(config.first, config.second, config.strength);
    network.set_coupling_enabled(true);

    render(network, 1);

    const double decay = network.get_mode(0).decay_coefficient;
    EXPECT_NEAR(network.get_mode(0).amplitude,
        (1.0 - config.strength * 0.5) * decay, 1e-12);
    EXPECT_NEAR(network.get_mode(1).amplitude,
        config.strength * 0.5 * decay, 1e-12);
    EXPECT_NEAR(network.get_mode(0).amplitude + network.get_mode(1).amplitude,
        decay, 1e-12);
}

INSTANTIATE_TEST_SUITE_P(
    Transfer,
    ModalCouplingTest,
    ::testing::Values(
        CouplingCase { "Quarter", 0, 1, 0.25 },
        CouplingCase { "Half", 0, 1, 0.5 },
        CouplingCase { "Full", 0, 1, 1.0 },
        CouplingCase { "Reversed", 1, 0, 0.5 }),
    [](const ::testing::TestParamInfo<CouplingCase>& info) { return std::string(info.param.name); });

TEST(ModalCouplingExtraTest, AConfiguredCouplingDoesNotTransferWhileDisabled)
{
    ModalNetwork network(2, 440.0);
    network.excite_mode(0);
    network.set_mode_coupling(0, 1, 0.8);

    render(network, 1);
    EXPECT_DOUBLE_EQ(network.get_mode(1).amplitude, 0.0);

    network.set_coupling_enabled(true);
    render(network, 1);
    EXPECT_GT(network.get_mode(1).amplitude, 0.0);
}

TEST(ModalCouplingExtraTest, RemovingOrClearingCouplingsStopsTransfer)
{
    ModalNetwork network(2, 440.0);
    network.set_coupling_enabled(true);
    network.set_mode_coupling(0, 1, 0.8);
    network.remove_mode_coupling(1, 0);
    network.excite_mode(0);

    render(network, 1);
    EXPECT_DOUBLE_EQ(network.get_mode(1).amplitude, 0.0);

    network.set_mode_coupling(0, 1, 0.8);
    network.clear_couplings();
    render(network, 1);
    EXPECT_DOUBLE_EQ(network.get_mode(1).amplitude, 0.0);
}

class ModalCouplingBoundaryTest : public ::testing::TestWithParam<std::pair<double, double>> { };

TEST_P(ModalCouplingBoundaryTest, StrengthIsClampedBeforeTransfer)
{
    const auto [requested, effective] = GetParam();
    ModalNetwork network(2, 440.0);
    network.set_mode_coupling(0, 1, requested);
    network.set_coupling_enabled(true);
    network.excite_mode(0);

    render(network, 1);

    const double decay = network.get_mode(0).decay_coefficient;
    EXPECT_NEAR(network.get_mode(1).amplitude, 0.5 * effective * decay, 1e-12);
}

INSTANTIATE_TEST_SUITE_P(
    Limits,
    ModalCouplingBoundaryTest,
    ::testing::Values(
        std::pair { -0.5, 0.0 },
        std::pair { 0.0, 0.0 },
        std::pair { 1.0, 1.0 },
        std::pair { 1.5, 1.0 }));

TEST(ModalCouplingExtraTest, InvalidIndicesAndSelfCouplingAreIgnored)
{
    ModalNetwork network(2, 440.0);
    network.set_mode_coupling(0, 2, 0.5);
    network.set_mode_coupling(2, 0, 0.5);
    network.set_mode_coupling(1, 1, 0.5);

    EXPECT_TRUE(network.get_couplings().empty());
}

class ModalSampleExciterTest : public ::testing::TestWithParam<size_t> { };

TEST_P(ModalSampleExciterTest, AStoredSampleAddsEnergyAtItsSelectedFrame)
{
    const size_t pulse_position = GetParam();
    std::vector<double> pulse(4, 0.0);
    pulse[pulse_position] = 1.0;

    ModalNetwork reference(1, 440.0);
    reference.set_exciter_type(ModalNetwork::ExciterType::SAMPLE);
    reference.set_exciter_sample(std::vector<double>(4, 0.0));
    reference.excite();

    ModalNetwork sounded(1, 440.0);
    sounded.set_exciter_type(ModalNetwork::ExciterType::SAMPLE);
    sounded.set_exciter_sample(pulse);
    sounded.excite();

    render(reference, 4);
    render(sounded, 4);

    const double decay = sounded.get_mode(0).decay_coefficient;
    EXPECT_NEAR(sounded.get_mode(0).amplitude - reference.get_mode(0).amplitude,
        std::pow(decay, 4.0 - static_cast<double>(pulse_position)), 1e-12);
}

INSTANTIATE_TEST_SUITE_P(
    PulsePositions,
    ModalSampleExciterTest,
    ::testing::Values(size_t { 0 }, size_t { 1 }, size_t { 3 }));

class ModalNoiseExciterTest : public ::testing::TestWithParam<NoiseCase> { };

TEST_P(ModalNoiseExciterTest, BurstExcitesAResetMode)
{
    ModalNetwork network(1, 440.0);
    network.set_exciter_type(GetParam().type);
    network.set_exciter_duration(128.0 / static_cast<double>(network.get_sample_rate()));
    if (GetParam().type == ModalNetwork::ExciterType::FILTERED_NOISE) {
        network.set_exciter_filter(std::make_shared<Nodes::Filters::FIR>(std::vector<double> { 1.0 }));
    }
    network.excite();
    network.reset();

    const auto audio = render(network, 128);

    ASSERT_EQ(audio.size(), 128U);
    EXPECT_GT(peak(audio), 1e-5);
    EXPECT_TRUE(std::ranges::all_of(audio, [](double sample) { return std::isfinite(sample); }));
}

INSTANTIATE_TEST_SUITE_P(
    Bursts,
    ModalNoiseExciterTest,
    ::testing::Values(
        NoiseCase { "WhiteNoise", ModalNetwork::ExciterType::NOISE_BURST },
        NoiseCase { "FilteredNoise", ModalNetwork::ExciterType::FILTERED_NOISE }),
    [](const ::testing::TestParamInfo<NoiseCase>& info) { return std::string(info.param.name); });

class ModalContinuousExciterTest : public ::testing::TestWithParam<size_t> { };

TEST_P(ModalContinuousExciterTest, RepeatedSourcePeaksSustainTheRing)
{
    ModalNetwork driven(1, 440.0, ModalNetwork::Spectrum::HARMONIC, 0.005);
    driven.set_exciter_type(ModalNetwork::ExciterType::CONTINUOUS);
    driven.set_exciter_node(pulse_train(GetParam()));
    driven.excite();

    ModalNetwork undriven(1, 440.0, ModalNetwork::Spectrum::HARMONIC, 0.005);
    undriven.set_exciter_type(ModalNetwork::ExciterType::CONTINUOUS);
    undriven.excite();

    for (size_t block = 0; block < 8; ++block) {
        render(driven, 512);
        render(undriven, 512);
    }

    EXPECT_GT(driven.get_mode(0).amplitude, 5e-4);
    EXPECT_DOUBLE_EQ(undriven.get_mode(0).amplitude, 0.0);
    const auto voice = driven.get_node_audio_buffer(0);
    ASSERT_TRUE(voice.has_value());
    EXPECT_GT(peak(*voice), 1e-5);
}

INSTANTIATE_TEST_SUITE_P(
    PulsePeriods,
    ModalContinuousExciterTest,
    ::testing::Values(size_t { 4 }, size_t { 8 }, size_t { 16 }));

TEST(ModalReleaseTest, ReleaseLetsAContinuousSourceRingOut)
{
    ModalNetwork network(1, 440.0, ModalNetwork::Spectrum::HARMONIC, 0.005);
    network.set_exciter_type(ModalNetwork::ExciterType::CONTINUOUS);
    network.set_exciter_node(pulse_train(8));
    network.excite();

    for (size_t block = 0; block < 4; ++block) {
        render(network, 512);
    }
    ASSERT_GT(network.get_mode(0).amplitude, 0.0);

    network.release();
    for (size_t block = 0; block < 8; ++block) {
        render(network, 512);
    }

    EXPECT_DOUBLE_EQ(network.get_mode(0).amplitude, 0.0);
}

class ModalDecayTest : public ::testing::TestWithParam<double> { };

TEST_P(ModalDecayTest, MultiplierChangesTheAmplitudeAfterOneDecayTime)
{
    ModalNetwork network(1, 440.0, ModalNetwork::Spectrum::HARMONIC, 0.01);
    network.excite_mode(0);
    network.set_decay_multiplier(GetParam());

    render(network, 480);

    EXPECT_NEAR(network.get_mode(0).amplitude,
        std::exp(-1.0 / GetParam()), 1e-10);
}

INSTANTIATE_TEST_SUITE_P(
    Multipliers,
    ModalDecayTest,
    ::testing::Values(0.5, 1.0, 2.0));

TEST(ModalBufferTest, ModeBuffersAreReplacedEachBatch)
{
    ModalNetwork network(2, 440.0);
    network.excite_mode(0);

    render(network, 64);
    ASSERT_TRUE(network.get_node_audio_buffer(0).has_value());
    EXPECT_EQ(network.get_node_audio_buffer(0)->size(), 64U);

    render(network, 17);
    ASSERT_TRUE(network.get_node_audio_buffer(0).has_value());
    ASSERT_TRUE(network.get_node_audio_buffer(1).has_value());
    EXPECT_EQ(network.get_node_audio_buffer(0)->size(), 17U);
    EXPECT_EQ(network.get_node_audio_buffer(1)->size(), 17U);
    EXPECT_FALSE(network.get_node_audio_buffer(2).has_value());
}

TEST(ModalMappingTest, BroadcastFrequencyRetunesAllModes)
{
    auto source = std::make_shared<Polynomial>([](double) { return 330.0; });
    source->process_sample(0.0);

    ModalNetwork network(std::vector<double> { 1.0, 1.5, 2.0 }, 220.0);
    network.map_parameter("frequency", source);
    render(network, 1);

    EXPECT_DOUBLE_EQ(network.get_fundamental(), 330.0);
    EXPECT_DOUBLE_EQ(network.get_mode(0).current_frequency, 330.0);
    EXPECT_DOUBLE_EQ(network.get_mode(1).current_frequency, 495.0);
    EXPECT_DOUBLE_EQ(network.get_mode(2).current_frequency, 660.0);
}

TEST(ModalMappingTest, BroadcastDecayChangesTheRingingEnvelope)
{
    auto source = std::make_shared<Polynomial>([](double) { return 2.0; });
    source->process_sample(0.0);

    ModalNetwork network(1, 440.0, ModalNetwork::Spectrum::HARMONIC, 0.01);
    network.excite_mode(0);
    network.map_parameter("decay", source);
    render(network, 480);

    EXPECT_NEAR(network.get_mode(0).amplitude, std::exp(-0.5), 1e-10);
}

TEST(ModalMappingTest, BroadcastAmplitudeScalesTheCurrentRing)
{
    auto source = std::make_shared<Polynomial>([](double) { return 0.25; });
    source->process_sample(0.0);

    ModalNetwork network(1, 440.0);
    network.excite_mode(0);
    network.map_parameter("amplitude", source);
    render(network, 1);

    EXPECT_NEAR(network.get_mode(0).amplitude,
        0.25 * network.get_mode(0).decay_coefficient, 1e-12);
}

TEST(ModalMappingTest, BroadcastScaleChangesTheMix)
{
    auto source = std::make_shared<Polynomial>([](double) { return 0.25; });
    source->process_sample(0.0);

    ModalNetwork reference(1, 440.0);
    reference.excite_mode(0);
    const auto original = render(reference, 64);

    ModalNetwork scaled(1, 440.0);
    scaled.excite_mode(0);
    scaled.map_parameter("scale", source);
    const auto reduced = render(scaled, 64);

    ASSERT_EQ(reduced.size(), original.size());
    for (size_t i = 0; i < original.size(); ++i) {
        EXPECT_NEAR(reduced[i], 0.25 * original[i], 1e-12) << "sample " << i;
    }
}

TEST(ModalMappingTest, UnmappingStopsBroadcastUpdates)
{
    auto source = std::make_shared<Polynomial>([](double) { return 330.0; });
    source->process_sample(0.0);

    ModalNetwork network(1, 220.0);
    network.map_parameter("frequency", source);
    network.unmap_parameter("frequency");
    render(network, 1);

    EXPECT_DOUBLE_EQ(network.get_fundamental(), 220.0);
}

TEST(ModalMappingTest, OneToOneAmplitudeScalesMatchingModesIndependently)
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
    network.map_parameter("amplitude", source);
    render(network, 1);

    const double decay = network.get_mode(0).decay_coefficient;
    EXPECT_NEAR(network.get_mode(0).amplitude, first * decay, 1e-12);
    EXPECT_NEAR(network.get_mode(1).amplitude, 0.5 * second * decay, 1e-12);
}

TEST(ModalMappingTest, OneToOneDetuneRetunesMatchingModesIndependently)
{
    auto source = std::make_shared<ModalNetwork>(2, 6000.0);
    render(*source, 2);
    const double first = source->get_node_output(0).value();
    const double second = source->get_node_output(1).value();

    ModalNetwork network(2, 440.0);
    network.map_parameter("detune", source);
    render(network, 1);

    EXPECT_NEAR(network.get_mode(0).current_frequency,
        440.0 * std::pow(2.0, first / 12.0), 1e-12);
    EXPECT_NEAR(network.get_mode(1).current_frequency,
        880.0 * std::pow(2.0, second / 12.0), 1e-12);
}

TEST(ModalMappingTest, OneToOneMappingRequiresMatchingModeCounts)
{
    auto source = std::make_shared<ModalNetwork>(1, 6000.0);
    render(*source, 2);

    ModalNetwork network(2, 440.0);
    network.excite_mode(0);
    network.excite_mode(1);
    network.map_parameter("amplitude", source);
    render(network, 1);

    const double decay = network.get_mode(0).decay_coefficient;
    EXPECT_NEAR(network.get_mode(0).amplitude, decay, 1e-12);
    EXPECT_NEAR(network.get_mode(1).amplitude, 0.5 * decay, 1e-12);
}

    const std::array<ExciterLabelCase, 5> kExciterLabels { {
        { "Impulse", ModalNetwork::ExciterType::IMPULSE, "IMPULSE" },
        { "NoiseBurst", ModalNetwork::ExciterType::NOISE_BURST, "NOISE_BURST" },
        { "FilteredNoise", ModalNetwork::ExciterType::FILTERED_NOISE, "FILTERED_NOISE" },
        { "Sample", ModalNetwork::ExciterType::SAMPLE, "SAMPLE" },
        { "Continuous", ModalNetwork::ExciterType::CONTINUOUS, "CONTINUOUS" },
    } };

    const std::array<SpectrumLabelCase, 4> kSpectrumLabels { {
        { "Harmonic", ModalNetwork::Spectrum::HARMONIC, "HARMONIC" },
        { "Inharmonic", ModalNetwork::Spectrum::INHARMONIC, "INHARMONIC" },
        { "Stretched", ModalNetwork::Spectrum::STRETCHED, "STRETCHED" },
        { "Custom", ModalNetwork::Spectrum::CUSTOM, "CUSTOM" },
    } };

class ModalOneShotTest : public ::testing::TestWithParam<OneShotCase> { };

TEST_P(ModalOneShotTest, InjectionStopsOnceTheExciterIsSpent)
{
    const auto& config = GetParam();
    ModalNetwork network(1, 440.0, ModalNetwork::Spectrum::HARMONIC, 1.0);
    network.set_exciter_type(config.type);
    network.set_exciter_sample(config.sample);
    network.excite(0.5);

    render(network, 8);
    render(network, 1);
    const double before = network.get_mode(0).amplitude;
    render(network, 1);

    EXPECT_NEAR(network.get_mode(0).amplitude,
        before * network.get_mode(0).decay_coefficient, 1e-12);
}

INSTANTIATE_TEST_SUITE_P(
    OneShots,
    ModalOneShotTest,
    ::testing::Values(
        OneShotCase { "Impulse", ModalNetwork::ExciterType::IMPULSE, {} },
        OneShotCase { "Sample", ModalNetwork::ExciterType::SAMPLE, { 1.0, 1.0, 1.0, 1.0 } }),
    [](const ::testing::TestParamInfo<OneShotCase>& info) { return std::string(info.param.name); });

class ModalExciterSwitchTest : public ::testing::TestWithParam<ExciterLabelCase> { };

TEST_P(ModalExciterSwitchTest, SwitchingTypeSilencesTheRingAndDisarmsTheExciter)
{
    ModalNetwork network(3, 440.0);
    network.excite();

    network.set_exciter_type(GetParam().type);

    EXPECT_EQ(network.get_exciter_type(), GetParam().type);
    for (size_t i = 0; i < network.get_node_count(); ++i) {
        EXPECT_DOUBLE_EQ(network.get_mode(i).amplitude, 0.0) << "mode " << i;
    }
    EXPECT_DOUBLE_EQ(peak(render(network, 128)), 0.0);
}

INSTANTIATE_TEST_SUITE_P(
    ExciterTypes,
    ModalExciterSwitchTest,
    ::testing::ValuesIn(kExciterLabels),
    [](const ::testing::TestParamInfo<ExciterLabelCase>& info) { return std::string(info.param.name); });

class ModalMetadataExciterTest : public ::testing::TestWithParam<ExciterLabelCase> { };

TEST_P(ModalMetadataExciterTest, MetadataNamesTheExciterType)
{
    ModalNetwork network(2, 440.0);
    network.set_exciter_type(GetParam().type);

    EXPECT_EQ(network.get_metadata().at("exciter_type"), GetParam().label);
}

INSTANTIATE_TEST_SUITE_P(
    ExciterTypes,
    ModalMetadataExciterTest,
    ::testing::ValuesIn(kExciterLabels),
    [](const ::testing::TestParamInfo<ExciterLabelCase>& info) { return std::string(info.param.name); });

class ModalMetadataSpectrumTest : public ::testing::TestWithParam<SpectrumLabelCase> { };

TEST_P(ModalMetadataSpectrumTest, MetadataNamesTheSpectrum)
{
    const auto network = GetParam().spectrum == ModalNetwork::Spectrum::CUSTOM
        ? std::make_unique<ModalNetwork>(std::vector<double> { 1.0, 1.5 }, 440.0)
        : std::make_unique<ModalNetwork>(2, 440.0, GetParam().spectrum);

    EXPECT_EQ(network->get_metadata().at("spectrum"), GetParam().label);
}

INSTANTIATE_TEST_SUITE_P(
    Spectra,
    ModalMetadataSpectrumTest,
    ::testing::ValuesIn(kSpectrumLabels),
    [](const ::testing::TestParamInfo<SpectrumLabelCase>& info) { return std::string(info.param.name); });

TEST(ModalMetadataTest, MetadataReportsCouplingState)
{
    ModalNetwork network(3, 440.0);
    network.set_mode_coupling(0, 1, 0.3);
    network.set_mode_coupling(1, 2, 0.3);

    EXPECT_EQ(network.get_metadata().at("coupling_enabled"), "false");
    EXPECT_EQ(network.get_metadata().at("coupling_count"), "2");

    network.set_coupling_enabled(true);
    EXPECT_EQ(network.get_metadata().at("coupling_enabled"), "true");
}

class ModalAmplitudeFloorTest : public ::testing::TestWithParam<FloorCase> { };

TEST_P(ModalAmplitudeFloorTest, ModesBelowTheAudibleFloorAreSnappedToZero)
{
    ModalNetwork network(1, 440.0);
    network.excite_mode(0, GetParam().strength);

    render(network, 1);

    if (GetParam().silenced) {
        EXPECT_DOUBLE_EQ(network.get_mode(0).amplitude, 0.0);
    } else {
        EXPECT_GT(network.get_mode(0).amplitude, 0.0);
    }
}

INSTANTIATE_TEST_SUITE_P(
    Floor,
    ModalAmplitudeFloorTest,
    ::testing::Values(
        FloorCase { "Below", 5e-5, true },
        FloorCase { "AtThreshold", 1e-4, true },
        FloorCase { "Above", 2e-4, false }),
    [](const ::testing::TestParamInfo<FloorCase>& info) { return std::string(info.param.name); });

class ModalDecayClampTest : public ::testing::TestWithParam<double> { };

TEST_P(ModalDecayClampTest, MultiplierIsFlooredAtOneHundredth)
{
    ModalNetwork floored(1, 440.0, ModalNetwork::Spectrum::HARMONIC, 0.01);
    floored.set_decay_multiplier(0.01);

    ModalNetwork network(1, 440.0, ModalNetwork::Spectrum::HARMONIC, 0.01);
    network.set_decay_multiplier(GetParam());

    EXPECT_DOUBLE_EQ(network.get_mode(0).decay_coefficient,
        floored.get_mode(0).decay_coefficient);
}

INSTANTIATE_TEST_SUITE_P(
    BelowFloor,
    ModalDecayClampTest,
    ::testing::Values(0.0, -2.0, 0.005, 0.01));

TEST(ModalEnabledTest, ADisabledNetworkIsSilentAndHoldsItsRing)
{
    ModalNetwork network(1, 440.0);
    network.excite_mode(0);
    network.set_enabled(false);

    const auto silent = render(network, 64);

    ASSERT_EQ(silent.size(), 64U);
    EXPECT_DOUBLE_EQ(peak(silent), 0.0);
    EXPECT_DOUBLE_EQ(network.get_mode(0).amplitude, 1.0);

    network.set_enabled(true);
    EXPECT_GT(peak(render(network, 64)), 1e-4);
}

TEST(ModalContinuousGateTest, ANodeAloneDoesNotSoundUntilExcited)
{
    ModalNetwork network(1, 440.0, ModalNetwork::Spectrum::HARMONIC, 0.005);
    network.set_exciter_type(ModalNetwork::ExciterType::CONTINUOUS);
    network.set_exciter_node(pulse_train(8));

    for (size_t block = 0; block < 4; ++block) {
        render(network, 512);
    }

    EXPECT_DOUBLE_EQ(network.get_mode(0).amplitude, 0.0);
}

TEST(ModalContinuousGateTest, ExcitingAgainAfterReleaseResumesTheDrive)
{
    ModalNetwork network(1, 440.0, ModalNetwork::Spectrum::HARMONIC, 0.005);
    network.set_exciter_type(ModalNetwork::ExciterType::CONTINUOUS);
    network.set_exciter_node(pulse_train(8));
    network.excite();
    render(network, 512);

    network.release();
    for (size_t block = 0; block < 8; ++block) {
        render(network, 512);
    }
    ASSERT_DOUBLE_EQ(network.get_mode(0).amplitude, 0.0);

    network.excite();
    for (size_t block = 0; block < 8; ++block) {
        render(network, 512);
    }

    EXPECT_GT(network.get_mode(0).amplitude, 5e-4);
}

TEST(ModalSpectrumExtraTest, InharmonicTableExtendsAtConstantSpacingAndTruncates)
{
    ModalNetwork long_bar(10, 100.0, ModalNetwork::Spectrum::INHARMONIC);
    EXPECT_NEAR(long_bar.get_mode(7).frequency_ratio, 31.86, 1e-9);
    EXPECT_NEAR(long_bar.get_mode(8).frequency_ratio, 38.66, 1e-9);
    EXPECT_NEAR(long_bar.get_mode(9).frequency_ratio, 45.46, 1e-9);

    ModalNetwork short_bar(3, 100.0, ModalNetwork::Spectrum::INHARMONIC);
    ASSERT_EQ(short_bar.get_node_count(), 3U);
    EXPECT_NEAR(short_bar.get_mode(2).frequency_ratio, 5.404, 1e-9);
}

TEST(ModalCouplingExtraTest, SettingAPairAgainReplacesItInEitherOrder)
{
    ModalNetwork network(2, 440.0);
    network.set_mode_coupling(0, 1, 0.2);
    network.set_mode_coupling(1, 0, 0.6);

    ASSERT_EQ(network.get_couplings().size(), 1U);
    EXPECT_DOUBLE_EQ(network.get_couplings().front().strength, 0.6);
}

TEST(ModalExcitationTest, OutOfRangeModeIndicesAreIgnored)
{
    ModalNetwork network(2, 440.0);

    network.excite_mode(2);
    network.excite_mode(100);

    EXPECT_DOUBLE_EQ(network.get_mode(0).amplitude, 0.0);
    EXPECT_DOUBLE_EQ(network.get_mode(1).amplitude, 0.0);
    EXPECT_FALSE(network.get_node_output(2).has_value());
}

TEST(ModalMappingTest, ResetRestoresDetunedFrequencies)
{
    auto source = std::make_shared<ModalNetwork>(2, 6000.0);
    render(*source, 2);

    ModalNetwork network(2, 440.0);
    network.map_parameter("detune", source);
    render(network, 1);
    ASSERT_NE(network.get_mode(0).current_frequency, network.get_mode(0).base_frequency);

    network.unmap_parameter("detune");
    network.reset();

    EXPECT_DOUBLE_EQ(network.get_mode(0).current_frequency, network.get_mode(0).base_frequency);
    EXPECT_DOUBLE_EQ(network.get_mode(1).current_frequency, network.get_mode(1).base_frequency);
}

TEST(ModalConfigurationTest, ANewNetworkStartsWithAnImpulseExciter)
{
    ModalNetwork network(4, 220.0);

    EXPECT_EQ(network.get_exciter_type(), ModalNetwork::ExciterType::IMPULSE);
    EXPECT_FALSE(network.is_coupling_enabled());
}

TEST(ModalConfigurationTest, SpatialDistributionRoundTripsAndRejectsTheWrongSize)
{
    ModalNetwork network(4, 220.0);
    const std::vector<double> distribution { 1.0, 0.5, 0.25, 0.125 };

    network.set_spatial_distribution(distribution);
    network.set_spatial_distribution({ 1.0, 0.5 });

    EXPECT_EQ(network.get_spatial_distribution(), distribution);
}

TEST(ModalCouplingExtraTest, CouplingsAreCountedRemovedAndCleared)
{
    ModalNetwork network(8, 220.0);
    network.set_mode_coupling(0, 3, 0.2);
    network.set_mode_coupling(1, 4, 0.15);
    network.set_mode_coupling(2, 5, 0.1);
    ASSERT_EQ(network.get_couplings().size(), 3U);

    network.set_coupling_enabled(true);
    EXPECT_TRUE(network.is_coupling_enabled());

    network.remove_mode_coupling(1, 4);
    EXPECT_EQ(network.get_couplings().size(), 2U);

    network.clear_couplings();
    EXPECT_TRUE(network.get_couplings().empty());
}

TEST(ModalMetadataTest, MetadataReportsFundamentalAndNodeCount)
{
    ModalNetwork network(6, 440.0, ModalNetwork::Spectrum::INHARMONIC);

    const auto metadata = network.get_metadata();

    EXPECT_EQ(metadata.at("fundamental"), "440.000000 Hz");
    EXPECT_EQ(metadata.at("node_count"), "6");
}

}

}
