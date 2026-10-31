#include "gtest/gtest.h"

#include "MayaFlux/Buffers/AudioBuffer.hpp"
#include "MayaFlux/Buffers/BufferManager.hpp"
#include "MayaFlux/Kakshya/Source/DynamicSoundStream.hpp"
#include "MayaFlux/Kinesis/Tendency/TimeMap.hpp"
#include "MayaFlux/Kriya/SamplingPipeline.hpp"
#include "MayaFlux/Vruta/ChronUtils.hpp"

namespace MayaFlux::Test {
namespace {

    using Kakshya::DataVariant;
    using Kakshya::DynamicSoundStream;
    using Kakshya::OrganizationStrategy;
    using Kakshya::StreamSlice;
    using Kriya::SamplingPipeline;

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

    std::vector<double> values_of(const DataVariant& data)
    {
        const auto* values = std::get_if<std::vector<double>>(&data);
        return values ? *values : std::vector<double> {};
    }

    struct Rig {
        std::unique_ptr<SamplingPipeline> sampler;
        std::vector<std::vector<double>> blocks;
    };

    class SamplingPipelineTest : public ::testing::Test {
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
            for (const auto& rig : rigs) {
                if (rig->sampler) {
                    rig->sampler->pipeline().end();
                }
            }
            rigs.clear();
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

        Rig& make_rig(const std::shared_ptr<DynamicSoundStream>& source, uint32_t channel = 0)
        {
            auto rig = std::make_unique<Rig>();
            rig->sampler = std::make_unique<SamplingPipeline>(
                source, *manager, *scheduler, channel, static_cast<uint32_t>(block));
            Rig* raw = rig.get();
            rig->sampler->capture().on_data_ready([raw](DataVariant& data, uint32_t) {
                raw->blocks.push_back(values_of(data));
            });
            rigs.push_back(std::move(rig));
            return *rigs.back();
        }

        Rig& make_rig()
        {
            return make_rig(stream);
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

        [[nodiscard]] std::vector<double> silence() const
        {
            return std::vector<double>(block, 0.0);
        }

        uint64_t block {};
        std::shared_ptr<Vruta::TaskScheduler> scheduler;
        std::shared_ptr<Buffers::BufferManager> manager;
        std::shared_ptr<DynamicSoundStream> stream;
        std::vector<std::unique_ptr<Rig>> rigs;
    };

    TEST_F(SamplingPipelineTest, AnUnbuiltSamplerCapturesNothing)
    {
        auto& rig = make_rig();

        tick(3);

        EXPECT_TRUE(rig.blocks.empty());
        EXPECT_FALSE(rig.sampler->pipeline().is_running());
    }

    TEST_F(SamplingPipelineTest, SlicesFromTheStreamKnowTheirFrameRange)
    {
        auto& rig = make_rig();

        const StreamSlice whole = rig.sampler->slice_from_stream(3);
        const StreamSlice part = rig.sampler->slice_from_range(10, 20, 2);

        EXPECT_EQ(whole.start_frame(), 0U);
        EXPECT_EQ(whole.end_frame(), k_stream_blocks * block - 1);
        EXPECT_EQ(whole.index, 3U);
        EXPECT_EQ(part.start_frame(), 10U);
        EXPECT_EQ(part.end_frame(), 20U);
        EXPECT_EQ(part.index, 2U);
        EXPECT_EQ(rig.sampler->get_stream(), stream);
    }

    TEST_F(SamplingPipelineTest, LoadingStoresAnInactiveSliceAtTheIndex)
    {
        auto& rig = make_rig();
        StreamSlice slice = rig.sampler->slice_from_stream();
        slice.active = true;

        StreamSlice& stored = rig.sampler->load(2, slice);

        EXPECT_EQ(stored.index, 2U);
        EXPECT_FALSE(stored.active);
    }

    TEST_F(SamplingPipelineTest, PlayingASliceDeliversTheStreamBlockByBlock)
    {
        auto& rig = make_rig();

        rig.sampler->play(0, rig.sampler->slice_from_stream());
        tick(k_stream_blocks);

        ASSERT_EQ(rig.blocks.size(), k_stream_blocks);
        for (uint64_t i = 0; i < k_stream_blocks; ++i) {
            EXPECT_EQ(rig.blocks[i], ramp_blocks(i, 1, block)) << i;
        }
    }

    TEST_F(SamplingPipelineTest, AOneShotVoiceFallsSilentAndDeactivatesAtTheEnd)
    {
        auto& rig = make_rig();

        rig.sampler->play(0, rig.sampler->slice_from_stream());
        tick(k_stream_blocks + 2);

        ASSERT_EQ(rig.blocks.size(), k_stream_blocks + 2);
        EXPECT_EQ(rig.blocks[k_stream_blocks], silence());
        EXPECT_EQ(rig.blocks[k_stream_blocks + 1], silence());
        EXPECT_FALSE(rig.sampler->slice(0).active);
    }

    TEST_F(SamplingPipelineTest, AContinuousVoiceWrapsToTheStartOfItsSlice)
    {
        auto& rig = make_rig();

        rig.sampler->play_continuous(0, rig.sampler->slice_from_stream());
        tick(k_stream_blocks + 2);

        ASSERT_EQ(rig.blocks.size(), k_stream_blocks + 2);
        EXPECT_EQ(rig.blocks[k_stream_blocks], ramp_blocks(0, 1, block));
        EXPECT_EQ(rig.blocks[k_stream_blocks + 1], ramp_blocks(1, 1, block));
        EXPECT_TRUE(rig.sampler->slice(0).active);
        EXPECT_TRUE(rig.sampler->slice(0).looping);
    }

    TEST_F(SamplingPipelineTest, ContinuousPlaybackOfALoadedSliceTurnsLoopingOn)
    {
        auto& rig = make_rig();
        rig.sampler->build();
        rig.sampler->load(0, rig.sampler->slice_from_stream());

        rig.sampler->play_continuous(0);
        tick(k_stream_blocks + 1);

        ASSERT_EQ(rig.blocks.size(), k_stream_blocks + 1);
        EXPECT_EQ(rig.blocks[k_stream_blocks], ramp_blocks(0, 1, block));
    }

    TEST_F(SamplingPipelineTest, PlayingALoadedSliceByIndexStartsItFromTheBeginning)
    {
        auto& rig = make_rig();
        rig.sampler->build();
        rig.sampler->load(0, rig.sampler->slice_from_stream());

        rig.sampler->play(0);
        tick(2);

        ASSERT_EQ(rig.blocks.size(), 2U);
        EXPECT_EQ(rig.blocks[0], ramp_blocks(0, 1, block));
        EXPECT_EQ(rig.blocks[1], ramp_blocks(1, 1, block));
    }

    TEST_F(SamplingPipelineTest, ARangeSlicePlaysOnlyItsFrames)
    {
        auto& rig = make_rig();

        rig.sampler->play(0, rig.sampler->slice_from_range(block, 2 * block - 1));
        tick(3);

        ASSERT_EQ(rig.blocks.size(), 3U);
        EXPECT_EQ(rig.blocks[0], ramp_blocks(1, 1, block));
        EXPECT_EQ(rig.blocks[1], silence());
        EXPECT_EQ(rig.blocks[2], silence());
    }

    TEST_F(SamplingPipelineTest, VoicesOnOnePipelineAreSummed)
    {
        auto& rig = make_rig();

        rig.sampler->play(0, rig.sampler->slice_from_stream());
        rig.sampler->play(1, rig.sampler->slice_from_range(2 * block, 4 * block - 1));
        tick(2);

        ASSERT_EQ(rig.blocks.size(), 2U);
        EXPECT_EQ(rig.blocks[0], sum(ramp_blocks(0, 1, block), ramp_blocks(2, 1, block)));
        EXPECT_EQ(rig.blocks[1], sum(ramp_blocks(1, 1, block), ramp_blocks(3, 1, block)));
    }

    TEST_F(SamplingPipelineTest, TheScaleOfAVoiceMultipliesItsOutput)
    {
        auto& rig = make_rig();
        StreamSlice slice = rig.sampler->slice_from_stream();
        slice.scale = 0.25;

        rig.sampler->play(0, slice);
        tick(2);

        ASSERT_EQ(rig.blocks.size(), 2U);
        EXPECT_EQ(rig.blocks[0], scaled(ramp_blocks(0, 1, block), 0.25));
        EXPECT_EQ(rig.blocks[1], scaled(ramp_blocks(1, 1, block), 0.25));
    }

    TEST_F(SamplingPipelineTest, EditingTheScaleOfAPlayingVoiceTakesEffectOnTheNextBlock)
    {
        auto& rig = make_rig();

        rig.sampler->play_continuous(0, rig.sampler->slice_from_stream());
        tick(2);
        rig.sampler->slice(0).scale = 0.5;
        tick(2);

        ASSERT_EQ(rig.blocks.size(), 4U);
        EXPECT_EQ(rig.blocks[1], ramp_blocks(1, 1, block));
        EXPECT_EQ(rig.blocks[2], scaled(ramp_blocks(2, 1, block), 0.5));
        EXPECT_EQ(rig.blocks[3], scaled(ramp_blocks(3, 1, block), 0.5));
    }

    TEST_F(SamplingPipelineTest, AStoppedVoiceGoesSilentAtOnce)
    {
        auto& rig = make_rig();

        rig.sampler->play_continuous(0, rig.sampler->slice_from_stream());
        tick(2);
        rig.sampler->stop(0);
        tick(2);

        ASSERT_EQ(rig.blocks.size(), 4U);
        EXPECT_EQ(rig.blocks[1], ramp_blocks(1, 1, block));
        EXPECT_EQ(rig.blocks[2], silence());
        EXPECT_EQ(rig.blocks[3], silence());
        EXPECT_FALSE(rig.sampler->slice(0).active);
    }

    TEST_F(SamplingPipelineTest, StoppingOneVoiceLeavesTheOthersPlaying)
    {
        auto& rig = make_rig();

        rig.sampler->play_continuous(0, rig.sampler->slice_from_stream());
        rig.sampler->play_continuous(1, rig.sampler->slice_from_stream());
        tick(1);
        rig.sampler->stop(1);
        tick(1);

        ASSERT_EQ(rig.blocks.size(), 2U);
        EXPECT_EQ(rig.blocks[0], scaled(ramp_blocks(0, 1, block), 2.0));
        EXPECT_EQ(rig.blocks[1], ramp_blocks(1, 1, block));
    }

    TEST_F(SamplingPipelineTest, ReplayingAVoiceRestartsItFromTheBeginning)
    {
        auto& rig = make_rig();

        rig.sampler->play_continuous(0, rig.sampler->slice_from_stream());
        tick(2);
        rig.sampler->play(0);
        tick(1);

        ASSERT_EQ(rig.blocks.size(), 3U);
        EXPECT_EQ(rig.blocks[2], ramp_blocks(0, 1, block));
    }

    TEST_F(SamplingPipelineTest, IndicesPastTheLoadedVoicesAreRefusedWithoutEffect)
    {
        auto& rig = make_rig();
        rig.sampler->build();

        EXPECT_NO_THROW(rig.sampler->play(5));
        EXPECT_NO_THROW(rig.sampler->play_continuous(5));
        EXPECT_NO_THROW(rig.sampler->stop(5));
        tick(2);

        ASSERT_EQ(rig.blocks.size(), 2U);
        EXPECT_EQ(rig.blocks[0], silence());
        EXPECT_EQ(rig.blocks[1], silence());
    }

    TEST_F(SamplingPipelineTest, BuildingTwiceStartsOnlyOnePipeline)
    {
        auto& rig = make_rig();

        rig.sampler->build();
        rig.sampler->build();
        rig.sampler->play(0, rig.sampler->slice_from_stream());
        tick(3);

        EXPECT_EQ(rig.blocks.size(), 3U);
    }

    TEST_F(SamplingPipelineTest, AnIdleBuiltSamplerStillCapturesSilence)
    {
        auto& rig = make_rig();

        rig.sampler->build();
        tick(3);

        ASSERT_EQ(rig.blocks.size(), 3U);
        for (const auto& captured : rig.blocks) {
            EXPECT_EQ(captured, silence());
        }
    }

    TEST_F(SamplingPipelineTest, ABoundedBuildStopsAfterTheRequestedDuration)
    {
        auto& rig = make_rig();
        const uint64_t cycles = Vruta::seconds_to_blocks(0.1, k_rate, static_cast<uint32_t>(block));

        rig.sampler->build_for(100);
        rig.sampler->play_continuous(0, rig.sampler->slice_from_stream());
        tick(static_cast<uint32_t>(cycles) + 20);

        EXPECT_EQ(rig.blocks.size(), cycles);
        EXPECT_FALSE(rig.sampler->pipeline().is_running());
        EXPECT_EQ(rig.sampler->get_buffer()->get_data(), silence());
    }

    TEST_F(SamplingPipelineTest, RebuildingBeforeAnyBuildIsRefused)
    {
        auto& rig = make_rig();

        rig.sampler->rebuild_for(100);
        tick(3);

        EXPECT_TRUE(rig.blocks.empty());
        EXPECT_FALSE(rig.sampler->pipeline().is_running());
    }

    TEST_F(SamplingPipelineTest, EachSamplerPlaysTheStreamChannelOfItsOutput)
    {
        const auto stereo = make_stream(2);
        auto& left = make_rig(stereo, 0);
        auto& right = make_rig(stereo, 1);

        left.sampler->play(0, left.sampler->slice_from_stream());
        right.sampler->play(0, right.sampler->slice_from_stream());
        tick(2);

        ASSERT_EQ(left.blocks.size(), 2U);
        ASSERT_EQ(right.blocks.size(), 2U);
        EXPECT_EQ(left.blocks[0], ramp_blocks(0, 1, block, 0));
        EXPECT_EQ(right.blocks[0], ramp_blocks(0, 1, block, 1));
        EXPECT_EQ(left.blocks[1], ramp_blocks(1, 1, block, 0));
        EXPECT_EQ(right.blocks[1], ramp_blocks(1, 1, block, 1));
    }

    TEST_F(SamplingPipelineTest, ASourceChannelOverridesTheOutputChannel)
    {
        const auto stereo = make_stream(2);
        auto& right = make_rig(stereo, 1);
        StreamSlice slice = right.sampler->slice_from_stream();
        slice.source_channel = 0;

        right.sampler->play(0, slice);
        tick(1);

        ASSERT_EQ(right.blocks.size(), 1U);
        EXPECT_EQ(right.blocks[0], ramp_blocks(0, 1, block, 0));
    }

    TEST_F(SamplingPipelineTest, SamplersOnTheSameStreamKeepIndependentCursors)
    {
        auto& first = make_rig();
        auto& second = make_rig();

        first.sampler->play_continuous(0, first.sampler->slice_from_stream());
        tick(2);
        second.sampler->play_continuous(0, second.sampler->slice_from_stream());
        tick(1);

        ASSERT_EQ(first.blocks.size(), 3U);
        ASSERT_EQ(second.blocks.size(), 1U);
        EXPECT_EQ(first.blocks[2], ramp_blocks(2, 1, block));
        EXPECT_EQ(second.blocks[0], ramp_blocks(0, 1, block));
    }

    TEST_F(SamplingPipelineTest, ATimeMapDrivesTheVoiceThroughTheStream)
    {
        auto& rig = make_rig();
        StreamSlice slice = rig.sampler->slice_from_stream();
        slice.time_map = std::make_shared<const Kinesis::TimeMap>(
            Kinesis::TimeMaps::quadratic(0.0, static_cast<double>(k_rate), 0.0));

        rig.sampler->play(0, slice);
        tick(k_stream_blocks + 1);

        ASSERT_EQ(rig.blocks.size(), k_stream_blocks + 1);
        for (uint64_t i = 0; i < k_stream_blocks; ++i) {
            expect_near(rig.blocks[i], ramp_blocks(i, 1, block));
        }
        EXPECT_EQ(rig.blocks[k_stream_blocks], silence());
        EXPECT_FALSE(rig.sampler->slice(0).active);
    }

    TEST_F(SamplingPipelineTest, AStationaryTimeMapHoldsOneFrame)
    {
        auto& rig = make_rig();
        StreamSlice slice = rig.sampler->slice_from_stream();
        slice.time_map = std::make_shared<const Kinesis::TimeMap>(
            Kinesis::TimeMaps::quadratic(700.0, 0.0, 0.0));

        rig.sampler->play(0, slice);
        tick(3);

        ASSERT_EQ(rig.blocks.size(), 3U);
        for (const auto& captured : rig.blocks) {
            expect_near(captured, std::vector<double>(block, sample(700)));
        }
        EXPECT_TRUE(rig.sampler->slice(0).active);
    }

    TEST_F(SamplingPipelineTest, ASlowerTimeMapReadsBetweenFrames)
    {
        auto& rig = make_rig();
        StreamSlice slice = rig.sampler->slice_from_stream();
        slice.time_map = std::make_shared<const Kinesis::TimeMap>(
            Kinesis::TimeMaps::quadratic(0.0, static_cast<double>(k_rate) / 2.0, 0.0));

        rig.sampler->play(0, slice);
        tick(1);

        ASSERT_EQ(rig.blocks.size(), 1U);
        std::vector<double> expected;
        for (uint64_t n = 0; n < block; ++n) {
            expected.push_back(1.0 + 0.5 * static_cast<double>(n));
        }
        expect_near(rig.blocks[0], expected);
    }

    TEST_F(SamplingPipelineTest, AVoiceCanBeRetriggeredAfterItsOneShotEnded)
    {
        auto& rig = make_rig();

        rig.sampler->play(0, rig.sampler->slice_from_stream());
        tick(k_stream_blocks + 2);
        rig.sampler->play(0);
        tick(2);

        ASSERT_EQ(rig.blocks.size(), k_stream_blocks + 4);
        EXPECT_EQ(rig.blocks[k_stream_blocks + 1], silence());
        EXPECT_EQ(rig.blocks[k_stream_blocks + 2], ramp_blocks(0, 1, block));
        EXPECT_EQ(rig.blocks[k_stream_blocks + 3], ramp_blocks(1, 1, block));
    }

    TEST_F(SamplingPipelineTest, AVoiceStartedLaterJoinsWithoutDisturbingTheOnesPlaying)
    {
        auto& rig = make_rig();

        rig.sampler->play_continuous(0, rig.sampler->slice_from_stream());
        tick(2);
        rig.sampler->play(1, rig.sampler->slice_from_range(3 * block, 4 * block - 1));
        tick(2);

        ASSERT_EQ(rig.blocks.size(), 4U);
        EXPECT_EQ(rig.blocks[1], ramp_blocks(1, 1, block));
        EXPECT_EQ(rig.blocks[2], sum(ramp_blocks(2, 1, block), ramp_blocks(3, 1, block)));
        EXPECT_EQ(rig.blocks[3], ramp_blocks(3, 1, block));
    }

    TEST_F(SamplingPipelineTest, ALoopCountEndsTheVoiceAfterThatManyPasses)
    {
        auto& rig = make_rig();
        StreamSlice slice = rig.sampler->slice_from_stream();
        slice.loop_count = 2;

        rig.sampler->play_continuous(0, slice);
        tick(2 * k_stream_blocks + 2);

        ASSERT_EQ(rig.blocks.size(), 2 * k_stream_blocks + 2);
        for (uint64_t i = 0; i < 2 * k_stream_blocks; ++i) {
            EXPECT_EQ(rig.blocks[i], ramp_blocks(i % k_stream_blocks, 1, block)) << i;
        }
        EXPECT_EQ(rig.blocks[2 * k_stream_blocks], silence());
        EXPECT_EQ(rig.blocks[2 * k_stream_blocks + 1], silence());
        EXPECT_FALSE(rig.sampler->slice(0).active);
    }

    TEST_F(SamplingPipelineTest, AReversedTimeMapPlaysTheStreamBackwards)
    {
        auto& rig = make_rig();
        const double last = static_cast<double>(k_stream_blocks * block - 1);
        StreamSlice slice = rig.sampler->slice_from_stream();
        slice.time_map = std::make_shared<const Kinesis::TimeMap>(
            Kinesis::TimeMaps::quadratic(last, -static_cast<double>(k_rate), 0.0));

        rig.sampler->play(0, slice);
        tick(2);

        ASSERT_EQ(rig.blocks.size(), 2U);
        std::vector<double> expected;
        for (uint64_t n = 0; n < 2 * block; ++n) {
            expected.push_back(sample(k_stream_blocks * block - 1 - n));
        }
        expect_near(rig.blocks[0], std::vector<double>(expected.begin(), expected.begin() + static_cast<std::ptrdiff_t>(block)));
        expect_near(rig.blocks[1], std::vector<double>(expected.begin() + static_cast<std::ptrdiff_t>(block), expected.end()));
    }

    TEST_F(SamplingPipelineTest, ALoopingTimeMapWrapsAroundTheSlice)
    {
        auto& rig = make_rig();
        StreamSlice slice = rig.sampler->slice_from_stream();
        slice.looping = true;
        slice.time_map = std::make_shared<const Kinesis::TimeMap>(
            Kinesis::TimeMaps::quadratic(0.0, static_cast<double>(k_rate), 0.0));

        rig.sampler->play(0, slice);
        tick(k_stream_blocks + 2);

        ASSERT_EQ(rig.blocks.size(), k_stream_blocks + 2);
        expect_near(rig.blocks[k_stream_blocks], ramp_blocks(0, 1, block));
        expect_near(rig.blocks[k_stream_blocks + 1], ramp_blocks(1, 1, block));
        EXPECT_TRUE(rig.sampler->slice(0).active);
    }

    TEST_F(SamplingPipelineTest, SwappingTheTimeMapOfAPlayingVoiceRestartsItsClock)
    {
        auto& rig = make_rig();
        const auto forward = [] {
            return std::make_shared<const Kinesis::TimeMap>(
                Kinesis::TimeMaps::quadratic(0.0, static_cast<double>(k_rate), 0.0));
        };
        StreamSlice slice = rig.sampler->slice_from_stream();
        slice.time_map = forward();

        rig.sampler->play(0, slice);
        tick(2);
        rig.sampler->slice(0).time_map = forward();
        tick(1);

        ASSERT_EQ(rig.blocks.size(), 3U);
        expect_near(rig.blocks[1], ramp_blocks(1, 1, block));
        expect_near(rig.blocks[2], ramp_blocks(0, 1, block));
    }

    TEST_F(SamplingPipelineTest, ReplayingATimeMappedVoiceRestartsItsClock)
    {
        auto& rig = make_rig();
        StreamSlice slice = rig.sampler->slice_from_stream();
        slice.time_map = std::make_shared<const Kinesis::TimeMap>(
            Kinesis::TimeMaps::quadratic(0.0, static_cast<double>(k_rate), 0.0));

        rig.sampler->play(0, slice);
        tick(2);
        rig.sampler->play(0);
        tick(1);

        ASSERT_EQ(rig.blocks.size(), 3U);
        expect_near(rig.blocks[2], ramp_blocks(0, 1, block));
    }

    TEST_F(SamplingPipelineTest, TriggeringAPlayingVoiceRestartsItFromTheStartOfItsSlice)
    {
        auto& rig = make_rig();

        rig.sampler->play_continuous(0, rig.sampler->slice_from_stream());
        tick(2);
        rig.sampler->trigger(0);
        tick(2);

        ASSERT_EQ(rig.blocks.size(), 4U);
        EXPECT_EQ(rig.blocks[2], ramp_blocks(0, 1, block));
        EXPECT_EQ(rig.blocks[3], ramp_blocks(1, 1, block));
    }

    TEST_F(SamplingPipelineTest, TriggeringALoadedIdleVoiceStartsIt)
    {
        auto& rig = make_rig();
        rig.sampler->build();
        rig.sampler->load(0, rig.sampler->slice_from_stream());

        tick(1);
        rig.sampler->trigger(0);
        tick(2);

        ASSERT_EQ(rig.blocks.size(), 3U);
        EXPECT_EQ(rig.blocks[0], silence());
        EXPECT_EQ(rig.blocks[1], ramp_blocks(0, 1, block));
        EXPECT_EQ(rig.blocks[2], ramp_blocks(1, 1, block));
    }

    TEST_F(SamplingPipelineTest, ADifferentVoiceCanBeTriggeredAgainAfterItsOneShotEnded)
    {
        auto& rig = make_rig();

        rig.sampler->play(0, rig.sampler->slice_from_stream());
        tick(k_stream_blocks + 1);
        rig.sampler->trigger(0);
        tick(2);

        ASSERT_EQ(rig.blocks.size(), k_stream_blocks + 3);
        EXPECT_EQ(rig.blocks[k_stream_blocks], silence());
        EXPECT_EQ(rig.blocks[k_stream_blocks + 1], ramp_blocks(0, 1, block));
        EXPECT_EQ(rig.blocks[k_stream_blocks + 2], ramp_blocks(1, 1, block));
    }

    TEST_F(SamplingPipelineTest, TriggeringPicksUpTheRegionAndScaleEditedBeforehand)
    {
        auto& rig = make_rig();

        rig.sampler->play(0, rig.sampler->slice_from_stream());
        tick(k_stream_blocks + 1);
        rig.sampler->slice(0).region = rig.sampler->slice_from_range(2 * block, 4 * block - 1).region;
        rig.sampler->slice(0).scale = 0.5;
        rig.sampler->trigger(0);
        tick(3);

        ASSERT_EQ(rig.blocks.size(), k_stream_blocks + 4);
        EXPECT_EQ(rig.blocks[k_stream_blocks + 1], scaled(ramp_blocks(2, 1, block), 0.5));
        EXPECT_EQ(rig.blocks[k_stream_blocks + 2], scaled(ramp_blocks(3, 1, block), 0.5));
        EXPECT_EQ(rig.blocks[k_stream_blocks + 3], silence());
    }

    TEST_F(SamplingPipelineTest, TriggeringATimeMappedVoiceRestartsItsClock)
    {
        auto& rig = make_rig();
        StreamSlice slice = rig.sampler->slice_from_stream();
        slice.time_map = std::make_shared<const Kinesis::TimeMap>(
            Kinesis::TimeMaps::quadratic(0.0, static_cast<double>(k_rate), 0.0));

        rig.sampler->play(0, slice);
        tick(2);
        rig.sampler->trigger(0);
        tick(1);

        ASSERT_EQ(rig.blocks.size(), 3U);
        expect_near(rig.blocks[2], ramp_blocks(0, 1, block));
    }

    TEST_F(SamplingPipelineTest, TriggeringAnIndexThatWasNeverLoadedIsRefusedWithoutEffect)
    {
        auto& rig = make_rig();
        rig.sampler->build();

        EXPECT_NO_THROW(rig.sampler->trigger(3));
        tick(2);

        ASSERT_EQ(rig.blocks.size(), 2U);
        EXPECT_EQ(rig.blocks[0], silence());
        EXPECT_EQ(rig.blocks[1], silence());
    }

    TEST_F(SamplingPipelineTest, AnEnvelopeScalesEachOutputSampleByItsGainAtThatTime)
    {
        auto& rig = make_rig();
        StreamSlice slice = rig.sampler->slice_from_stream();
        slice.with_envelope(Kinesis::TimeMaps::linear(0.0, 1.0, static_cast<double>(block) / k_rate));

        rig.sampler->play(0, slice);
        tick(2);

        ASSERT_EQ(rig.blocks.size(), 2U);
        std::vector<double> expected;
        for (uint64_t n = 0; n < block; ++n) {
            expected.push_back(sample(n) * static_cast<double>(n) / static_cast<double>(block));
        }
        expect_near(rig.blocks[0], expected);
        expect_near(rig.blocks[1], ramp_blocks(1, 1, block));
    }

    TEST_F(SamplingPipelineTest, AnEnvelopeComposesWithTheScaleOfTheVoice)
    {
        auto& rig = make_rig();
        StreamSlice slice = rig.sampler->slice_from_stream();
        slice.scale = 0.5;
        slice.with_envelope(Kinesis::TimeMaps::linear(0.0, 1.0, static_cast<double>(block) / k_rate));

        rig.sampler->play(0, slice);
        tick(2);

        ASSERT_EQ(rig.blocks.size(), 2U);
        expect_near(rig.blocks[1], scaled(ramp_blocks(1, 1, block), 0.5));
    }

    TEST_F(SamplingPipelineTest, AnEnvelopeIsMeasuredFromTheTriggerNotFromTheStreamPosition)
    {
        auto& rig = make_rig();
        StreamSlice slice = rig.sampler->slice_from_stream();
        slice.with_envelope(Kinesis::TimeMaps::linear(0.0, 1.0, static_cast<double>(block) / k_rate));

        rig.sampler->play_continuous(0, slice);
        tick(3);
        rig.sampler->trigger(0);
        tick(1);

        ASSERT_EQ(rig.blocks.size(), 4U);
        std::vector<double> expected;
        for (uint64_t n = 0; n < block; ++n) {
            expected.push_back(sample(n) * static_cast<double>(n) / static_cast<double>(block));
        }
        expect_near(rig.blocks[3], expected);
    }

    TEST_F(SamplingPipelineTest, AnEnvelopeIsNotStretchedByATimeMapOnTheSamePosition)
    {
        auto& rig = make_rig();
        StreamSlice slice = rig.sampler->slice_from_stream();
        slice.time_map = std::make_shared<const Kinesis::TimeMap>(
            Kinesis::TimeMaps::quadratic(0.0, 2.0 * static_cast<double>(k_rate), 0.0));
        slice.with_envelope(Kinesis::TimeMaps::linear(0.0, 1.0, static_cast<double>(block) / k_rate));

        rig.sampler->play(0, slice);
        tick(2);

        ASSERT_EQ(rig.blocks.size(), 2U);
        std::vector<double> expected;
        for (uint64_t n = 0; n < block; ++n) {
            expected.push_back(sample(2 * n) * static_cast<double>(n) / static_cast<double>(block));
        }
        expect_near(rig.blocks[0], expected);
        for (uint64_t n = 0; n < block; ++n) {
            EXPECT_NEAR(rig.blocks[1][n], sample(2 * (block + n)), 1e-6) << n;
        }
    }

    TEST_F(SamplingPipelineTest, ClearingTheEnvelopeOfAPlayingVoiceRestoresFullLevel)
    {
        auto& rig = make_rig();
        StreamSlice slice = rig.sampler->slice_from_stream();
        slice.with_envelope(Kinesis::TimeMap { .fn = [](const double&) { return 0.0; } });

        rig.sampler->play_continuous(0, slice);
        tick(2);
        rig.sampler->slice(0).envelope = nullptr;
        tick(1);

        ASSERT_EQ(rig.blocks.size(), 3U);
        EXPECT_EQ(rig.blocks[0], silence());
        EXPECT_EQ(rig.blocks[1], silence());
        EXPECT_EQ(rig.blocks[2], ramp_blocks(2, 1, block));
    }

    TEST_F(SamplingPipelineTest, AnOffsetTriggerSoundsThatManyFramesIntoTheNextBlock)
    {
        constexpr uint32_t offset = 100;
        auto& rig = make_rig();
        rig.sampler->build();
        rig.sampler->load(0, rig.sampler->slice_from_stream());

        rig.sampler->trigger(0, offset);
        tick(2);

        ASSERT_EQ(rig.blocks.size(), 2U);
        std::vector<double> first(block, 0.0);
        std::vector<double> second;
        for (uint64_t n = offset; n < block; ++n) {
            first[n] = sample(n - offset);
        }
        for (uint64_t n = 0; n < block; ++n) {
            second.push_back(sample(block - offset + n));
        }
        EXPECT_EQ(rig.blocks[0], first);
        EXPECT_EQ(rig.blocks[1], second);
    }

    TEST_F(SamplingPipelineTest, AnOffsetPastOneBlockWaitsWholeBlocksFirst)
    {
        constexpr uint32_t offset = 10;
        auto& rig = make_rig();
        rig.sampler->build();
        rig.sampler->load(0, rig.sampler->slice_from_stream());

        rig.sampler->trigger(0, static_cast<uint32_t>(block) + offset);
        tick(1);

        EXPECT_TRUE(rig.sampler->slice(0).active);
        tick(2);

        ASSERT_EQ(rig.blocks.size(), 3U);
        EXPECT_EQ(rig.blocks[0], silence());
        for (uint64_t n = 0; n < block; ++n) {
            EXPECT_EQ(rig.blocks[1][n], n < offset ? 0.0 : sample(n - offset)) << n;
        }
    }

    TEST_F(SamplingPipelineTest, ATriggerWithoutAnOffsetStillStartsOnTheBlockEdge)
    {
        auto& rig = make_rig();
        rig.sampler->build();
        rig.sampler->load(0, rig.sampler->slice_from_stream());

        rig.sampler->trigger(0, 0);
        tick(1);

        ASSERT_EQ(rig.blocks.size(), 1U);
        EXPECT_EQ(rig.blocks[0], ramp_blocks(0, 1, block));
    }

    TEST_F(SamplingPipelineTest, AnOffsetTimeMappedVoiceStartsItsClockOnTheOffsetFrame)
    {
        constexpr uint32_t offset = 100;
        auto& rig = make_rig();
        StreamSlice slice = rig.sampler->slice_from_stream();
        slice.time_map = std::make_shared<const Kinesis::TimeMap>(
            Kinesis::TimeMaps::quadratic(0.0, static_cast<double>(k_rate), 0.0));
        rig.sampler->build();
        rig.sampler->load(0, slice);

        rig.sampler->trigger(0, offset);
        tick(2);

        ASSERT_EQ(rig.blocks.size(), 2U);
        std::vector<double> first(block, 0.0);
        std::vector<double> second;
        for (uint64_t n = offset; n < block; ++n) {
            first[n] = sample(n - offset);
        }
        for (uint64_t n = 0; n < block; ++n) {
            second.push_back(sample(block - offset + n));
        }
        expect_near(rig.blocks[0], first);
        expect_near(rig.blocks[1], second);
    }

    TEST_F(SamplingPipelineTest, AnOffsetVoiceStartsItsEnvelopeOnTheOffsetFrame)
    {
        constexpr uint32_t offset = 100;
        auto& rig = make_rig();
        StreamSlice slice = rig.sampler->slice_from_stream();
        slice.with_envelope(Kinesis::TimeMaps::linear(0.0, 1.0, static_cast<double>(block) / k_rate));
        rig.sampler->build();
        rig.sampler->load(0, slice);

        rig.sampler->trigger(0, offset);
        tick(1);

        ASSERT_EQ(rig.blocks.size(), 1U);
        std::vector<double> expected(block, 0.0);
        for (uint64_t n = offset; n < block; ++n) {
            expected[n] = sample(n - offset) * static_cast<double>(n - offset) / static_cast<double>(block);
        }
        expect_near(rig.blocks[0], expected);
    }

    TEST_F(SamplingPipelineTest, ASamplersOutputReachesItsChannelAndLeavesWithIt)
    {
        auto& rig = make_rig();
        StreamSlice slice = rig.sampler->slice_from_stream();
        slice.scale = 1e-6;
        const auto heard = [this](uint32_t channel) {
            const auto& data = manager->get_buffer_data(Buffers::ProcessingToken::AUDIO_BACKEND, channel);
            return std::ranges::any_of(data, [](double value) { return value != 0.0; });
        };

        rig.sampler->play_continuous(0, slice);
        tick(1);
        manager->process_token(Buffers::ProcessingToken::AUDIO_BACKEND, 1);

        EXPECT_TRUE(heard(0));
        EXPECT_FALSE(heard(1));

        rig.sampler->pipeline().end();
        rig.sampler.reset();
        tick(1);
        manager->process_token(Buffers::ProcessingToken::AUDIO_BACKEND, 1);

        EXPECT_FALSE(heard(0));
    }

} // namespace
} // namespace MayaFlux::Test
