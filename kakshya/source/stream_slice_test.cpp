#include "gtest/gtest.h"

#include "MayaFlux/Kakshya/Source/StreamSlice.hpp"

namespace MayaFlux::Test {
namespace {

    using Kakshya::DynamicSoundStream;
    using Kakshya::StreamSlice;

    std::shared_ptr<DynamicSoundStream> make_stream(uint32_t channels, uint64_t frames)
    {
        auto stream = std::make_shared<DynamicSoundStream>(48000, channels);
        stream->ensure_capacity(frames);
        return stream;
    }

    TEST(StreamSliceTest, AWholeStreamSliceSpansEveryFrameAndChannel)
    {
        const auto stream = make_stream(3, 100);

        const StreamSlice slice = StreamSlice::from_stream(stream);

        EXPECT_EQ(slice.stream, stream);
        EXPECT_EQ(slice.start_frame(), 0U);
        EXPECT_EQ(slice.end_frame(), 99U);
        EXPECT_EQ(slice.start_channel(), 0U);
        EXPECT_EQ(slice.end_channel(), 2U);
    }

    TEST(StreamSliceTest, AnEmptyStreamSlicesToAZeroFrameRange)
    {
        const StreamSlice slice = StreamSlice::from_stream(make_stream(2, 0));

        EXPECT_EQ(slice.start_frame(), 0U);
        EXPECT_EQ(slice.end_frame(), 0U);
        EXPECT_EQ(slice.end_channel(), 1U);
    }

    TEST(StreamSliceTest, ASliceStartsWithNeutralPlaybackState)
    {
        const StreamSlice slice = StreamSlice::from_stream(make_stream(2, 10));

        EXPECT_DOUBLE_EQ(slice.speed, 1.0);
        EXPECT_DOUBLE_EQ(slice.scale, 1.0);
        EXPECT_DOUBLE_EQ(slice.cursor_remainder, 0.0);
        EXPECT_FALSE(slice.looping);
        EXPECT_FALSE(slice.active);
        EXPECT_EQ(slice.index, 0U);
        EXPECT_EQ(slice.loop_count, 0U);
        EXPECT_EQ(slice.time_map, nullptr);
        EXPECT_EQ(slice.repeat, nullptr);
        EXPECT_FALSE(slice.source_channel.has_value());
    }

    TEST(StreamSliceTest, ASliceCarriesItsIndex)
    {
        EXPECT_EQ(StreamSlice::from_stream(make_stream(2, 10), 5).index, 5U);
        EXPECT_EQ(StreamSlice::from_frame_range(make_stream(2, 10), 1, 2, 6).index, 6U);
        EXPECT_EQ(StreamSlice::from_region(make_stream(2, 10), 1, 2, 0, 1, 7).index, 7U);
    }

    TEST(StreamSliceTest, AFrameRangeSliceSpansEveryChannel)
    {
        const auto stream = make_stream(4, 100);

        const StreamSlice slice = StreamSlice::from_frame_range(stream, 10, 59);

        EXPECT_EQ(slice.start_frame(), 10U);
        EXPECT_EQ(slice.end_frame(), 59U);
        EXPECT_EQ(slice.start_channel(), 0U);
        EXPECT_EQ(slice.end_channel(), 3U);
    }

    TEST(StreamSliceTest, ARegionSliceTakesBothFrameAndChannelBounds)
    {
        const auto stream = make_stream(4, 100);

        const StreamSlice slice = StreamSlice::from_region(stream, 5, 20, 1, 2);

        EXPECT_EQ(slice.start_frame(), 5U);
        EXPECT_EQ(slice.end_frame(), 20U);
        EXPECT_EQ(slice.start_channel(), 1U);
        EXPECT_EQ(slice.end_channel(), 2U);
    }

    TEST(StreamSliceTest, TheRegionIsAnAudioRegion)
    {
        const StreamSlice slice = StreamSlice::from_frame_range(make_stream(2, 10), 2, 4);

        EXPECT_EQ(slice.region.start_coordinates, (std::vector<uint64_t> { 2, 0 }));
        EXPECT_EQ(slice.region.end_coordinates, (std::vector<uint64_t> { 4, 1 }));
        EXPECT_EQ(std::any_cast<const char*>(slice.region.attributes.at("type")), std::string_view("audio_region"));
    }

    TEST(StreamSliceTest, BuilderCallsSetTheirFieldAndChain)
    {
        StreamSlice slice = StreamSlice::from_stream(make_stream(2, 10));

        StreamSlice& chained = slice.with_speed(2.0).with_looping(true).with_scale(0.5).with_loop_count(3).with_source_channel(1);

        EXPECT_EQ(&chained, &slice);
        EXPECT_DOUBLE_EQ(slice.speed, 2.0);
        EXPECT_TRUE(slice.looping);
        EXPECT_DOUBLE_EQ(slice.scale, 0.5);
        EXPECT_EQ(slice.loop_count, 3U);
        EXPECT_EQ(slice.source_channel, 1U);
    }

    TEST(StreamSliceTest, ATimeMapIsStoredAndEvaluates)
    {
        StreamSlice slice = StreamSlice::from_stream(make_stream(2, 10));

        slice.with_time_map(Kinesis::TimeMap { .fn = [](const double& t) { return t * 3.0; } });

        ASSERT_NE(slice.time_map, nullptr);
        EXPECT_DOUBLE_EQ((*slice.time_map)(2.0), 6.0);
    }

    TEST(StreamSliceTest, ARepeatLengthIsStoredAndEvaluates)
    {
        StreamSlice slice = StreamSlice::from_stream(make_stream(2, 10));

        slice.with_repeat(Kinesis::TimeMap { .fn = [](const double& t) { return 1.0 + t; } });

        ASSERT_NE(slice.repeat, nullptr);
        EXPECT_DOUBLE_EQ((*slice.repeat)(1.0), 2.0);
    }

    TEST(StreamSliceTest, CopiesShareTheStreamAndTheirMaps)
    {
        StreamSlice slice = StreamSlice::from_stream(make_stream(2, 10));
        slice.with_time_map(Kinesis::TimeMap { .fn = [](const double& t) { return t; } });

        const StreamSlice copy = slice;

        EXPECT_EQ(copy.stream, slice.stream);
        EXPECT_EQ(copy.time_map, slice.time_map);
    }

    TEST(StreamSliceTest, ReplacingAMapOnACopyLeavesTheOriginalAlone)
    {
        StreamSlice slice = StreamSlice::from_stream(make_stream(2, 10));
        slice.with_time_map(Kinesis::TimeMap { .fn = [](const double& t) { return t; } });
        StreamSlice copy = slice;

        copy.with_time_map(Kinesis::TimeMap { .fn = [](const double& t) { return -t; } });

        EXPECT_DOUBLE_EQ((*slice.time_map)(2.0), 2.0);
        EXPECT_DOUBLE_EQ((*copy.time_map)(2.0), -2.0);
    }

} // namespace
} // namespace MayaFlux::Test
