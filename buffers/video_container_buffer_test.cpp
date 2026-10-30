#include "gtest/gtest.h"

#include "../kakshya/video_test_support.hpp"

#include "MayaFlux/Buffers/AudioBuffer.hpp"
#include "MayaFlux/Buffers/Container/VideoContainerBuffer.hpp"
#include "MayaFlux/Buffers/Textures/TextureBuffer.hpp"
#include "MayaFlux/Kakshya/Processors/FrameAccessProcessor.hpp"

namespace MayaFlux::Test {
namespace {

    using Buffers::TextureBuffer;
    using Buffers::VideoContainerBuffer;
    using Buffers::VideoStreamReader;
    using Kakshya::DynamicVideoStream;
    using Kakshya::FrameAccessProcessor;
    using Kakshya::ImageFormat;
    using Kakshya::ProcessingState;
    using Kakshya::VideoStreamContainer;
    using namespace VideoSupport;

    constexpr uint64_t k_frames = 3;

    class CallbackGuard {
    public:
        explicit CallbackGuard(std::shared_ptr<Kakshya::StreamContainer> container)
            : m_container(std::move(container))
        {
        }

        CallbackGuard(const CallbackGuard&) = delete;
        CallbackGuard& operator=(const CallbackGuard&) = delete;

        ~CallbackGuard()
        {
            if (m_container) {
                m_container->unregister_state_change_callback();
            }
        }

    private:
        std::shared_ptr<Kakshya::StreamContainer> m_container;
    };

    class VideoStreamReaderTest : public ::testing::Test {
    protected:
        void SetUp() override
        {
            stream = make_flat_rgba8(k_frames);
            stream->create_default_processor();
            frame_processor()->set_global_fps(k_fps);
            buffer = std::make_shared<TextureBuffer>(k_width, k_height, ImageFormat::RGBA8);
        }

        void TearDown() override
        {
            stream->unregister_state_change_callback();
        }

        [[nodiscard]] std::shared_ptr<FrameAccessProcessor> frame_processor() const
        {
            return std::dynamic_pointer_cast<FrameAccessProcessor>(stream->get_default_processor());
        }

        [[nodiscard]] std::shared_ptr<VideoStreamReader> reader_for(const std::shared_ptr<Kakshya::StreamContainer>& container) const
        {
            return std::make_shared<VideoStreamReader>(container);
        }

        [[nodiscard]] std::shared_ptr<VideoStreamReader> reader() const
        {
            return reader_for(stream);
        }

        std::shared_ptr<VideoProbe> stream;
        std::shared_ptr<TextureBuffer> buffer;
    };

    TEST_F(VideoStreamReaderTest, DefaultsToFlagUpdates)
    {
        const auto r = reader();

        EXPECT_TRUE(r->get_update_flags());
        EXPECT_EQ(r->get_container(), stream);
    }

    TEST_F(VideoStreamReaderTest, AttachingRegistersAReaderAndFlagsTheBuffer)
    {
        const auto r = reader();
        buffer->mark_for_processing(false);
        ASSERT_FALSE(stream->has_active_readers());

        r->on_attach(buffer);

        EXPECT_TRUE(stream->has_active_readers());
        EXPECT_TRUE(buffer->has_data_for_cycle());
    }

    TEST_F(VideoStreamReaderTest, DetachingRemovesTheReader)
    {
        const auto r = reader();
        r->on_attach(buffer);

        r->on_detach(buffer);

        EXPECT_FALSE(stream->has_active_readers());
    }

    TEST_F(VideoStreamReaderTest, AttachingBeforeAnyFrameIsProcessedLeavesThePixelsEmpty)
    {
        const auto r = reader();

        r->on_attach(buffer);

        EXPECT_TRUE(buffer->get_pixel_data().empty());
    }

    TEST_F(VideoStreamReaderTest, FillsTheTextureWithTheCurrentFrame)
    {
        const auto r = reader();
        r->on_attach(buffer);

        r->processing_function(buffer);

        EXPECT_EQ(buffer->get_pixel_data(), frame_bytes(0));
    }

    TEST_F(VideoStreamReaderTest, EachPassShowsTheNextFrame)
    {
        const auto r = reader();
        r->on_attach(buffer);

        r->processing_function(buffer);
        r->processing_function(buffer);

        EXPECT_EQ(buffer->get_pixel_data(), frame_bytes(1));
    }

    TEST_F(VideoStreamReaderTest, FillingMarksTheTextureDirty)
    {
        const auto r = reader();
        r->on_attach(buffer);
        buffer->clear_dirty_flag();

        r->processing_function(buffer);

        EXPECT_TRUE(buffer->is_texture_dirty());
    }

    TEST_F(VideoStreamReaderTest, AFinishedPassLeavesTheContainerReadyAndTheTokenFree)
    {
        const auto r = reader();
        r->on_attach(buffer);

        r->processing_function(buffer);

        EXPECT_EQ(stream->get_processing_state(), ProcessingState::READY);
        EXPECT_FALSE(stream->has_processing_token(0));
    }

    TEST_F(VideoStreamReaderTest, FillingFlagsTheBufferForProcessingUnlessFlagUpdatesAreOff)
    {
        const auto flagging = reader();
        flagging->on_attach(buffer);
        buffer->mark_for_processing(false);
        flagging->processing_function(buffer);
        EXPECT_TRUE(buffer->has_data_for_cycle());

        const auto quiet_buffer = std::make_shared<TextureBuffer>(k_width, k_height, ImageFormat::RGBA8);
        const auto quiet = reader();
        quiet->set_update_flags(false);
        quiet->on_attach(quiet_buffer);
        quiet_buffer->mark_for_processing(false);
        quiet->processing_function(quiet_buffer);

        EXPECT_FALSE(quiet_buffer->has_data_for_cycle());
        EXPECT_FALSE(quiet_buffer->get_pixel_data().empty());
    }

    TEST_F(VideoStreamReaderTest, AContainerAtItsEndMarksTheBufferWithoutReading)
    {
        const auto r = reader();
        r->on_attach(buffer);
        stream->set_read_position({ k_frames });

        r->processing_function(buffer);

        EXPECT_TRUE(buffer->needs_removal());
        EXPECT_TRUE(buffer->get_pixel_data().empty());
    }

    TEST_F(VideoStreamReaderTest, ARemovalRequestFromTheContainerReachesTheBuffer)
    {
        const auto r = reader();
        r->on_attach(buffer);
        stream->update_processing_state(ProcessingState::NEEDS_REMOVAL);

        r->processing_function(buffer);

        EXPECT_TRUE(buffer->needs_removal());
    }

    TEST_F(VideoStreamReaderTest, ARemovalRequestIsIgnoredWhenFlagUpdatesAreOff)
    {
        const auto r = reader();
        r->set_update_flags(false);
        r->on_attach(buffer);
        stream->update_processing_state(ProcessingState::NEEDS_REMOVAL);

        r->processing_function(buffer);

        EXPECT_FALSE(buffer->needs_removal());
    }

    TEST_F(VideoStreamReaderTest, AnAlreadyProcessedFrameIsNotProcessedAgain)
    {
        const auto r = reader();
        r->on_attach(buffer);
        stream->get_processed_data() = { std::vector<uint8_t>(k_rgba8_frame_bytes, 9) };
        stream->update_processing_state(ProcessingState::PROCESSED);

        r->processing_function(buffer);

        EXPECT_EQ(buffer->get_pixel_data(), std::vector<uint8_t>(k_rgba8_frame_bytes, 9));
        EXPECT_EQ(frame_processor()->get_current_frame(), 0U);
    }

    TEST_F(VideoStreamReaderTest, AShortFrameUploadsAPartialSurface)
    {
        const auto r = reader();
        r->on_attach(buffer);
        stream->get_processed_data() = { std::vector<uint8_t>(16, 7) };
        stream->update_processing_state(ProcessingState::PROCESSED);

        r->processing_function(buffer);

        EXPECT_EQ(buffer->get_pixel_data(), std::vector<uint8_t>(16, 7));
    }

    TEST_F(VideoStreamReaderTest, ABatchLargerThanTheTextureIsCutToTheTexture)
    {
        const auto r = reader();
        frame_processor()->set_frames_per_batch(2);
        r->on_attach(buffer);

        r->processing_function(buffer);

        EXPECT_EQ(buffer->get_pixel_data(), frame_bytes(0));
    }

    TEST_F(VideoStreamReaderTest, ProcessedDataThatIsNotBytesIsIgnored)
    {
        const auto r = reader();
        r->on_attach(buffer);
        stream->get_processed_data() = { std::vector<float>(8, 1.0F) };
        stream->update_processing_state(ProcessingState::PROCESSED);

        r->processing_function(buffer);

        EXPECT_TRUE(buffer->get_pixel_data().empty());
    }

    TEST_F(VideoStreamReaderTest, AnUnreadyContainerRefusesAttachment)
    {
        const auto empty = std::make_shared<VideoStreamContainer>(k_width, k_height, ImageFormat::RGBA8, k_fps);
        const CallbackGuard guard { empty };
        VideoStreamReader r(empty);

        EXPECT_THROW(r.on_attach(buffer), std::runtime_error);
    }

    TEST_F(VideoStreamReaderTest, AFailedAttachmentLeavesNoReaderRegistered)
    {
        const auto empty = std::make_shared<VideoStreamContainer>(k_width, k_height, ImageFormat::RGBA8, k_fps);
        const CallbackGuard guard { empty };
        VideoStreamReader r(empty);

        EXPECT_THROW(r.on_attach(buffer), std::runtime_error);

        EXPECT_FALSE(empty->has_active_readers());
    }

    TEST_F(VideoStreamReaderTest, ABufferThatIsNotATextureIsLeftAlone)
    {
        const auto r = reader();
        const auto audio = std::make_shared<Buffers::AudioBuffer>(0, 4);

        EXPECT_NO_THROW(r->on_attach(audio));
        EXPECT_NO_THROW(r->processing_function(audio));
    }

    TEST_F(VideoStreamReaderTest, TwoReadersMustBothConsumeBeforeTheContainerRecycles)
    {
        const auto first = reader();
        const auto second = reader();
        first->on_attach(buffer);
        second->on_attach(buffer);

        first->processing_function(buffer);
        EXPECT_EQ(stream->get_processing_state(), ProcessingState::PROCESSED);
        EXPECT_FALSE(stream->all_dimensions_consumed());

        second->processing_function(buffer);
        EXPECT_EQ(stream->get_processing_state(), ProcessingState::READY);
    }

    TEST_F(VideoStreamReaderTest, ANewCycleWaitsForEveryReaderAgain)
    {
        const auto first = reader();
        const auto second = reader();
        first->on_attach(buffer);
        second->on_attach(buffer);
        first->processing_function(buffer);
        second->processing_function(buffer);
        ASSERT_EQ(stream->get_processing_state(), ProcessingState::READY);

        first->processing_function(buffer);

        EXPECT_EQ(stream->get_processing_state(), ProcessingState::PROCESSED);
        EXPECT_FALSE(stream->all_dimensions_consumed());
    }

    TEST_F(VideoStreamReaderTest, ChangingTheContainerMovesTheReaderRegistration)
    {
        const auto other = make_flat_rgba8(2);
        const CallbackGuard guard { other };
        const auto r = reader();
        r->on_attach(buffer);

        r->set_container(other);

        EXPECT_EQ(r->get_container(), other);
        EXPECT_FALSE(stream->has_active_readers());
        EXPECT_TRUE(other->has_active_readers());
    }

    TEST_F(VideoStreamReaderTest, WithoutAContainerOrBufferNothingHappens)
    {
        VideoStreamReader detached(nullptr);
        const auto r = reader();

        EXPECT_NO_THROW(detached.processing_function(buffer));
        EXPECT_NO_THROW(detached.on_attach(buffer));
        EXPECT_NO_THROW(r->processing_function(nullptr));
        EXPECT_NO_THROW(r->on_attach(nullptr));
    }

    TEST(VideoContainerBufferTest, TakesItsGeometryAndFormatFromTheContainer)
    {
        const auto stream = make_flat_rgba8(k_frames);
        const CallbackGuard guard { stream };

        const VideoContainerBuffer buffer(stream);

        EXPECT_EQ(buffer.get_width(), k_width);
        EXPECT_EQ(buffer.get_height(), k_height);
        EXPECT_EQ(buffer.get_format(), ImageFormat::RGBA8);
        EXPECT_EQ(buffer.get_container(), stream);
    }

    TEST(VideoContainerBufferTest, TheContainersFormatOverridesTheRequestedOne)
    {
        const auto gray = std::make_shared<VideoProbe>(k_width, k_height, ImageFormat::R8, k_fps);
        const CallbackGuard guard { gray };

        const VideoContainerBuffer buffer(gray, ImageFormat::RGBA8);

        EXPECT_EQ(buffer.get_format(), ImageFormat::R8);
        EXPECT_EQ(buffer.get_width(), k_width);
        EXPECT_EQ(buffer.get_height(), k_height);
    }

    TEST(VideoContainerBufferTest, ABufferNeedsAContainer)
    {
        EXPECT_THROW(VideoContainerBuffer(nullptr), std::invalid_argument);
    }

    TEST(VideoContainerBufferTest, StartsWithAReaderOnItsContainer)
    {
        const auto stream = make_flat_rgba8(k_frames);
        const CallbackGuard guard { stream };

        const VideoContainerBuffer buffer(stream);

        ASSERT_NE(buffer.get_video_reader(), nullptr);
        EXPECT_EQ(buffer.get_video_reader()->get_container(), stream);
    }

    TEST(VideoContainerBufferTest, SettingTheContainerRepointsTheBufferAndItsReader)
    {
        const auto stream = make_flat_rgba8(k_frames);
        const auto other = make_flat_rgba8(2);
        const CallbackGuard first_guard { stream };
        const CallbackGuard second_guard { other };
        VideoContainerBuffer buffer(stream);

        buffer.set_container(other);

        EXPECT_EQ(buffer.get_container(), other);
        EXPECT_EQ(buffer.get_video_reader()->get_container(), other);
    }

    TEST(VideoContainerBufferTest, ItsReaderPlaysALiveStreamIntoTheTexture)
    {
        const auto live = std::make_shared<DynamicVideoStream>(rgba8_spec(8, k_fps));
        const CallbackGuard guard { live };
        live->create_default_processor();
        append_ramp(*live, 3);
        const auto buffer = std::make_shared<VideoContainerBuffer>(live);
        const auto reader = buffer->get_video_reader();
        reader->on_attach(buffer);

        reader->processing_function(buffer);
        EXPECT_EQ(buffer->get_pixel_data(), frame_bytes(0));

        reader->processing_function(buffer);
        EXPECT_EQ(buffer->get_pixel_data(), frame_bytes(1));
    }

} // namespace
} // namespace MayaFlux::Test
