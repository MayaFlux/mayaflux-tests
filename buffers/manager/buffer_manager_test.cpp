#include "gtest/gtest.h"

#include "MayaFlux/Buffers/AudioBuffer.hpp"
#include "MayaFlux/Buffers/BufferManager.hpp"
#include "MayaFlux/Buffers/BufferProcessor.hpp"
#include "MayaFlux/Buffers/Input/InputAudioBuffer.hpp"
#include "MayaFlux/Buffers/Root/RootAudioBuffer.hpp"
#include "MayaFlux/Buffers/Root/RootParallelAudioBuffer.hpp"

namespace MayaFlux::Test {

using Buffers::AudioBuffer;
using Buffers::AudioProcessingFunction;
using Buffers::BufferManager;
using Buffers::ProcessingToken;
using Buffers::RootAudioBuffer;

namespace {

    class PreferredSizeGuard {
    public:
        PreferredSizeGuard()
            : m_saved(Buffers::s_preferred_buffer_size)
        {
        }

        PreferredSizeGuard(const PreferredSizeGuard&) = delete;
        PreferredSizeGuard& operator=(const PreferredSizeGuard&) = delete;
        PreferredSizeGuard(PreferredSizeGuard&&) = delete;
        PreferredSizeGuard& operator=(PreferredSizeGuard&&) = delete;

        ~PreferredSizeGuard() { Buffers::s_preferred_buffer_size = m_saved; }

    private:
        uint32_t m_saved;
    };

    class FillProcessor final : public Buffers::BufferProcessor {
    public:
        explicit FillProcessor(double value)
            : m_value(value)
        {
        }

        void processing_function(const std::shared_ptr<Buffers::Buffer>& buffer) override
        {
            if (auto audio = std::dynamic_pointer_cast<AudioBuffer>(buffer)) {
                std::ranges::fill(audio->get_data(), m_value);
            }
        }

    private:
        double m_value;
    };

    class RunCounter final : public Buffers::BufferProcessor {
    public:
        void processing_function(const std::shared_ptr<Buffers::Buffer>&) override { ++runs; }

        int runs { 0 };
    };

    class BufferManagerFixture : public ::testing::Test {
    protected:
        std::shared_ptr<BufferManager> make_manager(uint32_t out = 2, uint32_t in = 0, uint32_t block = 512)
        {
            return std::make_shared<BufferManager>(out, in, 48000, block);
        }

        std::shared_ptr<AudioBuffer> make_child(double value)
        {
            auto child = std::make_shared<AudioBuffer>();
            child->resize(manager->get_buffer_size(ProcessingToken::AUDIO_BACKEND));
            std::ranges::fill(child->get_data(), value);
            return child;
        }

        PreferredSizeGuard guard;
        std::shared_ptr<BufferManager> manager;
    };

}

using BufferManagerTest = BufferManagerFixture;

TEST_F(BufferManagerTest, ConstructionSetsTheChannelsBlockSizeRateAndDefaultToken)
{
    manager = make_manager(2, 0, 512);

    EXPECT_EQ(manager->get_num_channels(ProcessingToken::AUDIO_BACKEND), 2U);
    EXPECT_EQ(manager->get_buffer_size(ProcessingToken::AUDIO_BACKEND), 512U);
    EXPECT_EQ(manager->get_sample_rate(), 48000U);
    EXPECT_EQ(manager->get_default_audio_token(), ProcessingToken::AUDIO_BACKEND);
}

TEST_F(BufferManagerTest, ConstructionCreatesOneRootPerOutputChannel)
{
    manager = make_manager(3, 0, 512);

    for (uint32_t channel = 0; channel < 3; ++channel) {
        const auto root = manager->get_root_audio_buffer(ProcessingToken::AUDIO_BACKEND, channel);
        ASSERT_NE(root, nullptr);
        EXPECT_EQ(root->get_channel_id(), channel);
        EXPECT_EQ(root->get_num_samples(), 512U);
    }
}

TEST_F(BufferManagerTest, ConstructionWithoutInputChannelsHasNoInput)
{
    manager = make_manager(2, 0, 512);

    EXPECT_EQ(manager->get_num_input_channels(), 0U);
}

TEST_F(BufferManagerTest, ConstructionWithInputChannelsCreatesThemForEachChannel)
{
    manager = make_manager(2, 2, 512);

    EXPECT_EQ(manager->get_num_input_channels(), 2U);
    EXPECT_NE(manager->get_input_buffer(0), nullptr);
    EXPECT_NE(manager->get_input_buffer(1), nullptr);
}

TEST_F(BufferManagerTest, TheDefaultAudioTokenIsActiveFromTheStart)
{
    manager = make_manager();

    const auto tokens = manager->get_active_tokens();

    EXPECT_NE(std::ranges::find(tokens, ProcessingToken::AUDIO_BACKEND), tokens.end());
}

TEST_F(BufferManagerTest, TheGlobalChainIsOneSharedObject)
{
    manager = make_manager();

    const auto first = manager->get_global_processing_chain();

    ASSERT_NE(first, nullptr);
    EXPECT_EQ(manager->get_global_processing_chain(), first);
}

TEST_F(BufferManagerTest, CreateAudioBufferSizesItToTheTokenAndRegistersIt)
{
    manager = make_manager(2, 0, 512);

    const auto buffer = manager->create_audio_buffer<AudioBuffer>(ProcessingToken::AUDIO_BACKEND, 1);

    ASSERT_NE(buffer, nullptr);
    EXPECT_EQ(buffer->get_channel_id(), 1U);
    EXPECT_EQ(buffer->get_num_samples(), 512U);
    const auto& registered = manager->get_buffers(ProcessingToken::AUDIO_BACKEND, 1);
    EXPECT_NE(std::ranges::find(registered, buffer), registered.end());
}

TEST_F(BufferManagerTest, AddThenRemoveBufferUpdatesTheChannelList)
{
    manager = make_manager(2, 0, 512);
    const auto child = make_child(0.0);

    manager->add_buffer(child, ProcessingToken::AUDIO_BACKEND, 0);
    EXPECT_EQ(manager->get_buffers(ProcessingToken::AUDIO_BACKEND, 0).size(), 1U);

    manager->remove_buffer(child, ProcessingToken::AUDIO_BACKEND, 0);
    EXPECT_TRUE(manager->get_buffers(ProcessingToken::AUDIO_BACKEND, 0).empty());
}

TEST_F(BufferManagerTest, ChildSamplesReachTheOutputThroughProcessChannel)
{
    manager = make_manager(2, 0, 512);
    manager->add_buffer(make_child(0.25), ProcessingToken::AUDIO_BACKEND, 0);

    manager->process_channel(ProcessingToken::AUDIO_BACKEND, 0, 512);

    const auto& output = manager->get_buffer_data(ProcessingToken::AUDIO_BACKEND, 0);
    EXPECT_TRUE(std::ranges::all_of(output, [](double sample) { return sample == 0.25; }));
}

TEST_F(BufferManagerTest, ProcessTokenProcessesEveryChannelOfTheToken)
{
    manager = make_manager(2, 0, 512);
    manager->add_buffer(make_child(0.1), ProcessingToken::AUDIO_BACKEND, 0);
    manager->add_buffer(make_child(0.2), ProcessingToken::AUDIO_BACKEND, 1);

    manager->process_token(ProcessingToken::AUDIO_BACKEND, 512);

    EXPECT_DOUBLE_EQ(manager->get_buffer_data(ProcessingToken::AUDIO_BACKEND, 0).front(), 0.1);
    EXPECT_DOUBLE_EQ(manager->get_buffer_data(ProcessingToken::AUDIO_BACKEND, 1).front(), 0.2);
}

TEST_F(BufferManagerTest, ProcessAllTokensReachesTheDefaultAudioToken)
{
    manager = make_manager(2, 0, 512);
    manager->add_buffer(make_child(0.3), ProcessingToken::AUDIO_BACKEND, 0);

    manager->process_all_tokens();

    EXPECT_DOUBLE_EQ(manager->get_buffer_data(ProcessingToken::AUDIO_BACKEND, 0).front(), 0.3);
}

TEST_F(BufferManagerTest, ProcessingAnInputTokenIsANoOp)
{
    manager = make_manager(2, 0, 512);
    manager->add_buffer(make_child(0.4), ProcessingToken::AUDIO_BACKEND, 0);

    EXPECT_NO_THROW(manager->process_token(ProcessingToken::INPUT_BACKEND, 512));

    EXPECT_DOUBLE_EQ(manager->get_buffer_data(ProcessingToken::AUDIO_BACKEND, 0).front(), 0.0);
}

TEST_F(BufferManagerTest, ACustomTokenProcessorReplacesTheDefaultProcessing)
{
    manager = make_manager(2, 0, 512);
    manager->add_buffer(make_child(0.25), ProcessingToken::AUDIO_BACKEND, 0);
    int calls = 0;

    manager->register_audio_token_processor(ProcessingToken::AUDIO_BACKEND,
        [&calls](std::vector<std::shared_ptr<RootAudioBuffer>>& roots, uint32_t) {
            ++calls;
            for (auto& root : roots) {
                std::ranges::fill(root->get_data(), 0.5);
            }
        });

    manager->process_token(ProcessingToken::AUDIO_BACKEND, 512);

    EXPECT_EQ(calls, 1);
    EXPECT_DOUBLE_EQ(manager->get_buffer_data(ProcessingToken::AUDIO_BACKEND, 0).front(), 0.5);
    EXPECT_DOUBLE_EQ(manager->get_buffer_data(ProcessingToken::AUDIO_BACKEND, 1).front(), 0.5);
}

TEST_F(BufferManagerTest, ResizingTheAudioTokenChangesItsBufferSize)
{
    manager = make_manager(2, 0, 512);

    manager->resize_buffers(ProcessingToken::AUDIO_BACKEND, 256);

    EXPECT_EQ(manager->get_buffer_size(ProcessingToken::AUDIO_BACKEND), 256U);
    EXPECT_EQ(manager->get_num_channels(ProcessingToken::AUDIO_BACKEND), 2U);
}

TEST_F(BufferManagerTest, ResizingANonAudioTokenLeavesTheAudioSizeAlone)
{
    manager = make_manager(2, 0, 512);

    manager->resize_buffers(ProcessingToken::GRAPHICS_BACKEND, 256);

    EXPECT_EQ(manager->get_buffer_size(ProcessingToken::AUDIO_BACKEND), 512U);
}

TEST_F(BufferManagerTest, EnsureChannelsGrowsTheChannelCountAndRoots)
{
    manager = make_manager(2, 0, 512);

    manager->ensure_channels(ProcessingToken::AUDIO_BACKEND, 4);

    EXPECT_EQ(manager->get_num_channels(ProcessingToken::AUDIO_BACKEND), 4U);
    const auto root = manager->get_root_audio_buffer(ProcessingToken::AUDIO_BACKEND, 3);
    ASSERT_NE(root, nullptr);
    EXPECT_EQ(root->get_channel_id(), 3U);
}

TEST_F(BufferManagerTest, TheDefaultOutputIsLimitedToFullScale)
{
    manager = make_manager(2, 0, 512);
    manager->add_buffer(make_child(10.0), ProcessingToken::AUDIO_BACKEND, 0);

    manager->process_channel(ProcessingToken::AUDIO_BACKEND, 0, 512);

    const auto& output = manager->get_buffer_data(ProcessingToken::AUDIO_BACKEND, 0);
    EXPECT_TRUE(std::ranges::all_of(output, [](double sample) { return std::abs(sample) <= 1.0; }));
}

TEST_F(BufferManagerTest, AddProcessorToABufferRunsItBeforeTheChannelSum)
{
    manager = make_manager(2, 0, 512);
    const auto child = make_child(0.0);
    manager->add_buffer(child, ProcessingToken::AUDIO_BACKEND, 0);
    manager->add_processor(std::make_shared<FillProcessor>(0.6), child);

    manager->process_channel(ProcessingToken::AUDIO_BACKEND, 0, 512);

    EXPECT_DOUBLE_EQ(manager->get_buffer_data(ProcessingToken::AUDIO_BACKEND, 0).front(), 0.6);
}

TEST_F(BufferManagerTest, RemoveProcessorFromABufferStopsItRunning)
{
    manager = make_manager(2, 0, 512);
    const auto child = make_child(0.0);
    const auto processor = std::make_shared<FillProcessor>(0.6);
    manager->add_buffer(child, ProcessingToken::AUDIO_BACKEND, 0);
    manager->add_processor(processor, child);
    manager->remove_processor(processor, child);
    std::ranges::fill(child->get_data(), 0.2);

    manager->process_channel(ProcessingToken::AUDIO_BACKEND, 0, 512);

    EXPECT_DOUBLE_EQ(manager->get_buffer_data(ProcessingToken::AUDIO_BACKEND, 0).front(), 0.2);
}

TEST_F(BufferManagerTest, AddProcessorToAChannelRunsAfterTheSum)
{
    manager = make_manager(2, 0, 512);
    manager->add_buffer(make_child(0.25), ProcessingToken::AUDIO_BACKEND, 0);
    manager->add_processor(std::make_shared<FillProcessor>(0.4), ProcessingToken::AUDIO_BACKEND, 0);

    manager->process_channel(ProcessingToken::AUDIO_BACKEND, 0, 512);

    EXPECT_DOUBLE_EQ(manager->get_buffer_data(ProcessingToken::AUDIO_BACKEND, 0).front(), 0.4);
}

TEST_F(BufferManagerTest, AddProcessorToATokenRunsOnEveryChannel)
{
    manager = make_manager(2, 0, 512);
    manager->add_processor(std::make_shared<FillProcessor>(0.4), ProcessingToken::AUDIO_BACKEND);

    manager->process_token(ProcessingToken::AUDIO_BACKEND, 512);

    EXPECT_DOUBLE_EQ(manager->get_buffer_data(ProcessingToken::AUDIO_BACKEND, 0).front(), 0.4);
    EXPECT_DOUBLE_EQ(manager->get_buffer_data(ProcessingToken::AUDIO_BACKEND, 1).front(), 0.4);
}

TEST_F(BufferManagerTest, RemoveProcessorFromAChannelLeavesTheOtherChannels)
{
    manager = make_manager(2, 0, 512);
    const auto processor = std::make_shared<FillProcessor>(0.4);
    manager->add_processor(processor, ProcessingToken::AUDIO_BACKEND);

    manager->remove_processor_from_channel(processor, ProcessingToken::AUDIO_BACKEND, 0);
    manager->process_token(ProcessingToken::AUDIO_BACKEND, 512);

    EXPECT_NE(manager->get_buffer_data(ProcessingToken::AUDIO_BACKEND, 0).front(), 0.4);
    EXPECT_DOUBLE_EQ(manager->get_buffer_data(ProcessingToken::AUDIO_BACKEND, 1).front(), 0.4);
}

TEST_F(BufferManagerTest, RemoveProcessorFromTheTokenClearsEveryChannel)
{
    manager = make_manager(2, 0, 512);
    const auto processor = std::make_shared<FillProcessor>(0.4);
    manager->add_processor(processor, ProcessingToken::AUDIO_BACKEND);

    manager->remove_processor_from_token(processor, ProcessingToken::AUDIO_BACKEND);
    manager->process_token(ProcessingToken::AUDIO_BACKEND, 512);

    EXPECT_NE(manager->get_buffer_data(ProcessingToken::AUDIO_BACKEND, 0).front(), 0.4);
    EXPECT_NE(manager->get_buffer_data(ProcessingToken::AUDIO_BACKEND, 1).front(), 0.4);
}

TEST_F(BufferManagerTest, SetFinalProcessorRunsAfterTheChain)
{
    manager = make_manager(2, 0, 512);
    manager->add_buffer(make_child(0.25), ProcessingToken::AUDIO_BACKEND, 0);
    manager->set_final_processor(std::make_shared<FillProcessor>(0.3), ProcessingToken::AUDIO_BACKEND);

    manager->process_channel(ProcessingToken::AUDIO_BACKEND, 0, 512);

    EXPECT_DOUBLE_EQ(manager->get_buffer_data(ProcessingToken::AUDIO_BACKEND, 0).front(), 0.3);
}

TEST_F(BufferManagerTest, AttachQuickProcessToABufferRunsIt)
{
    manager = make_manager(2, 0, 512);
    const auto child = make_child(0.0);
    manager->add_buffer(child, ProcessingToken::AUDIO_BACKEND, 0);

    const auto quick = manager->attach_quick_process(
        AudioProcessingFunction { [](const std::shared_ptr<AudioBuffer>& buffer) {
            std::ranges::fill(buffer->get_data(), 0.7);
        } },
        child, ProcessingToken::AUDIO_BACKEND);
    manager->process_channel(ProcessingToken::AUDIO_BACKEND, 0, 512);

    ASSERT_NE(quick, nullptr);
    EXPECT_DOUBLE_EQ(manager->get_buffer_data(ProcessingToken::AUDIO_BACKEND, 0).front(), 0.7);
}

TEST_F(BufferManagerTest, AttachQuickProcessToAChannelRunsOnlyThere)
{
    manager = make_manager(2, 0, 512);

    const auto quick = manager->attach_quick_process(
        AudioProcessingFunction { [](const std::shared_ptr<AudioBuffer>& buffer) {
            std::ranges::fill(buffer->get_data(), 0.2);
        } },
        ProcessingToken::AUDIO_BACKEND, 1);
    manager->process_token(ProcessingToken::AUDIO_BACKEND, 512);

    ASSERT_NE(quick, nullptr);
    EXPECT_NE(manager->get_buffer_data(ProcessingToken::AUDIO_BACKEND, 0).front(), 0.2);
    EXPECT_DOUBLE_EQ(manager->get_buffer_data(ProcessingToken::AUDIO_BACKEND, 1).front(), 0.2);
}

TEST_F(BufferManagerTest, AttachQuickProcessToATokenRunsOnEveryChannel)
{
    manager = make_manager(2, 0, 512);

    const auto quick = manager->attach_quick_process(
        AudioProcessingFunction { [](const std::shared_ptr<AudioBuffer>& buffer) {
            std::ranges::fill(buffer->get_data(), 0.2);
        } },
        ProcessingToken::AUDIO_BACKEND);
    manager->process_token(ProcessingToken::AUDIO_BACKEND, 512);

    ASSERT_NE(quick, nullptr);
    EXPECT_DOUBLE_EQ(manager->get_buffer_data(ProcessingToken::AUDIO_BACKEND, 0).front(), 0.2);
    EXPECT_DOUBLE_EQ(manager->get_buffer_data(ProcessingToken::AUDIO_BACKEND, 1).front(), 0.2);
}

TEST_F(BufferManagerTest, InterleavedFillRoundTripsThroughTheToken)
{
    manager = make_manager(2, 0, 512);
    const std::vector<double> interleaved { 1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0 };
    std::vector<double> out(interleaved.size(), 0.0);

    manager->fill_from_interleaved(interleaved.data(), 4, ProcessingToken::AUDIO_BACKEND, 2);
    manager->fill_interleaved(out.data(), 4, ProcessingToken::AUDIO_BACKEND, 2);

    EXPECT_EQ(out, interleaved);
}

TEST_F(BufferManagerTest, CloneBufferForChannelsReturnsOneCloneOnEachTargetChannel)
{
    manager = make_manager(2, 0, 512);
    const auto source = make_child(0.5);

    const auto clones = manager->clone_buffer_for_channels(source, { 1 }, ProcessingToken::AUDIO_BACKEND);

    ASSERT_EQ(clones.size(), 1U);
    EXPECT_EQ(clones[0]->get_channel_id(), 1U);
    EXPECT_NE(clones[0], source);
}

TEST_F(BufferManagerTest, SupplyBufferMixesIntoTheChannelUntilRemoved)
{
    manager = make_manager(2, 0, 512);
    const auto source = make_child(0.5);

    ASSERT_TRUE(manager->supply_buffer_to(source, ProcessingToken::AUDIO_BACKEND, 1));
    manager->process_channel(ProcessingToken::AUDIO_BACKEND, 1, 512);
    EXPECT_DOUBLE_EQ(manager->get_buffer_data(ProcessingToken::AUDIO_BACKEND, 1).front(), 0.5);

    EXPECT_TRUE(manager->remove_supplied_buffer(source, ProcessingToken::AUDIO_BACKEND, 1));
    EXPECT_FALSE(manager->remove_supplied_buffer(source, ProcessingToken::AUDIO_BACKEND, 1));
    manager->process_channel(ProcessingToken::AUDIO_BACKEND, 1, 512);
    EXPECT_DOUBLE_EQ(manager->get_buffer_data(ProcessingToken::AUDIO_BACKEND, 1).front(), 0.0);
}

TEST_F(BufferManagerTest, RoutingCanBeRequestedAndAdvancedWithoutThrowing)
{
    manager = make_manager(2, 0, 512);
    const auto source = make_child(0.5);
    manager->add_buffer(source, ProcessingToken::AUDIO_BACKEND, 0);

    EXPECT_NO_THROW(manager->route_buffer_to_channel(source, 1, 4, ProcessingToken::AUDIO_BACKEND));
    EXPECT_NO_THROW(manager->update_routing_states(ProcessingToken::AUDIO_BACKEND));
    EXPECT_NO_THROW(manager->cleanup_completed_routing(ProcessingToken::AUDIO_BACKEND));
}

TEST_F(BufferManagerTest, ProcessInputDeinterleavesIntoTheInputChannels)
{
    manager = make_manager(2, 2, 512);
    std::vector<double> input { 0.1, 0.2, 0.3, 0.4 };

    manager->process_input(input.data(), 2, 2);

    const auto left = manager->get_input_buffer(0);
    const auto right = manager->get_input_buffer(1);
    ASSERT_NE(left, nullptr);
    ASSERT_NE(right, nullptr);
    EXPECT_DOUBLE_EQ(left->get_data()[0], 0.1);
    EXPECT_DOUBLE_EQ(left->get_data()[1], 0.3);
    EXPECT_DOUBLE_EQ(right->get_data()[0], 0.2);
    EXPECT_DOUBLE_EQ(right->get_data()[1], 0.4);
}

TEST_F(BufferManagerTest, AParallelBufferProcessorRunsOnceAndOnlyWhenTheParallelTokenIsProcessed)
{
    manager = make_manager(2, 0, 512);
    const auto child = make_child(0.0);
    const auto counter = std::make_shared<RunCounter>();
    manager->add_buffer(child, ProcessingToken::AUDIO_PARALLEL, 0);
    manager->add_processor(counter, child, ProcessingToken::AUDIO_PARALLEL);

    manager->process_token(ProcessingToken::AUDIO_BACKEND, 512);
    EXPECT_EQ(counter->runs, 0);

    manager->process_token(ProcessingToken::AUDIO_PARALLEL, 512);
    EXPECT_EQ(counter->runs, 1);
}

TEST_F(BufferManagerTest, TheParallelUnitMixesItsChildrenAndLeavesTheBackendUnitUntouched)
{
    manager = make_manager(2, 0, 512);
    manager->add_buffer(make_child(0.25), ProcessingToken::AUDIO_PARALLEL, 0);
    manager->add_buffer(make_child(0.75), ProcessingToken::AUDIO_PARALLEL, 0);

    manager->process_token(ProcessingToken::AUDIO_PARALLEL, 512);

    EXPECT_DOUBLE_EQ(manager->get_buffer_data(ProcessingToken::AUDIO_PARALLEL, 0).front(), 0.5);
    EXPECT_DOUBLE_EQ(manager->get_buffer_data(ProcessingToken::AUDIO_BACKEND, 0).front(), 0.0);
}

TEST_F(BufferManagerTest, EachParallelCycleCompletesAsOnePublishedBlockPerChannel)
{
    manager = make_manager(2, 0, 512);
    manager->add_buffer(make_child(0.25), ProcessingToken::AUDIO_PARALLEL, 0);
    manager->add_buffer(make_child(0.5), ProcessingToken::AUDIO_PARALLEL, 1);

    manager->process_token(ProcessingToken::AUDIO_PARALLEL, 512);
    manager->process_token(ProcessingToken::AUDIO_PARALLEL, 512);

    for (uint32_t channel = 0; channel < 2; ++channel) {
        const auto root = std::dynamic_pointer_cast<Buffers::RootParallelAudioBuffer>(
            manager->get_root_audio_buffer(ProcessingToken::AUDIO_PARALLEL, channel));
        ASSERT_NE(root, nullptr);
        EXPECT_DOUBLE_EQ(root->read_block().front(), channel == 0 ? 0.25 : 0.5);
        EXPECT_EQ(root->block_generation(), 2U);
    }
}

TEST_F(BufferManagerTest, TheBackendRootIsReadAsItsOwnDataAndNoParallelRootExistsForIt)
{
    manager = make_manager(2, 0, 512);
    manager->add_buffer(make_child(0.25), ProcessingToken::AUDIO_BACKEND, 0);
    manager->process_channel(ProcessingToken::AUDIO_BACKEND, 0, 512);

    const auto root = manager->get_root_audio_buffer(ProcessingToken::AUDIO_BACKEND, 0);

    EXPECT_EQ(std::dynamic_pointer_cast<Buffers::RootParallelAudioBuffer>(root), nullptr);
    EXPECT_EQ(root->read_block().data(), root->get_data().data());
}

TEST_F(BufferManagerTest, AParallelProcessorFeedsTheParallelRootMix)
{
    manager = make_manager(2, 0, 512);
    const auto child = make_child(0.0);
    manager->add_buffer(child, ProcessingToken::AUDIO_PARALLEL, 0);
    manager->add_processor(std::make_shared<FillProcessor>(0.6), child, ProcessingToken::AUDIO_PARALLEL);

    manager->process_token(ProcessingToken::AUDIO_PARALLEL, 512);

    EXPECT_DOUBLE_EQ(manager->get_buffer_data(ProcessingToken::AUDIO_PARALLEL, 0).front(), 0.6);
}

TEST_F(BufferManagerTest, AnInputListenerReceivesInputUntilUnregistered)
{
    manager = make_manager(2, 2, 512);
    const auto listener = std::make_shared<AudioBuffer>(0, 512);
    listener->resize(512);
    std::vector<double> first { 0.1, 0.2, 0.3, 0.4 };
    std::vector<double> second { 0.9, 0.8, 0.7, 0.6 };

    manager->register_input_listener(listener, 0);
    manager->process_input(first.data(), 2, 2);
    EXPECT_DOUBLE_EQ(listener->get_data()[0], 0.1);
    EXPECT_DOUBLE_EQ(listener->get_data()[1], 0.3);

    manager->unregister_input_listener(listener, 0);
    manager->process_input(second.data(), 2, 2);
    EXPECT_DOUBLE_EQ(listener->get_data()[0], 0.1);
}

}
