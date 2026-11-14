#include "gtest/gtest.h"
#include "../support/test_metadata.hpp"

#include "MayaFlux/Buffers/AudioBuffer.hpp"
#include "MayaFlux/Buffers/BufferManager.hpp"
#include "MayaFlux/Kakshya/Source/DynamicSoundStream.hpp"
#include "MayaFlux/Kriya/BufferPipeline.hpp"
#include "MayaFlux/Vruta/Scheduler.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {
namespace {

    constexpr uint32_t k_block = 512;
    constexpr uint32_t k_rate = 48000;

    double value(uint64_t block, uint32_t frame)
    {
        return 1e-4 * static_cast<double>(block * k_block + frame + 1);
    }

    std::vector<double> expected(uint64_t first_block, uint64_t blocks, double sign = 1.0)
    {
        std::vector<double> out;
        out.reserve(blocks * k_block);
        for (uint64_t b = first_block; b < first_block + blocks; ++b) {
            for (uint32_t f = 0; f < k_block; ++f) {
                out.push_back(sign * value(b, f));
            }
        }
        return out;
    }

    class RecordSyntheticTest : public ::testing::Test {
    protected:
        void SetUp() override
        {
            saved_size = Buffers::s_preferred_buffer_size;
            scheduler = std::make_shared<Vruta::TaskScheduler>(k_rate);
            manager = std::make_shared<Buffers::BufferManager>(2, 2, k_rate, k_block);
            stream = std::make_shared<Kakshya::DynamicSoundStream>(k_rate, 1);
        }

        void TearDown() override
        {
            Buffers::s_preferred_buffer_size = saved_size;
        }

        void cycle(uint64_t block)
        {
            std::vector<double> input(static_cast<size_t>(k_block) * 2);
            for (uint32_t f = 0; f < k_block; ++f) {
                input[static_cast<size_t>(f) * 2] = value(block, f);
                input[static_cast<size_t>(f) * 2 + 1] = -value(block, f);
            }
            manager->process_input(input.data(), 2, k_block);
            scheduler->process_buffer_cycle_tasks();
            scheduler->process_token(Vruta::ProcessingToken::SAMPLE_ACCURATE, k_block);
        }

        void cycles(uint64_t first, uint64_t count)
        {
            for (uint64_t b = first; b < first + count; ++b) {
                cycle(b);
            }
        }

        [[nodiscard]] std::vector<double> recorded(uint32_t channel = 0) const
        {
            const auto frames = stream->get_channel_frames(channel, 0, stream->get_write_head(channel));
            return { frames.begin(), frames.end() };
        }

        [[nodiscard]] bool heard_on(uint32_t channel, const std::shared_ptr<Buffers::AudioBuffer>& buffer) const
        {
            const auto& buffers = manager->get_buffers(Buffers::ProcessingToken::AUDIO_BACKEND, channel);
            return std::ranges::find(buffers, buffer) != buffers.end();
        }

        uint32_t saved_size {};
        std::shared_ptr<Vruta::TaskScheduler> scheduler;
        std::shared_ptr<Buffers::BufferManager> manager;
        std::shared_ptr<Kakshya::DynamicSoundStream> stream;
    };

    TEST_F(RecordSyntheticTest, EveryInputBlockOfTheCycleCountIsAppendedToTheStream)
    {
        const auto [pipeline, listener] = Kriya::record(scheduler, manager, stream, 0, 3);
        ASSERT_NE(pipeline, nullptr);
        ASSERT_NE(listener, nullptr);

        cycles(0, 3);
        EXPECT_EQ(stream->get_write_head(0), 3ULL * k_block);
        EXPECT_EQ(recorded(), expected(0, 3));

        cycles(3, 1);
        EXPECT_FALSE(pipeline->is_running());
        EXPECT_EQ(stream->get_write_head(0), 3ULL * k_block);
    }

    TEST_F(RecordSyntheticTest, RecordingStopsAfterTheCycleCountWhileInputKeepsArriving)
    {
        const auto [pipeline, listener] = Kriya::record(scheduler, manager, stream, 0, 3);

        cycles(0, 7);

        EXPECT_EQ(stream->get_write_head(0), 3ULL * k_block);
        EXPECT_EQ(recorded(), expected(0, 3));
    }

    TEST_F(RecordSyntheticTest, TheInputChannelSelectsWhichSignalIsRecorded)
    {
        const auto [pipeline, listener] = Kriya::record(scheduler, manager, stream, 1, 2);

        cycles(0, 2);

        EXPECT_EQ(recorded(), expected(0, 2, -1.0));
    }

    TEST_F(RecordSyntheticTest, TheStreamChannelSelectsWhereTheSignalIsWritten)
    {
        stream = std::make_shared<Kakshya::DynamicSoundStream>(k_rate, 2);
        const auto [pipeline, listener] = Kriya::record(scheduler, manager, stream, 0, 2, std::nullopt, 1);

        cycles(0, 2);

        EXPECT_EQ(stream->get_write_head(1), 2ULL * k_block);
        EXPECT_EQ(stream->get_write_head(0), 0U);
        EXPECT_EQ(recorded(1), expected(0, 2));
    }

    TEST_F(RecordSyntheticTest, TheListenerIsReleasedWhenTheRecordingEnds)
    {
        const auto [pipeline, listener] = Kriya::record(scheduler, manager, stream, 0, 2);

        cycles(0, 3);
        ASSERT_FALSE(pipeline->is_running());
        const auto last = listener->get_data()[0];
        cycles(9, 1);

        EXPECT_DOUBLE_EQ(last, value(2, 0));
        EXPECT_DOUBLE_EQ(listener->get_data()[0], last);
    }

    TEST_F(RecordSyntheticTest, WithoutAMonitorChannelTheInputIsNotMixedToAnyOutput)
    {
        const auto [pipeline, listener] = Kriya::record(scheduler, manager, stream, 0, 2);

        EXPECT_FALSE(heard_on(0, listener));
        EXPECT_FALSE(heard_on(1, listener));
    }

    TEST_F(RecordSyntheticTest, AMonitorChannelMixesTheInputToThatOutputUntilTheRecordingEnds)
    {
        const auto [pipeline, listener] = Kriya::record(scheduler, manager, stream, 0, 2, 1);

        EXPECT_TRUE(heard_on(1, listener));
        EXPECT_FALSE(heard_on(0, listener));

        cycles(0, 3);

        EXPECT_FALSE(pipeline->is_running());
        EXPECT_FALSE(heard_on(1, listener));
    }

    TEST_F(RecordSyntheticTest, AZeroCycleCountRecordsUntilThePipelineEnds)
    {
        const auto [pipeline, listener] = Kriya::record(scheduler, manager, stream, 0, 0);

        cycles(0, 4);
        EXPECT_TRUE(pipeline->is_running());
        EXPECT_EQ(stream->get_write_head(0), 4ULL * k_block);

        pipeline->end();
        const auto last = listener->get_data()[0];
        cycles(9, 2);

        EXPECT_FALSE(pipeline->is_running());
        EXPECT_EQ(stream->get_write_head(0), 4ULL * k_block);
        EXPECT_DOUBLE_EQ(listener->get_data()[0], last);
    }

    TEST_F(RecordSyntheticTest, RecordIntoRecordsAgainThroughTheSameListenerAndTheStreamKeepsAppending)
    {
        const auto [pipeline, listener] = Kriya::record(scheduler, manager, stream, 0, 2);
        cycles(0, 3);
        ASSERT_FALSE(pipeline->is_running());

        const auto again = Kriya::record_into(scheduler, manager, stream, listener, 0, 2);
        ASSERT_NE(again, nullptr);
        cycles(10, 3);

        EXPECT_FALSE(again->is_running());
        EXPECT_EQ(stream->get_write_head(0), 4ULL * k_block);
        auto want = expected(0, 2);
        const auto second = expected(10, 2);
        want.insert(want.end(), second.begin(), second.end());
        EXPECT_EQ(recorded(), want);
    }

    TEST_F(RecordSyntheticTest, RecordIntoMixesTheSuppliedListenerToTheMonitorChannelAndReleasesIt)
    {
        const auto [pipeline, listener] = Kriya::record(scheduler, manager, stream, 0, 1);
        cycles(0, 2);

        const auto again = Kriya::record_into(scheduler, manager, stream, listener, 0, 1, 0);
        ASSERT_NE(again, nullptr);
        EXPECT_TRUE(heard_on(0, listener));

        cycles(10, 2);

        EXPECT_FALSE(heard_on(0, listener));
    }

    TEST_F(RecordSyntheticTest, NullArgumentsAreRefused)
    {
        const auto no_scheduler = Kriya::record(nullptr, manager, stream, 0, 1);
        const auto no_manager = Kriya::record(scheduler, nullptr, stream, 0, 1);
        const auto no_stream = Kriya::record(scheduler, manager, nullptr, 0, 1);
        EXPECT_EQ(no_scheduler.first, nullptr);
        EXPECT_EQ(no_scheduler.second, nullptr);
        EXPECT_EQ(no_manager.first, nullptr);
        EXPECT_EQ(no_manager.second, nullptr);
        EXPECT_EQ(no_stream.first, nullptr);
        EXPECT_EQ(no_stream.second, nullptr);

        const auto listener = std::make_shared<Buffers::AudioBuffer>(0);
        EXPECT_EQ(Kriya::record_into(scheduler, manager, stream, nullptr, 0, 1), nullptr);
        EXPECT_EQ(Kriya::record_into(nullptr, manager, stream, listener, 0, 1), nullptr);
        EXPECT_EQ(Kriya::record_into(scheduler, nullptr, stream, listener, 0, 1), nullptr);
        EXPECT_EQ(Kriya::record_into(scheduler, manager, nullptr, listener, 0, 1), nullptr);
    }

}
}
