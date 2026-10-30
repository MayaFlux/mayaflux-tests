#include "gtest/gtest.h"

#include "../video_test_support.hpp"

#include "MayaFlux/Kakshya/NDData/DataAccess.hpp"
#include "MayaFlux/Kakshya/Region/RegionSegment.hpp"

namespace MayaFlux::Test {
namespace {

    using Kakshya::DataDimension;
    using Kakshya::DataModality;
    using Kakshya::ImageFormat;
    using Kakshya::ProcessingState;
    using Kakshya::Region;
    using Kakshya::RegionGroup;
    using Kakshya::VideoStreamContainer;
    using namespace VideoSupport;

    struct FormatCase {
        ImageFormat format;
        uint32_t channels;
        size_t bytes_per_pixel;
        std::type_index element;
        DataModality modality;
        const char* name;
    };

    class VideoStreamContainerFormatTest : public ::testing::TestWithParam<FormatCase> {
    };

    INSTANTIATE_TEST_SUITE_P(Formats, VideoStreamContainerFormatTest,
        ::testing::Values(
            FormatCase { ImageFormat::R8, 1, 1, typeid(uint8_t), DataModality::VIDEO_COLOR, "R8" },
            FormatCase { ImageFormat::RG8, 2, 2, typeid(uint8_t), DataModality::VIDEO_COLOR, "RG8" },
            FormatCase { ImageFormat::RGB8, 3, 3, typeid(uint8_t), DataModality::VIDEO_COLOR, "RGB8" },
            FormatCase { ImageFormat::RGBA8, 4, 4, typeid(uint8_t), DataModality::VIDEO_COLOR, "RGBA8" },
            FormatCase { ImageFormat::BGRA8, 4, 4, typeid(uint8_t), DataModality::VIDEO_COLOR, "BGRA8" },
            FormatCase { ImageFormat::R16, 1, 2, typeid(uint16_t), DataModality::VIDEO_COLOR, "R16" },
            FormatCase { ImageFormat::RGBA16, 4, 8, typeid(uint16_t), DataModality::VIDEO_COLOR, "RGBA16" },
            FormatCase { ImageFormat::R32F, 1, 4, typeid(float), DataModality::VIDEO_COLOR, "R32F" },
            FormatCase { ImageFormat::RGBA32F, 4, 16, typeid(float), DataModality::VIDEO_COLOR, "RGBA32F" },
            FormatCase { ImageFormat::DEPTH16, 1, 2, typeid(uint16_t), DataModality::VIDEO_DEPTH, "DEPTH16" },
            FormatCase { ImageFormat::DEPTH32F, 1, 4, typeid(float), DataModality::VIDEO_DEPTH, "DEPTH32F" }),
        [](const ::testing::TestParamInfo<FormatCase>& info) { return std::string(info.param.name); });

    TEST_P(VideoStreamContainerFormatTest, ConstructionDerivesStorageFromTheFormat)
    {
        const auto& c = GetParam();

        const VideoStreamContainer video(k_width, k_height, c.format, k_fps);

        EXPECT_EQ(video.get_format(), c.format);
        EXPECT_EQ(video.get_channels(), c.channels);
        EXPECT_EQ(video.get_bytes_per_pixel(), c.bytes_per_pixel);
        EXPECT_EQ(video.value_element_type(), c.element);
        EXPECT_EQ(video.get_structure().modality, c.modality);
        EXPECT_EQ(video.get_frame_byte_size(), k_width * k_height * c.bytes_per_pixel);
        EXPECT_EQ(video.get_frame_element_count(), k_width * k_height * c.channels);
        EXPECT_EQ(video.get_frame_size(), k_width * k_height * c.channels);
    }

    TEST_P(VideoStreamContainerFormatTest, TheComponentAxisIsDepthOnlyForRangeFormats)
    {
        const auto& c = GetParam();
        const VideoStreamContainer video(k_width, k_height, c.format, k_fps);

        const auto dims = video.get_dimensions();

        ASSERT_EQ(dims.size(), 4U);
        EXPECT_EQ(dims[0].role, DataDimension::Role::TIME);
        EXPECT_EQ(dims[1].role, DataDimension::Role::SPATIAL_Y);
        EXPECT_EQ(dims[2].role, DataDimension::Role::SPATIAL_X);
        EXPECT_EQ(dims[3].role, c.modality == DataModality::VIDEO_DEPTH ? DataDimension::Role::DEPTH : DataDimension::Role::CHANNEL);
        EXPECT_EQ(dims[1].size, k_height);
        EXPECT_EQ(dims[2].size, k_width);
        EXPECT_EQ(dims[3].size, c.channels);
    }

    TEST(VideoStreamContainerTest, FormatsWithoutAnElementTypeAreRefused)
    {
        EXPECT_THROW(VideoStreamContainer(k_width, k_height, ImageFormat::DEPTH24, k_fps), std::invalid_argument);
        EXPECT_THROW(VideoStreamContainer(k_width, k_height, ImageFormat::DEPTH24_STENCIL8, k_fps), std::invalid_argument);
    }

    TEST(VideoStreamContainerTest, ADefaultContainerIsEmptyAndIdle)
    {
        const VideoStreamContainer video;

        EXPECT_EQ(video.get_width(), 0U);
        EXPECT_EQ(video.get_height(), 0U);
        EXPECT_EQ(video.get_num_frames(), 0U);
        EXPECT_EQ(video.get_total_elements(), 0U);
        EXPECT_FALSE(video.has_data());
        EXPECT_FALSE(video.is_ready());
        EXPECT_FALSE(video.is_ring_mode());
        EXPECT_EQ(video.get_raw_data(), nullptr);
        EXPECT_EQ(video.get_processing_state(), ProcessingState::IDLE);
        EXPECT_EQ(video.get_temporal_rate(), 0U);
        EXPECT_TRUE(video.get_frame_pixels(0).empty());
    }

    TEST(VideoStreamContainerTest, ConstructionReportsItsGeometryAndRate)
    {
        const VideoStreamContainer video(k_width, k_height, ImageFormat::RGBA8, k_fps);

        EXPECT_EQ(video.get_width(), k_width);
        EXPECT_EQ(video.get_height(), k_height);
        EXPECT_DOUBLE_EQ(video.get_frame_rate(), k_fps);
        EXPECT_EQ(video.get_num_frames(), 0U);
        EXPECT_EQ(video.get_memory_layout(), Kakshya::MemoryLayout::ROW_MAJOR);
    }

    TEST(VideoStreamContainerTest, TimeAndPositionConvertAtTheFrameRate)
    {
        const VideoStreamContainer video(k_width, k_height, ImageFormat::RGBA8, k_fps);

        EXPECT_EQ(video.get_temporal_rate(), 30U);
        EXPECT_EQ(video.time_to_position(2.0), 60U);
        EXPECT_DOUBLE_EQ(video.position_to_time(15), 0.5);
    }

    TEST(VideoStreamContainerTest, WithoutAFrameRateTimeConversionsGiveZero)
    {
        const VideoStreamContainer video(k_width, k_height, ImageFormat::RGBA8, 0.0);

        EXPECT_EQ(video.time_to_position(2.0), 0U);
        EXPECT_DOUBLE_EQ(video.position_to_time(15), 0.0);
    }

    class VideoStreamContainerRingTest : public ::testing::Test {
    protected:
        static constexpr uint64_t k_total = 10;
        static constexpr uint32_t k_capacity = 4;

        void SetUp() override
        {
            video = make_ring_rgba8(k_total, k_capacity);
        }

        std::shared_ptr<VideoStreamContainer> video;
    };

    TEST_F(VideoStreamContainerRingTest, SettingUpARingReportsTheSourceExtent)
    {
        EXPECT_TRUE(video->is_ring_mode());
        EXPECT_EQ(video->get_ring_capacity(), k_capacity);
        EXPECT_EQ(video->get_total_source_frames(), k_total);
        EXPECT_EQ(video->get_num_frames(), k_total);
        EXPECT_EQ(video->get_total_elements(), k_total * k_width * k_height * 4);
        EXPECT_TRUE(video->has_data());
        EXPECT_TRUE(video->is_ready());
        EXPECT_EQ(video->get_processing_state(), ProcessingState::IDLE);
        EXPECT_EQ(video->get_cache_head(), 0U);
        EXPECT_DOUBLE_EQ(video->get_frame_rate(), k_fps);
        EXPECT_EQ(video->get_dimensions().at(0).size, k_total);
    }

    TEST_F(VideoStreamContainerRingTest, NoFrameIsAvailableUntilItIsCommitted)
    {
        for (uint64_t f = 0; f < k_total; ++f) {
            EXPECT_FALSE(video->is_frame_available(f)) << f;
            EXPECT_TRUE(video->get_frame_pixels(f).empty()) << f;
        }
    }

    TEST_F(VideoStreamContainerRingTest, ACommittedFrameIsAvailableWithItsPixels)
    {
        commit_pattern(*video, 3);

        EXPECT_TRUE(video->is_frame_available(3));
        EXPECT_FALSE(video->is_frame_available(2));
        EXPECT_FALSE(video->is_frame_available(7));
        EXPECT_EQ(pixels_of(*video, 3), frame_bytes(3));
        EXPECT_EQ(video->get_cache_head(), 3U);
    }

    TEST_F(VideoStreamContainerRingTest, FramesOneCapacityApartShareASlot)
    {
        EXPECT_EQ(video->mutable_slot_ptr(1), video->mutable_slot_ptr(1 + k_capacity));
        EXPECT_EQ(video->mutable_slot_ptr(2) - video->mutable_slot_ptr(1), static_cast<std::ptrdiff_t>(k_rgba8_frame_bytes));
    }

    TEST_F(VideoStreamContainerRingTest, CommittingTheNextLapEvictsTheOldFrame)
    {
        commit_pattern(*video, 1);
        commit_pattern(*video, 1 + k_capacity);

        EXPECT_FALSE(video->is_frame_available(1));
        EXPECT_TRUE(video->is_frame_available(1 + k_capacity));
        EXPECT_TRUE(video->get_frame_pixels(1).empty());
        EXPECT_EQ(pixels_of(*video, 1 + k_capacity), frame_bytes(1 + k_capacity));
    }

    TEST_F(VideoStreamContainerRingTest, SlotsPastTheSourceHaveNoPointer)
    {
        EXPECT_NE(video->mutable_slot_ptr(k_total - 1), nullptr);
        EXPECT_EQ(video->mutable_slot_ptr(k_total), nullptr);
        EXPECT_EQ(video->mutable_slot_ptr(k_total + 100), nullptr);
    }

    TEST_F(VideoStreamContainerRingTest, FramesPastTheSourceAreNeverAvailable)
    {
        EXPECT_TRUE(video->get_frame_pixels(k_total).empty());
        EXPECT_TRUE(video->get_frame_pixels(k_total + 100).empty());
    }

    TEST_F(VideoStreamContainerRingTest, TheCacheHeadOnlyMovesForward)
    {
        commit_pattern(*video, 5);
        commit_pattern(*video, 2);
        EXPECT_EQ(video->get_cache_head(), 5U);

        video->advance_cache_head(3);
        EXPECT_EQ(video->get_cache_head(), 5U);

        video->advance_cache_head(8);
        EXPECT_EQ(video->get_cache_head(), 8U);
    }

    TEST_F(VideoStreamContainerRingTest, InvalidatingTheRingDropsEveryFrame)
    {
        commit_pattern(*video, 0);
        commit_pattern(*video, 1);

        video->invalidate_ring();

        EXPECT_FALSE(video->is_frame_available(0));
        EXPECT_FALSE(video->is_frame_available(1));
        EXPECT_TRUE(video->get_frame_pixels(0).empty());
    }

    TEST_F(VideoStreamContainerRingTest, SettingUpAgainResetsTheRing)
    {
        commit_pattern(*video, 2);

        video->setup_ring(6, 2, k_width, k_height, ImageFormat::RGBA8, k_fps, 0);

        EXPECT_EQ(video->get_ring_capacity(), 2U);
        EXPECT_EQ(video->get_total_source_frames(), 6U);
        EXPECT_EQ(video->get_cache_head(), 0U);
        EXPECT_FALSE(video->is_frame_available(2));
    }

    TEST_F(VideoStreamContainerRingTest, SettingUpWithAnotherFormatResizesTheSlots)
    {
        video->setup_ring(4, 2, k_width, k_height, ImageFormat::RGBA32F, k_fps, 0);

        EXPECT_EQ(video->get_format(), ImageFormat::RGBA32F);
        EXPECT_EQ(video->get_frame_byte_size(), k_width * k_height * 16U);
        EXPECT_EQ(video->value_element_type(), std::type_index(typeid(float)));
        commit_pattern(*video, 1);
        EXPECT_EQ(video->get_frame_pixels(1).size(), k_width * k_height * 16U);
    }

    TEST_F(VideoStreamContainerRingTest, SettingUpWithADepthFormatSwitchesTheModality)
    {
        video->setup_ring(4, 2, k_width, k_height, ImageFormat::DEPTH32F, k_fps, 0);

        EXPECT_EQ(video->get_structure().modality, DataModality::VIDEO_DEPTH);
        EXPECT_EQ(video->get_dimensions().at(3).role, DataDimension::Role::DEPTH);
    }

    TEST_F(VideoStreamContainerRingTest, RawDataIsNotExposedInRingMode)
    {
        EXPECT_EQ(video->get_raw_data(), nullptr);
    }

    TEST_F(VideoStreamContainerRingTest, ReachingTheEndFollowsTheSourceNotTheRing)
    {
        video->set_read_position({ k_total - 1 });
        EXPECT_FALSE(video->is_at_end());

        video->set_read_position({ k_total });
        EXPECT_TRUE(video->is_at_end());
    }

    TEST_F(VideoStreamContainerRingTest, ReadPositionUpdatesDoNotThrowWithoutAnIOService)
    {
        video->set_refill_threshold(4);

        EXPECT_NO_THROW(video->update_read_position_for_channel(0, 3));

        EXPECT_EQ(video->get_read_position(), std::vector<uint64_t> { 3 });
    }

    TEST_F(VideoStreamContainerRingTest, ValuesReadOnlyFromCommittedFrames)
    {
        auto probe = std::make_shared<VideoProbe>();
        probe->setup_ring(k_total, k_capacity, k_width, k_height, ImageFormat::RGBA8, k_fps, 0);
        commit_pattern(*probe, 2);

        const uint8_t committed = probe->read_value<uint8_t>({ 2, 1, 3, 2 }, 77);
        const uint8_t missing = probe->read_value<uint8_t>({ 3, 1, 3, 2 }, 77);

        EXPECT_EQ(committed, frame_bytes(2)[(1 * k_width + 3) * 4 + 2]);
        EXPECT_EQ(missing, 77);
    }

    class VideoStreamContainerRefillTest : public ::testing::Test {
    protected:
        static constexpr uint64_t k_reader = 42;

        void ring_with_threshold(uint32_t threshold)
        {
            video = std::make_shared<VideoStreamContainer>();
            video->setup_ring(10, 4, k_width, k_height, ImageFormat::RGBA8, k_fps, threshold, k_reader);
        }

        RecordingIOService io;
        std::shared_ptr<VideoStreamContainer> video;
    };

    TEST_F(VideoStreamContainerRefillTest, RunningLowOnBufferedFramesRequestsADecode)
    {
        ring_with_threshold(3);
        for (uint64_t f = 0; f < 5; ++f) {
            commit_pattern(*video, f);
        }

        video->update_read_position_for_channel(0, 3);

        EXPECT_EQ(io.decodes, std::vector<uint64_t> { k_reader });
    }

    TEST_F(VideoStreamContainerRefillTest, PlentyOfBufferedFramesRequestsNothing)
    {
        ring_with_threshold(3);
        for (uint64_t f = 0; f < 5; ++f) {
            commit_pattern(*video, f);
        }

        video->update_read_position_for_channel(0, 1);

        EXPECT_TRUE(io.decodes.empty());
    }

    TEST_F(VideoStreamContainerRefillTest, AReaderAheadOfTheCacheRequestsADecode)
    {
        ring_with_threshold(3);
        commit_pattern(*video, 0);

        video->update_read_position_for_channel(0, 6);

        EXPECT_EQ(io.decodes, std::vector<uint64_t> { k_reader });
    }

    TEST_F(VideoStreamContainerRefillTest, AThresholdOfZeroNeverRequests)
    {
        ring_with_threshold(0);

        video->update_read_position_for_channel(0, 5);

        EXPECT_TRUE(io.decodes.empty());
    }

    TEST_F(VideoStreamContainerRefillTest, TheThresholdCanBeChangedAfterSetup)
    {
        ring_with_threshold(0);
        commit_pattern(*video, 0);
        video->set_refill_threshold(3);

        video->update_read_position_for_channel(0, 1);

        EXPECT_EQ(io.decodes, std::vector<uint64_t> { k_reader });
    }

    TEST_F(VideoStreamContainerRefillTest, AServiceWithoutADecodeRequestIsTolerated)
    {
        io.service().request_decode = nullptr;
        ring_with_threshold(3);

        EXPECT_NO_THROW(video->update_read_position_for_channel(0, 3));
        EXPECT_EQ(video->get_read_position(), std::vector<uint64_t> { 3 });
    }

    class VideoStreamContainerFlatTest : public ::testing::Test {
    protected:
        static constexpr uint64_t k_frames = 3;

        void SetUp() override
        {
            video = make_flat_rgba8(k_frames);
        }

        std::shared_ptr<VideoProbe> video;
    };

    TEST_F(VideoStreamContainerFlatTest, LoadedFramesAreSizedAndReadable)
    {
        EXPECT_FALSE(video->is_ring_mode());
        EXPECT_TRUE(video->has_data());
        EXPECT_TRUE(video->is_ready());
        EXPECT_EQ(video->get_num_frames(), k_frames);
        EXPECT_EQ(video->get_total_elements(), k_frames * k_rgba8_frame_bytes);
        EXPECT_EQ(video->get_dimensions().at(0).size, k_frames);
        EXPECT_NE(video->get_raw_data(), nullptr);
    }

    TEST_F(VideoStreamContainerFlatTest, EachFrameReadsBackItsPixels)
    {
        for (uint64_t f = 0; f < k_frames; ++f) {
            EXPECT_EQ(pixels_of(*video, f), frame_bytes(f)) << f;
        }
    }

    TEST_F(VideoStreamContainerFlatTest, FramesPastTheEndHaveNoPixels)
    {
        EXPECT_TRUE(video->get_frame_pixels(k_frames).empty());
        EXPECT_TRUE(video->get_frame_pixels(1000).empty());
    }

    TEST_F(VideoStreamContainerFlatTest, TheTypedFrameViewMatchesTheStorage)
    {
        const auto frame = video->get_frame(1);

        const auto values = frame.as<uint8_t>();
        EXPECT_EQ(std::vector<uint8_t>(values.begin(), values.end()), frame_bytes(1));
    }

    TEST_F(VideoStreamContainerFlatTest, ClearEmptiesTheContainerAndRewinds)
    {
        video->set_read_position({ 2 });

        video->clear();

        EXPECT_FALSE(video->has_data());
        EXPECT_EQ(video->get_num_frames(), 0U);
        EXPECT_EQ(video->get_total_elements(), 0U);
        EXPECT_EQ(video->get_read_position(), std::vector<uint64_t> { 0 });
        EXPECT_EQ(video->get_processing_state(), ProcessingState::IDLE);
        EXPECT_EQ(video->get_dimensions().at(0).size, 0U);
    }

    TEST_F(VideoStreamContainerFlatTest, ReadPositionIsSetAdvancedAndReset)
    {
        video->set_read_position({ 1 });
        EXPECT_EQ(video->get_read_position(), std::vector<uint64_t> { 1 });

        video->advance_read_position({ 2 });
        EXPECT_EQ(video->get_read_position(), std::vector<uint64_t> { 3 });

        video->reset_read_position();
        EXPECT_EQ(video->get_read_position(), std::vector<uint64_t> { 0 });
    }

    TEST_F(VideoStreamContainerFlatTest, ChannelUpdatesSetTheSingleTemporalPosition)
    {
        video->update_read_position_for_channel(3, 2);

        EXPECT_EQ(video->get_read_position(), std::vector<uint64_t> { 2 });
    }

    TEST_F(VideoStreamContainerFlatTest, TheEndIsReachedAtTheFrameCountUnlessLooping)
    {
        video->set_read_position({ k_frames - 1 });
        EXPECT_FALSE(video->is_at_end());

        video->set_read_position({ k_frames });
        EXPECT_TRUE(video->is_at_end());

        video->set_looping(true);
        EXPECT_FALSE(video->is_at_end());
    }

    TEST(VideoStreamContainerTest, AnEmptyContainerIsAtItsEnd)
    {
        EXPECT_TRUE(VideoStreamContainer(k_width, k_height, ImageFormat::RGBA8, k_fps).is_at_end());
    }

    TEST_F(VideoStreamContainerFlatTest, RemainingFramesCountDownAndStopAtZero)
    {
        video->set_read_position({ 1 });
        EXPECT_EQ(video->get_remaining_frames(), std::vector<uint64_t> { 2 });

        video->set_read_position({ 10 });
        EXPECT_EQ(video->get_remaining_frames(), std::vector<uint64_t> { 0 });
    }

    TEST_F(VideoStreamContainerFlatTest, SequentialReadsAdvanceByFramesAndClampAtTheEnd)
    {
        std::vector<double> out(4, -1.0);

        EXPECT_EQ(video->read_sequential(out, 2), 2U);
        EXPECT_EQ(video->get_read_position(), std::vector<uint64_t> { 2 });
        EXPECT_EQ(out, std::vector<double>(4, 0.0));

        EXPECT_EQ(video->read_sequential(out, 5), 1U);
        EXPECT_EQ(video->get_read_position(), std::vector<uint64_t> { 3 });
        EXPECT_EQ(video->read_sequential(out, 1), 0U);
    }

    TEST_F(VideoStreamContainerFlatTest, PeekingYieldsNothingAndDoesNotMove)
    {
        std::vector<double> out(4, -1.0);

        EXPECT_EQ(video->peek_sequential(out, 2, 0), 0U);

        EXPECT_EQ(out, std::vector<double>(4, 0.0));
        EXPECT_EQ(video->get_read_position(), std::vector<uint64_t> { 0 });
    }

    TEST_F(VideoStreamContainerFlatTest, LoopRegionsAreStoredAndLoopingToggles)
    {
        const Region region(std::vector<uint64_t> { 1, 0, 0, 0 }, std::vector<uint64_t> { 2, 1, 3, 3 });

        video->set_loop_region(region);
        video->set_looping(true);

        EXPECT_TRUE(video->is_looping());
        EXPECT_EQ(video->get_loop_region().start_coordinates, region.start_coordinates);
        EXPECT_EQ(video->get_loop_region().end_coordinates, region.end_coordinates);

        video->set_looping(false);
        EXPECT_FALSE(video->is_looping());
    }

    TEST_F(VideoStreamContainerFlatTest, CoordinatesMapToLinearIndicesAndBack)
    {
        const std::vector<uint64_t> coords { 1, 1, 2, 3 };

        const uint64_t linear = video->coordinates_to_linear_index(coords);

        EXPECT_EQ(linear, 1 * k_rgba8_frame_bytes + (1 * k_width + 2) * 4 + 3);
        EXPECT_EQ(video->linear_index_to_coordinates(linear), coords);
    }

    TEST_F(VideoStreamContainerFlatTest, ValuesRoundTripAsBytes)
    {
        const std::vector<uint64_t> at { 2, 1, 3, 2 };
        const size_t offset = (1 * k_width + 3) * 4 + 2;

        EXPECT_EQ(video->read_value<uint8_t>(at, 0), frame_bytes(2)[offset]);

        video->write_value<uint8_t>(at, 200);

        EXPECT_EQ(video->read_value<uint8_t>(at, 0), 200);
        EXPECT_EQ(pixels_of(*video, 2)[(1 * k_width + 3) * 4 + 2], 200);
    }

    TEST_F(VideoStreamContainerFlatTest, NormalisedValuesRoundTripThroughBytes)
    {
        const std::vector<uint64_t> at { 0, 0, 1, 0 };

        video->write_value<float>(at, 1.0F);
        EXPECT_EQ(video->read_value<uint8_t>(at, 0), 255);
        EXPECT_FLOAT_EQ(video->read_value<float>(at, -1.0F), 1.0F);

        video->write_value<double>(at, 0.0);
        EXPECT_EQ(video->read_value<uint8_t>(at, 7), 0);
        EXPECT_DOUBLE_EQ(video->read_value<double>(at, -1.0), 0.0);
    }

    TEST_F(VideoStreamContainerFlatTest, OutOfRangeAndShortCoordinatesAreIgnored)
    {
        const std::vector<std::vector<uint64_t>> bad {
            { k_frames, 0, 0, 0 },
            { 0, k_height, 0, 0 },
            { 0, 0, k_width, 0 },
            { 0, 0, 0, 4 },
            { 0, 0, 0 }
        };

        for (const auto& coords : bad) {
            EXPECT_EQ(video->read_value<uint8_t>(coords, 77), 77);
            video->write_value<uint8_t>(coords, 5);
        }

        EXPECT_EQ(pixels_of(*video, 0), frame_bytes(0));
    }

    TEST_F(VideoStreamContainerFlatTest, ValuesOfAnotherElementWidthAreIgnored)
    {
        const std::vector<uint64_t> at { 0, 0, 0, 0 };

        EXPECT_EQ(video->read_value<uint16_t>(at, 77), 77);
        video->write_value<uint16_t>(at, 5);

        EXPECT_EQ(pixels_of(*video, 0), frame_bytes(0));
    }

    TEST(VideoStreamContainerTest, SixteenBitValuesRoundTrip)
    {
        auto video = std::make_shared<VideoProbe>(2, 2, ImageFormat::R16, k_fps);
        video->load<uint16_t>(std::vector<uint16_t>(8, 0), 2);

        video->write_value<uint16_t>({ 1, 1, 0, 0 }, 4321);

        EXPECT_EQ(video->read_value<uint16_t>({ 1, 1, 0, 0 }, 0), 4321);
        EXPECT_EQ(video->read_value<uint16_t>({ 0, 1, 0, 0 }, 9), 0);
        EXPECT_EQ(video->read_value<uint8_t>({ 1, 1, 0, 0 }, 77), 77);
    }

    TEST(VideoStreamContainerTest, FloatValuesRoundTrip)
    {
        auto video = std::make_shared<VideoProbe>(2, 2, ImageFormat::DEPTH32F, k_fps);
        video->load<float>(std::vector<float>(8, 0.0F), 2);

        video->write_value<float>({ 1, 0, 1, 0 }, 0.25F);

        EXPECT_FLOAT_EQ(video->read_value<float>({ 1, 0, 1, 0 }, -1.0F), 0.25F);
        EXPECT_FLOAT_EQ(video->read_value<float>({ 0, 0, 1, 0 }, -1.0F), 0.0F);
    }

    TEST_F(VideoStreamContainerFlatTest, FramesCopyOutContiguously)
    {
        std::vector<uint8_t> out(2 * k_rgba8_frame_bytes, 0);

        video->copy_frames(out, 1, 2);

        EXPECT_EQ(out, frames_bytes(1, 2));
    }

    TEST_F(VideoStreamContainerFlatTest, CopyingWithTheWrongElementTypeIsRefused)
    {
        std::vector<float> out(k_rgba8_frame_bytes);

        EXPECT_THROW(video->copy_frames(out, 0, 1), std::runtime_error);
    }

    TEST_F(VideoStreamContainerFlatTest, CopyingIntoATooSmallBufferIsRefused)
    {
        std::vector<uint8_t> out(k_rgba8_frame_bytes - 1);

        EXPECT_THROW(video->copy_frames(out, 0, 1), std::runtime_error);
    }

    TEST_F(VideoStreamContainerFlatTest, RegionReadsReturnTheSelectedFrame)
    {
        const Region region(std::vector<uint64_t> { 1, 0, 0, 0 }, std::vector<uint64_t> { 1, k_height - 1, k_width - 1, 3 });

        const auto data = video->get_region_data(region);

        ASSERT_EQ(data.size(), 1U);
        EXPECT_EQ(std::get<std::vector<uint8_t>>(data[0]), frame_bytes(1));
    }

    TEST(VideoStreamContainerTest, RegionReadsOfAnEmptyContainerAreEmpty)
    {
        const VideoStreamContainer video(k_width, k_height, ImageFormat::RGBA8, k_fps);

        EXPECT_TRUE(video.get_region_data(Region(std::vector<uint64_t> { 0, 0, 0, 0 }, std::vector<uint64_t> { 0, 0, 0, 0 })).empty());
    }

    TEST_F(VideoStreamContainerFlatTest, RegionGroupAndSegmentReadsReturnTheWholeStore)
    {
        EXPECT_EQ(video->get_region_group_data(RegionGroup("all")).size(), 1U);
        EXPECT_EQ(video->get_segments_data({}).size(), 1U);
    }

    TEST_F(VideoStreamContainerFlatTest, RegionWritesLeaveTheDataAlone)
    {
        const Region region(std::vector<uint64_t> { 0, 0, 0, 0 }, std::vector<uint64_t> { 0, 0, 0, 0 });

        EXPECT_NO_THROW(video->set_region_data(region, { std::vector<uint8_t> { 1 } }));

        EXPECT_EQ(pixels_of(*video, 0), frame_bytes(0));
    }

    TEST(VideoStreamContainerTest, RegionGroupsAreStoredByNameAndRemovable)
    {
        VideoStreamContainer video(k_width, k_height, ImageFormat::RGBA8, k_fps);
        RegionGroup group("hits");
        group.add_region(Region(std::vector<uint64_t> { 0, 0, 0, 0 }, std::vector<uint64_t> { 1, 1, 1, 1 }));

        video.add_region_group(group);

        EXPECT_EQ(video.get_region_group("hits").regions.size(), 1U);
        EXPECT_EQ(video.get_all_region_groups().size(), 1U);

        video.remove_region_group("hits");

        EXPECT_TRUE(video.get_all_region_groups().empty());
        EXPECT_TRUE(video.get_region_group("hits").regions.empty());
    }

    TEST_F(VideoStreamContainerFlatTest, RegionsCountAsAlwaysLoaded)
    {
        const Region region(std::vector<uint64_t> { 0, 0, 0, 0 }, std::vector<uint64_t> { 0, 0, 0, 0 });

        EXPECT_TRUE(video->is_region_loaded(region));
    }

    TEST_F(VideoStreamContainerFlatTest, ChannelAccessExposesTheInterleavedStore)
    {
        const auto all = video->all_channel_data();

        ASSERT_EQ(all.size(), 1U);
        EXPECT_EQ(all[0].element_count(), k_frames * k_rgba8_frame_bytes);
        EXPECT_EQ(video->channel_data(3).element_count(), k_frames * k_rgba8_frame_bytes);
    }

    TEST(VideoStreamContainerTest, ChannelAccessOfAnEmptyContainerHasNothing)
    {
        VideoStreamContainer video(k_width, k_height, ImageFormat::RGBA8, k_fps);

        EXPECT_TRUE(video.all_channel_data().empty());
        EXPECT_EQ(video.channel_data(0).element_count(), 0U);
    }

    TEST_F(VideoStreamContainerFlatTest, ProcessedFramesNormaliseToFloat)
    {
        video->get_processed_data() = { std::vector<uint8_t> { 0, 51, 255 } };

        const auto values = video->processed_frame_as_float(0);

        ASSERT_EQ(values.size(), 3U);
        EXPECT_FLOAT_EQ(values[0], 0.0F);
        EXPECT_FLOAT_EQ(values[1], 0.2F);
        EXPECT_FLOAT_EQ(values[2], 1.0F);
    }

    TEST_F(VideoStreamContainerFlatTest, ProcessedFramesOutOfRangeAreEmpty)
    {
        EXPECT_TRUE(video->processed_frame_as_float(0).empty());

        video->get_processed_data() = { std::vector<uint8_t> { 1 } };
        EXPECT_TRUE(video->processed_frame_as_float(1).empty());
    }

    TEST_F(VideoStreamContainerFlatTest, InvalidatingTheFloatCacheRecomputesFromTheProcessedData)
    {
        video->get_processed_data() = { std::vector<uint8_t> { 0, 255 } };
        ASSERT_EQ(video->processed_frame_as_float(0).size(), 2U);

        video->get_processed_data() = { std::vector<uint8_t> { 255, 0 } };
        video->invalidate_float_frame_cache(0);

        const auto values = video->processed_frame_as_float(0);
        ASSERT_EQ(values.size(), 2U);
        EXPECT_FLOAT_EQ(values[0], 1.0F);
        EXPECT_FLOAT_EQ(values[1], 0.0F);
    }

    TEST_F(VideoStreamContainerFlatTest, MarkingReadyNeedsDataAndUnmarkingGoesIdle)
    {
        VideoStreamContainer empty(k_width, k_height, ImageFormat::RGBA8, k_fps);
        empty.mark_ready_for_processing(true);
        EXPECT_EQ(empty.get_processing_state(), ProcessingState::IDLE);
        EXPECT_FALSE(empty.is_ready_for_processing());

        video->mark_ready_for_processing(true);
        EXPECT_EQ(video->get_processing_state(), ProcessingState::READY);
        EXPECT_TRUE(video->is_ready_for_processing());

        video->mark_ready_for_processing(false);
        EXPECT_EQ(video->get_processing_state(), ProcessingState::IDLE);
    }

    TEST_F(VideoStreamContainerFlatTest, StateChangesReachTheCallbackOnlyWhenTheStateChanges)
    {
        std::vector<ProcessingState> seen;
        video->register_state_change_callback(
            [&seen](const std::shared_ptr<Kakshya::SignalSourceContainer>&, ProcessingState state) { seen.push_back(state); });

        video->update_processing_state(ProcessingState::READY);
        video->update_processing_state(ProcessingState::READY);
        video->update_processing_state(ProcessingState::PROCESSED);
        video->unregister_state_change_callback();
        video->update_processing_state(ProcessingState::IDLE);

        EXPECT_EQ(seen, (std::vector<ProcessingState> { ProcessingState::READY, ProcessingState::PROCESSED }));
    }

    TEST(VideoStreamContainerTest, ReadersAreCountedAndReleased)
    {
        VideoStreamContainer video(k_width, k_height, ImageFormat::RGBA8, k_fps);
        EXPECT_FALSE(video.has_active_readers());

        const uint32_t first = video.register_dimension_reader(0);
        const uint32_t second = video.register_dimension_reader(0);

        EXPECT_NE(first, second);
        EXPECT_TRUE(video.has_active_readers());

        video.unregister_dimension_reader(0);
        EXPECT_TRUE(video.has_active_readers());
        video.unregister_dimension_reader(0);
        EXPECT_FALSE(video.has_active_readers());
    }

    TEST(VideoStreamContainerTest, UnregisteringWithNoReadersDoesNotUnderflow)
    {
        VideoStreamContainer video(k_width, k_height, ImageFormat::RGBA8, k_fps);

        video.unregister_dimension_reader(0);

        EXPECT_FALSE(video.has_active_readers());
        EXPECT_TRUE(video.all_dimensions_consumed());
    }

    TEST(VideoStreamContainerTest, ConsumptionCompletesOnceEveryReaderHasMarked)
    {
        VideoStreamContainer video(k_width, k_height, ImageFormat::RGBA8, k_fps);
        const uint32_t first = video.register_dimension_reader(0);
        const uint32_t second = video.register_dimension_reader(0);

        EXPECT_FALSE(video.all_dimensions_consumed());
        video.mark_dimension_consumed(0, first);
        EXPECT_FALSE(video.all_dimensions_consumed());
        video.mark_dimension_consumed(0, second);
        EXPECT_TRUE(video.all_dimensions_consumed());
    }

    TEST(VideoStreamContainerTest, ConsumptionCanBeClearedForTheNextCycle)
    {
        VideoStreamContainer video(k_width, k_height, ImageFormat::RGBA8, k_fps);
        const uint32_t reader = video.register_dimension_reader(0);
        video.mark_dimension_consumed(0, reader);
        ASSERT_TRUE(video.all_dimensions_consumed());

        video.clear_all_consumption();

        EXPECT_FALSE(video.all_dimensions_consumed());
        EXPECT_TRUE(video.has_active_readers());
    }

    TEST(VideoStreamContainerTest, ProcessingTokensAreHeldByOneChannelAtATime)
    {
        VideoStreamContainer video(k_width, k_height, ImageFormat::RGBA8, k_fps);

        EXPECT_TRUE(video.try_acquire_processing_token(0));
        EXPECT_FALSE(video.try_acquire_processing_token(1));
        EXPECT_TRUE(video.has_processing_token(0));
        EXPECT_FALSE(video.has_processing_token(1));

        video.reset_processing_token();

        EXPECT_FALSE(video.has_processing_token(0));
        EXPECT_TRUE(video.try_acquire_processing_token(1));
    }

    TEST(VideoStreamContainerTest, TheProcessingChainIsCreatedOnceAndReused)
    {
        VideoStreamContainer video(k_width, k_height, ImageFormat::RGBA8, k_fps);

        const auto chain = video.get_processing_chain();

        ASSERT_NE(chain, nullptr);
        EXPECT_EQ(video.get_processing_chain(), chain);
    }

} // namespace
} // namespace MayaFlux::Test
