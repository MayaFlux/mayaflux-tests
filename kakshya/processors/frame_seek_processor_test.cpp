#include "gtest/gtest.h"

#include "../video_test_support.hpp"

#include "MayaFlux/Kakshya/Processors/FrameSeekProcessor.hpp"
#include "MayaFlux/Kinesis/Tendency/TendencyFactories.hpp"
#include "MayaFlux/Kinesis/Tendency/TimeMap.hpp"

namespace MayaFlux::Test {
namespace {

    using Kakshya::DynamicVideoStream;
    using Kakshya::FrameSeekProcessor;
    using Kakshya::Region;
    using namespace VideoSupport;

    constexpr double k_stream_rate = 10.0;
    constexpr uint64_t k_frames = 4;

    std::shared_ptr<const Kinesis::TimeMap> map_of(Kinesis::TimeMap map)
    {
        return std::make_shared<const Kinesis::TimeMap>(std::move(map));
    }

    std::shared_ptr<const Kinesis::TimeMap> hold_at(double position)
    {
        return map_of(Kinesis::constant<double, double>(position));
    }

    std::shared_ptr<const Kinesis::TimeMap> step_from(double position, double frames_per_second = k_stream_rate)
    {
        return map_of(Kinesis::TimeMaps::quadratic(position, frames_per_second, 0.0));
    }

    class FrameSeekProcessorTest : public ::testing::Test {
    protected:
        void SetUp() override
        {
            stream = std::make_shared<DynamicVideoStream>(rgba8_spec(0, k_stream_rate));
            append_ramp(*stream, k_frames);
        }

        void start()
        {
            seek = stream->seek_processor();
        }

        std::vector<uint8_t> run()
        {
            seek->process(stream);
            return processed_bytes(*stream);
        }

        std::vector<std::vector<uint8_t>> run_many(int count)
        {
            std::vector<std::vector<uint8_t>> seen;
            for (int i = 0; i < count; ++i) {
                seen.push_back(run());
            }
            return seen;
        }

        static std::vector<std::vector<uint8_t>> frames(std::initializer_list<uint64_t> indices)
        {
            std::vector<std::vector<uint8_t>> expected;
            for (const uint64_t f : indices) {
                expected.push_back(frame_bytes(f));
            }
            return expected;
        }

        std::shared_ptr<DynamicVideoStream> stream;
        std::shared_ptr<FrameSeekProcessor> seek;
    };

    TEST_F(FrameSeekProcessorTest, WithoutAMapItReadsLikeFrameAccess)
    {
        start();

        EXPECT_EQ(run_many(3), frames({ 0, 1, 2 }));
    }

    TEST_F(FrameSeekProcessorTest, AConstantMapHoldsOneFrame)
    {
        start();
        seek->set_time_map(hold_at(2.0));

        EXPECT_EQ(run_many(3), frames({ 2, 2, 2 }));
    }

    TEST_F(FrameSeekProcessorTest, MapPositionsAreFlooredToFrames)
    {
        start();
        seek->set_time_map(step_from(0.5));

        EXPECT_EQ(run_many(4), frames({ 0, 1, 2, 3 }));
    }

    TEST_F(FrameSeekProcessorTest, ANegativeRateReadsBackwardsAndStopsAtTheFirstFrame)
    {
        start();
        seek->set_time_map(step_from(3.5, -k_stream_rate));

        EXPECT_EQ(run_many(5), frames({ 3, 2, 1, 0, 0 }));
    }

    TEST_F(FrameSeekProcessorTest, PositionsOutsideTheStreamAreHeldAtTheEnds)
    {
        start();

        seek->set_time_map(hold_at(100.0));
        EXPECT_EQ(run(), frame_bytes(k_frames - 1));

        seek->set_time_map(hold_at(-5.0));
        EXPECT_EQ(run(), frame_bytes(0));
    }

    TEST_F(FrameSeekProcessorTest, ALoopingStreamWrapsPositions)
    {
        stream->set_looping(true);
        start();
        seek->set_time_map(step_from(0.5));

        EXPECT_EQ(run_many(5), frames({ 0, 1, 2, 3, 0 }));
    }

    TEST_F(FrameSeekProcessorTest, ALoopRegionBoundsTheWrap)
    {
        stream->set_looping(true);
        stream->set_loop_region(Region(std::vector<uint64_t> { 1, 0, 0, 0 }, std::vector<uint64_t> { 2, 1, 3, 3 }));
        start();
        seek->set_time_map(step_from(1.5));

        EXPECT_EQ(run_many(4), frames({ 1, 2, 1, 2 }));
    }

    TEST_F(FrameSeekProcessorTest, ABatchReadsOnePositionPerFrame)
    {
        start();
        seek->set_frames_per_batch(2);
        seek->set_time_map(step_from(0.5));

        EXPECT_EQ(run(), frames_bytes(0, 2));
        EXPECT_EQ(run(), frames_bytes(2, 2));
    }

    TEST_F(FrameSeekProcessorTest, TheContainersReadPositionFollowsTheLastFrameRead)
    {
        start();
        seek->set_time_map(hold_at(2.5));

        run();

        EXPECT_EQ(stream->get_read_position(), std::vector<uint64_t> { 2 });
    }

    TEST_F(FrameSeekProcessorTest, SettingTheSameMapKeepsTheClockRunning)
    {
        start();
        const auto map = step_from(0.5);
        seek->set_time_map(map);
        run_many(2);

        seek->set_time_map(map);

        EXPECT_EQ(run(), frame_bytes(2));
    }

    TEST_F(FrameSeekProcessorTest, SettingAnotherMapRestartsTheClock)
    {
        start();
        seek->set_time_map(step_from(0.5));
        run_many(2);

        seek->set_time_map(step_from(0.5));

        EXPECT_EQ(run(), frame_bytes(0));
    }

    TEST_F(FrameSeekProcessorTest, ResettingRestartsTheClock)
    {
        start();
        seek->set_time_map(step_from(0.5));
        run_many(3);

        seek->reset();

        EXPECT_EQ(run(), frame_bytes(0));
    }

    TEST_F(FrameSeekProcessorTest, ClearingTheMapReturnsToFrameAccess)
    {
        start();
        seek->set_time_map(hold_at(3.0));
        EXPECT_EQ(run(), frame_bytes(3));

        seek->set_time_map(nullptr);
        seek->set_current_frame(1);

        EXPECT_EQ(run(), frame_bytes(1));
    }

    TEST_F(FrameSeekProcessorTest, ARepeatRestartsTheMapAtEachLength)
    {
        start();
        seek->set_time_map(step_from(0.5));
        seek->set_repeat(hold_at(0.3));

        EXPECT_EQ(run_many(6), frames({ 0, 1, 2, 0, 1, 2 }));
    }

    TEST_F(FrameSeekProcessorTest, TheMapSeesFramesAppendedLater)
    {
        auto live = std::make_shared<DynamicVideoStream>(rgba8_spec(0, k_stream_rate));
        append_ramp(*live, 2);
        stream = live;
        start();
        seek->set_time_map(hold_at(3.0));

        EXPECT_EQ(run(), frame_bytes(1));

        append_ramp(*stream, 2, 2);
        EXPECT_EQ(run(), frame_bytes(3));
    }

    TEST_F(FrameSeekProcessorTest, TrailingFollowsTheWriteHeadByTheLag)
    {
        start();

        seek->trail(static_cast<double>(stream->get_write_head()), 1.5);

        EXPECT_EQ(run_many(3), frames({ 2, 3, 3 }));
    }

    TEST_F(FrameSeekProcessorTest, ALagThatChangesMovesTheTap)
    {
        start();

        seek->trail(static_cast<double>(stream->get_write_head()), Kinesis::TimeMaps::linear(1.5, 5.5, 0.2));

        EXPECT_EQ(run_many(3), frames({ 2, 1, 0 }));
    }

    TEST_F(FrameSeekProcessorTest, BlendingMixesTheFramesEitherSideOfAFractionalPosition)
    {
        auto pair = std::make_shared<DynamicVideoStream>(rgba8_spec(0, k_stream_rate));
        const std::vector<uint8_t> dark(k_rgba8_frame_bytes, 0);
        const std::vector<uint8_t> bright(k_rgba8_frame_bytes, 100);
        pair->append_frame(std::span<const uint8_t>(dark));
        pair->append_frame(std::span<const uint8_t>(bright));
        stream = pair;
        start();
        seek->set_time_map(hold_at(0.5));

        EXPECT_EQ(run(), dark);

        seek->set_blend(true);
        for (const uint8_t value : run()) {
            EXPECT_NEAR(value, 50, 1);
        }
    }

    TEST_F(FrameSeekProcessorTest, AMappedFrameThatWasOverwrittenRepeatsThePreviousOutput)
    {
        stream = std::make_shared<DynamicVideoStream>(rgba8_spec(k_frames, k_stream_rate));
        append_ramp(*stream, k_frames);
        start();
        seek->set_time_map(hold_at(1.5));
        ASSERT_EQ(run(), frame_bytes(1));

        append_ramp(*stream, 6, k_frames);

        EXPECT_FALSE(stream->is_frame_available(1));
        EXPECT_EQ(run(), frame_bytes(1));
    }

    TEST_F(FrameSeekProcessorTest, AMappedFrameThatIsGoneBeforeAnyOutputIsBlack)
    {
        stream = std::make_shared<DynamicVideoStream>(rgba8_spec(k_frames, k_stream_rate));
        append_ramp(*stream, 10);
        start();
        seek->set_time_map(hold_at(1.5));

        EXPECT_EQ(run(), std::vector<uint8_t>(k_rgba8_frame_bytes, 0));
    }

    TEST_F(FrameSeekProcessorTest, AnEmptyStreamProducesNothing)
    {
        stream = std::make_shared<DynamicVideoStream>(rgba8_spec(0, k_stream_rate));
        start();
        seek->set_time_map(hold_at(2.0));

        EXPECT_TRUE(run().empty());
    }

    TEST_F(FrameSeekProcessorTest, AnUnattachedProcessorProducesNothing)
    {
        auto loose = std::make_shared<FrameSeekProcessor>();
        loose->set_time_map(hold_at(1.0));

        EXPECT_NO_THROW(loose->process(stream));

        EXPECT_TRUE(processed_bytes(*stream).empty());
    }

    TEST_F(FrameSeekProcessorTest, AnotherContainerIsNotProcessed)
    {
        start();
        seek->set_time_map(hold_at(1.0));
        auto other = std::make_shared<DynamicVideoStream>(rgba8_spec(0, k_stream_rate));
        append_ramp(*other, 2);

        seek->process(other);

        EXPECT_TRUE(processed_bytes(*other).empty());
    }

} // namespace
} // namespace MayaFlux::Test
