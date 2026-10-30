#include "gtest/gtest.h"

#include "../video_test_support.hpp"

#include "MayaFlux/Kakshya/NDData/ImageData.hpp"
#include "MayaFlux/Kakshya/Processors/FrameSeekProcessor.hpp"

namespace MayaFlux::Test {
namespace {

    using Kakshya::DynamicVideoStream;
    using Kakshya::FrameSeekProcessor;
    using Kakshya::ImageData;
    using Kakshya::ImageFormat;
    using Kakshya::ProcessingState;
    using Kakshya::VideoStreamSpec;
    using namespace VideoSupport;

    constexpr uint64_t k_ring = 4;
    constexpr uint64_t k_roomy_ring = 16;

    std::shared_ptr<DynamicVideoStream> make_stream(uint64_t ring_frames = 0, double rate = k_fps)
    {
        return std::make_shared<DynamicVideoStream>(rgba8_spec(ring_frames, rate));
    }

    ImageData image_of(uint64_t frame, uint32_t width = k_width, uint32_t height = k_height, ImageFormat format = ImageFormat::RGBA8)
    {
        auto image = ImageData::allocate(width, height, 4, format);
        const auto bytes = frame_bytes(frame, image->byte_size());
        std::memcpy(image->data(), bytes.data(), bytes.size());
        return *image;
    }

    struct StreamMode {
        uint64_t ring_frames;
        const char* name;
    };

    class DynamicVideoStreamModeTest : public ::testing::TestWithParam<StreamMode> {
    protected:
        void SetUp() override
        {
            stream = make_stream(GetParam().ring_frames);
        }

        [[nodiscard]] bool ring() const { return GetParam().ring_frames > 0; }

        std::shared_ptr<DynamicVideoStream> stream;
    };

    INSTANTIATE_TEST_SUITE_P(Modes, DynamicVideoStreamModeTest,
        ::testing::Values(
            StreamMode { 0, "Linear" },
            StreamMode { k_roomy_ring, "Ring" }),
        [](const ::testing::TestParamInfo<StreamMode>& info) { return std::string(info.param.name); });

    TEST_P(DynamicVideoStreamModeTest, ConstructionReportsTheSpecAndStartsEmpty)
    {
        EXPECT_EQ(stream->get_width(), k_width);
        EXPECT_EQ(stream->get_height(), k_height);
        EXPECT_EQ(stream->get_format(), ImageFormat::RGBA8);
        EXPECT_DOUBLE_EQ(stream->get_frame_rate(), k_fps);
        EXPECT_EQ(stream->get_write_head(), 0U);
        EXPECT_EQ(stream->get_num_frames(), 0U);
        EXPECT_EQ(stream->is_ring_mode(), ring());
        EXPECT_EQ(stream->get_ring_capacity(), GetParam().ring_frames);
        EXPECT_TRUE(stream->get_frame_pixels(0).empty());
    }

    TEST_P(DynamicVideoStreamModeTest, AppendingStoresTheFramesAndAdvancesTheHead)
    {
        EXPECT_EQ(append_ramp(*stream, 4), 4U);

        EXPECT_EQ(stream->get_write_head(), 4U);
        EXPECT_EQ(stream->get_num_frames(), 4U);
        EXPECT_EQ(stream->get_total_elements(), 4U * k_rgba8_frame_bytes);
        EXPECT_TRUE(stream->has_data());
        EXPECT_EQ(stream->get_dimensions().at(0).size, 4U);
    }

    TEST_P(DynamicVideoStreamModeTest, AppendedFramesReadBack)
    {
        append_ramp(*stream, 4);

        for (uint64_t f = 0; f < 4; ++f) {
            EXPECT_EQ(pixels_of(*stream, f), frame_bytes(f)) << f;
        }
    }

    TEST_P(DynamicVideoStreamModeTest, FramesPastTheHeadAreNotReadable)
    {
        append_ramp(*stream, 4);

        EXPECT_TRUE(stream->get_frame_pixels(4).empty());
        EXPECT_TRUE(stream->get_frame_pixels(1000).empty());
    }

    TEST_P(DynamicVideoStreamModeTest, FramesOfTheWrongSizeAreRefused)
    {
        const std::vector<uint8_t> none;
        const std::vector<uint8_t> shorter(k_rgba8_frame_bytes - 1, 1);
        const std::vector<uint8_t> longer(k_rgba8_frame_bytes + 1, 1);

        EXPECT_FALSE(stream->append_frame(std::span<const uint8_t>(none)));
        EXPECT_FALSE(stream->append_frame(std::span<const uint8_t>(shorter)));
        EXPECT_FALSE(stream->append_frame(std::span<const uint8_t>(longer)));

        EXPECT_EQ(stream->get_write_head(), 0U);
        EXPECT_EQ(stream->get_num_frames(), 0U);
    }

    TEST_P(DynamicVideoStreamModeTest, AMatchingImageIsAppended)
    {
        EXPECT_TRUE(stream->append_frame(image_of(3)));

        EXPECT_EQ(stream->get_write_head(), 1U);
        EXPECT_EQ(pixels_of(*stream, 0), frame_bytes(3));
    }

    TEST_P(DynamicVideoStreamModeTest, AnImageOfAnotherSizeOrFormatIsRefused)
    {
        EXPECT_FALSE(stream->append_frame(image_of(0, k_width + 1, k_height)));
        EXPECT_FALSE(stream->append_frame(image_of(0, k_width, k_height + 1)));
        EXPECT_FALSE(stream->append_frame(image_of(0, k_width, k_height, ImageFormat::BGRA8)));

        EXPECT_EQ(stream->get_write_head(), 0U);
    }

    TEST_P(DynamicVideoStreamModeTest, AMissingGpuImageIsRefused)
    {
        EXPECT_FALSE(stream->append_frame(std::shared_ptr<Core::VKImage> {}));

        EXPECT_EQ(stream->get_write_head(), 0U);
    }

    TEST_P(DynamicVideoStreamModeTest, AppendingMakesTheStreamReady)
    {
        append_ramp(*stream, 1);

        EXPECT_EQ(stream->get_processing_state(), ProcessingState::READY);
        EXPECT_TRUE(stream->is_ready());
        EXPECT_TRUE(stream->is_ready_for_processing());
    }

    TEST_P(DynamicVideoStreamModeTest, ASnapshotKeepsTheMostRecentFramesOldestFirst)
    {
        append_ramp(*stream, 5);

        const auto copy = stream->snapshot(3);

        ASSERT_NE(copy, nullptr);
        EXPECT_EQ(copy->get_num_frames(), 3U);
        EXPECT_FALSE(copy->is_ring_mode());
        EXPECT_EQ(copy->get_width(), k_width);
        EXPECT_EQ(copy->get_height(), k_height);
        EXPECT_EQ(copy->get_format(), ImageFormat::RGBA8);
        EXPECT_DOUBLE_EQ(copy->get_frame_rate(), k_fps);
        for (uint64_t f = 0; f < 3; ++f) {
            EXPECT_EQ(pixels_of(*copy, f), frame_bytes(f + 2)) << f;
        }
    }

    TEST_P(DynamicVideoStreamModeTest, ASnapshotIsCappedAtWhatWasWritten)
    {
        append_ramp(*stream, 3);

        const auto copy = stream->snapshot(100);

        ASSERT_NE(copy, nullptr);
        EXPECT_EQ(copy->get_num_frames(), 3U);
    }

    TEST_P(DynamicVideoStreamModeTest, NothingToSnapshotGivesNull)
    {
        EXPECT_EQ(stream->snapshot(4), nullptr);

        append_ramp(*stream, 2);
        EXPECT_EQ(stream->snapshot(0), nullptr);
    }

    TEST_P(DynamicVideoStreamModeTest, ASnapshotIsIndependentOfTheSource)
    {
        append_ramp(*stream, 3);
        const auto copy = stream->snapshot(3);

        append_ramp(*stream, 3, 10);

        EXPECT_EQ(copy->get_num_frames(), 3U);
        EXPECT_EQ(pixels_of(*copy, 2), frame_bytes(2));
    }

    TEST_P(DynamicVideoStreamModeTest, ClearRestartsTheHeadAndTheReadPosition)
    {
        append_ramp(*stream, 4);
        stream->set_read_position({ 3 });

        stream->clear();

        EXPECT_EQ(stream->get_write_head(), 0U);
        EXPECT_EQ(stream->get_num_frames(), 0U);
        EXPECT_EQ(stream->get_read_position(), std::vector<uint64_t> { 0 });
        EXPECT_TRUE(stream->get_frame_pixels(0).empty());
        EXPECT_EQ(stream->is_ring_mode(), ring());
    }

    TEST_P(DynamicVideoStreamModeTest, AClearedStreamTakesNewFramesFromZero)
    {
        append_ramp(*stream, 4);
        stream->clear();

        append_ramp(*stream, 2, 9);

        EXPECT_EQ(stream->get_write_head(), 2U);
        EXPECT_EQ(pixels_of(*stream, 0), frame_bytes(9));
        EXPECT_EQ(pixels_of(*stream, 1), frame_bytes(10));
    }

    TEST_P(DynamicVideoStreamModeTest, TheDefaultProcessorIsASeekProcessorAttachedToTheStream)
    {
        EXPECT_EQ(stream->get_default_processor(), nullptr);

        stream->create_default_processor();

        const auto seek = std::dynamic_pointer_cast<FrameSeekProcessor>(stream->get_default_processor());
        ASSERT_NE(seek, nullptr);
        EXPECT_EQ(seek->get_frame_byte_size(), k_rgba8_frame_bytes);
        EXPECT_EQ(stream->seek_processor(), seek);
    }

    TEST_P(DynamicVideoStreamModeTest, TheSeekProcessorIsInstalledOnFirstUse)
    {
        const auto seek = stream->seek_processor();

        ASSERT_NE(seek, nullptr);
        EXPECT_EQ(stream->get_default_processor(), seek);
        EXPECT_EQ(stream->seek_processor(), seek);
    }

    TEST_P(DynamicVideoStreamModeTest, TheDefaultProcessorPlaysFramesAsTheyArrive)
    {
        stream->create_default_processor();
        append_ramp(*stream, 3);

        for (uint64_t f = 0; f < 3; ++f) {
            stream->process_default();
            EXPECT_EQ(processed_bytes(*stream), frame_bytes(f)) << f;
        }
        EXPECT_EQ(stream->get_processing_state(), ProcessingState::PROCESSED);
    }

    TEST_P(DynamicVideoStreamModeTest, PlaybackRepeatsTheNewestFrameAtTheLiveEdge)
    {
        stream->create_default_processor();
        append_ramp(*stream, 2);
        std::vector<std::vector<uint8_t>> seen;

        for (int i = 0; i < 4; ++i) {
            stream->process_default();
            seen.push_back(processed_bytes(*stream));
        }

        EXPECT_EQ(seen, (std::vector<std::vector<uint8_t>> { frame_bytes(0), frame_bytes(1), frame_bytes(1), frame_bytes(1) }));
    }

    TEST_P(DynamicVideoStreamModeTest, PlaybackPicksUpFramesAppendedAfterItReachedTheEdge)
    {
        stream->create_default_processor();
        append_ramp(*stream, 1);
        stream->process_default();
        stream->process_default();

        append_ramp(*stream, 1, 1);
        stream->process_default();
        stream->process_default();

        EXPECT_EQ(processed_bytes(*stream), frame_bytes(1));
    }

    class DynamicVideoStreamLinearTest : public ::testing::Test {
    protected:
        void SetUp() override
        {
            stream = make_stream();
        }

        std::shared_ptr<DynamicVideoStream> stream;
    };

    TEST_F(DynamicVideoStreamLinearTest, StartsEmptyAndIdle)
    {
        EXPECT_FALSE(stream->has_data());
        EXPECT_FALSE(stream->is_ready());
        EXPECT_EQ(stream->get_processing_state(), ProcessingState::IDLE);
        EXPECT_EQ(stream->get_raw_data(), nullptr);
    }

    TEST_F(DynamicVideoStreamLinearTest, GrowsWithEveryFrameAndKeepsEarlierOnes)
    {
        EXPECT_EQ(append_ramp(*stream, 40), 40U);

        EXPECT_EQ(stream->get_num_frames(), 40U);
        EXPECT_EQ(pixels_of(*stream, 0), frame_bytes(0));
        EXPECT_EQ(pixels_of(*stream, 20), frame_bytes(20));
        EXPECT_EQ(pixels_of(*stream, 39), frame_bytes(39));
        EXPECT_NE(stream->get_raw_data(), nullptr);
    }

    TEST_F(DynamicVideoStreamLinearTest, ReservingRoomDoesNotCreateFrames)
    {
        stream->ensure_capacity(8);

        EXPECT_EQ(stream->get_num_frames(), 0U);
        EXPECT_FALSE(stream->has_data());

        append_ramp(*stream, 8);
        EXPECT_EQ(stream->get_num_frames(), 8U);
        EXPECT_EQ(pixels_of(*stream, 7), frame_bytes(7));
    }

    TEST_F(DynamicVideoStreamLinearTest, TheEndIsTheWriteHead)
    {
        append_ramp(*stream, 2);

        stream->set_read_position({ 1 });
        EXPECT_FALSE(stream->is_at_end());

        stream->set_read_position({ 2 });
        EXPECT_TRUE(stream->is_at_end());
    }

    TEST_F(DynamicVideoStreamLinearTest, EnablingTheRingDiscardsWhatWasAppended)
    {
        append_ramp(*stream, 3);

        stream->enable_circular_buffer(k_ring);

        EXPECT_TRUE(stream->is_ring_mode());
        EXPECT_EQ(stream->get_ring_capacity(), k_ring);
        EXPECT_EQ(stream->get_write_head(), 0U);
        EXPECT_EQ(stream->get_num_frames(), 0U);
        EXPECT_TRUE(stream->get_frame_pixels(0).empty());
    }

    TEST_F(DynamicVideoStreamLinearTest, ARingCapacityOfZeroOrBeyondTheSlotLimitIsRefused)
    {
        stream->enable_circular_buffer(0);
        EXPECT_FALSE(stream->is_ring_mode());

        stream->enable_circular_buffer(uint64_t { UINT32_MAX } + 1);
        EXPECT_FALSE(stream->is_ring_mode());
    }

    TEST(DynamicVideoStreamTest, ASpecWithARingStartsCircular)
    {
        const auto stream = make_stream(k_ring);

        EXPECT_TRUE(stream->is_ring_mode());
        EXPECT_EQ(stream->get_ring_capacity(), k_ring);
    }

    TEST(DynamicVideoStreamTest, AStreamWithoutAFrameSizeTakesNothing)
    {
        DynamicVideoStream stream(VideoStreamSpec { .frame_rate = k_fps, .ring_frames = k_ring });
        const std::vector<uint8_t> bytes(4, 1);

        EXPECT_FALSE(stream.is_ring_mode());
        EXPECT_FALSE(stream.append_frame(std::span<const uint8_t>(bytes)));
        EXPECT_EQ(stream.get_write_head(), 0U);

        stream.enable_circular_buffer(k_ring);
        EXPECT_FALSE(stream.is_ring_mode());
    }

    TEST(DynamicVideoStreamTest, FormatsOtherThanBytesAreStoredAsBytes)
    {
        DynamicVideoStream stream(VideoStreamSpec { .width = k_width, .height = k_height, .format = ImageFormat::RGBA32F, .frame_rate = k_fps });
        const size_t frame_size = size_t { k_width } * k_height * 16;

        EXPECT_EQ(stream.get_frame_byte_size(), frame_size);
        EXPECT_TRUE(stream.append_frame(std::span<const uint8_t>(frame_bytes(2, frame_size))));
        EXPECT_FALSE(stream.append_frame(std::span<const uint8_t>(frame_bytes(2, k_rgba8_frame_bytes))));

        EXPECT_EQ(stream.get_write_head(), 1U);
        EXPECT_EQ(pixels_of(stream, 0), frame_bytes(2, frame_size));
    }

    class DynamicVideoStreamRingTest : public ::testing::Test {
    protected:
        void SetUp() override
        {
            stream = make_stream(k_ring);
        }

        std::shared_ptr<DynamicVideoStream> stream;
    };

    TEST_F(DynamicVideoStreamRingTest, OnlyTheMostRecentFramesStayReadable)
    {
        append_ramp(*stream, 6);

        for (uint64_t f : { 0U, 1U }) {
            EXPECT_FALSE(stream->is_frame_available(f)) << f;
            EXPECT_TRUE(stream->get_frame_pixels(f).empty()) << f;
        }
        for (uint64_t f = 2; f < 6; ++f) {
            EXPECT_TRUE(stream->is_frame_available(f)) << f;
            EXPECT_EQ(pixels_of(*stream, f), frame_bytes(f)) << f;
        }
    }

    TEST_F(DynamicVideoStreamRingTest, TheHeadKeepsCountingPastTheCapacity)
    {
        append_ramp(*stream, 10);

        EXPECT_EQ(stream->get_write_head(), 10U);
        EXPECT_EQ(stream->get_num_frames(), 10U);
        EXPECT_EQ(stream->get_cache_head(), 9U);
        EXPECT_EQ(stream->get_ring_capacity(), k_ring);
    }

    TEST_F(DynamicVideoStreamRingTest, ARingNeverReachesItsEnd)
    {
        append_ramp(*stream, 2);

        stream->set_read_position({ 100 });

        EXPECT_FALSE(stream->is_at_end());
    }

    TEST_F(DynamicVideoStreamRingTest, AnOldFrameIsOverwrittenByTheNextLap)
    {
        append_ramp(*stream, 4);
        const auto before = pixels_of(*stream, 1);

        append_ramp(*stream, 1, 4);

        EXPECT_FALSE(stream->is_frame_available(0));
        EXPECT_TRUE(stream->is_frame_available(1));
        EXPECT_EQ(pixels_of(*stream, 1), before);
        EXPECT_EQ(pixels_of(*stream, 4), frame_bytes(4));
    }

    TEST_F(DynamicVideoStreamRingTest, ASnapshotUnrollsTheWrap)
    {
        append_ramp(*stream, 6);

        const auto all = stream->snapshot(k_ring);
        const auto newest = stream->snapshot(2);

        ASSERT_NE(all, nullptr);
        ASSERT_NE(newest, nullptr);
        EXPECT_EQ(all->get_num_frames(), k_ring);
        for (uint64_t f = 0; f < k_ring; ++f) {
            EXPECT_EQ(pixels_of(*all, f), frame_bytes(f + 2)) << f;
        }
        EXPECT_EQ(newest->get_num_frames(), 2U);
        EXPECT_EQ(pixels_of(*newest, 0), frame_bytes(4));
        EXPECT_EQ(pixels_of(*newest, 1), frame_bytes(5));
    }

    TEST_F(DynamicVideoStreamRingTest, ASnapshotIsCappedAtTheCapacity)
    {
        append_ramp(*stream, 6);

        const auto copy = stream->snapshot(100);

        ASSERT_NE(copy, nullptr);
        EXPECT_EQ(copy->get_num_frames(), k_ring);
    }

    TEST_F(DynamicVideoStreamRingTest, APartlyFilledRingSnapshotsOnlyWhatWasWritten)
    {
        append_ramp(*stream, 2);

        const auto copy = stream->snapshot(k_ring);

        ASSERT_NE(copy, nullptr);
        EXPECT_EQ(copy->get_num_frames(), 2U);
        EXPECT_EQ(pixels_of(*copy, 0), frame_bytes(0));
    }

    TEST_F(DynamicVideoStreamRingTest, ClearKeepsTheCapacityAndDropsEveryFrame)
    {
        append_ramp(*stream, 6);

        stream->clear();

        EXPECT_TRUE(stream->is_ring_mode());
        EXPECT_EQ(stream->get_ring_capacity(), k_ring);
        EXPECT_EQ(stream->get_write_head(), 0U);
        EXPECT_EQ(stream->get_cache_head(), 0U);
        for (uint64_t f = 0; f < 6; ++f) {
            EXPECT_FALSE(stream->is_frame_available(f)) << f;
        }
    }

    TEST_F(DynamicVideoStreamRingTest, ReservingRoomChangesNothing)
    {
        append_ramp(*stream, 2);

        EXPECT_NO_THROW(stream->ensure_capacity(100));

        EXPECT_EQ(stream->get_ring_capacity(), k_ring);
        EXPECT_EQ(stream->get_write_head(), 2U);
        EXPECT_EQ(pixels_of(*stream, 1), frame_bytes(1));
    }

    TEST_F(DynamicVideoStreamRingTest, APlayheadLeftBehindRepeatsItsLastOutput)
    {
        stream->create_default_processor();
        append_ramp(*stream, 4);
        stream->process_default();
        ASSERT_EQ(processed_bytes(*stream), frame_bytes(0));

        append_ramp(*stream, 6, 4);
        stream->process_default();

        EXPECT_FALSE(stream->is_frame_available(1));
        EXPECT_EQ(processed_bytes(*stream), frame_bytes(0));
    }

    TEST_F(DynamicVideoStreamRingTest, LaggingHoldsTheFirstFrameUntilTheLagHasElapsed)
    {
        stream = make_stream(k_roomy_ring);
        stream->lag_behind_head(2.5);
        append_ramp(*stream, 8);
        const auto seek = stream->seek_processor();
        std::vector<std::vector<uint8_t>> seen;

        for (int i = 0; i < 6; ++i) {
            seek->process(stream);
            seen.push_back(processed_bytes(*stream));
        }

        EXPECT_EQ(seen[0], frame_bytes(0));
        EXPECT_EQ(seen[3], frame_bytes(0));
        EXPECT_EQ(seen[4], frame_bytes(1));
        EXPECT_EQ(seen[5], frame_bytes(2));
    }

} // namespace
} // namespace MayaFlux::Test
