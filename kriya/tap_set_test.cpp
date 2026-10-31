#include "gtest/gtest.h"

#include "MayaFlux/Buffers/AudioBuffer.hpp"
#include "MayaFlux/Buffers/BufferManager.hpp"
#include "MayaFlux/Kakshya/Source/DynamicSoundStream.hpp"
#include "MayaFlux/Kriya/TapSet.hpp"

namespace MayaFlux::Test {
namespace {

    using Kakshya::DynamicSoundStream;
    using Kakshya::OrganizationStrategy;
    using Kriya::SamplingPipeline;
    using Kriya::TapSet;
    using Kriya::TapSetBuilder;

    using Blocks = std::vector<std::vector<double>>;

    constexpr uint32_t k_rate = 48000;
    constexpr uint64_t k_stream_blocks = 4;

    double sample(uint64_t index, uint32_t channel = 0)
    {
        return 1.0 + static_cast<double>(index) + 10000.0 * static_cast<double>(channel);
    }

    std::vector<double> ramp_blocks(uint64_t first_block, uint64_t blocks, uint64_t block, uint32_t channel = 0)
    {
        std::vector<double> values;
        values.reserve(blocks * block);
        for (uint64_t i = first_block * block; i < (first_block + blocks) * block; ++i) {
            values.push_back(sample(i, channel));
        }
        return values;
    }

    std::vector<double> scaled(std::vector<double> values, double gain)
    {
        for (double& value : values) {
            value *= gain;
        }
        return values;
    }

    std::vector<double> sum(const std::vector<double>& a, const std::vector<double>& b)
    {
        std::vector<double> total(a.size());
        std::ranges::transform(a, b, total.begin(), std::plus<> {});
        return total;
    }

    void expect_near(const std::vector<double>& actual, const std::vector<double>& expected)
    {
        ASSERT_EQ(actual.size(), expected.size());
        for (size_t i = 0; i < actual.size(); ++i) {
            EXPECT_NEAR(actual[i], expected[i], 1e-6) << i;
        }
    }

    bool differs(const std::vector<double>& a, const std::vector<double>& b)
    {
        for (size_t i = 0; i < a.size(); ++i) {
            if (std::abs(a[i] - b[i]) > 1e-6) {
                return true;
            }
        }
        return false;
    }

    size_t first_sound(const Blocks& blocks)
    {
        for (size_t i = 0; i < blocks.size(); ++i) {
            if (std::ranges::any_of(blocks[i], [](double value) { return value != 0.0; })) {
                return i;
            }
        }
        return blocks.size();
    }

    class TapSetTest : public ::testing::Test {
    protected:
        void SetUp() override
        {
            scheduler = std::make_shared<Vruta::TaskScheduler>(k_rate);
            manager = std::make_shared<Buffers::BufferManager>();
            block = Buffers::s_preferred_buffer_size;
            stream = make_stream(1);
        }

        void TearDown() override
        {
            for (const auto& pipeline : pipelines) {
                pipeline->pipeline().end();
            }
            pipelines.clear();
            scheduler.reset();
        }

        [[nodiscard]] std::shared_ptr<DynamicSoundStream> make_stream(uint32_t channels) const
        {
            auto result = std::make_shared<DynamicSoundStream>(k_rate, channels);
            result->get_structure().organization = OrganizationStrategy::PLANAR;
            std::vector<std::vector<double>> columns;
            for (uint32_t c = 0; c < channels; ++c) {
                columns.push_back(ramp_blocks(0, k_stream_blocks, block, c));
            }
            std::vector<std::span<const double>> spans(columns.begin(), columns.end());
            result->write_frames(std::move(spans), 0);
            return result;
        }

        [[nodiscard]] std::shared_ptr<DynamicSoundStream> make_appended_stream() const
        {
            auto result = std::make_shared<DynamicSoundStream>(k_rate, 1);
            result->get_structure().organization = OrganizationStrategy::PLANAR;
            const auto ramp = ramp_blocks(0, k_stream_blocks, block);
            result->append_frames(ramp, 0);
            return result;
        }

        [[nodiscard]] TapSetBuilder builder(const std::shared_ptr<DynamicSoundStream>& source) const
        {
            return TapSetBuilder(source, *manager, *scheduler, static_cast<uint32_t>(block));
        }

        [[nodiscard]] TapSetBuilder builder() const
        {
            return builder(stream);
        }

        TapSet start(TapSetBuilder& taps)
        {
            TapSet started = taps.start();
            for (size_t tap = 0; tap < started.tap_count(); ++tap) {
                for (size_t output = 0;; ++output) {
                    try {
                        pipelines.push_back(started.sampler(tap, output));
                    } catch (const std::out_of_range&) {
                        break;
                    }
                }
            }
            return started;
        }

        void tick()
        {
            scheduler->process_buffer_cycle_tasks();
            scheduler->process_token(Vruta::ProcessingToken::SAMPLE_ACCURATE, block);
        }

        void tick(uint32_t count)
        {
            for (uint32_t i = 0; i < count; ++i) {
                tick();
            }
        }

        Blocks listen(const TapSet& taps, size_t tap, uint32_t ticks, size_t output = 0)
        {
            const auto sampler = taps.sampler(tap, output);
            Blocks heard;
            for (uint32_t i = 0; i < ticks; ++i) {
                tick();
                heard.push_back(sampler->get_buffer()->get_data());
            }
            return heard;
        }

        std::vector<Blocks> listen_to(const std::vector<std::shared_ptr<SamplingPipeline>>& samplers, uint32_t ticks)
        {
            std::vector<Blocks> heard(samplers.size());
            for (uint32_t i = 0; i < ticks; ++i) {
                tick();
                for (size_t s = 0; s < samplers.size(); ++s) {
                    heard[s].push_back(samplers[s]->get_buffer()->get_data());
                }
            }
            return heard;
        }

        [[nodiscard]] std::vector<double> silence() const
        {
            return std::vector<double>(block, 0.0);
        }

        [[nodiscard]] double blocks_in_seconds(double blocks) const
        {
            return blocks * static_cast<double>(block) / static_cast<double>(k_rate);
        }

        uint64_t block {};
        std::shared_ptr<Vruta::TaskScheduler> scheduler;
        std::shared_ptr<Buffers::BufferManager> manager;
        std::shared_ptr<DynamicSoundStream> stream;
        std::vector<std::shared_ptr<SamplingPipeline>> pipelines;
    };

    TEST_F(TapSetTest, ANullStreamGivesAnEmptySet)
    {
        auto taps = builder(nullptr);

        const TapSet started = taps.tap().start();

        EXPECT_EQ(started.tap_count(), 0U);
    }

    TEST_F(TapSetTest, ARegionTooShortGivesAnEmptySet)
    {
        auto taps = builder();

        const TapSet started = taps.tap().region(5, 6).start();

        EXPECT_EQ(started.tap_count(), 0U);
    }

    TEST_F(TapSetTest, StartingWithoutDeclaringATapMakesOneDefaultTap)
    {
        auto taps = builder();

        const TapSet started = start(taps);

        EXPECT_EQ(started.tap_count(), 1U);
    }

    TEST_F(TapSetTest, TapsAreNumberedInDeclarationOrder)
    {
        auto taps = builder().tap().tap().tap();

        const TapSet started = start(taps);

        EXPECT_EQ(started.tap_count(), 3U);
    }

    TEST_F(TapSetTest, ADefaultTapPlaysTheWholeStreamAtOnceAndLoops)
    {
        auto taps = builder().tap();
        const TapSet started = start(taps);

        const auto heard = listen(started, 0, k_stream_blocks + 2);

        ASSERT_EQ(heard.size(), k_stream_blocks + 2);
        for (size_t i = 0; i < heard.size(); ++i) {
            expect_near(heard[i], ramp_blocks(i % k_stream_blocks, 1, block));
        }
    }

    TEST_F(TapSetTest, WithLoopingOffATapPlaysTheRegionOnceThenFallsSilent)
    {
        auto taps = builder().loop(false).tap();
        const TapSet started = start(taps);

        const auto heard = listen(started, 0, k_stream_blocks + 2);

        for (size_t i = 0; i < k_stream_blocks; ++i) {
            expect_near(heard[i], ramp_blocks(i, 1, block));
        }
        EXPECT_EQ(heard[k_stream_blocks], silence());
        EXPECT_EQ(heard[k_stream_blocks + 1], silence());
    }

    TEST_F(TapSetTest, ADoubleSpeedTapReadsEveryOtherFrame)
    {
        auto taps = builder().tap().speed(2.0);
        const TapSet started = start(taps);

        const auto heard = listen(started, 0, 2);

        std::vector<double> first;
        std::vector<double> second;
        for (uint64_t n = 0; n < block; ++n) {
            first.push_back(sample(2 * n));
            second.push_back(sample(2 * (block + n)));
        }
        expect_near(heard[0], first);
        expect_near(heard[1], second);
    }

    TEST_F(TapSetTest, ABackwardTapReadsFromTheEndOfTheRegionDownwards)
    {
        auto taps = builder().tap().backward();
        const TapSet started = start(taps);

        const auto heard = listen(started, 0, 2);

        std::vector<double> first;
        std::vector<double> second;
        const uint64_t last = k_stream_blocks * block - 1;
        for (uint64_t n = 0; n < block; ++n) {
            first.push_back(sample(last - n));
            second.push_back(sample(last - block - n));
        }
        expect_near(heard[0], first);
        expect_near(heard[1], second);
    }

    TEST_F(TapSetTest, ANegativeSpeedIsTheSameAsBackward)
    {
        auto negative = builder().tap().speed(-1.0);
        auto backward = builder().tap().speed(1.0).backward();
        const TapSet first = start(negative);
        const TapSet second = start(backward);

        const auto a = listen_to({ first.sampler(0), second.sampler(0) }, 2);

        ASSERT_EQ(a.size(), 2U);
        for (size_t i = 0; i < 2; ++i) {
            expect_near(a[0][i], a[1][i]);
        }
        EXPECT_NE(a[0][0], silence());
    }

    TEST_F(TapSetTest, TheLevelOfATapScalesItsOutput)
    {
        auto taps = builder().tap().level(0.25);
        const TapSet started = start(taps);

        const auto heard = listen(started, 0, 2);

        expect_near(heard[0], scaled(ramp_blocks(0, 1, block), 0.25));
        expect_near(heard[1], scaled(ramp_blocks(1, 1, block), 0.25));
    }

    TEST_F(TapSetTest, ChangingTheLevelWhilePlayingTakesEffectOnTheNextBlock)
    {
        auto taps = builder().tap();
        TapSet started = start(taps);

        auto heard = listen(started, 0, 2);
        started.level(0, 0.5);
        const auto later = listen(started, 0, 2);

        expect_near(heard[1], ramp_blocks(1, 1, block));
        expect_near(later[0], scaled(ramp_blocks(2, 1, block), 0.5));
        expect_near(later[1], scaled(ramp_blocks(3, 1, block), 0.5));
    }

    TEST_F(TapSetTest, ZeroSpeedHoldsTheTapOnTheFrameItReached)
    {
        auto taps = builder().tap();
        TapSet started = start(taps);

        listen(started, 0, 2);
        started.speed(0, 0.0);
        const auto held = listen(started, 0, 3);

        for (const auto& heard : held) {
            expect_near(heard, std::vector<double>(block, sample(2 * block)));
        }
    }

    TEST_F(TapSetTest, ChangingTheSpeedCarriesOnFromWhereTheTapIs)
    {
        auto taps = builder().tap();
        TapSet started = start(taps);

        listen(started, 0, 2);
        started.speed(0, 2.0);
        const auto faster = listen(started, 0, 1);

        std::vector<double> expected;
        for (uint64_t n = 0; n < block; ++n) {
            expected.push_back(sample(2 * block + 2 * n));
        }
        expect_near(faster[0], expected);
    }

    TEST_F(TapSetTest, ANegativeSpeedWhilePlayingReversesFromWhereTheTapIs)
    {
        auto taps = builder().tap();
        TapSet started = start(taps);

        listen(started, 0, 2);
        started.speed(0, -1.0);
        const auto reversed = listen(started, 0, 1);

        std::vector<double> expected;
        for (uint64_t n = 0; n < block; ++n) {
            expected.push_back(sample(2 * block - n));
        }
        expect_near(reversed[0], expected);
    }

    TEST_F(TapSetTest, ATapEntersAfterItsDelayAndStartsFromTheBeginning)
    {
        constexpr uint32_t entry = 20;
        auto taps = builder().tap().tap().on_channel(1).enters_after(blocks_in_seconds(entry));
        const TapSet started = start(taps);

        const auto heard = listen_to({ started.sampler(0), started.sampler(1) }, entry + 8);

        EXPECT_EQ(first_sound(heard[0]), 0U);
        const size_t entered = first_sound(heard[1]);
        EXPECT_GE(entered, entry - 2);
        EXPECT_LE(entered, entry + 3);
        ASSERT_LT(entered + 1, heard[1].size());
        expect_near(heard[1][entered], ramp_blocks(0, 1, block));
        expect_near(heard[1][entered + 1], ramp_blocks(1, 1, block));
    }

    TEST_F(TapSetTest, EntriesHappenInTimeOrderWhateverTheDeclarationOrder)
    {
        constexpr uint32_t early = 10;
        constexpr uint32_t late = 30;
        auto taps = builder()
                        .tap().on_channel(0).enters_after(blocks_in_seconds(late))
                        .tap().on_channel(1).enters_after(blocks_in_seconds(early));
        const TapSet started = start(taps);

        const auto heard = listen_to({ started.sampler(0), started.sampler(1) }, late + 8);

        const size_t first = first_sound(heard[1]);
        const size_t second = first_sound(heard[0]);
        EXPECT_GE(first, early - 2);
        EXPECT_LE(first, early + 3);
        EXPECT_GE(second, late - 2);
        EXPECT_LE(second, late + 3);
    }

    TEST_F(TapSetTest, ALateTapJoinsAnEarlierOneOnTheSameChannel)
    {
        constexpr uint32_t entry = 10;
        auto taps = builder().tap().tap().enters_after(blocks_in_seconds(entry));
        const TapSet started = start(taps);

        const auto heard = listen(started, 0, entry + 8);

        size_t joined = heard.size();
        for (size_t i = 0; i < heard.size(); ++i) {
            if (differs(heard[i], ramp_blocks(i % k_stream_blocks, 1, block))) {
                joined = i;
                break;
            }
        }
        EXPECT_GE(joined, entry - 2);
        EXPECT_LE(joined, entry + 3);
        ASSERT_LT(joined + 1, heard.size());
        expect_near(heard[joined], sum(ramp_blocks(joined % k_stream_blocks, 1, block), ramp_blocks(0, 1, block)));
        expect_near(heard[joined + 1], sum(ramp_blocks((joined + 1) % k_stream_blocks, 1, block), ramp_blocks(1, 1, block)));
    }

    TEST_F(TapSetTest, TapsOnTheSameChannelShareOnePipelineAndSumTheirOutput)
    {
        auto taps = builder().tap().tap();
        const TapSet started = start(taps);

        const auto heard = listen(started, 1, 2);

        EXPECT_EQ(started.sampler(0), started.sampler(1));
        expect_near(heard[0], scaled(ramp_blocks(0, 1, block), 2.0));
        expect_near(heard[1], scaled(ramp_blocks(1, 1, block), 2.0));
    }

    TEST_F(TapSetTest, TapsOnDifferentChannelsHaveTheirOwnPipelines)
    {
        auto taps = builder().tap().on_channel(0).tap().on_channel(1);
        const TapSet started = start(taps);

        EXPECT_NE(started.sampler(0), started.sampler(1));
    }

    TEST_F(TapSetTest, AMonoSourceSoundsOnEveryOutputOfATap)
    {
        auto taps = builder().tap().on_channels({ 0, 1 });
        const TapSet started = start(taps);

        const auto heard = listen_to({ started.sampler(0, 0), started.sampler(0, 1) }, 2);

        for (size_t i = 0; i < 2; ++i) {
            expect_near(heard[0][i], ramp_blocks(i, 1, block));
            expect_near(heard[1][i], ramp_blocks(i, 1, block));
        }
    }

    TEST_F(TapSetTest, AStereoSourceKeepsLeftAndRightOnTheirOutputs)
    {
        const auto stereo = make_stream(2);
        auto taps = builder(stereo).tap().on_channels({ 0, 1 });
        const TapSet started = start(taps);

        const auto heard = listen_to({ started.sampler(0, 0), started.sampler(0, 1) }, 2);

        for (size_t i = 0; i < 2; ++i) {
            expect_near(heard[0][i], ramp_blocks(i, 1, block, 0));
            expect_near(heard[1][i], ramp_blocks(i, 1, block, 1));
        }
    }

    TEST_F(TapSetTest, AChosenSourceChannelPlaysOnEveryOutput)
    {
        const auto stereo = make_stream(2);
        auto taps = builder(stereo).tap().on_channels({ 0, 1 }).from_channel(1);
        const TapSet started = start(taps);

        const auto heard = listen_to({ started.sampler(0, 0), started.sampler(0, 1) }, 1);

        expect_near(heard[0][0], ramp_blocks(0, 1, block, 1));
        expect_near(heard[1][0], ramp_blocks(0, 1, block, 1));
    }

    TEST_F(TapSetTest, TheOutputsOfATapChangeSpeedTogether)
    {
        auto taps = builder().tap().on_channels({ 0, 1 });
        TapSet started = start(taps);
        const std::vector<std::shared_ptr<SamplingPipeline>> outputs { started.sampler(0, 0), started.sampler(0, 1) };

        listen_to(outputs, 2);
        started.speed(0, 0.0);
        const auto held = listen_to(outputs, 2);

        for (size_t i = 0; i < 2; ++i) {
            expect_near(held[0][i], std::vector<double>(block, sample(2 * block)));
            expect_near(held[1][i], std::vector<double>(block, sample(2 * block)));
        }
    }

    TEST_F(TapSetTest, ARegionRestrictsTheTapAndLoopsInsideIt)
    {
        auto taps = builder().tap().region(block, 3 * block);
        const TapSet started = start(taps);

        const auto heard = listen(started, 0, 4);

        expect_near(heard[0], ramp_blocks(1, 1, block));
        expect_near(heard[1], ramp_blocks(2, 1, block));
        expect_near(heard[2], ramp_blocks(1, 1, block));
        expect_near(heard[3], ramp_blocks(2, 1, block));
    }

    TEST_F(TapSetTest, ARegionPlayedOnceEndsOnItsLastFrame)
    {
        auto taps = builder().loop(false).tap().region(block, 3 * block);
        const TapSet started = start(taps);

        const auto heard = listen(started, 0, 4);

        expect_near(heard[0], ramp_blocks(1, 1, block));
        expect_near(heard[1], ramp_blocks(2, 1, block));
        EXPECT_EQ(heard[2], silence());
        EXPECT_EQ(heard[3], silence());
    }

    TEST_F(TapSetTest, ABackwardTapInARegionStartsAtTheRegionsLastFrame)
    {
        auto taps = builder().tap().backward().region(block, 3 * block);
        const TapSet started = start(taps);

        const auto heard = listen(started, 0, 1);

        std::vector<double> expected;
        for (uint64_t n = 0; n < block; ++n) {
            expected.push_back(sample(3 * block - 1 - n));
        }
        expect_near(heard[0], expected);
    }

    TEST_F(TapSetTest, ARegionPastTheEndOfTheStreamIsClampedToIt)
    {
        auto taps = builder().tap().region(0, 100 * block);
        const TapSet started = start(taps);

        const auto heard = listen(started, 0, k_stream_blocks + 1);

        expect_near(heard[k_stream_blocks - 1], ramp_blocks(k_stream_blocks - 1, 1, block));
        expect_near(heard[k_stream_blocks], ramp_blocks(0, 1, block));
    }

    TEST_F(TapSetTest, ARepeatingTapRestartsFromTheRegionStartEveryPeriod)
    {
        auto taps = builder().tap().repeat_every(blocks_in_seconds(2.0));
        const TapSet started = start(taps);

        const auto heard = listen(started, 0, 6);

        expect_near(heard[0], ramp_blocks(0, 1, block));
        expect_near(heard[1], ramp_blocks(1, 1, block));
        expect_near(heard[2], ramp_blocks(0, 1, block));
        expect_near(heard[3], ramp_blocks(1, 1, block));
        expect_near(heard[4], ramp_blocks(0, 1, block));
        expect_near(heard[5], ramp_blocks(1, 1, block));
    }

    TEST_F(TapSetTest, AShrinkingRepeatLengthShortensEachRestart)
    {
        auto taps = builder().tap().repeat_every(Kinesis::TimeMap { .fn = [this](const double& t) {
            return t < blocks_in_seconds(2.0) ? blocks_in_seconds(2.0) : blocks_in_seconds(1.0);
        } });
        const TapSet started = start(taps);

        const auto heard = listen(started, 0, 5);

        expect_near(heard[0], ramp_blocks(0, 1, block));
        expect_near(heard[1], ramp_blocks(1, 1, block));
        expect_near(heard[2], ramp_blocks(0, 1, block));
        expect_near(heard[3], ramp_blocks(0, 1, block));
        expect_near(heard[4], ramp_blocks(0, 1, block));
    }

    TEST_F(TapSetTest, ALaggedTapTrailsTheWriteHeadByTheGivenTime)
    {
        const auto ring = make_appended_stream();
        auto taps = builder(ring).tap().lag(blocks_in_seconds(3.0));
        const TapSet started = start(taps);

        const auto heard = listen(started, 0, 4);

        expect_near(heard[0], ramp_blocks(1, 1, block));
        expect_near(heard[1], ramp_blocks(2, 1, block));
        expect_near(heard[2], ramp_blocks(3, 1, block));
        expect_near(heard[3], ramp_blocks(0, 1, block));
    }

    TEST_F(TapSetTest, ALagShorterThanTwoBlocksIsRaisedToTwoBlocks)
    {
        const auto ring = make_appended_stream();
        auto taps = builder(ring).tap().lag(0.0);
        const TapSet started = start(taps);

        const auto heard = listen(started, 0, 3);

        expect_near(heard[0], ramp_blocks(2, 1, block));
        expect_near(heard[1], ramp_blocks(3, 1, block));
        expect_near(heard[2], ramp_blocks(0, 1, block));
    }

    TEST_F(TapSetTest, ALagFollowingATimeMapGlidesToTheNewLagAcrossTheBlockBeforeTheChange)
    {
        const auto ring = make_appended_stream();
        auto taps = builder(ring).tap().lag(Kinesis::TimeMap { .fn = [this](const double& t) {
            return t < blocks_in_seconds(1.0) ? blocks_in_seconds(3.0) : blocks_in_seconds(2.0);
        } });
        const TapSet started = start(taps);

        const auto heard = listen(started, 0, 3);

        std::vector<double> glide;
        for (uint64_t n = 0; n < block; ++n) {
            glide.push_back(sample(block + 2 * n));
        }
        expect_near(heard[0], glide);
        expect_near(heard[1], ramp_blocks(3, 1, block));
    }

    TEST_F(TapSetTest, SpeedHasNoEffectOnALaggedTap)
    {
        const auto ring = make_appended_stream();
        auto taps = builder(ring).tap().lag(blocks_in_seconds(3.0));
        TapSet started = start(taps);

        started.speed(0, 0.0);
        const auto heard = listen(started, 0, 2);

        expect_near(heard[0], ramp_blocks(1, 1, block));
        expect_near(heard[1], ramp_blocks(2, 1, block));
    }

    TEST_F(TapSetTest, StoppingSilencesEveryTapAtOnce)
    {
        auto taps = builder().tap().on_channel(0).tap().on_channel(1);
        TapSet started = start(taps);
        const std::vector<std::shared_ptr<SamplingPipeline>> outputs { started.sampler(0), started.sampler(1) };

        listen_to(outputs, 2);
        started.stop();
        const auto after = listen_to(outputs, 2);

        for (const auto& heard : after) {
            EXPECT_EQ(heard[0], silence());
            EXPECT_EQ(heard[1], silence());
        }
    }

    TEST_F(TapSetTest, StoppingCancelsTapsThatHaveNotEnteredYet)
    {
        constexpr uint32_t entry = 12;
        auto taps = builder().tap().on_channel(0).tap().on_channel(1).enters_after(blocks_in_seconds(entry));
        TapSet started = start(taps);
        const std::vector<std::shared_ptr<SamplingPipeline>> outputs { started.sampler(0), started.sampler(1) };

        listen_to(outputs, 3);
        started.stop();
        const auto after = listen_to(outputs, entry + 8);

        EXPECT_EQ(first_sound(after[1]), after[1].size());
    }

    TEST_F(TapSetTest, ACopyOfASetKeepsItsTapsPlayingAfterTheOriginalIsGone)
    {
        auto taps = builder().tap();
        std::optional<TapSet> original = start(taps);
        const TapSet copy = *original;

        listen(copy, 0, 1);
        original.reset();
        const auto heard = listen(copy, 0, 2);

        expect_near(heard[0], ramp_blocks(1, 1, block));
        expect_near(heard[1], ramp_blocks(2, 1, block));
    }

} // namespace
} // namespace MayaFlux::Test
