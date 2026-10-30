#include "gtest/gtest.h"

#include "MayaFlux/Buffers/AudioBuffer.hpp"
#include "MayaFlux/Buffers/Container/StreamSliceProcessor.hpp"

namespace MayaFlux::Test {
namespace {

    using Buffers::AudioBuffer;
    using Buffers::StreamSliceProcessor;
    using Kakshya::DynamicSoundStream;
    using Kakshya::OrganizationStrategy;
    using Kakshya::StreamSlice;

    constexpr uint32_t k_rate = 1000;
    constexpr uint32_t k_channels = 2;
    constexpr uint64_t k_frames = 16;
    constexpr uint32_t k_block = 4;

    double sample(uint64_t frame, uint32_t channel)
    {
        return 1.0 + 10.0 * static_cast<double>(frame) + static_cast<double>(channel);
    }

    std::vector<double> ramp(uint32_t channel, uint64_t first, uint64_t count, double scale = 1.0)
    {
        std::vector<double> values;
        for (uint64_t f = first; f < first + count; ++f) {
            values.push_back(scale * sample(f, channel));
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
            EXPECT_NEAR(actual[i], expected[i], 1e-9) << i;
        }
    }

    std::shared_ptr<DynamicSoundStream> make_stream(OrganizationStrategy organization)
    {
        auto stream = std::make_shared<DynamicSoundStream>(k_rate, k_channels);
        stream->get_structure().organization = organization;

        if (organization == OrganizationStrategy::INTERLEAVED) {
            std::vector<double> flat;
            for (uint64_t f = 0; f < k_frames; ++f) {
                for (uint32_t c = 0; c < k_channels; ++c) {
                    flat.push_back(sample(f, c));
                }
            }
            stream->write_frames(std::vector<std::span<const double>> { std::span<const double>(flat) }, 0);
        } else {
            std::vector<std::vector<double>> columns;
            for (uint32_t c = 0; c < k_channels; ++c) {
                columns.push_back(ramp(c, 0, k_frames));
            }
            std::vector<std::span<const double>> spans(columns.begin(), columns.end());
            stream->write_frames(std::move(spans), 0);
        }
        return stream;
    }

    std::shared_ptr<AudioBuffer> make_buffer(uint32_t channel)
    {
        auto buffer = std::make_shared<AudioBuffer>(channel, k_block);
        buffer->resize(k_block);
        return buffer;
    }

    class StreamSliceProcessorTest : public ::testing::TestWithParam<OrganizationStrategy> {
    protected:
        void SetUp() override
        {
            stream = make_stream(GetParam());
            buffer = make_buffer(0);
            processor = std::make_shared<StreamSliceProcessor>();
            processor->on_attach(buffer);
        }

        std::vector<double> play()
        {
            processor->processing_function(buffer);
            return buffer->get_data();
        }

        std::shared_ptr<DynamicSoundStream> stream;
        std::shared_ptr<AudioBuffer> buffer;
        std::shared_ptr<StreamSliceProcessor> processor;
    };

    INSTANTIATE_TEST_SUITE_P(Organizations, StreamSliceProcessorTest,
        ::testing::Values(OrganizationStrategy::INTERLEAVED, OrganizationStrategy::PLANAR),
        [](const ::testing::TestParamInfo<OrganizationStrategy>& info) {
            return std::string(info.param == OrganizationStrategy::INTERLEAVED ? "Interleaved" : "Planar");
        });

    TEST_P(StreamSliceProcessorTest, IsCompatibleWithAudioBuffersOnly)
    {
        EXPECT_TRUE(processor->is_compatible_with(buffer));
        EXPECT_FALSE(processor->is_compatible_with(nullptr));
    }

    TEST_P(StreamSliceProcessorTest, StartsWithNoSlots)
    {
        EXPECT_EQ(processor->slot_count(), 0U);
        EXPECT_FALSE(processor->any_active());
    }

    TEST_P(StreamSliceProcessorTest, LoadingBeforeAttachmentIsIgnored)
    {
        StreamSliceProcessor unattached;

        unattached.load(0, StreamSlice::from_stream(stream));

        EXPECT_EQ(unattached.slot_count(), 0U);
        EXPECT_EQ(stream->allocate_dynamic_slot(), 0U);
    }

    TEST_P(StreamSliceProcessorTest, LoadingASliceWithoutAStreamIsIgnored)
    {
        processor->load(0, StreamSlice {});

        EXPECT_EQ(processor->slot_count(), 0U);
    }

    TEST_P(StreamSliceProcessorTest, LoadedSlotsAreInactiveAndKnowTheirIndex)
    {
        StreamSlice slice = StreamSlice::from_stream(stream);
        slice.active = true;

        processor->load(0, slice);

        EXPECT_EQ(processor->slot_count(), 1U);
        EXPECT_FALSE(processor->slice(0).active);
        EXPECT_EQ(processor->slice(0).index, 0U);
        EXPECT_FALSE(processor->any_active());
    }

    TEST_P(StreamSliceProcessorTest, LoadingPastTheEndGrowsThePoolToThatIndex)
    {
        processor->load(2, StreamSlice::from_stream(stream));

        EXPECT_EQ(processor->slot_count(), 3U);
        EXPECT_EQ(processor->slice(2).index, 2U);
        EXPECT_EQ(processor->slice(2).stream, stream);
    }

    TEST_P(StreamSliceProcessorTest, AnInactiveProcessorWritesSilence)
    {
        processor->load(0, StreamSlice::from_stream(stream));
        std::ranges::fill(buffer->get_data(), 7.0);

        EXPECT_EQ(play(), std::vector<double>(k_block, 0.0));
    }

    TEST_P(StreamSliceProcessorTest, BindingPlaysTheSliceFromItsFirstFrame)
    {
        processor->load(0, StreamSlice::from_frame_range(stream, 4, 11));

        processor->bind(0);

        EXPECT_TRUE(processor->any_active());
        expect_near(play(), ramp(0, 4, k_block));
        expect_near(play(), ramp(0, 8, k_block));
    }

    TEST_P(StreamSliceProcessorTest, AOneShotPlaysItsWholeRegionThenStopsAndReportsTheSlot)
    {
        std::vector<size_t> ended;
        processor->set_on_end([&ended](size_t index) { ended.push_back(index); });
        processor->load(0, StreamSlice::from_frame_range(stream, 4, 11));
        processor->bind(0);

        play();
        EXPECT_TRUE(ended.empty());
        expect_near(play(), ramp(0, 8, k_block));

        EXPECT_EQ(ended, (std::vector<size_t> { 0 }));
        EXPECT_FALSE(processor->any_active());
        EXPECT_FALSE(processor->slice(0).active);
        EXPECT_EQ(play(), std::vector<double>(k_block, 0.0));
    }

    TEST_P(StreamSliceProcessorTest, AOneShotPlaysItsLastFrameAndNothingBeyond)
    {
        processor->load(0, StreamSlice::from_frame_range(stream, 0, 5));
        processor->bind(0);

        play();
        const auto last = play();

        expect_near(last, (std::vector<double> { sample(4, 0), sample(5, 0), 0.0, 0.0 }));
        EXPECT_FALSE(processor->any_active());
    }

    TEST_P(StreamSliceProcessorTest, ASliceOfOneFramePlaysThatFrame)
    {
        processor->load(0, StreamSlice::from_frame_range(stream, 3, 3));
        processor->bind(0);

        expect_near(play(), (std::vector<double> { sample(3, 0), 0.0, 0.0, 0.0 }));
        EXPECT_FALSE(processor->any_active());
    }

    TEST_P(StreamSliceProcessorTest, BindingAgainRestartsFromTheRegionStart)
    {
        processor->load(0, StreamSlice::from_frame_range(stream, 2, 13));
        processor->bind(0);
        play();

        processor->bind(0);

        expect_near(play(), ramp(0, 2, k_block));
    }

    TEST_P(StreamSliceProcessorTest, UnbindingSilencesTheSlot)
    {
        processor->load(0, StreamSlice::from_stream(stream));
        processor->bind(0);
        play();

        processor->unbind(0);

        EXPECT_FALSE(processor->any_active());
        EXPECT_EQ(play(), std::vector<double>(k_block, 0.0));
    }

    TEST_P(StreamSliceProcessorTest, MissingSlotsAreIgnoredWhenBoundOrUnbound)
    {
        EXPECT_NO_THROW(processor->bind(5));
        EXPECT_NO_THROW(processor->unbind(5));
        EXPECT_FALSE(processor->any_active());
    }

    TEST_P(StreamSliceProcessorTest, TheBuffersChannelSelectsTheStreamChannel)
    {
        const auto second = make_buffer(1);
        StreamSliceProcessor other;
        other.on_attach(second);
        other.load(0, StreamSlice::from_stream(stream));
        other.bind(0);

        other.processing_function(second);

        expect_near(second->get_data(), ramp(1, 0, k_block));
    }

    TEST_P(StreamSliceProcessorTest, ASourceChannelOverridesTheBuffersChannel)
    {
        StreamSlice slice = StreamSlice::from_stream(stream);
        slice.with_source_channel(1);
        processor->load(0, slice);
        processor->bind(0);

        expect_near(play(), ramp(1, 0, k_block));
    }

    TEST_P(StreamSliceProcessorTest, ScaleMultipliesTheOutput)
    {
        StreamSlice slice = StreamSlice::from_stream(stream);
        slice.with_scale(0.5);
        processor->load(0, slice);
        processor->bind(0);

        expect_near(play(), ramp(0, 0, k_block, 0.5));
    }

    TEST_P(StreamSliceProcessorTest, ActiveSlotsAreMixedIntoTheBuffer)
    {
        processor->load(0, StreamSlice::from_frame_range(stream, 0, 3));
        processor->load(1, StreamSlice::from_frame_range(stream, 8, 11));
        processor->bind(0);
        processor->bind(1);

        expect_near(play(), sum(ramp(0, 0, k_block), ramp(0, 8, k_block)));
    }

    TEST_P(StreamSliceProcessorTest, OnlyBoundSlotsContributeToTheMix)
    {
        processor->load(0, StreamSlice::from_frame_range(stream, 0, 3));
        processor->load(1, StreamSlice::from_frame_range(stream, 8, 11));
        processor->bind(1);

        expect_near(play(), ramp(0, 8, k_block));
    }

    TEST_P(StreamSliceProcessorTest, EachSlotEndsOnItsOwnAndReportsItsIndex)
    {
        std::vector<size_t> ended;
        processor->set_on_end([&ended](size_t index) { ended.push_back(index); });
        processor->load(0, StreamSlice::from_frame_range(stream, 0, 3));
        processor->load(1, StreamSlice::from_frame_range(stream, 0, 7));
        processor->bind(0);
        processor->bind(1);

        play();
        EXPECT_EQ(ended, (std::vector<size_t> { 0 }));
        EXPECT_TRUE(processor->any_active());
        play();

        EXPECT_EQ(ended, (std::vector<size_t> { 0, 1 }));
        EXPECT_FALSE(processor->any_active());
    }

    TEST_P(StreamSliceProcessorTest, ALoopingSliceKeepsPlaying)
    {
        StreamSlice slice = StreamSlice::from_frame_range(stream, 0, 7);
        slice.with_looping(true);
        processor->load(0, slice);
        processor->bind(0);

        expect_near(play(), ramp(0, 0, k_block));
        expect_near(play(), ramp(0, 4, k_block));
        expect_near(play(), ramp(0, 0, k_block));
        EXPECT_TRUE(processor->any_active());
    }

    TEST_P(StreamSliceProcessorTest, ASliceEndingOnePastTheLastFrameStillLoopsTheWholeStream)
    {
        StreamSlice slice = StreamSlice::from_frame_range(stream, 0, k_frames);
        slice.with_looping(true);
        processor->load(0, slice);
        processor->bind(0);

        for (uint64_t block = 0; block < k_frames / k_block; ++block) {
            expect_near(play(), ramp(0, block * k_block, k_block));
        }

        EXPECT_TRUE(processor->any_active());
        expect_near(play(), ramp(0, 0, k_block));
    }

    TEST_P(StreamSliceProcessorTest, ASliceEndingOnePastTheLastFrameStopsOnItsLastBlock)
    {
        int ended = 0;
        processor->set_on_end([&ended](size_t) { ++ended; });
        processor->load(0, StreamSlice::from_frame_range(stream, 0, k_frames));
        processor->bind(0);

        for (uint64_t block = 0; block < k_frames / k_block; ++block) {
            play();
        }

        EXPECT_EQ(ended, 1);
        EXPECT_FALSE(processor->any_active());
    }

    TEST_P(StreamSliceProcessorTest, AWholeStreamSliceLoopsAcrossItsLastFrame)
    {
        StreamSlice slice = StreamSlice::from_stream(stream);
        slice.with_looping(true);
        processor->load(0, slice);
        processor->bind(0);

        for (uint64_t block = 0; block < k_frames / k_block; ++block) {
            play();
        }

        expect_near(play(), ramp(0, 0, k_block));
        EXPECT_TRUE(processor->any_active());
    }

    TEST_P(StreamSliceProcessorTest, AWholeStreamSliceIncludesItsLastFrame)
    {
        processor->load(0, StreamSlice::from_stream(stream));
        processor->bind(0);

        for (uint64_t block = 0; block + 1 < k_frames / k_block; ++block) {
            play();
        }

        expect_near(play(), ramp(0, k_frames - k_block, k_block));
    }

    TEST_P(StreamSliceProcessorTest, ALoopCountStopsALoopingSlice)
    {
        int ended = 0;
        processor->set_on_end([&ended](size_t) { ++ended; });
        StreamSlice slice = StreamSlice::from_frame_range(stream, 0, 7);
        slice.with_looping(true).with_loop_count(2);
        processor->load(0, slice);
        processor->bind(0);

        for (int i = 0; i < 3; ++i) {
            play();
            EXPECT_TRUE(processor->any_active()) << i;
        }
        play();

        EXPECT_FALSE(processor->any_active());
        EXPECT_EQ(ended, 1);
    }

    TEST_P(StreamSliceProcessorTest, EditsToTheSliceTakeEffectOnTheNextBlock)
    {
        processor->load(0, StreamSlice::from_stream(stream));
        processor->bind(0);
        expect_near(play(), ramp(0, 0, k_block));

        processor->slice(0).with_speed(2.0);

        expect_near(play(), ramp(0, 4, k_block));
        expect_near(play(), ramp(0, 12, k_block));
    }

    TEST_P(StreamSliceProcessorTest, ATimeMapDrivesThePlayback)
    {
        StreamSlice slice = StreamSlice::from_stream(stream);
        slice.with_time_map(Kinesis::TimeMap { .fn = [](const double& t) { return t * k_rate; } });
        processor->load(0, slice);
        processor->bind(0);

        expect_near(play(), ramp(0, 0, k_block));
        expect_near(play(), ramp(0, 4, k_block));
    }

    TEST_P(StreamSliceProcessorTest, LoadingASlotAgainReplacesItAndKeepsOneStreamSlot)
    {
        processor->load(0, StreamSlice::from_stream(stream));
        processor->load(0, StreamSlice::from_frame_range(stream, 8, 15));
        processor->bind(0);

        expect_near(play(), ramp(0, 8, k_block));
        EXPECT_EQ(stream->allocate_dynamic_slot(), 1U);
    }

    TEST_P(StreamSliceProcessorTest, DetachingReleasesTheStreamSlotsAndClearsTheSlices)
    {
        processor->load(0, StreamSlice::from_stream(stream));
        processor->bind(0);

        processor->on_detach(buffer);

        EXPECT_EQ(processor->slice(0).stream, nullptr);
        EXPECT_FALSE(processor->any_active());
        EXPECT_EQ(stream->allocate_dynamic_slot(), 0U);
    }

    TEST_P(StreamSliceProcessorTest, TwoProcessorsPlayOneStreamIndependently)
    {
        const auto second_buffer = make_buffer(0);
        StreamSliceProcessor other;
        other.on_attach(second_buffer);
        processor->load(0, StreamSlice::from_stream(stream));
        other.load(0, StreamSlice::from_stream(stream));
        processor->bind(0);
        other.bind(0);

        play();
        play();
        other.processing_function(second_buffer);

        expect_near(second_buffer->get_data(), ramp(0, 0, k_block));
        expect_near(play(), ramp(0, 8, k_block));
    }

    TEST_P(StreamSliceProcessorTest, NullBuffersAreIgnored)
    {
        EXPECT_NO_THROW(processor->processing_function(nullptr));
        EXPECT_NO_THROW(processor->on_attach(nullptr));
    }

} // namespace
} // namespace MayaFlux::Test
