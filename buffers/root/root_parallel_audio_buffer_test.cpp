#include "gtest/gtest.h"

#include <atomic>
#include <thread>

#include "MayaFlux/Buffers/AudioBuffer.hpp"
#include "MayaFlux/Buffers/Root/RootParallelAudioBuffer.hpp"

namespace MayaFlux::Test {

using Buffers::AudioBuffer;
using Buffers::RootParallelAudioBuffer;

namespace {

    class RootParallelAudioBufferFixture : public ::testing::Test {
    protected:
        void SetUp() override
        {
            root = std::make_shared<RootParallelAudioBuffer>(0);
            root->initialize();
            root->set_mix_normalization(false);
            size = root->get_num_samples();
        }

        std::shared_ptr<AudioBuffer> make_child(double value)
        {
            auto child = std::make_shared<AudioBuffer>();
            child->resize(size);
            std::ranges::fill(child->get_data(), value);
            root->add_child_buffer(child);
            return child;
        }

        void publish(const std::shared_ptr<AudioBuffer>& child, double value)
        {
            std::ranges::fill(child->get_data(), value);
            root->process_default();
            root->handle_cycle_complete();
        }

        static bool uniform(std::span<const double> block, double value)
        {
            return std::ranges::all_of(block, [value](double sample) { return sample == value; });
        }

        std::shared_ptr<RootParallelAudioBuffer> root;
        uint32_t size { 0 };
    };

}

using RootParallelAudioBufferTest = RootParallelAudioBufferFixture;

TEST_F(RootParallelAudioBufferTest, AFreshRootReadsASilentBlockOfItsSizeAtGenerationZero)
{
    const auto block = root->read_block();

    EXPECT_EQ(block.size(), size);
    EXPECT_TRUE(uniform(block, 0.0));
    EXPECT_EQ(root->block_generation(), 0U);
}

TEST_F(RootParallelAudioBufferTest, ACompletedCycleIsPublishedAsTheLatestBlock)
{
    const auto child = make_child(0.5);

    publish(child, 0.5);

    const auto block = root->read_block();
    EXPECT_TRUE(uniform(block, 0.5));
    EXPECT_EQ(root->block_generation(), 1U);
}

TEST_F(RootParallelAudioBufferTest, ReadingTwiceWithoutAPublishRepeatsTheBlockAtTheSameGeneration)
{
    const auto child = make_child(0.5);
    publish(child, 0.5);

    const auto first = root->read_block();
    const double* first_data = first.data();
    const auto second = root->read_block();

    EXPECT_EQ(second.data(), first_data);
    EXPECT_TRUE(uniform(second, 0.5));
    EXPECT_EQ(root->block_generation(), 1U);
}

TEST_F(RootParallelAudioBufferTest, TheLatestPublishWinsWhenSeveralLandBeforeARead)
{
    const auto child = make_child(0.0);

    publish(child, 0.1);
    publish(child, 0.2);
    publish(child, 0.3);

    const auto block = root->read_block();
    EXPECT_TRUE(uniform(block, 0.3));
    EXPECT_EQ(root->block_generation(), 3U);
}

TEST_F(RootParallelAudioBufferTest, AHeldBlockIsUntouchedByTwoLaterPublishes)
{
    const auto child = make_child(0.0);
    publish(child, 0.1);
    const auto held = root->read_block();
    ASSERT_TRUE(uniform(held, 0.1));

    publish(child, 0.2);
    publish(child, 0.3);

    EXPECT_TRUE(uniform(held, 0.1));
    EXPECT_TRUE(uniform(root->read_block(), 0.3));
    EXPECT_EQ(root->block_generation(), 3U);
}

TEST_F(RootParallelAudioBufferTest, AMixInProgressIsNotVisibleToTheReader)
{
    const auto child = make_child(0.5);
    publish(child, 0.5);
    ASSERT_TRUE(uniform(root->read_block(), 0.5));

    std::ranges::fill(child->get_data(), 0.9);
    root->process_default();

    EXPECT_TRUE(uniform(root->read_block(), 0.5));
    EXPECT_EQ(root->block_generation(), 1U);
}

TEST_F(RootParallelAudioBufferTest, WhatFollowsTheMixIsPartOfThePublishedBlock)
{
    const auto child = make_child(0.25);
    std::ranges::fill(child->get_data(), 0.25);
    root->process_default();
    for (double& sample : root->get_data()) {
        sample *= 2.0;
    }

    root->handle_cycle_complete();

    EXPECT_TRUE(uniform(root->read_block(), 0.5));
}

TEST_F(RootParallelAudioBufferTest, ResizeResizesThePublishedBlocksAndSilencesThem)
{
    const auto child = make_child(0.5);
    publish(child, 0.5);

    root->resize(128);

    const auto block = root->read_block();
    EXPECT_EQ(block.size(), 128U);
    EXPECT_TRUE(uniform(block, 0.0));
}

TEST_F(RootParallelAudioBufferTest, ABackendStyleReadSeesTheWorkingBufferOnTheBaseClass)
{
    RootParallelAudioBuffer* parallel = root.get();
    Buffers::RootAudioBuffer& base = *parallel;
    const auto child = make_child(0.5);
    std::ranges::fill(child->get_data(), 0.5);
    root->process_default();

    EXPECT_EQ(base.get_data().front(), 0.5);
    EXPECT_TRUE(uniform(root->read_block(), 0.0));
}

TEST_F(RootParallelAudioBufferTest, AConcurrentReaderNeverSeesATornOrZeroedBlock)
{
    constexpr uint64_t publishes = 20000;
    std::atomic<bool> done { false };
    std::atomic<uint64_t> torn { 0 };
    std::atomic<uint64_t> regressions { 0 };
    std::atomic<uint64_t> mismatched { 0 };
    std::atomic<uint64_t> reads { 0 };

    std::thread consumer([&] {
        uint64_t last = 0;
        const auto check = [&] {
            const auto block = root->read_block();
            const uint64_t generation = root->block_generation();

            if (!uniform(block, block.front())) {
                ++torn;
            }
            if (generation < last) {
                ++regressions;
            }
            if (block.front() != static_cast<double>(generation)) {
                ++mismatched;
            }
            last = generation;
            ++reads;
        };

        while (!done.load(std::memory_order_acquire)) {
            check();
        }
        check();
    });

    for (uint64_t generation = 1; generation <= publishes; ++generation) {
        auto& data = root->get_data();
        std::ranges::fill(data, 0.0);
        std::ranges::fill(data, static_cast<double>(generation));
        root->handle_cycle_complete();
    }
    done.store(true, std::memory_order_release);
    consumer.join();

    EXPECT_EQ(torn.load(), 0U);
    EXPECT_EQ(regressions.load(), 0U);
    EXPECT_EQ(mismatched.load(), 0U);
    EXPECT_GT(reads.load(), 0U);
    EXPECT_EQ(root->block_generation(), publishes);
}

}
