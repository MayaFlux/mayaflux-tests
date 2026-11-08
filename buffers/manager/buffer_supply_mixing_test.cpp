#include "gtest/gtest.h"

#include "MayaFlux/Buffers/AudioBuffer.hpp"
#include "MayaFlux/Buffers/BufferProcessingChain.hpp"
#include "MayaFlux/Buffers/Managers/BufferAccessControl.hpp"
#include "MayaFlux/Buffers/Managers/BufferSupplyMixing.hpp"
#include "MayaFlux/Buffers/Managers/TokenUnitManager.hpp"
#include "MayaFlux/Buffers/Root/MixProcessor.hpp"

namespace MayaFlux::Test {

using Buffers::AudioBuffer;
using Buffers::BufferAccessControl;
using Buffers::BufferProcessingChain;
using Buffers::BufferRoutingState;
using Buffers::BufferSupplyMixing;
using Buffers::MixProcessor;
using Buffers::TokenUnitManager;

namespace {

    constexpr uint32_t UNIT_SIZE = 512;

    class BufferSupplyMixingFixture : public ::testing::Test {
    protected:
        void SetUp() override
        {
            units = std::make_unique<TokenUnitManager>(Buffers::AUDIO_BACKEND, Buffers::GRAPHICS_BACKEND, UNIT_SIZE);
            access = std::make_unique<BufferAccessControl>(*units);
            mixing = std::make_unique<BufferSupplyMixing>(*units, *access);
        }

        std::shared_ptr<BufferProcessingChain> channel_chain(uint32_t channel)
        {
            return access->get_audio_processing_chain(Buffers::AUDIO_BACKEND, channel);
        }

        std::shared_ptr<Buffers::RootAudioBuffer> root(uint32_t channel)
        {
            return access->get_root_audio_buffer(Buffers::AUDIO_BACKEND, channel);
        }

        std::shared_ptr<AudioBuffer> make_source(double level)
        {
            auto source = std::make_shared<AudioBuffer>();
            std::ranges::fill(source->get_data(), level);
            return source;
        }

        std::unique_ptr<TokenUnitManager> units;
        std::unique_ptr<BufferAccessControl> access;
        std::unique_ptr<BufferSupplyMixing> mixing;
    };

}

using BufferSupplyMixingTest = BufferSupplyMixingFixture;

TEST_F(BufferSupplyMixingTest, SupplyRegistersAMixSourceOnTheTargetChannelRoot)
{
    access->ensure_audio_channels(Buffers::AUDIO_BACKEND, 2);
    auto source = make_source(1.0);

    EXPECT_TRUE(mixing->supply_audio_buffer_to(source, Buffers::AUDIO_BACKEND, 1));

    EXPECT_NE(channel_chain(1)->get_processor<MixProcessor>(root(1)), nullptr);
    EXPECT_EQ(channel_chain(0)->get_processor<MixProcessor>(root(0)), nullptr);
}

TEST_F(BufferSupplyMixingTest, SupplyToTheBuffersOwnChannelIsRefusedUnlessForced)
{
    access->ensure_audio_channels(Buffers::AUDIO_BACKEND, 1);
    auto source = make_source(1.0);
    access->add_buffer(source, Buffers::AUDIO_BACKEND, 0);

    EXPECT_FALSE(mixing->supply_audio_buffer_to(source, Buffers::AUDIO_BACKEND, 0));
    EXPECT_TRUE(mixing->supply_audio_buffer_to(source, Buffers::AUDIO_BACKEND, 0, 1.0, true));
}

TEST_F(BufferSupplyMixingTest, SupplyRefusesANullBufferAMissingChannelAndAMissingToken)
{
    access->ensure_audio_channels(Buffers::AUDIO_BACKEND, 1);
    auto source = make_source(1.0);

    EXPECT_FALSE(mixing->supply_audio_buffer_to(nullptr, Buffers::AUDIO_BACKEND, 0));
    EXPECT_FALSE(mixing->supply_audio_buffer_to(source, Buffers::AUDIO_BACKEND, 5));
    EXPECT_FALSE(mixing->supply_audio_buffer_to(source, Buffers::AUDIO_PARALLEL, 0));
}

TEST_F(BufferSupplyMixingTest, SupplyRefusesABufferWithNoSamples)
{
    access->ensure_audio_channels(Buffers::AUDIO_BACKEND, 2);
    auto source = make_source(1.0);
    source->resize(0);

    EXPECT_FALSE(mixing->supply_audio_buffer_to(source, Buffers::AUDIO_BACKEND, 1));
}

TEST_F(BufferSupplyMixingTest, ProcessingTheChannelAddsTheSuppliedSourceScaledByItsLevel)
{
    access->ensure_audio_channels(Buffers::AUDIO_BACKEND, 2);
    auto source = make_source(1.0);
    ASSERT_TRUE(mixing->supply_audio_buffer_to(source, Buffers::AUDIO_BACKEND, 1, 0.5));

    channel_chain(1)->process(root(1));

    EXPECT_DOUBLE_EQ(root(1)->get_data().front(), 0.5);
    EXPECT_DOUBLE_EQ(root(1)->get_data().back(), 0.5);
}

TEST_F(BufferSupplyMixingTest, SupplyingAgainUpdatesTheLevelAndNeverDoublesTheSource)
{
    access->ensure_audio_channels(Buffers::AUDIO_BACKEND, 2);
    auto source = make_source(1.0);
    ASSERT_TRUE(mixing->supply_audio_buffer_to(source, Buffers::AUDIO_BACKEND, 1, 0.5));
    ASSERT_TRUE(mixing->supply_audio_buffer_to(source, Buffers::AUDIO_BACKEND, 1, 0.25, true));
    root(1)->clear();

    channel_chain(1)->process(root(1));

    EXPECT_DOUBLE_EQ(root(1)->get_data().front(), 0.25);
}

TEST_F(BufferSupplyMixingTest, RemoveSuppliedReportsWhetherTheSourceWasRegistered)
{
    access->ensure_audio_channels(Buffers::AUDIO_BACKEND, 2);
    auto source = make_source(1.0);
    auto stranger = make_source(1.0);
    ASSERT_TRUE(mixing->supply_audio_buffer_to(source, Buffers::AUDIO_BACKEND, 1));

    EXPECT_FALSE(mixing->remove_supplied_audio_buffer(stranger, Buffers::AUDIO_BACKEND, 1));
    EXPECT_TRUE(mixing->remove_supplied_audio_buffer(source, Buffers::AUDIO_BACKEND, 1));
    EXPECT_FALSE(mixing->remove_supplied_audio_buffer(source, Buffers::AUDIO_BACKEND, 1));
}

TEST_F(BufferSupplyMixingTest, InterleavedInputIsSplitAcrossChannels)
{
    access->ensure_audio_channels(Buffers::AUDIO_BACKEND, 2);
    std::vector<double> interleaved { 1, -1, 2, -2, 3, -3, 4, -4 };

    mixing->fill_audio_from_interleaved(interleaved.data(), 4, Buffers::AUDIO_BACKEND, 2);

    EXPECT_EQ(access->get_audio_buffer_data(Buffers::AUDIO_BACKEND, 0)[0], 1.0);
    EXPECT_EQ(access->get_audio_buffer_data(Buffers::AUDIO_BACKEND, 0)[3], 4.0);
    EXPECT_EQ(access->get_audio_buffer_data(Buffers::AUDIO_BACKEND, 1)[0], -1.0);
    EXPECT_EQ(access->get_audio_buffer_data(Buffers::AUDIO_BACKEND, 1)[3], -4.0);
}

TEST_F(BufferSupplyMixingTest, InterleavedOutputRoundTripsTheInput)
{
    access->ensure_audio_channels(Buffers::AUDIO_BACKEND, 2);
    std::vector<double> interleaved { 1, -1, 2, -2, 3, -3, 4, -4 };
    mixing->fill_audio_from_interleaved(interleaved.data(), 4, Buffers::AUDIO_BACKEND, 2);
    std::vector<double> out(8, 0.0);

    mixing->fill_audio_interleaved(out.data(), 4, Buffers::AUDIO_BACKEND, 2);

    EXPECT_EQ(out, interleaved);
}

TEST_F(BufferSupplyMixingTest, ChannelsBeyondTheUnitAreNotWrittenButTheStrideStillCountsThem)
{
    access->ensure_audio_channels(Buffers::AUDIO_BACKEND, 2);
    std::vector<double> interleaved { 1, 2, 3, 4, 5, 6 };

    mixing->fill_audio_from_interleaved(interleaved.data(), 2, Buffers::AUDIO_BACKEND, 3);

    EXPECT_EQ(access->get_audio_buffer_data(Buffers::AUDIO_BACKEND, 0)[0], 1.0);
    EXPECT_EQ(access->get_audio_buffer_data(Buffers::AUDIO_BACKEND, 0)[1], 4.0);
    EXPECT_EQ(access->get_audio_buffer_data(Buffers::AUDIO_BACKEND, 1)[0], 2.0);
    EXPECT_EQ(access->get_audio_buffer_data(Buffers::AUDIO_BACKEND, 1)[1], 5.0);
}

TEST_F(BufferSupplyMixingTest, InterleavedIoWithAMissingTokenTouchesNothing)
{
    std::vector<double> interleaved { 1, 2, 3, 4 };
    std::vector<double> out(4, -7.0);

    mixing->fill_audio_from_interleaved(interleaved.data(), 2, Buffers::AUDIO_PARALLEL, 2);
    mixing->fill_audio_interleaved(out.data(), 2, Buffers::AUDIO_PARALLEL, 2);

    EXPECT_FALSE(units->has_audio_unit(Buffers::AUDIO_PARALLEL));
    EXPECT_EQ(out, std::vector<double>(4, -7.0));
}

TEST_F(BufferSupplyMixingTest, CloningCopiesTheDataOntoEachRequestedChannel)
{
    access->ensure_audio_channels(Buffers::AUDIO_BACKEND, 3);
    auto source = make_source(0.5);

    auto clones = mixing->clone_audio_buffer_for_channels(source, { 0, 2 }, Buffers::AUDIO_BACKEND);

    ASSERT_EQ(clones.size(), 2U);
    EXPECT_EQ(clones[0]->get_channel_id(), 0U);
    EXPECT_EQ(clones[1]->get_channel_id(), 2U);
    EXPECT_EQ(clones[1]->get_data(), source->get_data());
    EXPECT_EQ(access->get_audio_buffers(Buffers::AUDIO_BACKEND, 2), (std::vector<std::shared_ptr<AudioBuffer>> { clones[1] }));
}

TEST_F(BufferSupplyMixingTest, CloningRefusesEmptyChannelListsNullBuffersAndMissingTokens)
{
    access->ensure_audio_channels(Buffers::AUDIO_BACKEND, 1);
    auto source = make_source(0.5);

    EXPECT_TRUE(mixing->clone_audio_buffer_for_channels(source, {}, Buffers::AUDIO_BACKEND).empty());
    EXPECT_TRUE(mixing->clone_audio_buffer_for_channels(nullptr, { 0 }, Buffers::AUDIO_BACKEND).empty());
    EXPECT_TRUE(mixing->clone_audio_buffer_for_channels(source, { 0 }, Buffers::AUDIO_PARALLEL).empty());
}

TEST_F(BufferSupplyMixingTest, AnOutOfRangeCloneChannelStopsTheCloningAndKeepsTheEarlierClones)
{
    access->ensure_audio_channels(Buffers::AUDIO_BACKEND, 2);
    auto source = make_source(0.5);

    auto clones = mixing->clone_audio_buffer_for_channels(source, { 1, 9 }, Buffers::AUDIO_BACKEND);

    ASSERT_EQ(clones.size(), 1U);
    EXPECT_EQ(access->get_audio_buffers(Buffers::AUDIO_BACKEND, 1), (std::vector<std::shared_ptr<AudioBuffer>> { clones[0] }));
}

TEST_F(BufferSupplyMixingTest, RoutingSetsTheRoutingStateAndSuppliesTheTargetAtZeroLevel)
{
    access->ensure_audio_channels(Buffers::AUDIO_BACKEND, 2);
    auto source = make_source(1.0);
    access->add_buffer(source, Buffers::AUDIO_BACKEND, 0);

    mixing->route_buffer_to_channel(source, 1, 4, Buffers::AUDIO_BACKEND);

    const auto& state = source->get_routing_state();
    EXPECT_EQ(state.phase, BufferRoutingState::ACTIVE);
    EXPECT_EQ(state.from_channel, 0U);
    EXPECT_EQ(state.to_channel, 1U);
    EXPECT_EQ(state.fade_cycles, 4U);
    EXPECT_NE(channel_chain(1)->get_processor<MixProcessor>(root(1)), nullptr);
    channel_chain(1)->process(root(1));
    EXPECT_DOUBLE_EQ(root(1)->get_data().front(), 0.0);
}

TEST_F(BufferSupplyMixingTest, ARouteToTheSameOrAMissingChannelChangesNothing)
{
    access->ensure_audio_channels(Buffers::AUDIO_BACKEND, 2);
    auto source = make_source(1.0);
    access->add_buffer(source, Buffers::AUDIO_BACKEND, 0);

    mixing->route_buffer_to_channel(source, 0, 4, Buffers::AUDIO_BACKEND);
    mixing->route_buffer_to_channel(source, 5, 4, Buffers::AUDIO_BACKEND);

    EXPECT_EQ(source->get_routing_state().phase, BufferRoutingState::NONE);
    EXPECT_EQ(source->get_channel_id(), 0U);
}

TEST_F(BufferSupplyMixingTest, ARouteWithNoFadeTakesOneBlock)
{
    access->ensure_audio_channels(Buffers::AUDIO_BACKEND, 2);
    auto source = make_source(1.0);
    access->add_buffer(source, Buffers::AUDIO_BACKEND, 0);

    mixing->route_buffer_to_channel(source, 1, 0, Buffers::AUDIO_BACKEND);

    EXPECT_EQ(source->get_routing_state().fade_cycles, 1U);
}

TEST_F(BufferSupplyMixingTest, RoutingFadesTheTargetLevelAcrossBlocks)
{
    access->ensure_audio_channels(Buffers::AUDIO_BACKEND, 2);
    auto source = make_source(1.0);
    access->add_buffer(source, Buffers::AUDIO_BACKEND, 0);
    mixing->route_buffer_to_channel(source, 1, 4, Buffers::AUDIO_BACKEND);

    mixing->update_routing_states_for_cycle(Buffers::AUDIO_BACKEND);

    EXPECT_EQ(source->get_routing_state().cycles_elapsed, 1U);
    EXPECT_EQ(source->get_routing_state().phase, BufferRoutingState::ACTIVE);
    channel_chain(1)->process(root(1));
    EXPECT_DOUBLE_EQ(root(1)->get_data().front(), 0.25);
}

TEST_F(BufferSupplyMixingTest, ACompletedRouteMovesTheBufferToItsTargetChannel)
{
    access->ensure_audio_channels(Buffers::AUDIO_BACKEND, 2);
    auto source = make_source(1.0);
    access->add_buffer(source, Buffers::AUDIO_BACKEND, 0);
    mixing->route_buffer_to_channel(source, 1, 2, Buffers::AUDIO_BACKEND);
    mixing->update_routing_states_for_cycle(Buffers::AUDIO_BACKEND);
    mixing->update_routing_states_for_cycle(Buffers::AUDIO_BACKEND);
    ASSERT_EQ(source->get_routing_state().phase, BufferRoutingState::COMPLETED);

    mixing->cleanup_completed_routing(Buffers::AUDIO_BACKEND);

    EXPECT_EQ(access->get_audio_buffers(Buffers::AUDIO_BACKEND, 0).size(), 0U);
    EXPECT_EQ(access->get_audio_buffers(Buffers::AUDIO_BACKEND, 1), (std::vector<std::shared_ptr<AudioBuffer>> { source }));
    EXPECT_EQ(source->get_channel_id(), 1U);
    EXPECT_FALSE(source->needs_routing());
    EXPECT_FALSE(mixing->remove_supplied_audio_buffer(source, Buffers::AUDIO_BACKEND, 1));
}

TEST_F(BufferSupplyMixingTest, CleanupLeavesAnUnfinishedRouteWhereItIs)
{
    access->ensure_audio_channels(Buffers::AUDIO_BACKEND, 2);
    auto source = make_source(1.0);
    access->add_buffer(source, Buffers::AUDIO_BACKEND, 0);
    mixing->route_buffer_to_channel(source, 1, 3, Buffers::AUDIO_BACKEND);
    mixing->update_routing_states_for_cycle(Buffers::AUDIO_BACKEND);

    mixing->cleanup_completed_routing(Buffers::AUDIO_BACKEND);

    EXPECT_EQ(access->get_audio_buffers(Buffers::AUDIO_BACKEND, 0), (std::vector<std::shared_ptr<AudioBuffer>> { source }));
    EXPECT_EQ(source->get_routing_state().phase, BufferRoutingState::ACTIVE);
}

}
