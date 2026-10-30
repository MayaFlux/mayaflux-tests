#include "gtest/gtest.h"

#include "MayaFlux/Kakshya/Processors/CursorAccessProcessor.hpp"
#include "MayaFlux/Kakshya/Source/DynamicSoundStream.hpp"

namespace MayaFlux::Test {
namespace {

    using Kakshya::CursorAccessProcessor;
    using Kakshya::DataVariant;
    using Kakshya::DynamicSoundStream;
    using Kakshya::OrganizationStrategy;
    using Kakshya::SoundStreamContainer;

    constexpr uint32_t k_rate = 1000;
    constexpr uint32_t k_channels = 2;
    constexpr uint64_t k_frames = 16;
    constexpr uint64_t k_block = 4;

    double sample(uint64_t frame, uint32_t channel)
    {
        return 1.0 + 10.0 * static_cast<double>(frame) + static_cast<double>(channel);
    }

    std::vector<double> ramp(uint32_t channel, uint64_t first, uint64_t count)
    {
        std::vector<double> values;
        for (uint64_t f = first; f < first + count; ++f) {
            values.push_back(sample(f, channel));
        }
        return values;
    }

    std::shared_ptr<DynamicSoundStream> make_stream(OrganizationStrategy organization, uint64_t frames = k_frames)
    {
        auto stream = std::make_shared<DynamicSoundStream>(k_rate, k_channels);
        stream->get_structure().organization = organization;

        if (organization == OrganizationStrategy::INTERLEAVED) {
            std::vector<double> flat;
            for (uint64_t f = 0; f < frames; ++f) {
                for (uint32_t c = 0; c < k_channels; ++c) {
                    flat.push_back(sample(f, c));
                }
            }
            stream->write_frames(std::vector<std::span<const double>> { std::span<const double>(flat) }, 0);
        } else {
            std::vector<std::vector<double>> columns;
            for (uint32_t c = 0; c < k_channels; ++c) {
                columns.push_back(ramp(c, 0, frames));
            }
            std::vector<std::span<const double>> spans(columns.begin(), columns.end());
            stream->write_frames(std::move(spans), 0);
        }
        return stream;
    }

    std::vector<double> channel_of(const std::vector<DataVariant>& pd, OrganizationStrategy organization, uint32_t channel)
    {
        if (organization == OrganizationStrategy::INTERLEAVED) {
            const auto& flat = std::get<std::vector<double>>(pd.at(0));
            std::vector<double> values;
            for (size_t f = 0; f * k_channels + channel < flat.size(); ++f) {
                values.push_back(flat[f * k_channels + channel]);
            }
            return values;
        }
        return std::get<std::vector<double>>(pd.at(channel));
    }

    class CursorAccessProcessorTest : public ::testing::TestWithParam<OrganizationStrategy> {
    protected:
        void SetUp() override
        {
            stream = make_stream(GetParam());
            processor = std::make_shared<CursorAccessProcessor>(k_block);
        }

        void attach()
        {
            processor->on_attach(stream);
        }

        std::vector<double> run(uint32_t channel = 0)
        {
            processor->process(stream);
            return channel_of(stream->get_dynamic_data(processor->get_slot_index()), GetParam(), channel);
        }

        std::shared_ptr<DynamicSoundStream> stream;
        std::shared_ptr<CursorAccessProcessor> processor;
    };

    INSTANTIATE_TEST_SUITE_P(Organizations, CursorAccessProcessorTest,
        ::testing::Values(OrganizationStrategy::INTERLEAVED, OrganizationStrategy::PLANAR),
        [](const ::testing::TestParamInfo<OrganizationStrategy>& info) {
            return std::string(info.param == OrganizationStrategy::INTERLEAVED ? "Interleaved" : "Planar");
        });

    TEST_P(CursorAccessProcessorTest, StartsInactiveWithoutASlot)
    {
        EXPECT_FALSE(processor->is_active());
        EXPECT_FALSE(processor->is_processing());
        EXPECT_EQ(processor->get_slot_index(), std::numeric_limits<uint32_t>::max());
        EXPECT_EQ(processor->loop_count(), 0U);
    }

    TEST_P(CursorAccessProcessorTest, AttachingAllocatesADynamicSlotAndDetachingReleasesIt)
    {
        attach();

        const uint32_t slot = processor->get_slot_index();
        EXPECT_NE(slot, std::numeric_limits<uint32_t>::max());
        EXPECT_NO_THROW((void)stream->get_dynamic_data(slot));

        processor->on_detach(stream);

        EXPECT_EQ(stream->allocate_dynamic_slot(), slot);
    }

    TEST_P(CursorAccessProcessorTest, TwoProcessorsOnOneStreamGetIndependentSlots)
    {
        const auto other = std::make_shared<CursorAccessProcessor>(k_block);

        attach();
        other->on_attach(stream);

        EXPECT_NE(processor->get_slot_index(), other->get_slot_index());
    }

    TEST_P(CursorAccessProcessorTest, AttachingToAnotherContainerKindIsRefused)
    {
        const auto plain = std::make_shared<SoundStreamContainer>(k_rate, k_channels);

        EXPECT_NO_THROW(processor->on_attach(plain));
        EXPECT_NO_THROW(processor->process(plain));
        EXPECT_NO_THROW(processor->on_detach(plain));

        EXPECT_EQ(processor->get_slot_index(), std::numeric_limits<uint32_t>::max());
    }

    TEST_P(CursorAccessProcessorTest, AnInactiveProcessorWritesSilenceAndDoesNotAdvance)
    {
        attach();

        EXPECT_EQ(run(0), std::vector<double>(k_block, 0.0));
        EXPECT_EQ(run(1), std::vector<double>(k_block, 0.0));

        EXPECT_EQ(processor->cursor(), 0U);
        EXPECT_FALSE(processor->is_active());
    }

    TEST_P(CursorAccessProcessorTest, TheLoopRegionDefaultsToTheWholeStream)
    {
        attach();

        EXPECT_EQ(processor->loop_start(), 0U);
        EXPECT_EQ(processor->loop_end(), k_frames);
    }

    TEST_P(CursorAccessProcessorTest, ResettingActivatesAndReadsFromTheLoopStart)
    {
        processor->set_loop_region(2, 10);
        attach();

        processor->reset();

        EXPECT_TRUE(processor->is_active());
        EXPECT_EQ(run(0), ramp(0, 2, k_block));
        EXPECT_EQ(processor->cursor(), 6U);
    }

    TEST_P(CursorAccessProcessorTest, EveryChannelOfTheBlockIsFilled)
    {
        attach();
        processor->reset();

        processor->process(stream);

        const auto& pd = stream->get_dynamic_data(processor->get_slot_index());
        EXPECT_EQ(channel_of(pd, GetParam(), 0), ramp(0, 0, k_block));
        EXPECT_EQ(channel_of(pd, GetParam(), 1), ramp(1, 0, k_block));
    }

    TEST_P(CursorAccessProcessorTest, ConsecutiveBlocksFollowOneAnother)
    {
        attach();
        processor->reset();

        EXPECT_EQ(run(0), ramp(0, 0, k_block));
        EXPECT_EQ(run(0), ramp(0, 4, k_block));
        EXPECT_EQ(run(0), ramp(0, 8, k_block));
        EXPECT_EQ(run(0), ramp(0, 12, k_block));
        EXPECT_EQ(processor->cursor(), 0U);
    }

    TEST_P(CursorAccessProcessorTest, AOneShotEndsAtTheLoopEndAndFiresTheCallbackOnce)
    {
        processor->set_loop_region(0, 8);
        int ended = 0;
        processor->set_on_end([&ended] { ++ended; });
        attach();
        processor->reset();

        run();
        EXPECT_TRUE(processor->is_active());
        EXPECT_EQ(ended, 0);
        EXPECT_EQ(run(), ramp(0, 4, k_block));

        EXPECT_FALSE(processor->is_active());
        EXPECT_EQ(ended, 1);
        EXPECT_EQ(processor->cursor(), 0U);
        EXPECT_EQ(run(), std::vector<double>(k_block, 0.0));
        EXPECT_EQ(ended, 1);
    }

    TEST_P(CursorAccessProcessorTest, AOneShotNeverPlaysPastItsLoopEnd)
    {
        processor->set_loop_region(0, 6);
        attach();
        processor->reset();

        run();
        const auto last = run();

        EXPECT_EQ(last, ramp(0, 4, 2));
        EXPECT_FALSE(processor->is_active());
    }

    TEST_P(CursorAccessProcessorTest, ABlockPastTheEndOfTheStreamIsShort)
    {
        stream = make_stream(GetParam(), 6);
        attach();
        processor->reset();

        run();
        EXPECT_EQ(run(0), ramp(0, 4, 2));
    }

    TEST_P(CursorAccessProcessorTest, LoopingWrapsToTheLoopStartAndKeepsPlaying)
    {
        processor->set_loop_region(0, 8);
        processor->set_looping(true);
        attach();
        processor->reset();

        run();
        EXPECT_EQ(run(), ramp(0, 4, k_block));
        EXPECT_TRUE(processor->is_active());
        EXPECT_EQ(processor->cursor(), 0U);
        EXPECT_EQ(run(), ramp(0, 0, k_block));
        EXPECT_TRUE(processor->is_active());
    }

    TEST_P(CursorAccessProcessorTest, ALoopEndPastTheStreamIsTreatedAsTheStreamEnd)
    {
        processor->set_loop_region(0, k_frames + 5);
        processor->set_looping(true);
        attach();
        processor->reset();

        for (uint64_t block = 0; block < k_frames / k_block; ++block) {
            EXPECT_EQ(run(), ramp(0, block * k_block, k_block)) << block;
        }

        EXPECT_TRUE(processor->is_active());
        EXPECT_EQ(run(), ramp(0, 0, k_block));
    }

    TEST_P(CursorAccessProcessorTest, AOneShotWithAnEndPastTheStreamStopsOnItsLastBlock)
    {
        processor->set_loop_region(0, k_frames + 1);
        int ended = 0;
        processor->set_on_end([&ended] { ++ended; });
        attach();
        processor->reset();

        for (uint64_t block = 0; block < k_frames / k_block; ++block) {
            run();
        }

        EXPECT_FALSE(processor->is_active());
        EXPECT_EQ(ended, 1);
    }

    TEST_P(CursorAccessProcessorTest, ALoopCountStopsPlaybackAfterThatManyLoops)
    {
        processor->set_loop_region(0, 8);
        processor->set_looping(true);
        processor->set_loop_count(2);
        int ended = 0;
        processor->set_on_end([&ended] { ++ended; });
        attach();
        processor->reset();

        for (int i = 0; i < 3; ++i) {
            run();
            EXPECT_TRUE(processor->is_active()) << i;
        }
        run();

        EXPECT_FALSE(processor->is_active());
        EXPECT_EQ(ended, 1);
        EXPECT_EQ(processor->loop_count(), 2U);
    }

    TEST_P(CursorAccessProcessorTest, ResettingRestartsTheLoopCount)
    {
        processor->set_loop_region(0, 8);
        processor->set_looping(true);
        processor->set_loop_count(1);
        attach();
        processor->reset();
        run();
        run();
        ASSERT_FALSE(processor->is_active());

        processor->reset();

        EXPECT_TRUE(processor->is_active());
        EXPECT_EQ(run(), ramp(0, 0, k_block));
    }

    TEST_P(CursorAccessProcessorTest, SpeedScalesHowFarEachBlockAdvances)
    {
        processor->set_speed(2.0);
        attach();
        processor->reset();

        EXPECT_EQ(run(), ramp(0, 0, k_block));
        EXPECT_EQ(processor->cursor(), 8U);
        EXPECT_EQ(run(), ramp(0, 8, k_block));
    }

    TEST_P(CursorAccessProcessorTest, SubFrameAdvancesAccumulate)
    {
        processor->set_speed(0.625);
        attach();
        processor->reset();

        std::vector<uint64_t> cursors;
        for (int i = 0; i < 4; ++i) {
            run();
            cursors.push_back(processor->cursor());
        }

        EXPECT_EQ(cursors, (std::vector<uint64_t> { 2, 5, 7, 10 }));
    }

    TEST_P(CursorAccessProcessorTest, NonPositiveSpeedsAreIgnored)
    {
        processor->set_speed(2.0);
        processor->set_speed(0.0);
        processor->set_speed(-1.0);
        attach();
        processor->reset();

        run();

        EXPECT_EQ(processor->cursor(), 8U);
    }

    TEST_P(CursorAccessProcessorTest, StoppingKeepsTheCursorAndResettingRewinds)
    {
        attach();
        processor->reset();
        run();

        processor->stop();

        EXPECT_FALSE(processor->is_active());
        EXPECT_EQ(processor->cursor(), 4U);
        EXPECT_EQ(run(), std::vector<double>(k_block, 0.0));
        EXPECT_EQ(processor->cursor(), 4U);

        processor->reset();
        EXPECT_EQ(processor->cursor(), 0U);
        EXPECT_TRUE(processor->is_active());
    }

    TEST_P(CursorAccessProcessorTest, TheBlockSizeCanChange)
    {
        attach();
        processor->reset();

        processor->set_frames_per_block(2);

        EXPECT_EQ(run(), ramp(0, 0, 2));
        EXPECT_EQ(processor->cursor(), 2U);
    }

    TEST_P(CursorAccessProcessorTest, AnInactiveBlockHasTheCurrentBlockSize)
    {
        attach();
        processor->set_frames_per_block(3);

        EXPECT_EQ(run(1), std::vector<double>(3, 0.0));
    }

    TEST_P(CursorAccessProcessorTest, TheLoopRegionIsInFramesNotSamples)
    {
        processor->set_loop_region(5, 9);
        attach();
        processor->reset();

        EXPECT_EQ(processor->loop_start(), 5U);
        EXPECT_EQ(processor->loop_end(), 9U);
        EXPECT_EQ(run(1), ramp(1, 5, k_block));
    }

    void expect_near(const std::vector<double>& actual, const std::vector<double>& expected)
    {
        ASSERT_EQ(actual.size(), expected.size());
        for (size_t i = 0; i < actual.size(); ++i) {
            EXPECT_NEAR(actual[i], expected[i], 1e-9) << i;
        }
    }

    class CursorTimeMapTest : public CursorAccessProcessorTest {
    protected:
        static std::shared_ptr<const Kinesis::TimeMap> map_of(std::function<double(double)> fn)
        {
            return std::make_shared<const Kinesis::TimeMap>(Kinesis::TimeMap {
                .fn = [fn = std::move(fn)](const double& t) { return fn(t); } });
        }

        void start_with(const std::shared_ptr<const Kinesis::TimeMap>& map, uint64_t region_end = k_frames)
        {
            processor->set_loop_region(0, region_end);
            processor->set_time_map(map);
            attach();
            processor->reset();
        }
    };

    INSTANTIATE_TEST_SUITE_P(Organizations, CursorTimeMapTest,
        ::testing::Values(OrganizationStrategy::INTERLEAVED, OrganizationStrategy::PLANAR),
        [](const ::testing::TestParamInfo<OrganizationStrategy>& info) {
            return std::string(info.param == OrganizationStrategy::INTERLEAVED ? "Interleaved" : "Planar");
        });

    TEST_P(CursorTimeMapTest, ARealtimeMapReadsFramesLikeNormalSpeed)
    {
        start_with(map_of([](double t) { return t * k_rate; }));

        expect_near(run(0), ramp(0, 0, k_block));
        expect_near(run(0), ramp(0, 4, k_block));
        expect_near(run(1), ramp(1, 8, k_block));
    }

    TEST_P(CursorTimeMapTest, FractionalPositionsInterpolateLinearly)
    {
        start_with(map_of([](double t) { return t * k_rate * 0.5; }));

        const auto out = run(0);

        expect_near(out, (std::vector<double> { sample(0, 0), sample(0, 0) + 5.0, sample(1, 0), sample(1, 0) + 5.0 }));
    }

    TEST_P(CursorTimeMapTest, AReversingMapPlaysBackwards)
    {
        start_with(map_of([](double t) { return 15.0 - t * k_rate; }));

        expect_near(run(0), (std::vector<double> { sample(15, 0), sample(14, 0), sample(13, 0), sample(12, 0) }));
        expect_near(run(0), (std::vector<double> { sample(11, 0), sample(10, 0), sample(9, 0), sample(8, 0) }));
    }

    TEST_P(CursorTimeMapTest, AFixedPositionHoldsOneFrame)
    {
        start_with(map_of([](double) { return 3.0; }));

        expect_near(run(1), std::vector<double>(k_block, sample(3, 1)));
    }

    TEST_P(CursorTimeMapTest, LeavingTheRegionEndsTheVoiceAfterTheFramesInside)
    {
        int ended = 0;
        processor->set_on_end([&ended] { ++ended; });
        start_with(map_of([](double t) { return t * k_rate + 14.0; }));

        const auto out = run(0);

        expect_near(out, (std::vector<double> { sample(14, 0), sample(15, 0), 0.0, 0.0 }));
        EXPECT_FALSE(processor->is_active());
        EXPECT_EQ(ended, 1);
        expect_near(run(0), std::vector<double>(k_block, 0.0));
    }

    TEST_P(CursorTimeMapTest, ALoopingMapWrapsInsideTheRegion)
    {
        processor->set_looping(true);
        start_with(map_of([](double t) { return t * k_rate; }), 8);

        run();
        expect_near(run(0), ramp(0, 4, k_block));
        expect_near(run(0), ramp(0, 0, k_block));
        EXPECT_TRUE(processor->is_active());
    }

    TEST_P(CursorTimeMapTest, ADifferentMapRestartsTheClockButTheSameMapDoesNot)
    {
        const auto map = map_of([](double t) { return t * k_rate; });
        start_with(map);
        run();
        run();

        processor->set_time_map(map);
        expect_near(run(0), ramp(0, 8, k_block));

        processor->set_time_map(map_of([](double t) { return t * k_rate; }));
        expect_near(run(0), ramp(0, 0, k_block));
    }

    TEST_P(CursorTimeMapTest, ResettingRestartsTheClock)
    {
        start_with(map_of([](double t) { return t * k_rate; }));
        run();
        run();

        processor->reset();

        expect_near(run(0), ramp(0, 0, k_block));
    }

    TEST_P(CursorTimeMapTest, ClearingTheMapGoesBackToSpeedReading)
    {
        start_with(map_of([](double t) { return t * k_rate; }));
        run();

        processor->set_time_map(nullptr);

        expect_near(run(0), ramp(0, 4, k_block));
    }

    TEST_P(CursorTimeMapTest, ARepeatRestartsTheMapEveryLength)
    {
        start_with(map_of([](double t) { return t * k_rate; }));
        processor->set_repeat(map_of([](double) { return 0.004; }));

        expect_near(run(0), ramp(0, 0, k_block));
        expect_near(run(0), ramp(0, 0, k_block));
    }

    TEST_P(CursorTimeMapTest, ARepeatShorterThanTheBlockRepeatsInsideIt)
    {
        start_with(map_of([](double t) { return t * k_rate; }));
        processor->set_repeat(map_of([](double) { return 0.002; }));

        expect_near(run(0), (std::vector<double> { sample(0, 0), sample(1, 0), sample(0, 0), sample(1, 0) }));
    }

    TEST_P(CursorTimeMapTest, ClearingTheRepeatLetsTheMapRunOn)
    {
        start_with(map_of([](double t) { return t * k_rate; }));
        processor->set_repeat(map_of([](double) { return 0.004; }));
        run();
        run();

        processor->set_repeat(nullptr);

        expect_near(run(0), ramp(0, 8, k_block));
    }

} // namespace
} // namespace MayaFlux::Test
