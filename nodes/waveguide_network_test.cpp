#include "../test_config.h"

#include "MayaFlux/Nodes/Filters/FIR.hpp"
#include "MayaFlux/Nodes/Generators/Polynomial.hpp"
#include "MayaFlux/Nodes/Network/WaveguideNetwork.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <ranges>
#include <span>
#include <string>
#include <vector>

namespace MayaFlux::Test {

using Nodes::Generator::Polynomial;
using Nodes::Network::WaveguideNetwork;

namespace {

    using GuideType = WaveguideNetwork::WaveguideType;

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

    double energy(std::span<const double> samples)
    {
        double result = 0.0;
        for (double sample : samples) {
            result += sample * sample;
        }
        return result;
    }

    double rail_energy(const WaveguideNetwork& network)
    {
        double result = 0.0;
        for (const auto& segment : network.get_segments()) {
            for (size_t i = 0; i < segment.p_plus.capacity(); ++i) {
                result += segment.p_plus[i] * segment.p_plus[i];
                result += segment.p_minus[i] * segment.p_minus[i];
            }
        }
        return result;
    }

    std::shared_ptr<Nodes::Filters::FIR> tap(double gain)
    {
        return std::make_shared<Nodes::Filters::FIR>(std::vector<double> { gain });
    }

    std::shared_ptr<Polynomial> constant(double value)
    {
        auto source = std::make_shared<Polynomial>([value](double) { return value; });
        source->process_sample(0.0);
        return source;
    }

    struct GeometryCase {
        const char* name;
        GuideType type;
        double frequency;
    };

    struct PluckCase {
        const char* name;
        GuideType type;
        double strength;
    };

    struct StrikeCase {
        const char* name;
        GuideType type;
        double position;
    };

    struct LossCase {
        const char* name;
        double requested;
        double effective;
    };

    struct TerminationCase {
        const char* name;
        GuideType type;
        bool open_end;
    };

    struct PickupCase {
        const char* name;
        double requested;
        double clamped;
    };

    struct PitchCase {
        const char* name;
        GuideType type;
        double frequency;
    };

    struct TypeLabelCase {
        const char* name;
        GuideType type;
        const char* label;
    };

    struct ExciterLabelCase {
        const char* name;
        WaveguideNetwork::ExciterType type;
        const char* label;
    };

    struct PluckClampCase {
        const char* name;
        double requested;
        double clamped;
    };

    const std::array<TypeLabelCase, 2> kTypeLabels { {
        { "String", GuideType::STRING, "STRING" },
        { "Tube", GuideType::TUBE, "TUBE" },
    } };

    const std::array<ExciterLabelCase, 5> kExciterLabels { {
        { "Impulse", WaveguideNetwork::ExciterType::IMPULSE, "IMPULSE" },
        { "NoiseBurst", WaveguideNetwork::ExciterType::NOISE_BURST, "NOISE_BURST" },
        { "FilteredNoise", WaveguideNetwork::ExciterType::FILTERED_NOISE, "FILTERED_NOISE" },
        { "Sample", WaveguideNetwork::ExciterType::SAMPLE, "SAMPLE" },
        { "Continuous", WaveguideNetwork::ExciterType::CONTINUOUS, "CONTINUOUS" },
    } };

    size_t dominant_period(std::span<const double> samples, size_t max_lag)
    {
        double mean = 0.0;
        for (double sample : samples) {
            mean += sample;
        }
        mean /= static_cast<double>(samples.size());

        std::vector<double> centered(samples.begin(), samples.end());
        for (double& sample : centered) {
            sample -= mean;
        }

        std::vector<double> correlation(max_lag + 2, 0.0);
        for (size_t lag = 1; lag <= max_lag + 1; ++lag) {
            for (size_t i = 0; i + lag < centered.size(); ++i) {
                correlation[lag] += centered[i] * centered[i + lag];
            }
        }

        size_t start = 1;
        while (start <= max_lag && correlation[start] >= 0.0) {
            ++start;
        }

        double best = 0.0;
        for (size_t lag = start; lag <= max_lag; ++lag) {
            best = std::max(best, correlation[lag]);
        }

        for (size_t lag = start; lag <= max_lag; ++lag) {
            if (correlation[lag] >= 0.9 * best
                && correlation[lag] >= correlation[lag - 1]
                && correlation[lag] >= correlation[lag + 1]) {
                return lag;
            }
        }
        return 0;
    }

    size_t ring_period(GuideType type, double frequency)
    {
        WaveguideNetwork network(type, frequency);
        network.set_loss_factor(1.0);
        network.set_loop_filter(tap(1.0));
        network.set_pickup_position(0.1);
        network.pluck(0.3);

        const auto audio = render(network, 8704);
        return dominant_period(std::span<const double>(audio).subspan(512),
            static_cast<size_t>(5.0 * 48000.0 / frequency));
    }

    std::shared_ptr<Nodes::Node> sine_source(size_t period)
    {
        auto position = std::make_shared<size_t>(0);
        return std::make_shared<Polynomial>([position, period](double) {
            const double phase = static_cast<double>((*position)++ % period) / static_cast<double>(period);
            return std::sin(2.0 * M_PI * phase);
        });
    }

class WaveguideGeometryTest : public ::testing::TestWithParam<GeometryCase> { };

TEST_P(WaveguideGeometryTest, DelayCapacityAndPropagationFollowTheStructure)
{
    const auto& config = GetParam();
    WaveguideNetwork network(config.type, config.frequency, 48000.0);

    const auto& segment = network.get_segments().at(0);
    const double passes = config.type == GuideType::TUBE ? 4.0 : 1.0;
    const auto delay = static_cast<size_t>(48000.0 / (passes * config.frequency) - 0.5);
    EXPECT_EQ(network.get_node_count(), 1U);
    EXPECT_EQ(segment.p_plus.capacity(), delay + 2);
    EXPECT_EQ(segment.p_minus.capacity(), delay + 2);
    EXPECT_EQ(segment.mode,
        config.type == GuideType::STRING
            ? WaveguideNetwork::WaveguideSegment::PropagationMode::UNIDIRECTIONAL
            : WaveguideNetwork::WaveguideSegment::PropagationMode::BIDIRECTIONAL);
}

INSTANTIATE_TEST_SUITE_P(
    Structures,
    WaveguideGeometryTest,
    ::testing::Values(
        GeometryCase { "StringLow", GuideType::STRING, 110.0 },
        GeometryCase { "StringFractional", GuideType::STRING, 440.0 },
        GeometryCase { "TubeLow", GuideType::TUBE, 110.0 },
        GeometryCase { "TubeFractional", GuideType::TUBE, 440.0 }),
    [](const ::testing::TestParamInfo<GeometryCase>& info) { return std::string(info.param.name); });

TEST(WaveguideTuningTest, LowerFrequencyExpandsTheDelayAndMinimumIsTwentyHertz)
{
    WaveguideNetwork network(GuideType::STRING, 880.0);
    const size_t old_capacity = network.get_segments().at(0).p_plus.capacity();

    network.set_fundamental(110.0);
    EXPECT_GT(network.get_segments().at(0).p_plus.capacity(), old_capacity);
    EXPECT_EQ(network.get_segments().at(0).p_plus.capacity(),
        static_cast<size_t>(48000.0 / 110.0 - 0.5) + 2);

    network.set_fundamental(5.0);
    EXPECT_DOUBLE_EQ(network.get_fundamental(), 20.0);
    EXPECT_EQ(network.get_segments().at(0).p_plus.capacity(), 2401U);
}

class WaveguidePluckShapeTest : public ::testing::TestWithParam<double> { };

TEST_P(WaveguidePluckShapeTest, SeedsAZeroMeanTriangleAtTheRequestedPosition)
{
    const double position = GetParam();
    WaveguideNetwork network(GuideType::STRING, 480.0);
    network.pluck(position, 0.75);

    const auto& segment = network.get_segments().at(0);
    const size_t delay = segment.p_plus.capacity() - 2;
    const size_t crest = static_cast<size_t>(position * static_cast<double>(delay));
    const double base = segment.p_plus[delay - 1];
    EXPECT_NEAR(segment.p_plus[delay - 1 - crest] - base, 0.75, 1e-12);
    EXPECT_NEAR(segment.p_plus[0] - base, 0.75 / static_cast<double>(delay - crest), 1e-12);
    double total = 0.0;
    for (size_t i = 0; i < delay; ++i) {
        total += segment.p_plus[i];
    }
    EXPECT_NEAR(total, 0.0, 1e-9);
    for (size_t i = 0; i < segment.p_minus.capacity(); ++i) {
        EXPECT_DOUBLE_EQ(segment.p_minus[i], 0.0);
    }
}

INSTANTIATE_TEST_SUITE_P(
    Positions,
    WaveguidePluckShapeTest,
    ::testing::Values(0.25, 0.5, 0.75));

class WaveguidePluckStrengthTest : public ::testing::TestWithParam<PluckCase> { };

TEST_P(WaveguidePluckStrengthTest, StrengthScalesTheRenderedWave)
{
    const auto& config = GetParam();
    WaveguideNetwork reference(config.type, 480.0);
    WaveguideNetwork scaled(config.type, 480.0);
    reference.pluck(0.4, 1.0);
    scaled.pluck(0.4, config.strength);

    const auto original = render(reference, 256);
    const auto changed = render(scaled, 256);

    ASSERT_EQ(original.size(), changed.size());
    ASSERT_GT(peak(original), 0.1);
    for (size_t i = 0; i < original.size(); ++i) {
        EXPECT_NEAR(changed[i], config.strength * original[i], 1e-12) << "sample " << i;
    }
}

INSTANTIATE_TEST_SUITE_P(
    Strengths,
    WaveguidePluckStrengthTest,
    ::testing::Values(
        PluckCase { "StringQuarter", GuideType::STRING, 0.25 },
        PluckCase { "StringDouble", GuideType::STRING, 2.0 },
        PluckCase { "TubeQuarter", GuideType::TUBE, 0.25 },
        PluckCase { "TubeDouble", GuideType::TUBE, 2.0 }),
    [](const ::testing::TestParamInfo<PluckCase>& info) { return std::string(info.param.name); });

class WaveguideStrikeTest : public ::testing::TestWithParam<StrikeCase> { };

TEST_P(WaveguideStrikeTest, StrikeCreatesFiniteTravelingWaves)
{
    const auto& config = GetParam();
    WaveguideNetwork network(config.type, 480.0);
    network.strike(config.position, 1.0);

    const auto audio = render(network, 512);

    ASSERT_EQ(audio.size(), 512U);
    EXPECT_GT(peak(audio), 1e-4);
    EXPECT_TRUE(std::ranges::all_of(audio, [](double sample) { return std::isfinite(sample); }));
    if (config.type == GuideType::TUBE) {
        const auto& minus = network.get_segments().at(0).p_minus;
        double reflected_peak = 0.0;
        for (size_t i = 0; i < minus.capacity(); ++i) {
            reflected_peak = std::max(reflected_peak, std::abs(minus[i]));
        }
        EXPECT_GT(reflected_peak, 0.0);
    }
}

INSTANTIATE_TEST_SUITE_P(
    Strikes,
    WaveguideStrikeTest,
    ::testing::Values(
        StrikeCase { "StringBridge", GuideType::STRING, 0.1 },
        StrikeCase { "StringCentre", GuideType::STRING, 0.5 },
        StrikeCase { "TubeBridge", GuideType::TUBE, 0.1 },
        StrikeCase { "TubeCentre", GuideType::TUBE, 0.5 }),
    [](const ::testing::TestParamInfo<StrikeCase>& info) { return std::string(info.param.name); });

TEST(WaveguideMeasurementTest, TubePressureAndVelocityReadDifferentRailCombinations)
{
    WaveguideNetwork pressure(GuideType::TUBE, 480.0);
    WaveguideNetwork velocity(GuideType::TUBE, 480.0);
    pressure.pluck(0.5);
    velocity.pluck(0.5);
    velocity.set_measurement_mode(WaveguideNetwork::MeasurementMode::VELOCITY);

    const auto pressure_audio = render(pressure, 512);
    const auto velocity_audio = render(velocity, 512);

    double largest_difference = 0.0;
    for (size_t i = 0; i < pressure_audio.size(); ++i) {
        largest_difference = std::max(largest_difference,
            std::abs(pressure_audio[i] - velocity_audio[i]));
    }
    EXPECT_GT(largest_difference, 1e-4);
}

TEST(WaveguideMeasurementTest, TubeOutputEqualsTheMeasuredRailSumOrDifference)
{
    WaveguideNetwork pressure(GuideType::TUBE, 480.0);
    WaveguideNetwork velocity(GuideType::TUBE, 480.0);
    pressure.pluck(0.5);
    velocity.pluck(0.5);
    pressure.set_pickup_position(0.0);
    velocity.set_pickup_position(0.0);
    velocity.set_measurement_mode(WaveguideNetwork::MeasurementMode::VELOCITY);

    const auto pressure_audio = render(pressure, 10);
    const auto velocity_audio = render(velocity, 10);
    const auto& pressure_segment = pressure.get_segments().at(0);
    const auto& velocity_segment = velocity.get_segments().at(0);

    ASSERT_GT(std::abs(pressure_segment.p_minus[0]), 1e-4);
    EXPECT_DOUBLE_EQ(pressure_audio.back(),
        pressure_segment.p_plus[0] + pressure_segment.p_minus[0]);
    EXPECT_DOUBLE_EQ(velocity_audio.back(),
        velocity_segment.p_plus[0] - velocity_segment.p_minus[0]);
}

TEST(WaveguideReflectionTest, TubeOpenAndClosedEndsPropagateWithOppositeSigns)
{
    WaveguideNetwork tube(GuideType::TUBE, 480.0);
    tube.set_loss_factor(1.0);
    tube.set_loop_filter(tap(1.0));
    tube.pluck(0.5);
    render(tube, 3);

    const size_t delay = tube.get_segments().at(0).p_plus.capacity() - 2;
    const auto& open_state = tube.get_segments().at(0);
    const double open_arrival = 0.5
        * (open_state.p_plus[delay] + open_state.p_plus[delay + 1]);
    ASSERT_GT(open_arrival, 0.0);

    render(tube, 1);
    EXPECT_NEAR(tube.get_segments().at(0).p_minus[0], -open_arrival, 1e-12);

    render(tube, static_cast<unsigned int>(delay) + 1);
    const auto& closed_state = tube.get_segments().at(0);
    const double closed_arrival = 0.5
        * (closed_state.p_minus[delay] + closed_state.p_minus[delay + 1]);
    ASSERT_LT(closed_arrival, 0.0);

    render(tube, 1);
    EXPECT_NEAR(tube.get_segments().at(0).p_plus[0], closed_arrival, 1e-12);
}

TEST(WaveguideReflectionTest, LumpedStringLoopReturnsTheWaveWithItsSign)
{
    WaveguideNetwork string(GuideType::STRING, 480.0);
    string.set_loss_factor(1.0);
    string.set_loop_filter(tap(1.0));
    string.pluck(0.5);
    render(string, 3);

    const auto& segment = string.get_segments().at(0);
    const size_t delay = segment.p_plus.capacity() - 2;
    const double arrival = 0.5 * (segment.p_plus[delay] + segment.p_plus[delay + 1]);
    ASSERT_GT(std::abs(arrival), 1e-4);

    render(string, 1);
    EXPECT_NEAR(string.get_segments().at(0).p_plus[0], arrival, 1e-12);
}

class WaveguideLossBoundaryTest : public ::testing::TestWithParam<LossCase> { };

TEST_P(WaveguideLossBoundaryTest, LossFactorIsClampedToTheUnitInterval)
{
    WaveguideNetwork network(GuideType::STRING, 480.0);
    network.set_loss_factor(GetParam().requested);

    EXPECT_DOUBLE_EQ(network.get_loss_factor(), GetParam().effective);
}

INSTANTIATE_TEST_SUITE_P(
    Limits,
    WaveguideLossBoundaryTest,
    ::testing::Values(
        LossCase { "BelowZero", -0.5, 0.0 },
        LossCase { "WithinRange", 0.85, 0.85 },
        LossCase { "AboveOne", 1.5, 1.0 }),
    [](const ::testing::TestParamInfo<LossCase>& info) { return std::string(info.param.name); });

TEST(WaveguideLossTest, LowerLossFactorReducesLateRingEnergy)
{
    WaveguideNetwork shorter(GuideType::STRING, 480.0);
    WaveguideNetwork longer(GuideType::STRING, 480.0);
    shorter.set_loop_filter(tap(1.0));
    longer.set_loop_filter(tap(1.0));
    shorter.set_loss_factor(0.8);
    longer.set_loss_factor(1.0);
    shorter.pluck(0.5);
    longer.pluck(0.5);

    const auto short_audio = render(shorter, 2048);
    const auto long_audio = render(longer, 2048);

    EXPECT_GT(energy(std::span<const double>(long_audio).last(256)),
        energy(std::span<const double>(short_audio).last(256)));
}

class WaveguideTerminationTest : public ::testing::TestWithParam<TerminationCase> { };

TEST_P(WaveguideTerminationTest, ZeroGainTerminationStopsTheLateRing)
{
    const auto& config = GetParam();
    WaveguideNetwork open(config.type, 480.0);
    WaveguideNetwork blocked(config.type, 480.0);
    open.set_loss_factor(1.0);
    blocked.set_loss_factor(1.0);
    open.set_loop_filter(tap(1.0));
    blocked.set_loop_filter(tap(1.0));
    if (config.type == GuideType::TUBE) {
        if (config.open_end) {
            blocked.set_loop_filter_open(tap(0.0));
        } else {
            blocked.set_loop_filter_closed(tap(0.0));
        }
    } else {
        blocked.set_loop_filter(tap(0.0));
    }
    open.pluck(0.5);
    blocked.pluck(0.5);

    const auto sustained = render(open, 2048);
    const auto muted = render(blocked, 2048);

    EXPECT_GT(peak(std::span<const double>(sustained).last(256)), 1e-4);
    EXPECT_DOUBLE_EQ(peak(std::span<const double>(muted).last(256)), 0.0);
}

INSTANTIATE_TEST_SUITE_P(
    EndFilters,
    WaveguideTerminationTest,
    ::testing::Values(
        TerminationCase { "StringLoop", GuideType::STRING, false },
        TerminationCase { "TubeOpen", GuideType::TUBE, true },
        TerminationCase { "TubeClosed", GuideType::TUBE, false }),
    [](const ::testing::TestParamInfo<TerminationCase>& info) { return std::string(info.param.name); });

TEST(WaveguideTerminationTest, OpenAndClosedFiltersEachReduceTheRoundTrip)
{
    WaveguideNetwork unfiltered(GuideType::TUBE, 480.0);
    WaveguideNetwork open_only(GuideType::TUBE, 480.0);
    WaveguideNetwork closed_only(GuideType::TUBE, 480.0);
    WaveguideNetwork both(GuideType::TUBE, 480.0);

    for (auto* tube : { &unfiltered, &open_only, &closed_only, &both }) {
        tube->set_loss_factor(1.0);
        tube->set_loop_filter(tap(1.0));
        tube->pluck(0.5);
    }
    open_only.set_loop_filter_open(tap(0.5));
    closed_only.set_loop_filter_closed(tap(0.5));
    both.set_loop_filter_open(tap(0.5));
    both.set_loop_filter_closed(tap(0.5));

    render(unfiltered, 1024);
    render(open_only, 1024);
    render(closed_only, 1024);
    render(both, 1024);

    const double baseline_energy = rail_energy(unfiltered);
    const double open_energy = rail_energy(open_only);
    const double closed_energy = rail_energy(closed_only);
    const double both_energy = rail_energy(both);
    EXPECT_GT(baseline_energy, open_energy);
    EXPECT_GT(baseline_energy, closed_energy);
    EXPECT_GT(open_energy, both_energy);
    EXPECT_GT(closed_energy, both_energy);
}

class WaveguidePickupTest : public ::testing::TestWithParam<PickupCase> { };

TEST_P(WaveguidePickupTest, RequestedPositionIsClampedAndQuantizedToASample)
{
    const auto& config = GetParam();
    WaveguideNetwork network(GuideType::STRING, 480.0);
    network.set_pickup_position(config.requested);

    const size_t delay = network.get_segments().at(0).p_plus.capacity() - 2;
    const size_t tap_sample = static_cast<size_t>(config.clamped * static_cast<double>(delay));
    EXPECT_DOUBLE_EQ(network.get_pickup_position(),
        static_cast<double>(tap_sample) / static_cast<double>(delay));
}

INSTANTIATE_TEST_SUITE_P(
    TapPositions,
    WaveguidePickupTest,
    ::testing::Values(
        PickupCase { "BelowZero", -0.5, 0.0 },
        PickupCase { "Quarter", 0.25, 0.25 },
        PickupCase { "ThreeQuarters", 0.75, 0.75 },
        PickupCase { "AboveOne", 1.5, 1.0 }),
    [](const ::testing::TestParamInfo<PickupCase>& info) { return std::string(info.param.name); });

TEST(WaveguidePickupExtraTest, PositionSetBeforeFirstRenderRemainsSelected)
{
    WaveguideNetwork network(GuideType::STRING, 480.0);
    network.set_pickup_position(0.25);
    const double selected = network.get_pickup_position();

    render(network, 128);

    EXPECT_DOUBLE_EQ(network.get_pickup_position(), selected);
}

TEST(WaveguidePickupExtraTest, RetuningPreservesTheSelectedPosition)
{
    WaveguideNetwork network(GuideType::STRING, 480.0);
    network.set_pickup_position(0.25);

    network.set_fundamental(120.0);

    EXPECT_NEAR(network.get_pickup_position(), 0.25, 0.01);
}

TEST(WaveguidePickupExtraTest, MovingThePickupChangesTheRenderedWave)
{
    WaveguideNetwork near_bridge(GuideType::STRING, 480.0);
    WaveguideNetwork near_end(GuideType::STRING, 480.0);
    near_bridge.pluck(0.3);
    near_end.pluck(0.3);
    render(near_bridge, 1);
    render(near_end, 1);
    near_bridge.set_pickup_position(0.1);
    near_end.set_pickup_position(0.8);

    const auto first = render(near_bridge, 128);
    const auto second = render(near_end, 128);

    double largest_difference = 0.0;
    for (size_t i = 0; i < first.size(); ++i) {
        largest_difference = std::max(largest_difference, std::abs(first[i] - second[i]));
    }
    EXPECT_GT(largest_difference, 0.1);
}

class WaveguideContinuousTest : public ::testing::TestWithParam<GuideType> { };

TEST_P(WaveguideContinuousTest, ASourceDrivesACleanWaveguide)
{
    WaveguideNetwork driven(GetParam(), 480.0);
    WaveguideNetwork silent(GetParam(), 480.0);
    driven.set_exciter_node(constant(1.0));
    silent.set_exciter_node(constant(0.0));
    driven.set_exciter_type(WaveguideNetwork::ExciterType::CONTINUOUS);
    silent.set_exciter_type(WaveguideNetwork::ExciterType::CONTINUOUS);

    const auto audio = render(driven, 512);

    EXPECT_GT(peak(audio), 1e-4);
    EXPECT_DOUBLE_EQ(peak(render(silent, 512)), 0.0);
}

INSTANTIATE_TEST_SUITE_P(
    PropagationModes,
    WaveguideContinuousTest,
    ::testing::Values(GuideType::STRING, GuideType::TUBE));

TEST(WaveguideReleaseTest, ReleaseStopsAContinuousSource)
{
    WaveguideNetwork driven(GuideType::STRING, 480.0);
    WaveguideNetwork released(GuideType::STRING, 480.0);
    driven.set_exciter_node(constant(1.0));
    released.set_exciter_node(constant(1.0));
    driven.set_exciter_type(WaveguideNetwork::ExciterType::CONTINUOUS);
    released.set_exciter_type(WaveguideNetwork::ExciterType::CONTINUOUS);
    released.release();

    EXPECT_GT(peak(render(driven, 256)), 1e-4);
    EXPECT_DOUBLE_EQ(peak(render(released, 256)), 0.0);
}

TEST(WaveguideReleaseTest, ReleasedWaveguideRingsOutAfterActiveDriving)
{
    WaveguideNetwork network(GuideType::STRING, 480.0);
    network.set_loss_factor(0.5);
    network.set_exciter_node(constant(1.0));
    network.set_exciter_type(WaveguideNetwork::ExciterType::CONTINUOUS);
    ASSERT_GT(peak(render(network, 256)), 1e-4);

    network.release();
    const auto tail = render(network, 2048);

    EXPECT_LT(peak(std::span<const double>(tail).last(256)), 1e-4);
}

class WaveguideResetTest : public ::testing::TestWithParam<GuideType> { };

TEST_P(WaveguideResetTest, ResetClearsBothRailsAndTheRenderedRing)
{
    WaveguideNetwork network(GetParam(), 480.0);
    network.pluck(0.5);
    ASSERT_GT(peak(render(network, 256)), 0.1);

    network.reset();
    const auto silent = render(network, 256);

    EXPECT_DOUBLE_EQ(peak(silent), 0.0);
    for (const auto& segment : network.get_segments()) {
        for (size_t i = 0; i < segment.p_plus.capacity(); ++i) {
            EXPECT_DOUBLE_EQ(segment.p_plus[i], 0.0);
            EXPECT_DOUBLE_EQ(segment.p_minus[i], 0.0);
        }
    }
}

INSTANTIATE_TEST_SUITE_P(
    PropagationModes,
    WaveguideResetTest,
    ::testing::Values(GuideType::STRING, GuideType::TUBE));

TEST(WaveguideDisabledTest, DisabledProcessingPreservesTheSeedUntilReenabled)
{
    WaveguideNetwork paused(GuideType::STRING, 480.0);
    WaveguideNetwork reference(GuideType::STRING, 480.0);
    paused.pluck(0.5);
    reference.pluck(0.5);
    paused.set_enabled(false);

    EXPECT_DOUBLE_EQ(peak(render(paused, 128)), 0.0);
    paused.set_enabled(true);
    const auto resumed = render(paused, 128);
    const auto original = render(reference, 128);

    ASSERT_EQ(resumed.size(), original.size());
    for (size_t i = 0; i < original.size(); ++i) {
        EXPECT_NEAR(resumed[i], original[i], 1e-12) << "sample " << i;
    }
}

TEST(WaveguideOutputTest, CachedVoiceAndLastOutputMatchTheMix)
{
    WaveguideNetwork network(GuideType::STRING, 480.0);
    network.pluck(0.5);

    const auto audio = render(network, 64);
    const auto voice = network.get_node_audio_buffer(0);

    ASSERT_TRUE(voice.has_value());
    ASSERT_EQ(voice->size(), audio.size());
    EXPECT_TRUE(std::ranges::equal(*voice, audio));
    ASSERT_TRUE(network.get_node_output(0).has_value());
    EXPECT_DOUBLE_EQ(network.get_node_output(0).value(), audio.back());
    EXPECT_FALSE(network.get_node_output(1).has_value());
    EXPECT_FALSE(network.get_node_audio_buffer(1).has_value());
}

TEST(WaveguideMappingTest, BroadcastFrequencyRecomputesTheDelay)
{
    WaveguideNetwork network(GuideType::STRING, 480.0);
    network.map_parameter("frequency", constant(120.0));

    render(network, 1);

    EXPECT_DOUBLE_EQ(network.get_fundamental(), 120.0);
    EXPECT_EQ(network.get_segments().at(0).p_plus.capacity(),
        static_cast<size_t>(48000.0 / 120.0 - 0.5) + 2);
}

class WaveguideLossMappingTest : public ::testing::TestWithParam<const char*> { };

TEST_P(WaveguideLossMappingTest, BroadcastMappingSetsTheLossFactor)
{
    WaveguideNetwork network(GuideType::STRING, 480.0);
    network.map_parameter(GetParam(), constant(0.7));

    render(network, 1);

    EXPECT_DOUBLE_EQ(network.get_loss_factor(), 0.7);
}

INSTANTIATE_TEST_SUITE_P(
    Aliases,
    WaveguideLossMappingTest,
    ::testing::Values("loss", "damping"));

TEST(WaveguideMappingTest, BroadcastPositionMovesThePickup)
{
    WaveguideNetwork network(GuideType::STRING, 480.0);
    network.map_parameter("position", constant(0.25));

    render(network, 1);

    EXPECT_NEAR(network.get_pickup_position(), 0.25, 0.01);
}

TEST(WaveguideMappingTest, BroadcastScaleChangesTheAudioMix)
{
    WaveguideNetwork reference(GuideType::STRING, 480.0);
    WaveguideNetwork scaled(GuideType::STRING, 480.0);
    reference.pluck(0.5);
    scaled.pluck(0.5);
    scaled.map_parameter("scale", constant(0.25));

    const auto original = render(reference, 256);
    const auto reduced = render(scaled, 256);

    ASSERT_EQ(original.size(), reduced.size());
    for (size_t i = 0; i < original.size(); ++i) {
        EXPECT_NEAR(reduced[i], 0.25 * original[i], 1e-12) << "sample " << i;
    }
}

TEST(WaveguideMappingTest, NegativeBroadcastScaleMutesTheAudioMix)
{
    WaveguideNetwork network(GuideType::STRING, 480.0);
    network.pluck(0.5);
    network.map_parameter("scale", constant(-1.0));

    EXPECT_DOUBLE_EQ(peak(render(network, 256)), 0.0);
}

TEST(WaveguideMappingTest, UnmappingStopsFurtherFrequencyUpdates)
{
    WaveguideNetwork network(GuideType::STRING, 480.0);
    network.map_parameter("frequency", constant(240.0));
    render(network, 1);
    ASSERT_DOUBLE_EQ(network.get_fundamental(), 240.0);

    network.unmap_parameter("frequency");
    network.set_fundamental(480.0);
    render(network, 1);

    EXPECT_DOUBLE_EQ(network.get_fundamental(), 480.0);
}

TEST(WaveguideMappingTest, BroadcastFrequencyKeepsTheSelectedPickup)
{
    WaveguideNetwork network(GuideType::STRING, 480.0);
    network.set_pickup_position(0.25);
    network.map_parameter("frequency", constant(240.0));

    for (size_t block = 0; block < 4; ++block) {
        render(network, 64);
    }

    EXPECT_NEAR(network.get_pickup_position(), 0.25, 0.01);
}

class WaveguidePitchTest : public ::testing::TestWithParam<PitchCase> { };

TEST_P(WaveguidePitchTest, ALosslessRingRepeatsAtTheRequestedFundamental)
{
    const auto& config = GetParam();
    WaveguideNetwork network(config.type, config.frequency);
    network.set_loss_factor(1.0);
    network.set_loop_filter(tap(1.0));
    network.set_pickup_position(0.1);
    network.pluck(0.3);

    const auto audio = render(network, 8704);
    const double expected = 48000.0 / config.frequency;
    const size_t period = dominant_period(
        std::span<const double>(audio).subspan(512),
        static_cast<size_t>(5.0 * expected));

    EXPECT_NEAR(static_cast<double>(period), expected, std::max(2.0, 0.005 * expected));
}

INSTANTIATE_TEST_SUITE_P(
    Fundamentals,
    WaveguidePitchTest,
    ::testing::Values(
        PitchCase { "StringLow", GuideType::STRING, 220.0 },
        PitchCase { "StringMid", GuideType::STRING, 480.0 },
        PitchCase { "TubeLow", GuideType::TUBE, 220.0 },
        PitchCase { "TubeMid", GuideType::TUBE, 480.0 }),
    [](const ::testing::TestParamInfo<PitchCase>& info) { return std::string(info.param.name); });

class WaveguideMetadataTypeTest : public ::testing::TestWithParam<TypeLabelCase> { };

TEST_P(WaveguideMetadataTypeTest, MetadataNamesTheStructure)
{
    WaveguideNetwork network(GetParam().type, 480.0);

    EXPECT_EQ(network.get_metadata().at("type"), GetParam().label);
    EXPECT_EQ(network.get_type(), GetParam().type);
}

INSTANTIATE_TEST_SUITE_P(
    Structures,
    WaveguideMetadataTypeTest,
    ::testing::ValuesIn(kTypeLabels),
    [](const ::testing::TestParamInfo<TypeLabelCase>& info) { return std::string(info.param.name); });

class WaveguideMetadataExciterTest : public ::testing::TestWithParam<ExciterLabelCase> { };

TEST_P(WaveguideMetadataExciterTest, MetadataNamesTheExciterType)
{
    WaveguideNetwork network(GuideType::STRING, 480.0);
    network.set_exciter_type(GetParam().type);

    EXPECT_EQ(network.get_exciter_type(), GetParam().type);
    EXPECT_EQ(network.get_metadata().at("exciter_type"), GetParam().label);
}

INSTANTIATE_TEST_SUITE_P(
    ExciterTypes,
    WaveguideMetadataExciterTest,
    ::testing::ValuesIn(kExciterLabels),
    [](const ::testing::TestParamInfo<ExciterLabelCase>& info) { return std::string(info.param.name); });

class WaveguidePluckClampTest : public ::testing::TestWithParam<PluckClampCase> { };

TEST_P(WaveguidePluckClampTest, PluckPositionIsKeptOffTheTerminations)
{
    WaveguideNetwork requested(GuideType::STRING, 480.0);
    WaveguideNetwork clamped(GuideType::STRING, 480.0);
    requested.pluck(GetParam().requested);
    clamped.pluck(GetParam().clamped);

    const auto first = render(requested, 128);
    const auto second = render(clamped, 128);

    ASSERT_EQ(first.size(), second.size());
    for (size_t i = 0; i < first.size(); ++i) {
        EXPECT_NEAR(first[i], second[i], 1e-12) << "sample " << i;
    }
}

INSTANTIATE_TEST_SUITE_P(
    Limits,
    WaveguidePluckClampTest,
    ::testing::Values(
        PluckClampCase { "BelowZero", -1.0, 0.01 },
        PluckClampCase { "AtZero", 0.0, 0.01 },
        PluckClampCase { "AtOne", 1.0, 0.99 },
        PluckClampCase { "AboveOne", 2.0, 0.99 }),
    [](const ::testing::TestParamInfo<PluckClampCase>& info) { return std::string(info.param.name); });

class WaveguideReuseTest : public ::testing::TestWithParam<GuideType> { };

TEST_P(WaveguideReuseTest, ResetNetworkRepeatsAFreshNetworkExactly)
{
    WaveguideNetwork reused(GetParam(), 480.0);
    reused.pluck(0.4);
    render(reused, 256);
    reused.reset();
    reused.pluck(0.4);

    WaveguideNetwork fresh(GetParam(), 480.0);
    fresh.pluck(0.4);

    const auto again = render(reused, 256);
    const auto original = render(fresh, 256);

    ASSERT_EQ(again.size(), original.size());
    for (size_t i = 0; i < original.size(); ++i) {
        EXPECT_NEAR(again[i], original[i], 1e-12) << "sample " << i;
    }
}

INSTANTIATE_TEST_SUITE_P(
    PropagationModes,
    WaveguideReuseTest,
    ::testing::Values(GuideType::STRING, GuideType::TUBE));

class WaveguideRetuneTest : public ::testing::TestWithParam<GuideType> { };

TEST_P(WaveguideRetuneTest, RetuningMidRingStaysFinite)
{
    WaveguideNetwork network(GetParam(), 480.0);
    network.pluck(0.5);
    render(network, 256);

    network.set_fundamental(960.0);
    const auto raised = render(network, 256);
    network.set_fundamental(240.0);
    const auto lowered = render(network, 512);

    EXPECT_GT(peak(raised), 1e-4);
    EXPECT_TRUE(std::ranges::all_of(raised, [](double sample) { return std::isfinite(sample); }));
    EXPECT_TRUE(std::ranges::all_of(lowered, [](double sample) { return std::isfinite(sample); }));
}

INSTANTIATE_TEST_SUITE_P(
    PropagationModes,
    WaveguideRetuneTest,
    ::testing::Values(GuideType::STRING, GuideType::TUBE));

class WaveguideContinuousScaleTest : public ::testing::TestWithParam<GuideType> { };

TEST_P(WaveguideContinuousScaleTest, DrivenOutputScalesLinearlyWithTheSource)
{
    WaveguideNetwork full(GetParam(), 480.0);
    WaveguideNetwork half(GetParam(), 480.0);
    full.set_exciter_node(constant(0.2));
    half.set_exciter_node(constant(0.1));
    full.set_exciter_type(WaveguideNetwork::ExciterType::CONTINUOUS);
    half.set_exciter_type(WaveguideNetwork::ExciterType::CONTINUOUS);

    const auto original = render(full, 512);
    const auto reduced = render(half, 512);

    ASSERT_GT(peak(original), 1e-4);
    ASSERT_EQ(original.size(), reduced.size());
    for (size_t i = 0; i < original.size(); ++i) {
        EXPECT_NEAR(reduced[i], 0.5 * original[i], 1e-12) << "sample " << i;
    }
}

INSTANTIATE_TEST_SUITE_P(
    PropagationModes,
    WaveguideContinuousScaleTest,
    ::testing::Values(GuideType::STRING, GuideType::TUBE));

class WaveguideContinuousBoundTest : public ::testing::TestWithParam<GuideType> { };

TEST_P(WaveguideContinuousBoundTest, ASteadySourceDoesNotRunAway)
{
    WaveguideNetwork network(GetParam(), 480.0);
    network.set_exciter_node(constant(0.25));
    network.set_exciter_type(WaveguideNetwork::ExciterType::CONTINUOUS);

    const auto audio = render(network, 48000);

    EXPECT_TRUE(std::ranges::all_of(audio, [](double sample) { return std::isfinite(sample); }));
    EXPECT_LT(peak(audio), 50.0);
}

INSTANTIATE_TEST_SUITE_P(
    PropagationModes,
    WaveguideContinuousBoundTest,
    ::testing::Values(GuideType::STRING, GuideType::TUBE));

class WaveguideContinuousOrderTest : public ::testing::TestWithParam<GuideType> { };

TEST_P(WaveguideContinuousOrderTest, TypeAndNodeMayBeSetInEitherOrder)
{
    WaveguideNetwork node_first(GetParam(), 480.0);
    node_first.set_exciter_node(constant(1.0));
    node_first.set_exciter_type(WaveguideNetwork::ExciterType::CONTINUOUS);

    WaveguideNetwork type_first(GetParam(), 480.0);
    type_first.set_exciter_type(WaveguideNetwork::ExciterType::CONTINUOUS);
    type_first.set_exciter_node(constant(1.0));

    const auto expected = render(node_first, 512);
    const auto actual = render(type_first, 512);

    ASSERT_GT(peak(expected), 1e-4);
    ASSERT_EQ(expected.size(), actual.size());
    for (size_t i = 0; i < expected.size(); ++i) {
        EXPECT_NEAR(actual[i], expected[i], 1e-12) << "sample " << i;
    }
}

INSTANTIATE_TEST_SUITE_P(
    PropagationModes,
    WaveguideContinuousOrderTest,
    ::testing::Values(GuideType::STRING, GuideType::TUBE));

TEST(WaveguideContinuousOrderExtraTest, ANodeAloneStaysSilentUntilTheTypeIsContinuous)
{
    WaveguideNetwork network(GuideType::STRING, 480.0);
    network.set_exciter_node(constant(1.0));

    EXPECT_DOUBLE_EQ(peak(render(network, 256)), 0.0);
}

class WaveguideCeilingTest : public ::testing::TestWithParam<GuideType> { };

TEST_P(WaveguideCeilingTest, ADriveAtResonanceSettlesWithoutFlatteningTheWave)
{
    const size_t period = ring_period(GetParam(), 480.0);
    ASSERT_GT(period, 0U);

    WaveguideNetwork network(GetParam(), 480.0);
    network.set_exciter_node(sine_source(period));
    network.set_exciter_type(WaveguideNetwork::ExciterType::CONTINUOUS);

    const auto audio = render(network, 96000);
    const auto tail = std::span<const double>(audio).last(4096);
    const double top = peak(tail);

    EXPECT_TRUE(std::ranges::all_of(audio, [](double sample) { return std::isfinite(sample); }));
    EXPECT_GT(top, 2.0);
    EXPECT_LT(top, 16.0);
    const auto near_top = std::ranges::count_if(tail,
        [top](double sample) { return std::abs(sample) > 0.99 * top; });
    EXPECT_LT(static_cast<double>(near_top) / static_cast<double>(tail.size()), 0.25);
}

INSTANTIATE_TEST_SUITE_P(
    PropagationModes,
    WaveguideCeilingTest,
    ::testing::Values(GuideType::STRING, GuideType::TUBE));

class WaveguideConstructionTest : public ::testing::TestWithParam<GuideType> { };

TEST_P(WaveguideConstructionTest, ANewNetworkIsAnAudioSinkRingWithoutEndFilters)
{
    WaveguideNetwork network(GetParam(), 220.0);
    const auto& segment = network.get_segments().at(0);

    EXPECT_DOUBLE_EQ(network.get_fundamental(), 220.0);
    EXPECT_EQ(network.get_type(), GetParam());
    EXPECT_EQ(network.get_output_mode(), Nodes::Network::OutputMode::AUDIO_SINK);
    EXPECT_EQ(network.get_topology(), Nodes::Network::Topology::RING);
    EXPECT_EQ(network.get_exciter_type(), WaveguideNetwork::ExciterType::NOISE_BURST);
    EXPECT_NE(segment.loop_filter, nullptr);
    EXPECT_EQ(segment.loop_filter_closed, nullptr);
    EXPECT_EQ(segment.loop_filter_open, nullptr);
}

INSTANTIATE_TEST_SUITE_P(
    PropagationModes,
    WaveguideConstructionTest,
    ::testing::Values(GuideType::STRING, GuideType::TUBE));

class WaveguideQuietTest : public ::testing::TestWithParam<GuideType> { };

TEST_P(WaveguideQuietTest, UnexcitedWaveguideIsSilent)
{
    WaveguideNetwork network(GetParam(), 220.0);

    EXPECT_DOUBLE_EQ(peak(render(network, 512)), 0.0);
}

INSTANTIATE_TEST_SUITE_P(
    PropagationModes,
    WaveguideQuietTest,
    ::testing::Values(GuideType::STRING, GuideType::TUBE));

class WaveguideDecayTest : public ::testing::TestWithParam<GuideType> { };

TEST_P(WaveguideDecayTest, DefaultRingFadesOverTime)
{
    WaveguideNetwork network(GetParam(), 220.0);
    network.pluck(0.5);

    const auto early = render(network, 512);
    for (size_t block = 0; block < 100; ++block) {
        render(network, 512);
    }
    const auto late = render(network, 512);

    EXPECT_GT(energy(early), energy(late));
}

INSTANTIATE_TEST_SUITE_P(
    PropagationModes,
    WaveguideDecayTest,
    ::testing::Values(GuideType::STRING, GuideType::TUBE));

TEST(WaveguideMetadataTest, MetadataCarriesTheKeysAndTracksTheDelay)
{
    WaveguideNetwork network(GuideType::STRING, 220.0);

    auto metadata = network.get_metadata();
    for (const auto* key : { "type", "fundamental", "delay_length", "loss_factor",
             "pickup_position", "exciter_type" }) {
        EXPECT_TRUE(metadata.contains(key)) << key;
    }

    const auto before = metadata.at("delay_length");
    network.set_fundamental(440.0);

    EXPECT_NE(network.get_metadata().at("delay_length"), before);
}

TEST(WaveguideTerminationTest, TubeEndsFallBackToTheSharedLoopFilter)
{
    WaveguideNetwork tube(GuideType::TUBE, 480.0);
    tube.set_loss_factor(1.0);
    tube.set_loop_filter(tap(0.0));
    tube.pluck(0.5);

    const auto audio = render(tube, 2048);

    EXPECT_GT(peak(std::span<const double>(audio).first(256)), 1e-4);
    EXPECT_DOUBLE_EQ(peak(std::span<const double>(audio).last(256)), 0.0);
}

}

}
