#include "gtest/gtest.h"
#include "../../support/test_metadata.hpp"

#include "MayaFlux/Buffers/AudioBuffer.hpp"
#include "MayaFlux/Buffers/BufferProcessingChain.hpp"
#include "MayaFlux/Buffers/BufferProcessor.hpp"
#include "MayaFlux/Buffers/Managers/BufferAccessControl.hpp"
#include "MayaFlux/Buffers/Managers/BufferProcessingControl.hpp"
#include "MayaFlux/Buffers/Managers/TokenUnitManager.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {

using Buffers::AudioBuffer;
using Buffers::AudioProcessingFunction;
using Buffers::Buffer;
using Buffers::BufferAccessControl;
using Buffers::BufferProcessingChain;
using Buffers::BufferProcessingControl;
using Buffers::BufferProcessor;
using Buffers::TokenUnitManager;

namespace {

    constexpr uint32_t UNIT_SIZE = 512;

    class CountingProbe final : public BufferProcessor {
    public:
        void processing_function(const std::shared_ptr<Buffer>&) override
        {
            ++runs;
        }

        int runs { 0 };
    };

    bool holds(
        const std::shared_ptr<BufferProcessingChain>& chain,
        const std::shared_ptr<Buffer>& buffer,
        const std::shared_ptr<BufferProcessor>& processor)
    {
        const auto& processors = chain->get_processors(buffer);
        return std::ranges::find(processors, processor) != processors.end();
    }

}

class BufferProcessingControlTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        units = std::make_unique<TokenUnitManager>(Buffers::AUDIO_BACKEND, Buffers::GRAPHICS_BACKEND, UNIT_SIZE);
        access = std::make_unique<BufferAccessControl>(*units);
        control = std::make_unique<BufferProcessingControl>(*units, *access);
        probe = std::make_shared<CountingProbe>();
    }

    std::shared_ptr<BufferProcessingChain> channel_chain(uint32_t channel)
    {
        return access->get_audio_processing_chain(Buffers::AUDIO_BACKEND, channel);
    }

    std::shared_ptr<Buffers::RootAudioBuffer> root(uint32_t channel)
    {
        return access->get_root_audio_buffer(Buffers::AUDIO_BACKEND, channel);
    }

    std::unique_ptr<TokenUnitManager> units;
    std::unique_ptr<BufferAccessControl> access;
    std::unique_ptr<BufferProcessingControl> control;
    std::shared_ptr<CountingProbe> probe;
};

TEST_F(BufferProcessingControlTest, AddProcessorToBufferAttachesToItsOwnChainAndNotTheUnitChain)
{
    auto child = std::make_shared<AudioBuffer>();
    access->add_buffer(child, Buffers::AUDIO_BACKEND, 1);

    control->add_processor(probe, child, Buffers::AUDIO_BACKEND);

    EXPECT_TRUE(holds(child->get_processing_chain(), child, probe));
    EXPECT_FALSE(holds(channel_chain(1), child, probe));
    EXPECT_FALSE(holds(channel_chain(0), child, probe));
}

TEST_F(BufferProcessingControlTest, AddProcessorToABufferUnderTheParallelTokenAttachesToItsOwnChain)
{
    auto child = std::make_shared<AudioBuffer>();
    access->add_buffer(child, Buffers::AUDIO_PARALLEL, 0);

    control->add_processor(probe, child, Buffers::AUDIO_PARALLEL);

    EXPECT_TRUE(holds(child->get_processing_chain(), child, probe));
    EXPECT_FALSE(holds(access->get_audio_processing_chain(Buffers::AUDIO_PARALLEL, 0), child, probe));
    EXPECT_FALSE(units->has_audio_unit(Buffers::AUDIO_BACKEND));
}

TEST_F(BufferProcessingControlTest, AddProcessorWithATokenThatHasNoUnitIsDroppedAndCreatesNothing)
{
    auto child = std::make_shared<AudioBuffer>();
    access->add_buffer(child, Buffers::AUDIO_BACKEND, 0);

    control->add_processor(probe, child, Buffers::AUDIO_PARALLEL);

    EXPECT_FALSE(units->has_audio_unit(Buffers::AUDIO_PARALLEL));
    EXPECT_FALSE(holds(child->get_processing_chain(), child, probe));
}

TEST_F(BufferProcessingControlTest, AddProcessorToBufferWithNoMatchingChannelIsDropped)
{
    auto child = std::make_shared<AudioBuffer>();

    control->add_processor(probe, child, Buffers::AUDIO_BACKEND);

    EXPECT_FALSE(units->has_audio_unit(Buffers::AUDIO_BACKEND));
}

TEST_F(BufferProcessingControlTest, AddProcessorToAChannelReachesOnlyThatRoot)
{
    access->ensure_audio_channels(Buffers::AUDIO_BACKEND, 2);

    control->add_processor(probe, Buffers::AUDIO_BACKEND, 1);

    EXPECT_TRUE(holds(channel_chain(1), root(1), probe));
    EXPECT_FALSE(holds(channel_chain(0), root(0), probe));
}

TEST_F(BufferProcessingControlTest, AddProcessorToATokenReachesEveryChannel)
{
    access->ensure_audio_channels(Buffers::AUDIO_BACKEND, 3);

    control->add_processor(probe, Buffers::AUDIO_BACKEND);

    for (uint32_t channel = 0; channel < 3; ++channel) {
        EXPECT_TRUE(holds(channel_chain(channel), root(channel), probe)) << "channel " << channel;
    }
}

TEST_F(BufferProcessingControlTest, RemoveProcessorFromAChannelLeavesOtherChannelsAlone)
{
    access->ensure_audio_channels(Buffers::AUDIO_BACKEND, 2);
    control->add_processor(probe, Buffers::AUDIO_BACKEND);

    control->remove_processor_from_channel(probe, Buffers::AUDIO_BACKEND, 1);

    EXPECT_FALSE(holds(channel_chain(1), root(1), probe));
    EXPECT_TRUE(holds(channel_chain(0), root(0), probe));
}

TEST_F(BufferProcessingControlTest, RemoveProcessorFromABufferDetachesIt)
{
    auto child = std::make_shared<AudioBuffer>();
    access->add_buffer(child, Buffers::AUDIO_BACKEND, 1);
    control->add_processor(probe, child, Buffers::AUDIO_BACKEND);
    ASSERT_TRUE(holds(child->get_processing_chain(), child, probe));

    control->remove_processor(probe, child, Buffers::AUDIO_BACKEND);

    EXPECT_FALSE(holds(child->get_processing_chain(), child, probe));
    EXPECT_FALSE(holds(channel_chain(1), child, probe));
}

TEST_F(BufferProcessingControlTest, RemoveProcessorFromABufferUnderTheParallelTokenDetachesIt)
{
    auto child = std::make_shared<AudioBuffer>();
    access->add_buffer(child, Buffers::AUDIO_PARALLEL, 0);
    control->add_processor(probe, child, Buffers::AUDIO_PARALLEL);
    ASSERT_TRUE(holds(child->get_processing_chain(), child, probe));

    control->remove_processor(probe, child, Buffers::AUDIO_PARALLEL);

    EXPECT_FALSE(holds(child->get_processing_chain(), child, probe));
}

TEST_F(BufferProcessingControlTest, RemoveProcessorFromAMissingChannelCreatesNothing)
{
    EXPECT_NO_THROW(control->remove_processor_from_channel(probe, Buffers::AUDIO_BACKEND, 3));

    EXPECT_FALSE(units->has_audio_unit(Buffers::AUDIO_BACKEND));
}

TEST_F(BufferProcessingControlTest, ProcessingAChannelRunsOnlyThatChannelsProcessors)
{
    access->ensure_audio_channels(Buffers::AUDIO_BACKEND, 2);
    control->add_processor(probe, Buffers::AUDIO_BACKEND, 1);

    channel_chain(0)->process(root(0));
    EXPECT_EQ(probe->runs, 0);

    channel_chain(1)->process(root(1));
    EXPECT_EQ(probe->runs, 1);
}

TEST_F(BufferProcessingControlTest, FinalProcessorLandsOnEachRootsOwnChain)
{
    access->ensure_audio_channels(Buffers::AUDIO_BACKEND, 2);

    control->set_final_processor(probe, Buffers::AUDIO_BACKEND);

    for (uint32_t channel = 0; channel < 2; ++channel) {
        auto root_buffer = root(channel);
        EXPECT_EQ(root_buffer->get_processing_chain()->get_final_processor(root_buffer), probe) << "channel " << channel;
    }
}

TEST_F(BufferProcessingControlTest, ProcessingTheRootSumsItsChildrenIntoTheOutput)
{
    auto child = std::make_shared<AudioBuffer>();
    access->add_buffer(child, Buffers::AUDIO_BACKEND, 1);
    std::ranges::fill(child->get_data(), 0.5);

    root(1)->process_default();

    EXPECT_DOUBLE_EQ(root(1)->get_data().front(), 0.5);
    EXPECT_DOUBLE_EQ(root(1)->get_data().back(), 0.5);
}

TEST_F(BufferProcessingControlTest, AChildMarkedForRemovalIsNotSummedAndLeavesTheRoot)
{
    auto child = std::make_shared<AudioBuffer>();
    access->add_buffer(child, Buffers::AUDIO_BACKEND, 1);
    std::ranges::fill(child->get_data(), 0.5);
    child->mark_for_removal();

    root(1)->process_default();
    EXPECT_DOUBLE_EQ(root(1)->get_data().front(), 0.0);

    root(1)->process_default();
    EXPECT_TRUE(access->get_audio_buffers(Buffers::AUDIO_BACKEND, 1).empty());
}

TEST_F(BufferProcessingControlTest, AttachQuickProcessToABufferRegistersTheReturnedProcessorOnItsOwnChain)
{
    auto child = std::make_shared<AudioBuffer>();
    access->add_buffer(child, Buffers::AUDIO_BACKEND, 1);
    AudioProcessingFunction body = [](const std::shared_ptr<AudioBuffer>&) { };

    auto quick = control->attach_quick_process(body, child, Buffers::AUDIO_BACKEND);

    ASSERT_NE(quick, nullptr);
    EXPECT_TRUE(holds(child->get_processing_chain(), child, quick));
    EXPECT_FALSE(holds(channel_chain(1), child, quick));
}

TEST_F(BufferProcessingControlTest, AttachQuickProcessToAChannelRegistersOnTheRoot)
{
    AudioProcessingFunction body = [](const std::shared_ptr<AudioBuffer>&) { };

    auto quick = control->attach_quick_process(body, Buffers::AUDIO_BACKEND, 1);

    ASSERT_NE(quick, nullptr);
    EXPECT_TRUE(holds(channel_chain(1), root(1), quick));
}

}
