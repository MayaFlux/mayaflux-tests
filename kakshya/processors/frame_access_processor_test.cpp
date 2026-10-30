#include "gtest/gtest.h"

#include "../video_test_support.hpp"

#include "MayaFlux/Kakshya/Processors/FrameAccessProcessor.hpp"

namespace MayaFlux::Test {
namespace {

    using Kakshya::FrameAccessProcessor;
    using Kakshya::ImageFormat;
    using Kakshya::ProcessingState;
    using Kakshya::Region;
    using Kakshya::VideoStreamContainer;
    using namespace VideoSupport;

    constexpr uint64_t k_frames = 3;

    std::vector<uint8_t> output_of(const VideoStreamContainer& video)
    {
        const auto& processed = video.get_processed_data();
        if (processed.empty()) {
            return {};
        }
        return std::get<std::vector<uint8_t>>(processed[0]);
    }

    class FrameAccessProcessorTest : public ::testing::Test {
    protected:
        void SetUp() override
        {
            video = make_flat_rgba8(k_frames);
            processor = std::make_shared<FrameAccessProcessor>();
            processor->set_global_fps(k_fps);
        }

        void attach()
        {
            processor->on_attach(video);
        }

        std::vector<uint8_t> run()
        {
            processor->process(video);
            return output_of(*video);
        }

        std::shared_ptr<VideoProbe> video;
        std::shared_ptr<FrameAccessProcessor> processor;
    };

    TEST_F(FrameAccessProcessorTest, DefaultsToOneFrameWithAutoAdvance)
    {
        EXPECT_EQ(processor->get_frames_per_batch(), 1U);
        EXPECT_TRUE(processor->get_auto_advance());
        EXPECT_EQ(processor->get_current_frame(), 0U);
        EXPECT_EQ(processor->get_frame_byte_size(), 0U);
        EXPECT_FALSE(processor->is_processing());
    }

    TEST_F(FrameAccessProcessorTest, AttachingCachesTheFrameGeometryAndMarksTheContainerReady)
    {
        attach();

        EXPECT_EQ(processor->get_frame_byte_size(), k_rgba8_frame_bytes);
        EXPECT_EQ(video->get_processing_state(), ProcessingState::READY);
    }

    TEST_F(FrameAccessProcessorTest, AttachingAdoptsTheContainersReadPosition)
    {
        video->set_read_position({ 2 });

        attach();

        EXPECT_EQ(processor->get_current_frame(), 2U);
    }

    TEST_F(FrameAccessProcessorTest, AttachingToNothingDoesNothing)
    {
        EXPECT_NO_THROW(processor->on_attach(nullptr));
        EXPECT_EQ(processor->get_frame_byte_size(), 0U);
    }

    TEST_F(FrameAccessProcessorTest, AttachingToAContainerWithoutGeometryIsRefused)
    {
        auto bare = std::make_shared<VideoStreamContainer>();

        EXPECT_ANY_THROW(processor->on_attach(bare));
    }

    TEST_F(FrameAccessProcessorTest, AnOversizedBatchIsClampedToTheFrameCountOnAttach)
    {
        processor->set_frames_per_batch(10);

        attach();

        EXPECT_EQ(processor->get_frames_per_batch(), k_frames);
    }

    TEST_F(FrameAccessProcessorTest, AZeroBatchBecomesOne)
    {
        processor->set_frames_per_batch(0);

        EXPECT_EQ(processor->get_frames_per_batch(), 1U);
    }

    TEST_F(FrameAccessProcessorTest, ProcessingBeforeAttachingProducesNothing)
    {
        EXPECT_TRUE(run().empty());
    }

    TEST_F(FrameAccessProcessorTest, ProcessingAnotherContainerProducesNothing)
    {
        auto other = make_flat_rgba8(k_frames);
        attach();

        processor->process(other);

        EXPECT_TRUE(output_of(*other).empty());
        EXPECT_TRUE(output_of(*video).empty());
    }

    TEST_F(FrameAccessProcessorTest, ProcessingCopiesTheCurrentFramePixels)
    {
        processor->set_auto_advance(false);
        attach();

        EXPECT_EQ(run(), frame_bytes(0));
        EXPECT_FALSE(processor->is_processing());
    }

    TEST_F(FrameAccessProcessorTest, WithoutAutoAdvanceTheCursorStaysUntilMoved)
    {
        processor->set_auto_advance(false);
        attach();

        EXPECT_EQ(run(), frame_bytes(0));
        EXPECT_EQ(run(), frame_bytes(0));
        EXPECT_EQ(processor->get_current_frame(), 0U);

        processor->set_current_frame(2);
        EXPECT_EQ(run(), frame_bytes(2));
    }

    TEST_F(FrameAccessProcessorTest, ABatchCopiesConsecutiveFrames)
    {
        processor->set_auto_advance(false);
        processor->set_frames_per_batch(2);
        attach();

        EXPECT_EQ(run(), frames_bytes(0, 2));
    }

    TEST_F(FrameAccessProcessorTest, ABatchIsTruncatedAtTheEnd)
    {
        processor->set_auto_advance(false);
        processor->set_frames_per_batch(2);
        attach();
        processor->set_current_frame(2);

        EXPECT_EQ(run(), frame_bytes(2));
    }

    TEST_F(FrameAccessProcessorTest, PastTheEndWithoutLoopingProducesNothingNew)
    {
        processor->set_auto_advance(false);
        attach();
        processor->set_current_frame(k_frames);

        EXPECT_TRUE(run().empty());
    }

    TEST_F(FrameAccessProcessorTest, AutoAdvanceFollowsTheVideoRateOverTheRenderRate)
    {
        attach();

        EXPECT_EQ(run(), frame_bytes(0));
        EXPECT_EQ(processor->get_current_frame(), 1U);
        EXPECT_EQ(run(), frame_bytes(1));
        EXPECT_EQ(processor->get_current_frame(), 2U);
    }

    TEST_F(FrameAccessProcessorTest, AFasterRenderRateHoldsEachFrameAcrossTicks)
    {
        processor->set_global_fps(2.0 * k_fps);
        attach();

        EXPECT_EQ(run(), frame_bytes(0));
        EXPECT_EQ(processor->get_current_frame(), 0U);
        EXPECT_EQ(run(), frame_bytes(0));
        EXPECT_EQ(processor->get_current_frame(), 1U);
    }

    TEST_F(FrameAccessProcessorTest, ASlowerRenderRateSkipsFrames)
    {
        processor->set_global_fps(k_fps / 2.0);
        attach();

        run();

        EXPECT_EQ(processor->get_current_frame(), 2U);
    }

    TEST_F(FrameAccessProcessorTest, AutoAdvanceReportsTheNewPositionToTheContainer)
    {
        attach();

        run();

        EXPECT_EQ(video->get_read_position(), std::vector<uint64_t> { 1 });
    }

    TEST_F(FrameAccessProcessorTest, AutoAdvanceWithoutLoopingStopsOnTheLastFrame)
    {
        attach();

        run();
        run();
        run();
        EXPECT_EQ(processor->get_current_frame(), k_frames - 1);

        EXPECT_EQ(run(), frame_bytes(k_frames - 1));
        EXPECT_EQ(processor->get_current_frame(), k_frames - 1);
    }

    TEST_F(FrameAccessProcessorTest, AutoAdvanceWithLoopingWrapsToTheStart)
    {
        video->set_looping(true);
        attach();

        run();
        run();
        run();

        EXPECT_EQ(processor->get_current_frame(), 0U);
        EXPECT_EQ(run(), frame_bytes(0));
    }

    TEST_F(FrameAccessProcessorTest, AutoAdvanceWithALoopRegionWrapsToItsStart)
    {
        video->set_looping(true);
        video->set_loop_region(Region(std::vector<uint64_t> { 1, 0, 0, 0 }, std::vector<uint64_t> { 2, 1, 3, 3 }));
        attach();
        processor->set_current_frame(2);

        EXPECT_EQ(run(), frame_bytes(2));
        EXPECT_EQ(processor->get_current_frame(), 1U);
    }

    TEST_F(FrameAccessProcessorTest, LoopingRestartsWhenTheCursorIsPastTheEnd)
    {
        processor->set_auto_advance(false);
        video->set_looping(true);
        attach();
        processor->set_current_frame(k_frames);

        EXPECT_EQ(run(), frame_bytes(0));
        EXPECT_EQ(processor->get_current_frame(), 0U);
    }

    TEST_F(FrameAccessProcessorTest, DetachingForgetsTheContainerAndTheGeometry)
    {
        attach();

        processor->on_detach(video);

        EXPECT_EQ(processor->get_frame_byte_size(), 0U);
        EXPECT_EQ(processor->get_current_frame(), 0U);
        EXPECT_TRUE(run().empty());
    }

    TEST_F(FrameAccessProcessorTest, DepthFramesCopyTheirRawBytes)
    {
        auto depth = std::make_shared<VideoProbe>(k_width, k_height, ImageFormat::DEPTH32F, k_fps);
        std::vector<float> values(size_t { 2 } * k_width * k_height);
        std::ranges::generate(values, [n = 0.5F]() mutable { return n++; });
        depth->load<float>(values, 2);
        processor->set_auto_advance(false);
        processor->on_attach(depth);

        processor->process(depth);

        const auto out = output_of(*depth);
        ASSERT_EQ(out.size(), k_width * k_height * sizeof(float));
        EXPECT_EQ(std::memcmp(out.data(), values.data(), out.size()), 0);
        EXPECT_EQ(processor->get_frame_byte_size(), k_width * k_height * sizeof(float));
    }

    class FrameAccessProcessorRingTest : public ::testing::Test {
    protected:
        void SetUp() override
        {
            video = make_ring_rgba8(6, 4);
            processor = std::make_shared<FrameAccessProcessor>();
            processor->set_global_fps(k_fps);
        }

        std::shared_ptr<VideoStreamContainer> video;
        std::shared_ptr<FrameAccessProcessor> processor;
    };

    TEST_F(FrameAccessProcessorRingTest, CommittedFramesAreCopiedFromTheirSlots)
    {
        commit_pattern(*video, 0);
        commit_pattern(*video, 1);
        processor->set_auto_advance(false);
        processor->on_attach(video);

        processor->process(video);
        EXPECT_EQ(output_of(*video), frame_bytes(0));

        processor->set_current_frame(1);
        processor->process(video);
        EXPECT_EQ(output_of(*video), frame_bytes(1));
    }

    TEST_F(FrameAccessProcessorRingTest, AFrameNotYetDecodedIsOutputAsBlack)
    {
        commit_pattern(*video, 0);
        processor->set_auto_advance(false);
        processor->on_attach(video);
        processor->set_current_frame(2);

        processor->process(video);

        EXPECT_EQ(output_of(*video), std::vector<uint8_t>(k_rgba8_frame_bytes, 0));
    }

    TEST_F(FrameAccessProcessorRingTest, ALateFrameRepeatsThePreviousOutput)
    {
        commit_pattern(*video, 0);
        processor->set_auto_advance(false);
        processor->on_attach(video);
        processor->process(video);
        processor->set_current_frame(2);

        processor->process(video);

        EXPECT_EQ(output_of(*video), frame_bytes(0));
    }

    TEST_F(FrameAccessProcessorRingTest, ALateFrameDoesNotStopTheCursor)
    {
        commit_pattern(*video, 0);
        processor->on_attach(video);
        processor->set_current_frame(2);

        processor->process(video);

        EXPECT_EQ(processor->get_current_frame(), 3U);
    }

    TEST_F(FrameAccessProcessorRingTest, ATornBatchIsRepeatedWholeNotMixed)
    {
        commit_pattern(*video, 0);
        commit_pattern(*video, 1);
        processor->set_auto_advance(false);
        processor->set_frames_per_batch(2);
        processor->on_attach(video);
        processor->process(video);
        processor->set_current_frame(1);

        processor->process(video);

        EXPECT_EQ(output_of(*video), frames_bytes(0, 2));
    }

    TEST_F(FrameAccessProcessorRingTest, TheCursorReachesTheSourceEndNotTheRingEnd)
    {
        processor->set_auto_advance(false);
        processor->on_attach(video);
        processor->set_current_frame(5);
        commit_pattern(*video, 5);

        processor->process(video);

        EXPECT_EQ(output_of(*video), frame_bytes(5));
    }

    class VideoDefaultProcessorTest : public ::testing::Test {
    protected:
        void SetUp() override
        {
            video = make_flat_rgba8(k_frames);
        }

        std::shared_ptr<VideoProbe> video;
    };

    TEST_F(VideoDefaultProcessorTest, ThereIsNoDefaultProcessorUntilOneIsCreated)
    {
        EXPECT_EQ(video->get_default_processor(), nullptr);
    }

    TEST_F(VideoDefaultProcessorTest, TheDefaultProcessorIsAFrameAccessProcessorAttachedToTheContainer)
    {
        video->create_default_processor();

        const auto processor = std::dynamic_pointer_cast<FrameAccessProcessor>(video->get_default_processor());

        ASSERT_NE(processor, nullptr);
        EXPECT_EQ(processor->get_frame_byte_size(), k_rgba8_frame_bytes);
        EXPECT_EQ(video->get_processing_state(), ProcessingState::READY);
    }

    TEST_F(VideoDefaultProcessorTest, ProcessingTheDefaultWalksReadyProcessingProcessed)
    {
        video->create_default_processor();
        std::vector<ProcessingState> seen;
        video->register_state_change_callback(
            [&seen](const std::shared_ptr<Kakshya::SignalSourceContainer>&, ProcessingState state) { seen.push_back(state); });

        video->process_default();

        EXPECT_EQ(seen, (std::vector<ProcessingState> { ProcessingState::PROCESSING, ProcessingState::PROCESSED }));
        EXPECT_EQ(output_of(*video), frame_bytes(0));
    }

    TEST_F(VideoDefaultProcessorTest, EachDefaultPassAdvancesOneFrame)
    {
        video->create_default_processor();
        std::dynamic_pointer_cast<FrameAccessProcessor>(video->get_default_processor())->set_global_fps(k_fps);

        video->process_default();
        video->process_default();

        EXPECT_EQ(output_of(*video), frame_bytes(1));
        EXPECT_EQ(video->get_read_position(), std::vector<uint64_t> { 2 });
    }

    TEST_F(VideoDefaultProcessorTest, ProcessingWithoutADefaultDoesNothing)
    {
        video->mark_ready_for_processing(true);

        video->process_default();

        EXPECT_EQ(video->get_processing_state(), ProcessingState::READY);
        EXPECT_TRUE(output_of(*video).empty());
    }

    TEST_F(VideoDefaultProcessorTest, ProcessingAnIdleContainerDoesNothing)
    {
        video->create_default_processor();
        video->mark_ready_for_processing(false);

        video->process_default();

        EXPECT_EQ(video->get_processing_state(), ProcessingState::IDLE);
        EXPECT_TRUE(output_of(*video).empty());
    }

    TEST_F(VideoDefaultProcessorTest, ReplacingTheDefaultDetachesTheOldOneAndAttachesTheNew)
    {
        video->create_default_processor();
        const auto old = std::dynamic_pointer_cast<FrameAccessProcessor>(video->get_default_processor());
        auto replacement = std::make_shared<FrameAccessProcessor>();

        video->set_default_processor(replacement);

        EXPECT_EQ(old->get_frame_byte_size(), 0U);
        EXPECT_EQ(replacement->get_frame_byte_size(), k_rgba8_frame_bytes);
        EXPECT_EQ(video->get_default_processor(), replacement);
    }

    TEST_F(VideoDefaultProcessorTest, ClearingTheDefaultDetachesIt)
    {
        video->create_default_processor();
        const auto old = std::dynamic_pointer_cast<FrameAccessProcessor>(video->get_default_processor());

        video->set_default_processor(nullptr);

        EXPECT_EQ(video->get_default_processor(), nullptr);
        EXPECT_EQ(old->get_frame_byte_size(), 0U);
    }

} // namespace
} // namespace MayaFlux::Test
