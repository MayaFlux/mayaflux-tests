#include "gtest/gtest.h"

#include "MayaFlux/Buffers/AudioBuffer.hpp"
#include "MayaFlux/Buffers/BufferProcessingChain.hpp"
#include "MayaFlux/Buffers/BufferProcessor.hpp"
#include "MayaFlux/Buffers/BufferSpec.hpp"

namespace MayaFlux::Test {

using Buffers::AudioBuffer;
using Buffers::Buffer;
using Buffers::BufferProcessingChain;
using Buffers::BufferProcessor;
using Buffers::BufferRoutingState;

namespace {

    class PreferredSizeGuard {
    public:
        explicit PreferredSizeGuard(uint32_t block)
        {
            Buffers::s_preferred_buffer_size = block;
        }

        PreferredSizeGuard(const PreferredSizeGuard&) = delete;
        PreferredSizeGuard& operator=(const PreferredSizeGuard&) = delete;
        PreferredSizeGuard(PreferredSizeGuard&&) = delete;
        PreferredSizeGuard& operator=(PreferredSizeGuard&&) = delete;

        ~PreferredSizeGuard() { Buffers::s_preferred_buffer_size = m_saved; }

    private:
        uint32_t m_saved { Buffers::s_preferred_buffer_size };
    };

    class ProbeProcessor final : public BufferProcessor {
    public:
        void processing_function(const std::shared_ptr<Buffer>& buffer) override
        {
            ++runs;
            last = buffer;
        }

        void on_attach(const std::shared_ptr<Buffer>&) override { ++attached; }
        void on_detach(const std::shared_ptr<Buffer>&) override { ++detached; }

        int runs { 0 };
        int attached { 0 };
        int detached { 0 };
        std::shared_ptr<Buffer> last;
    };

    std::shared_ptr<AudioBuffer> make_buffer(uint32_t channel, uint32_t samples)
    {
        auto buffer = std::make_shared<AudioBuffer>(channel, samples);
        buffer->resize(samples);
        return buffer;
    }

}

TEST(AudioBufferTest, DefaultConstructionUsesThePreferredSizeAndChannelZero)
{
    const PreferredSizeGuard guard(256);

    const auto buffer = std::make_shared<AudioBuffer>();

    EXPECT_EQ(buffer->get_channel_id(), 0U);
    EXPECT_EQ(buffer->get_num_samples(), 256U);
    EXPECT_EQ(buffer->get_data().size(), 256U);
    EXPECT_NE(buffer->get_processing_chain(), nullptr);
    EXPECT_EQ(buffer->get_default_processor(), nullptr);
}

TEST(AudioBufferTest, ConstructionKeepsAMatchingSize)
{
    const PreferredSizeGuard guard(256);

    const auto buffer = std::make_shared<AudioBuffer>(3, 256);

    EXPECT_EQ(buffer->get_channel_id(), 3U);
    EXPECT_EQ(buffer->get_num_samples(), 256U);
}

TEST(AudioBufferTest, ConstructionCoercesAMismatchedSizeToThePreferredSize)
{
    const PreferredSizeGuard guard(256);

    const auto small = std::make_shared<AudioBuffer>(0, 64);
    const auto large = std::make_shared<AudioBuffer>(0, 512);

    EXPECT_EQ(small->get_num_samples(), 256U);
    EXPECT_EQ(small->get_data().size(), 256U);
    EXPECT_EQ(large->get_num_samples(), 256U);
    EXPECT_EQ(large->get_data().size(), 256U);
}

TEST(AudioBufferTest, SetupSetsTheChannelAndResizesTheData)
{
    auto buffer = make_buffer(0, 16);

    buffer->setup(2, 32);

    EXPECT_EQ(buffer->get_channel_id(), 2U);
    EXPECT_EQ(buffer->get_num_samples(), 32U);
    EXPECT_EQ(buffer->get_data().size(), 32U);
}

TEST(AudioBufferTest, ResizeChangesTheSampleCountAndTheDataTogether)
{
    auto buffer = make_buffer(0, 16);

    buffer->resize(10);
    EXPECT_EQ(buffer->get_num_samples(), 10U);
    EXPECT_EQ(buffer->get_data().size(), 10U);

    buffer->resize(0);
    EXPECT_EQ(buffer->get_num_samples(), 0U);
    EXPECT_TRUE(buffer->get_data().empty());
}

TEST(AudioBufferTest, SetNumSamplesMatchesResize)
{
    auto buffer = make_buffer(0, 16);

    buffer->set_num_samples(7);

    EXPECT_EQ(buffer->get_num_samples(), 7U);
    EXPECT_EQ(buffer->get_data().size(), 7U);
}

TEST(AudioBufferTest, SetChannelIdChangesOnlyTheId)
{
    auto buffer = make_buffer(0, 16);
    const auto size = buffer->get_num_samples();

    buffer->set_channel_id(5);

    EXPECT_EQ(buffer->get_channel_id(), 5U);
    EXPECT_EQ(buffer->get_num_samples(), size);
}

TEST(AudioBufferTest, GetSampleAliasesTheData)
{
    auto buffer = make_buffer(0, 16);

    buffer->get_sample(3) = 0.5;

    EXPECT_EQ(buffer->get_data()[3], 0.5);
    const auto& view = *buffer;
    EXPECT_EQ(view.get_data()[3], 0.5);
}

TEST(AudioBufferTest, ClearZeroesTheDataButKeepsItsSize)
{
    auto buffer = make_buffer(0, 16);
    std::ranges::fill(buffer->get_data(), 0.7);

    buffer->clear();

    EXPECT_EQ(buffer->get_data(), std::vector<double>(16, 0.0));
    EXPECT_EQ(buffer->get_num_samples(), 16U);
}

TEST(AudioBufferTest, SettingADefaultProcessorAttachesItAndDetachesThePrevious)
{
    auto buffer = make_buffer(0, 16);
    auto first = std::make_shared<ProbeProcessor>();
    auto second = std::make_shared<ProbeProcessor>();

    buffer->set_default_processor(first);
    EXPECT_EQ(first->attached, 1);
    EXPECT_EQ(first->detached, 0);

    buffer->set_default_processor(second);
    EXPECT_EQ(first->detached, 1);
    EXPECT_EQ(second->attached, 1);
    EXPECT_EQ(buffer->get_default_processor(), second);
}

TEST(AudioBufferTest, ClearingTheDefaultProcessorDetachesIt)
{
    auto buffer = make_buffer(0, 16);
    auto probe = std::make_shared<ProbeProcessor>();
    buffer->set_default_processor(probe);

    buffer->set_default_processor(nullptr);

    EXPECT_EQ(probe->detached, 1);
    EXPECT_EQ(buffer->get_default_processor(), nullptr);
}

TEST(AudioBufferTest, ProcessDefaultRunsTheDefaultProcessorOnThisBuffer)
{
    auto buffer = make_buffer(0, 16);
    auto probe = std::make_shared<ProbeProcessor>();
    buffer->set_default_processor(probe);

    buffer->process_default();

    EXPECT_EQ(probe->runs, 1);
    EXPECT_EQ(probe->last, buffer);
}

TEST(AudioBufferTest, ProcessDefaultIsANoOpWithoutAProcessorOrWhenDisabled)
{
    auto buffer = make_buffer(0, 16);
    EXPECT_NO_THROW(buffer->process_default());

    auto probe = std::make_shared<ProbeProcessor>();
    buffer->set_default_processor(probe);
    EXPECT_TRUE(buffer->needs_default_processing());

    buffer->enforce_default_processing(false);
    buffer->process_default();

    EXPECT_FALSE(buffer->needs_default_processing());
    EXPECT_EQ(probe->runs, 0);
}

TEST(AudioBufferTest, SetProcessingChainReplacesTheChainWhenForced)
{
    auto buffer = make_buffer(0, 16);
    auto replacement = std::make_shared<BufferProcessingChain>();

    buffer->set_processing_chain(replacement, true);

    EXPECT_EQ(buffer->get_processing_chain(), replacement);
}

TEST(AudioBufferTest, SetProcessingChainMergesIntoTheExistingChainWhenNotForced)
{
    auto buffer = make_buffer(0, 16);
    const auto existing = buffer->get_processing_chain();
    auto incoming = std::make_shared<BufferProcessingChain>();
    auto probe = std::make_shared<ProbeProcessor>();
    incoming->add_processor(probe, buffer);

    buffer->set_processing_chain(incoming);

    EXPECT_EQ(buffer->get_processing_chain(), existing);
    EXPECT_TRUE(existing->has_processors(buffer));
}

TEST(AudioBufferTest, EachBufferOwnsItsOwnChainByDefault)
{
    auto first = make_buffer(0, 16);
    auto second = make_buffer(1, 16);

    EXPECT_NE(first->get_processing_chain(), second->get_processing_chain());
}

TEST(AudioBufferTest, CloneCarriesTheChannelDataSizeAndDefaultProcessor)
{
    auto source = make_buffer(0, 32);
    std::ranges::fill(source->get_data(), 0.5);
    auto probe = std::make_shared<ProbeProcessor>();
    source->set_default_processor(probe);

    const auto clone = source->clone_to(4U);

    ASSERT_NE(clone, nullptr);
    EXPECT_NE(clone, source);
    EXPECT_EQ(clone->get_channel_id(), 4U);
    EXPECT_EQ(clone->get_num_samples(), 32U);
    EXPECT_EQ(clone->get_data(), source->get_data());
    EXPECT_EQ(clone->get_default_processor(), probe);
}

TEST(AudioBufferTest, CloneDataIsIndependentOfTheSource)
{
    auto source = make_buffer(0, 8);
    const auto clone = source->clone_to(1U);

    clone->get_data()[0] = 0.9;

    EXPECT_EQ(source->get_data()[0], 0.0);
}

TEST(AudioBufferTest, CloneSharesTheSourceChain)
{
    auto source = make_buffer(0, 8);

    const auto clone = source->clone_to(1U);

    EXPECT_EQ(clone->get_processing_chain(), source->get_processing_chain());
}

TEST(AudioBufferTest, CloneByDescriptorMatchesCloneByChannel)
{
    auto source = make_buffer(0, 8);

    const auto clone = std::dynamic_pointer_cast<AudioBuffer>(source->clone_to(static_cast<uint8_t>(4)));

    ASSERT_NE(clone, nullptr);
    EXPECT_EQ(clone->get_channel_id(), 4U);
    EXPECT_EQ(clone->get_num_samples(), 8U);
}

TEST(AudioBufferTest, ReadOnceCopiesTheSourceWhenSizesMatch)
{
    auto source = make_buffer(0, 16);
    std::ranges::fill(source->get_data(), 0.25);
    auto destination = make_buffer(1, 16);

    EXPECT_TRUE(destination->read_once(source));

    EXPECT_EQ(destination->get_data(), source->get_data());
    EXPECT_TRUE(destination->has_data_for_cycle());
}

TEST(AudioBufferTest, ReadOnceRefusesANullSourceOrASizeMismatch)
{
    auto destination = make_buffer(0, 16);
    std::ranges::fill(destination->get_data(), 0.3);
    auto mismatched = make_buffer(0, 8);
    std::ranges::fill(mismatched->get_data(), 0.9);

    EXPECT_FALSE(destination->read_once(nullptr));
    EXPECT_FALSE(destination->read_once(mismatched));

    EXPECT_EQ(destination->get_data(), std::vector<double>(16, 0.3));
}

TEST(AudioBufferTest, ReadOnceRefusesASourceThatIsBeingProcessedUnlessForced)
{
    auto source = make_buffer(0, 16);
    std::ranges::fill(source->get_data(), 0.25);
    ASSERT_TRUE(source->try_acquire_processing());
    auto destination = make_buffer(1, 16);

    EXPECT_FALSE(destination->read_once(source));
    EXPECT_EQ(destination->get_data(), std::vector<double>(16, 0.0));

    EXPECT_TRUE(destination->read_once(source, true));
    EXPECT_EQ(destination->get_data(), source->get_data());
    source->release_processing();
}

TEST(AudioBufferTest, ReadOnceRefusesWhileTheDestinationIsBeingProcessed)
{
    auto source = make_buffer(0, 16);
    auto destination = make_buffer(1, 16);
    ASSERT_TRUE(destination->try_acquire_processing());

    EXPECT_FALSE(destination->read_once(source));
    destination->release_processing();
}

TEST(AudioBufferTest, TryAcquireProcessingIsExclusiveUntilReleased)
{
    auto buffer = make_buffer(0, 16);
    EXPECT_FALSE(buffer->is_processing());

    EXPECT_TRUE(buffer->try_acquire_processing());
    EXPECT_TRUE(buffer->is_processing());
    EXPECT_FALSE(buffer->try_acquire_processing());

    buffer->release_processing();

    EXPECT_FALSE(buffer->is_processing());
    EXPECT_TRUE(buffer->try_acquire_processing());
    buffer->release_processing();
}

TEST(AudioBufferTest, HasDataForCycleFollowsTheDataFlag)
{
    auto buffer = make_buffer(0, 16);
    EXPECT_TRUE(buffer->has_data_for_cycle());

    buffer->mark_for_processing(false);
    EXPECT_FALSE(buffer->has_data_for_cycle());

    buffer->mark_for_processing(true);
    EXPECT_TRUE(buffer->has_data_for_cycle());
}

TEST(AudioBufferTest, MarkForRemovalIsSticky)
{
    auto buffer = make_buffer(0, 16);
    EXPECT_FALSE(buffer->needs_removal());

    buffer->mark_for_removal();

    EXPECT_TRUE(buffer->needs_removal());
}

TEST(AudioBufferTest, InternalUsageRoundTrips)
{
    auto buffer = make_buffer(0, 16);
    EXPECT_FALSE(buffer->is_internal_only());

    buffer->force_internal_usage(true);
    EXPECT_TRUE(buffer->is_internal_only());

    buffer->force_internal_usage(false);
    EXPECT_FALSE(buffer->is_internal_only());
}

TEST(AudioBufferTest, RoutingStateDefaultsToIdleAndDrivesNeedsRouting)
{
    auto buffer = make_buffer(0, 16);
    const auto& defaults = buffer->get_routing_state();

    EXPECT_EQ(defaults.phase, BufferRoutingState::NONE);
    EXPECT_EQ(defaults.from_amount, 1.0);
    EXPECT_EQ(defaults.to_amount, 0.0);
    EXPECT_EQ(defaults.fade_cycles, 0U);
    EXPECT_FALSE(buffer->needs_routing());

    buffer->get_routing_state().phase = BufferRoutingState::ACTIVE;
    EXPECT_TRUE(buffer->needs_routing());

    buffer->get_routing_state().phase = BufferRoutingState::COMPLETED;
    EXPECT_TRUE(buffer->needs_routing());

    buffer->get_routing_state().phase = BufferRoutingState::NONE;
    EXPECT_FALSE(buffer->needs_routing());
}

}
