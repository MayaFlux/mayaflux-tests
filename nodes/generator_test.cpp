#include "gtest/gtest.h"

#include "MayaFlux/Nodes/Conduit/Constant.hpp"
#include "MayaFlux/Nodes/Generators/Counter.hpp"
#include "MayaFlux/Nodes/Generators/Impulse.hpp"
#include "MayaFlux/Nodes/Generators/Phasor.hpp"
#include "MayaFlux/Nodes/Generators/Random.hpp"
#include "MayaFlux/Nodes/Generators/Sine.hpp"

namespace MayaFlux::Test {
namespace {

    using Nodes::Constant;
    using Nodes::ModulatorRole;
    using Nodes::Node;
    using Nodes::NodeContext;
    using Nodes::Generator::Counter;
    using Nodes::Generator::GeneratorContext;
    using Nodes::Generator::Impulse;
    using Nodes::Generator::Phasor;
    using Nodes::Generator::Random;
    using Nodes::Generator::RandomContext;
    using Nodes::Generator::Sine;
    using Kinesis::Stochastic::Algorithm;

    constexpr double k_rate = 48000.0;
    constexpr double k_tolerance = 1e-9;

    std::vector<double> run(Node& node, size_t count)
    {
        std::vector<double> samples;
        samples.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            samples.push_back(node.process_sample(0.0));
        }
        return samples;
    }

    void expect_near(const std::vector<double>& actual, const std::vector<double>& expected)
    {
        ASSERT_EQ(actual.size(), expected.size());
        for (size_t i = 0; i < actual.size(); ++i) {
            EXPECT_NEAR(actual[i], expected[i], k_tolerance) << "sample " << i;
        }
    }

    struct SineCase {
        const char* name;
        float frequency;
        double amplitude;
        float offset;
    };

    class GeneratorSineTest : public ::testing::TestWithParam<SineCase> {
    protected:
        [[nodiscard]] std::vector<double> expected(size_t count) const
        {
            std::vector<double> out;
            for (size_t n = 0; n < count; ++n) {
                const double phase = 2.0 * std::numbers::pi * GetParam().frequency * static_cast<double>(n) / k_rate;
                out.push_back(GetParam().amplitude * std::sin(phase + GetParam().offset));
            }
            return out;
        }

        Sine node { GetParam().frequency, GetParam().amplitude, GetParam().offset };
    };

    TEST_P(GeneratorSineTest, SamplesFollowTheSineOfTheAccumulatedPhase)
    {
        expect_near(run(node, 256), expected(256));
    }

    TEST_P(GeneratorSineTest, BatchMatchesConsecutiveSamples)
    {
        Sine other { GetParam().frequency, GetParam().amplitude, GetParam().offset };

        expect_near(node.process_batch(64), run(other, 64));
    }

    TEST_P(GeneratorSineTest, LastOutputIsTheReturnedSample)
    {
        const double sample = node.process_sample(0.0);

        EXPECT_DOUBLE_EQ(node.get_last_output(), sample);
    }

    TEST_P(GeneratorSineTest, ResetRestartsThePhaseWithTheGivenParameters)
    {
        run(node, 37);

        node.reset(GetParam().frequency, GetParam().amplitude, GetParam().offset);

        expect_near(run(node, 32), expected(32));
    }

    INSTANTIATE_TEST_SUITE_P(
        Parameters,
        GeneratorSineTest,
        ::testing::Values(
            SineCase { "Low", 100.0F, 1.0, 0.0F },
            SineCase { "Mid", 1000.0F, 0.5, 0.0F },
            SineCase { "Quarter", 12000.0F, 0.25, 0.0F },
            SineCase { "Offset", 1000.0F, 1.0, 0.5F },
            SineCase { "AboveNyquist", 30000.0F, 1.0, 0.0F }),
        [](const ::testing::TestParamInfo<SineCase>& info) { return std::string(info.param.name); });

    class GeneratorSineBehaviourTest : public ::testing::Test {
    protected:
        Sine node { 1000.0F, 1.0 };
    };

    TEST_F(GeneratorSineBehaviourTest, StartsAtZeroForZeroOffset)
    {
        EXPECT_NEAR(node.process_sample(0.0), 0.0, k_tolerance);
    }

    TEST_F(GeneratorSineBehaviourTest, RepeatsAfterOnePeriod)
    {
        const auto samples = run(node, 96);

        for (size_t n = 0; n < 48; ++n) {
            EXPECT_NEAR(samples[n], samples[n + 48], 1e-7) << "sample " << n;
        }
    }

    TEST_F(GeneratorSineBehaviourTest, SetFrequencyMatchesConstructingAtThatFrequency)
    {
        Sine reference { 2500.0F, 1.0 };
        node.set_frequency(2500.0F);

        EXPECT_FLOAT_EQ(node.get_frequency(), 2500.0F);
        expect_near(run(node, 64), run(reference, 64));
    }

    TEST_F(GeneratorSineBehaviourTest, SetParamsAppliesFrequencyAmplitudeAndOffset)
    {
        node.set_params(2000.0F, 0.5, 0.25F);

        EXPECT_FLOAT_EQ(node.get_frequency(), 2000.0F);
        EXPECT_DOUBLE_EQ(node.get_amplitude(), 0.5);
        EXPECT_NEAR(node.process_sample(0.0), 0.5 * std::sin(0.25), k_tolerance);
    }

    TEST_F(GeneratorSineBehaviourTest, InputIsMixedInAndHalved)
    {
        EXPECT_NEAR(node.process_sample(0.5), 0.25, k_tolerance);
    }

    TEST_F(GeneratorSineBehaviourTest, FrequencyModulatorAddsToTheFrequency)
    {
        Sine modulated { std::make_shared<Constant>(500.0), 500.0F, 1.0 };
        Sine reference { 1000.0F, 1.0 };

        expect_near(run(modulated, 64), run(reference, 64));
    }

    TEST_F(GeneratorSineBehaviourTest, AmplitudeModulatorAddsToTheAmplitude)
    {
        Sine modulated { 1000.0F, std::make_shared<Constant>(0.5), 0.25 };
        Sine reference { 1000.0F, 0.75 };

        expect_near(run(modulated, 64), run(reference, 64));
    }

    TEST_F(GeneratorSineBehaviourTest, ModulatorsAreListedWithTheirRoles)
    {
        auto frequency = std::make_shared<Constant>(1.0);
        auto amplitude = std::make_shared<Constant>(1.0);
        Sine both { frequency, amplitude, 440.0F, 1.0 };

        const auto modulators = both.get_modulators();

        ASSERT_EQ(modulators.size(), 2U);
        EXPECT_EQ(modulators[0].first, ModulatorRole::FrequencyMod);
        EXPECT_EQ(modulators[0].second, frequency);
        EXPECT_EQ(modulators[1].first, ModulatorRole::AmplitudeMod);
        EXPECT_EQ(modulators[1].second, amplitude);
        EXPECT_TRUE(node.get_modulators().empty());
    }

    TEST_F(GeneratorSineBehaviourTest, ClearingModulatorsRestartsAsAPlainSine)
    {
        Sine modulated { std::make_shared<Constant>(500.0), 500.0F, 1.0 };
        run(modulated, 20);

        modulated.clear_modulators();

        Sine reference { 500.0F, 1.0 };
        EXPECT_TRUE(modulated.get_modulators().empty());
        expect_near(run(modulated, 32), run(reference, 32));
    }

    struct PhasorCase {
        const char* name;
        float frequency;
        double step;
    };

    class GeneratorPhasorTest : public ::testing::TestWithParam<PhasorCase> {
    protected:
        Phasor node { GetParam().frequency };
    };

    TEST_P(GeneratorPhasorTest, RampsByTheFrequencyOverTheRateAndWraps)
    {
        double phase = 0.0;
        for (int n = 0; n < 40; ++n) {
            EXPECT_DOUBLE_EQ(node.process_sample(0.0), phase) << "sample " << n;
            phase += GetParam().step;
            if (phase >= 1.0) {
                phase -= 1.0;
            }
        }
    }

    TEST_P(GeneratorPhasorTest, AmplitudeAndOffsetShapeTheRamp)
    {
        Phasor shaped { GetParam().frequency, 2.0, 0.5F };

        double phase = 0.0;
        for (int n = 0; n < 24; ++n) {
            EXPECT_DOUBLE_EQ(shaped.process_sample(0.0), phase * 2.0 + 0.5) << "sample " << n;
            phase += GetParam().step;
            if (phase >= 1.0) {
                phase -= 1.0;
            }
        }
    }

    TEST_P(GeneratorPhasorTest, PhaseWrapHookFiresOncePerWrap)
    {
        int wraps = 0;
        node.on_phase_wrap([&wraps](GeneratorContext&) { ++wraps; });

        const int samples = 32;
        run(node, samples);

        EXPECT_EQ(wraps, static_cast<int>(samples * GetParam().step));
    }

    TEST_P(GeneratorPhasorTest, ThresholdHookFiresOncePerRisingCrossing)
    {
        int crossings = 0;
        node.on_threshold([&crossings](GeneratorContext&) { ++crossings; }, 0.5);

        const int samples = 32;
        run(node, samples);

        EXPECT_EQ(crossings, static_cast<int>(samples * GetParam().step));
    }

    INSTANTIATE_TEST_SUITE_P(
        Rates,
        GeneratorPhasorTest,
        ::testing::Values(
            PhasorCase { "Quarter", 12000.0F, 0.25 },
            PhasorCase { "Eighth", 6000.0F, 0.125 }),
        [](const ::testing::TestParamInfo<PhasorCase>& info) { return std::string(info.param.name); });

    class GeneratorPhasorBehaviourTest : public ::testing::Test {
    protected:
        Phasor node { 12000.0F };
    };

    TEST_F(GeneratorPhasorBehaviourTest, SetPhaseWrapsIntoTheUnitRange)
    {
        node.set_phase(0.75);
        EXPECT_DOUBLE_EQ(node.get_phase(), 0.75);

        node.set_phase(1.25);
        EXPECT_DOUBLE_EQ(node.get_phase(), 0.25);

        node.set_phase(-0.25);
        EXPECT_DOUBLE_EQ(node.get_phase(), 0.75);
    }

    TEST_F(GeneratorPhasorBehaviourTest, ResetAppliesTheGivenParametersAndPhase)
    {
        run(node, 3);

        node.reset(6000.0F, 2.0F, 0.5F, 0.5);

        EXPECT_FLOAT_EQ(node.get_frequency(), 6000.0F);
        EXPECT_DOUBLE_EQ(node.get_last_output(), 0.0);
        EXPECT_DOUBLE_EQ(node.process_sample(0.0), 0.5 * 2.0 + 0.5);
        EXPECT_DOUBLE_EQ(node.get_phase(), 0.625);
    }

    TEST_F(GeneratorPhasorBehaviourTest, SetFrequencyChangesTheStep)
    {
        node.set_frequency(6000.0F);

        EXPECT_FLOAT_EQ(node.get_frequency(), 6000.0F);
        run(node, 4);
        EXPECT_DOUBLE_EQ(node.get_phase(), 0.5);
    }

    TEST_F(GeneratorPhasorBehaviourTest, AmplitudeModulatorMultipliesTheRamp)
    {
        Phasor modulated { 12000.0F, std::make_shared<Constant>(2.0), 1.0 };

        const auto samples = run(modulated, 4);

        EXPECT_DOUBLE_EQ(samples[1], 0.25 * 2.0);
        EXPECT_DOUBLE_EQ(samples[3], 0.75 * 2.0);
    }

    TEST_F(GeneratorPhasorBehaviourTest, AmplitudeModulatedConstructorReportsItsFrequency)
    {
        Phasor modulated { 6000.0F, std::make_shared<Constant>(1.0), 1.0 };

        EXPECT_FLOAT_EQ(modulated.get_frequency(), 6000.0F);
    }

    TEST_F(GeneratorPhasorBehaviourTest, ModulatorsAreListedWithTheirRoles)
    {
        auto frequency = std::make_shared<Constant>(1.0);
        auto amplitude = std::make_shared<Constant>(1.0);
        Phasor both { frequency, amplitude, 1.0F, 1.0 };

        const auto modulators = both.get_modulators();

        ASSERT_EQ(modulators.size(), 2U);
        EXPECT_EQ(modulators[0].first, ModulatorRole::FrequencyMod);
        EXPECT_EQ(modulators[1].first, ModulatorRole::AmplitudeMod);
        EXPECT_TRUE(node.get_modulators().empty());

        both.clear_modulators();
        EXPECT_TRUE(both.get_modulators().empty());
    }

    struct ImpulseCase {
        const char* name;
        float frequency;
        size_t period;
    };

    class GeneratorImpulseTest : public ::testing::TestWithParam<ImpulseCase> {
    protected:
        Impulse node { GetParam().frequency, 1.5, 0.25F };
    };

    TEST_P(GeneratorImpulseTest, FiresOnTheFirstSampleOfEachPeriod)
    {
        const auto samples = run(node, GetParam().period * 5);

        for (size_t n = 0; n < samples.size(); ++n) {
            const double expected = (n % GetParam().period == 0) ? 1.5 + 0.25 : 0.25;
            EXPECT_DOUBLE_EQ(samples[n], expected) << "sample " << n;
        }
    }

    TEST_P(GeneratorImpulseTest, ImpulseHookFiresOncePerImpulse)
    {
        int impulses = 0;
        node.on_impulse([&impulses](GeneratorContext&) { ++impulses; });

        run(node, GetParam().period * 5);

        EXPECT_EQ(impulses, 5);
    }

    TEST_P(GeneratorImpulseTest, ResetRestartsThePeriodWithTheGivenParameters)
    {
        run(node, GetParam().period + 1);

        node.reset(GetParam().frequency, 2.0F, 0.0F);

        EXPECT_DOUBLE_EQ(node.process_sample(0.0), 2.0);
        EXPECT_DOUBLE_EQ(node.process_sample(0.0), 0.0);
    }

    INSTANTIATE_TEST_SUITE_P(
        Periods,
        GeneratorImpulseTest,
        ::testing::Values(
            ImpulseCase { "EveryFourth", 12000.0F, 4 },
            ImpulseCase { "EveryEighth", 6000.0F, 8 }),
        [](const ::testing::TestParamInfo<ImpulseCase>& info) { return std::string(info.param.name); });

    class GeneratorImpulseBehaviourTest : public ::testing::Test {
    protected:
        Impulse node { 12000.0F };
    };

    TEST_F(GeneratorImpulseBehaviourTest, SetFrequencyChangesThePeriod)
    {
        node.set_frequency(6000.0F);

        EXPECT_FLOAT_EQ(node.get_frequency(), 6000.0F);
        const auto samples = run(node, 16);
        EXPECT_DOUBLE_EQ(samples[0], 1.0);
        EXPECT_DOUBLE_EQ(samples[8], 1.0);
        EXPECT_DOUBLE_EQ(samples[4], 0.0);
    }

    TEST_F(GeneratorImpulseBehaviourTest, SetParamsAppliesFrequencyAmplitudeAndOffset)
    {
        node.set_params(6000.0F, 3.0, 0.5F);

        EXPECT_FLOAT_EQ(node.get_frequency(), 6000.0F);
        EXPECT_DOUBLE_EQ(node.get_amplitude(), 3.0);
        EXPECT_DOUBLE_EQ(node.process_sample(0.0), 3.5);
    }

    TEST_F(GeneratorImpulseBehaviourTest, AmplitudeModulatorDoesNotDriftTheBaseAmplitude)
    {
        Impulse modulated { 12000.0F, std::make_shared<Constant>(0.5), 1.0 };

        const auto samples = run(modulated, 16);

        for (size_t n = 0; n < samples.size(); n += 4) {
            EXPECT_DOUBLE_EQ(samples[n], 1.5) << "impulse at sample " << n;
        }
        EXPECT_DOUBLE_EQ(modulated.get_amplitude(), 1.0);
    }

    TEST_F(GeneratorImpulseBehaviourTest, NonPositiveFrequencyModulationStaysFinite)
    {
        Impulse modulated { std::make_shared<Constant>(-5.0), 12000.0F, 1.0 };

        const auto samples = run(modulated, 200);

        int impulses = 0;
        for (const double sample : samples) {
            EXPECT_TRUE(std::isfinite(sample));
            impulses += sample != 0.0 ? 1 : 0;
        }
        EXPECT_LE(impulses, 2);
    }

    TEST_F(GeneratorImpulseBehaviourTest, ModulatorsAreListedWithTheirRoles)
    {
        auto frequency = std::make_shared<Constant>(1.0);
        auto amplitude = std::make_shared<Constant>(1.0);
        Impulse both { frequency, amplitude, 1.0F, 1.0 };

        const auto modulators = both.get_modulators();

        ASSERT_EQ(modulators.size(), 2U);
        EXPECT_EQ(modulators[0].first, ModulatorRole::FrequencyMod);
        EXPECT_EQ(modulators[1].first, ModulatorRole::AmplitudeMod);
        EXPECT_TRUE(node.get_modulators().empty());
    }

    struct CounterCase {
        const char* name;
        uint32_t modulo;
        int32_t step;
        std::vector<uint32_t> counts;
    };

    class GeneratorCounterTest : public ::testing::TestWithParam<CounterCase> {
    protected:
        Counter node { GetParam().modulo, GetParam().step };
    };

    TEST_P(GeneratorCounterTest, CountSequenceFollowsStepAndModulo)
    {
        for (const uint32_t expected : GetParam().counts) {
            node.process_sample(0.0);
            EXPECT_EQ(node.get_count(), expected);
        }
    }

    TEST_P(GeneratorCounterTest, OutputIsTheCountNormalisedByTheModulo)
    {
        for (const uint32_t expected : GetParam().counts) {
            const double sample = node.process_sample(0.0);
            const double divisor = GetParam().modulo > 1 ? static_cast<double>(GetParam().modulo - 1) : 1.0;
            EXPECT_DOUBLE_EQ(sample, static_cast<double>(expected) / divisor);
        }
    }

    TEST_P(GeneratorCounterTest, ResetZeroesTheCount)
    {
        run(node, 3);

        node.reset();

        EXPECT_EQ(node.get_count(), 0U);
        node.process_sample(0.0);
        EXPECT_EQ(node.get_count(), GetParam().counts.front());
    }

    INSTANTIATE_TEST_SUITE_P(
        Sequences,
        GeneratorCounterTest,
        ::testing::Values(
            CounterCase { "UpByOne", 4, 1, { 1, 2, 3, 0, 1, 2, 3, 0 } },
            CounterCase { "UpByTwo", 8, 2, { 2, 4, 6, 0, 2, 4, 6, 0 } },
            CounterCase { "DownByOne", 4, -1, { 3, 2, 1, 0, 3, 2, 1, 0 } },
            CounterCase { "Unbounded", 0, 1, { 1, 2, 3, 4, 5, 6 } },
            CounterCase { "ModuloOne", 1, 1, { 0, 0, 0, 0 } }),
        [](const ::testing::TestParamInfo<CounterCase>& info) { return std::string(info.param.name); });

    class GeneratorCounterBehaviourTest : public ::testing::Test {
    protected:
        Counter node { 4U, 1 };
    };

    TEST_F(GeneratorCounterBehaviourTest, DefaultsAreSixteenAndOne)
    {
        Counter defaults;

        EXPECT_EQ(defaults.get_modulo(), 16U);
        EXPECT_EQ(defaults.get_step(), 1);
        EXPECT_EQ(defaults.get_count(), 0U);
        EXPECT_FLOAT_EQ(defaults.get_frequency(), 0.0F);
    }

    TEST_F(GeneratorCounterBehaviourTest, SettersChangeModuloAndStep)
    {
        node.set_modulo(8);
        node.set_step(3);

        EXPECT_EQ(node.get_modulo(), 8U);
        EXPECT_EQ(node.get_step(), 3);
        node.process_sample(0.0);
        EXPECT_EQ(node.get_count(), 3U);
    }

    TEST_F(GeneratorCounterBehaviourTest, AmplitudeScalesTheOutput)
    {
        node.set_amplitude(0.5);

        EXPECT_DOUBLE_EQ(node.process_sample(0.0), 0.5 * (1.0 / 3.0));
    }

    TEST_F(GeneratorCounterBehaviourTest, IncrementHookFiresEveryTick)
    {
        int increments = 0;
        node.on_increment([&increments](GeneratorContext&) { ++increments; });

        run(node, 12);

        EXPECT_EQ(increments, 12);
    }

    TEST_F(GeneratorCounterBehaviourTest, WrapHookFiresOnEachUpwardWrap)
    {
        int wraps = 0;
        node.on_wrap([&wraps](GeneratorContext&) { ++wraps; });

        run(node, 12);

        EXPECT_EQ(wraps, 3);
    }

    TEST_F(GeneratorCounterBehaviourTest, WrapHookFiresOnEachDownwardWrap)
    {
        Counter down { 4U, -1 };
        int wraps = 0;
        down.on_wrap([&wraps](GeneratorContext&) { ++wraps; });

        run(down, 12);

        EXPECT_EQ(wraps, 3);
    }

    TEST_F(GeneratorCounterBehaviourTest, CountHookFiresWhenTheTargetIsReached)
    {
        int hits = 0;
        node.on_count(2, [&hits](GeneratorContext&) { ++hits; });

        run(node, 12);

        EXPECT_EQ(hits, 3);
    }

    TEST_F(GeneratorCounterBehaviourTest, ContextReportsTheCountAsPhase)
    {
        double phase = -1.0;
        node.on_increment([&phase](GeneratorContext& ctx) { phase = ctx.phase; });

        run(node, 3);

        EXPECT_DOUBLE_EQ(phase, 3.0);
    }

    TEST_F(GeneratorCounterBehaviourTest, ResetTriggerRestartsTheCountOnEachRisingEdge)
    {
        auto trigger = std::make_shared<Impulse>(12000.0F);
        Counter triggered { trigger, 100U, 1 };

        std::vector<uint32_t> counts;
        for (int n = 0; n < 9; ++n) {
            triggered.process_sample(0.0);
            counts.push_back(triggered.get_count());
        }

        EXPECT_EQ(counts, (std::vector<uint32_t> { 1, 2, 3, 4, 1, 2, 3, 4, 1 }));
    }

    TEST_F(GeneratorCounterBehaviourTest, ClearingTheTriggerStopsTheResets)
    {
        auto trigger = std::make_shared<Impulse>(12000.0F);
        Counter triggered { trigger, 100U, 1 };
        run(triggered, 2);

        triggered.clear_reset_trigger();
        run(triggered, 8);

        EXPECT_EQ(triggered.get_count(), 10U);
        EXPECT_TRUE(triggered.get_modulators().empty());
    }

    TEST_F(GeneratorCounterBehaviourTest, TriggerIsListedAsASignalModulator)
    {
        auto trigger = std::make_shared<Impulse>(1.0F);
        Counter triggered { trigger, 8U, 1 };

        const auto modulators = triggered.get_modulators();

        ASSERT_EQ(modulators.size(), 1U);
        EXPECT_EQ(modulators[0].first, ModulatorRole::SignalMod);
        EXPECT_EQ(modulators[0].second, trigger);
        EXPECT_TRUE(node.get_modulators().empty());
    }

    using RandomRange = std::tuple<double, double, double>;

    class GeneratorRandomRangeTest : public ::testing::TestWithParam<RandomRange> {
    protected:
        void SetUp() override
        {
            node.set_range(std::get<0>(GetParam()), std::get<1>(GetParam()));
            node.set_amplitude(std::get<2>(GetParam()));
        }

        [[nodiscard]] double low() const
        {
            return std::min(std::get<0>(GetParam()), std::get<1>(GetParam())) * std::get<2>(GetParam());
        }

        [[nodiscard]] double high() const
        {
            return std::max(std::get<0>(GetParam()), std::get<1>(GetParam())) * std::get<2>(GetParam());
        }

        Random node;
    };

    TEST_P(GeneratorRandomRangeTest, SamplesStayWithinTheScaledRange)
    {
        for (const double sample : run(node, 2000)) {
            EXPECT_GE(sample, low());
            EXPECT_LE(sample, high());
        }
    }

    TEST_P(GeneratorRandomRangeTest, BatchStaysWithinTheScaledRange)
    {
        const auto batch = node.process_batch(500);

        ASSERT_EQ(batch.size(), 500U);
        for (const double sample : batch) {
            EXPECT_GE(sample, low());
            EXPECT_LE(sample, high());
        }
    }

    TEST_P(GeneratorRandomRangeTest, MeanSitsNearTheCentreOfTheRange)
    {
        const auto samples = run(node, 4000);
        const double mean = std::accumulate(samples.begin(), samples.end(), 0.0) / static_cast<double>(samples.size());

        EXPECT_NEAR(mean, (low() + high()) * 0.5, (high() - low()) * 0.05 + 1e-9);
    }

    INSTANTIATE_TEST_SUITE_P(
        Ranges,
        GeneratorRandomRangeTest,
        ::testing::Values(
            RandomRange { -1.0, 1.0, 1.0 },
            RandomRange { 0.0, 1.0, 1.0 },
            RandomRange { 2.0, 5.0, 1.0 },
            RandomRange { -3.0, -1.0, 1.0 },
            RandomRange { -1.0, 1.0, 0.5 },
            RandomRange { 0.0, 1.0, 4.0 }));

    class GeneratorRandomBehaviourTest : public ::testing::Test {
    protected:
        Random node;
    };

    TEST_F(GeneratorRandomBehaviourTest, DefaultRangeIsMinusOneToOne)
    {
        for (const double sample : run(node, 1000)) {
            EXPECT_GE(sample, -1.0);
            EXPECT_LE(sample, 1.0);
        }
    }

    TEST_F(GeneratorRandomBehaviourTest, InputIsAddedToTheSample)
    {
        for (int i = 0; i < 200; ++i) {
            const double sample = node.process_sample(10.0);
            EXPECT_GE(sample, 9.0);
            EXPECT_LE(sample, 11.0);
        }
    }

    TEST_F(GeneratorRandomBehaviourTest, DegenerateRangeIsConstant)
    {
        node.set_range(0.5, 0.5);
        node.set_amplitude(2.0);

        for (const double sample : run(node, 50)) {
            EXPECT_DOUBLE_EQ(sample, 1.0);
        }
    }

    TEST_F(GeneratorRandomBehaviourTest, LastOutputIsTheReturnedSample)
    {
        const double sample = node.process_sample(0.0);

        EXPECT_DOUBLE_EQ(node.get_last_output(), sample);
    }

    TEST_F(GeneratorRandomBehaviourTest, SamplesAreNotConstant)
    {
        const auto samples = run(node, 100);

        EXPECT_GT(std::set<double>(samples.begin(), samples.end()).size(), 50U);
    }

    TEST_F(GeneratorRandomBehaviourTest, ContextReportsTypeRangeAndAmplitude)
    {
        node.set_range(-2.0, 3.0);
        node.set_amplitude(0.5);
        node.process_sample(0.0);

        const auto* context = dynamic_cast<const RandomContext*>(&node.get_last_context());

        ASSERT_NE(context, nullptr);
        EXPECT_EQ(context->distribution_type, Algorithm::UNIFORM);
        EXPECT_DOUBLE_EQ(context->range_start, -2.0);
        EXPECT_DOUBLE_EQ(context->range_end, 3.0);
        EXPECT_DOUBLE_EQ(context->amplitude, 0.5);
    }

    struct AlgorithmCase {
        const char* name;
        Algorithm algorithm;
    };

    class GeneratorRandomAlgorithmTest : public ::testing::TestWithParam<AlgorithmCase> { };

    TEST_P(GeneratorRandomAlgorithmTest, ConstructedAlgorithmProducesFiniteSamples)
    {
        Random node(GetParam().algorithm);

        for (const double sample : run(node, 512)) {
            EXPECT_TRUE(std::isfinite(sample));
        }
    }

    TEST_P(GeneratorRandomAlgorithmTest, SwitchingAlgorithmKeepsProducingFiniteSamples)
    {
        Random node;
        run(node, 16);

        node.set_type(GetParam().algorithm);

        for (const double sample : run(node, 512)) {
            EXPECT_TRUE(std::isfinite(sample));
        }
        node.process_sample(0.0);
        const auto* context = dynamic_cast<const RandomContext*>(&node.get_last_context());
        ASSERT_NE(context, nullptr);
        EXPECT_EQ(context->distribution_type, GetParam().algorithm);
    }

    TEST_P(GeneratorRandomAlgorithmTest, BatchHasTheRequestedLengthAndIsFinite)
    {
        Random node(GetParam().algorithm);

        const auto batch = node.process_batch(300);

        ASSERT_EQ(batch.size(), 300U);
        for (const double sample : batch) {
            EXPECT_TRUE(std::isfinite(sample));
        }
    }

    INSTANTIATE_TEST_SUITE_P(
        Algorithms,
        GeneratorRandomAlgorithmTest,
        ::testing::Values(
            AlgorithmCase { "Uniform", Algorithm::UNIFORM },
            AlgorithmCase { "Normal", Algorithm::NORMAL },
            AlgorithmCase { "Exponential", Algorithm::EXPONENTIAL },
            AlgorithmCase { "Poisson", Algorithm::POISSON },
            AlgorithmCase { "Perlin", Algorithm::PERLIN },
            AlgorithmCase { "Gendy", Algorithm::GENDY },
            AlgorithmCase { "Brownian", Algorithm::BROWNIAN },
            AlgorithmCase { "Pink", Algorithm::PINK },
            AlgorithmCase { "Blue", Algorithm::BLUE }),
        [](const ::testing::TestParamInfo<AlgorithmCase>& info) { return std::string(info.param.name); });

    using DeterministicFactory = std::function<std::shared_ptr<Node>()>;

    struct DeterministicCase {
        const char* name;
        DeterministicFactory make;
    };

    class GeneratorStateTest : public ::testing::TestWithParam<DeterministicCase> {
    protected:
        std::shared_ptr<Node> a { GetParam().make() };
        std::shared_ptr<Node> b { GetParam().make() };
    };

    TEST_P(GeneratorStateTest, RestoreRewindsToTheSave)
    {
        run(*a, 3);
        run(*b, 3);
        const auto reference = run(*b, 9);

        a->save_state();
        run(*a, 9);
        a->restore_state();

        EXPECT_EQ(run(*a, 9), reference);
    }

    TEST_P(GeneratorStateTest, SaveAndRestoreRepeatCleanly)
    {
        run(*b, 5);
        const auto reference = run(*b, 6);

        run(*a, 5);
        for (int round = 0; round < 3; ++round) {
            a->save_state();
            run(*a, 6);
            a->restore_state();
        }

        EXPECT_EQ(run(*a, 6), reference);
    }

    TEST_P(GeneratorStateTest, BatchMatchesConsecutiveSamples)
    {
        EXPECT_EQ(a->process_batch(24), run(*b, 24));
    }

    TEST_P(GeneratorStateTest, TicksFireOncePerSampleAndStopWhileSaved)
    {
        int ticks = 0;
        a->on_tick([&ticks](NodeContext&) { ++ticks; });

        run(*a, 5);
        EXPECT_EQ(ticks, 5);

        a->save_state();
        run(*a, 5);
        EXPECT_EQ(ticks, 5);

        a->restore_state();
        run(*a, 5);
        EXPECT_EQ(ticks, 10);
    }

    TEST_P(GeneratorStateTest, NetworkedNodeNeverFiresTicks)
    {
        int ticks = 0;
        a->on_tick([&ticks](NodeContext&) { ++ticks; });
        a->set_in_network(true);

        run(*a, 5);

        EXPECT_EQ(ticks, 0);
        EXPECT_TRUE(a->is_in_network());
    }

    INSTANTIATE_TEST_SUITE_P(
        Generators,
        GeneratorStateTest,
        ::testing::Values(
            DeterministicCase { "Sine", [] { return std::make_shared<Sine>(1000.0F, 0.5); } },
            DeterministicCase { "Phasor", [] { return std::make_shared<Phasor>(12000.0F); } },
            DeterministicCase { "Impulse", [] { return std::make_shared<Impulse>(12000.0F); } },
            DeterministicCase { "Counter", [] { return std::make_shared<Counter>(5U, 1); } }),
        [](const ::testing::TestParamInfo<DeterministicCase>& info) { return std::string(info.param.name); });

}
}
