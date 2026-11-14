#include "gtest/gtest.h"
#include "../../support/test_metadata.hpp"

#include "MayaFlux/Buffers/BufferProcessingChain.hpp"
#include "MayaFlux/Buffers/Managers/TokenUnitManager.hpp"
#include "MayaFlux/Buffers/Root/RootParallelAudioBuffer.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {

using Buffers::TokenUnitManager;

namespace {

    constexpr uint32_t UNIT_SIZE = 512;
    constexpr uint32_t SMALL_SIZE = 128;
    constexpr uint32_t MEDIUM_SIZE = 256;

    class TokenUnitManagerFixture : public ::testing::Test {
    protected:
        void SetUp() override
        {
            units = std::make_unique<TokenUnitManager>(Buffers::AUDIO_BACKEND, Buffers::GRAPHICS_BACKEND, UNIT_SIZE);
        }

        std::unique_ptr<TokenUnitManager> units;
    };

}

using TokenUnitManagerTest = TokenUnitManagerFixture;

TEST_F(TokenUnitManagerTest, ReportsTheDefaultTokens)
{
    EXPECT_EQ(units->get_default_audio_token(), Buffers::AUDIO_BACKEND);
    EXPECT_EQ(units->get_default_graphics_token(), Buffers::GRAPHICS_BACKEND);
}

TEST_F(TokenUnitManagerTest, TheParallelUnitCreatesParallelRootsAndTheBackendUnitDoesNot)
{
    units->ensure_audio_channels(Buffers::AUDIO_PARALLEL, 2);
    units->ensure_audio_channels(Buffers::AUDIO_BACKEND, 2);

    for (uint32_t channel = 0; channel < 2; ++channel) {
        EXPECT_NE(std::dynamic_pointer_cast<Buffers::RootParallelAudioBuffer>(
                      units->get_audio_unit(Buffers::AUDIO_PARALLEL).get_buffer(channel)),
            nullptr)
            << "channel " << channel;
        EXPECT_EQ(std::dynamic_pointer_cast<Buffers::RootParallelAudioBuffer>(
                      units->get_audio_unit(Buffers::AUDIO_BACKEND).get_buffer(channel)),
            nullptr)
            << "channel " << channel;
    }
}

TEST_F(TokenUnitManagerTest, NoUnitsExistUntilRequested)
{
    EXPECT_FALSE(units->has_audio_unit(Buffers::AUDIO_BACKEND));
    EXPECT_FALSE(units->has_graphics_unit(Buffers::GRAPHICS_BACKEND));
    EXPECT_TRUE(units->get_active_audio_tokens().empty());
    EXPECT_TRUE(units->get_active_graphics_tokens().empty());
    EXPECT_EQ(units->get_audio_channel_count(Buffers::AUDIO_BACKEND), 0U);
    EXPECT_EQ(units->get_audio_buffer_size(Buffers::AUDIO_BACKEND), UNIT_SIZE);
}

TEST_F(TokenUnitManagerTest, GetOrCreateAudioUnitStartsEmptyAndIsIdempotent)
{
    auto& unit = units->get_or_create_audio_unit(Buffers::AUDIO_BACKEND);

    EXPECT_EQ(unit.channel_count, 0U);
    EXPECT_TRUE(unit.root_buffers.empty());
    EXPECT_EQ(unit.buffer_size, UNIT_SIZE);
    EXPECT_TRUE(units->has_audio_unit(Buffers::AUDIO_BACKEND));
    EXPECT_EQ(&units->get_or_create_audio_unit(Buffers::AUDIO_BACKEND), &unit);
    EXPECT_TRUE(units->get_active_audio_tokens().empty());
}

TEST_F(TokenUnitManagerTest, AccessToAMissingAudioUnitThrowsWithoutCreatingIt)
{
    const TokenUnitManager& view = *units;

    EXPECT_THROW(view.get_audio_unit(Buffers::AUDIO_PARALLEL), std::out_of_range);
    EXPECT_THROW(units->get_audio_unit_mutable(Buffers::AUDIO_PARALLEL), std::out_of_range);
    EXPECT_FALSE(units->has_audio_unit(Buffers::AUDIO_PARALLEL));
}

TEST_F(TokenUnitManagerTest, EnsureAudioChannelsCreatesRootsAtTheUnitSize)
{
    units->ensure_audio_channels(Buffers::AUDIO_BACKEND, 3);

    const auto& unit = units->get_audio_unit(Buffers::AUDIO_BACKEND);
    EXPECT_EQ(unit.channel_count, 3U);
    ASSERT_EQ(unit.root_buffers.size(), 3U);
    ASSERT_EQ(unit.processing_chains.size(), 3U);
    for (uint32_t channel = 0; channel < 3; ++channel) {
        ASSERT_NE(unit.root_buffers[channel], nullptr);
        EXPECT_EQ(unit.root_buffers[channel]->get_channel_id(), channel);
        EXPECT_EQ(unit.root_buffers[channel]->get_num_samples(), UNIT_SIZE);
        EXPECT_NE(unit.processing_chains[channel], nullptr);
    }
}

TEST_F(TokenUnitManagerTest, EnsureAudioChannelsNeverShrinks)
{
    units->ensure_audio_channels(Buffers::AUDIO_BACKEND, 3);

    units->ensure_audio_channels(Buffers::AUDIO_BACKEND, 1);

    EXPECT_EQ(units->get_audio_channel_count(Buffers::AUDIO_BACKEND), 3U);
}

TEST_F(TokenUnitManagerTest, NewChannelsAreActiveAndCarryTheTokenPolicy)
{
    units->ensure_audio_channels(Buffers::AUDIO_BACKEND, 2);

    const auto& unit = units->get_audio_unit(Buffers::AUDIO_BACKEND);
    for (uint32_t channel = 0; channel < 2; ++channel) {
        EXPECT_TRUE(unit.root_buffers[channel]->is_token_active());
        EXPECT_EQ(unit.processing_chains[channel]->get_preferred_token(), Buffers::AUDIO_BACKEND);
        EXPECT_EQ(unit.processing_chains[channel]->get_enforcement_strategy(),
            Buffers::TokenEnforcementStrategy::FILTERED);
    }
}

TEST_F(TokenUnitManagerTest, EnsureAndGetAudioUnitGrowsToIncludeTheChannel)
{
    auto& unit = units->ensure_and_get_audio_unit(Buffers::AUDIO_BACKEND, 2);

    EXPECT_EQ(unit.channel_count, 3U);
    EXPECT_EQ(&unit, &units->get_audio_unit_mutable(Buffers::AUDIO_BACKEND));
}

TEST_F(TokenUnitManagerTest, ResizeAudioBuffersUpdatesTheUnitAndEveryRoot)
{
    units->ensure_audio_channels(Buffers::AUDIO_BACKEND, 2);

    units->resize_audio_buffers(Buffers::AUDIO_BACKEND, SMALL_SIZE);

    EXPECT_EQ(units->get_audio_buffer_size(Buffers::AUDIO_BACKEND), SMALL_SIZE);
    const auto& unit = units->get_audio_unit(Buffers::AUDIO_BACKEND);
    for (uint32_t channel = 0; channel < 2; ++channel) {
        EXPECT_EQ(unit.root_buffers[channel]->get_num_samples(), SMALL_SIZE);
    }
}

TEST_F(TokenUnitManagerTest, ResizeAudioBuffersRecordsTheSizeEvenBeforeAnyChannelExists)
{
    units->resize_audio_buffers(Buffers::AUDIO_BACKEND, MEDIUM_SIZE);

    EXPECT_TRUE(units->has_audio_unit(Buffers::AUDIO_BACKEND));
    EXPECT_EQ(units->get_audio_channel_count(Buffers::AUDIO_BACKEND), 0U);
    EXPECT_EQ(units->get_audio_buffer_size(Buffers::AUDIO_BACKEND), MEDIUM_SIZE);
}

TEST_F(TokenUnitManagerTest, ValidateNumAudioChannelsEnsuresChannelsAndSize)
{
    units->validate_num_audio_channels(Buffers::AUDIO_BACKEND, 2, MEDIUM_SIZE);

    EXPECT_EQ(units->get_audio_channel_count(Buffers::AUDIO_BACKEND), 2U);
    EXPECT_EQ(units->get_audio_buffer_size(Buffers::AUDIO_BACKEND), MEDIUM_SIZE);
    const auto& unit = units->get_audio_unit(Buffers::AUDIO_BACKEND);
    EXPECT_EQ(unit.root_buffers[1]->get_num_samples(), MEDIUM_SIZE);
}

TEST_F(TokenUnitManagerTest, ActiveAudioTokensListOnlyUnitsWithChannels)
{
    units->get_or_create_audio_unit(Buffers::AUDIO_PARALLEL);
    units->ensure_audio_channels(Buffers::AUDIO_BACKEND, 1);

    EXPECT_EQ(units->get_active_audio_tokens(), (std::vector<Buffers::ProcessingToken> { Buffers::AUDIO_BACKEND }));
}

TEST_F(TokenUnitManagerTest, AudioUnitsAreIndependentPerToken)
{
    units->ensure_audio_channels(Buffers::AUDIO_BACKEND, 2);
    units->ensure_audio_channels(Buffers::AUDIO_PARALLEL, 1);

    units->resize_audio_buffers(Buffers::AUDIO_BACKEND, SMALL_SIZE);

    EXPECT_EQ(units->get_audio_channel_count(Buffers::AUDIO_BACKEND), 2U);
    EXPECT_EQ(units->get_audio_channel_count(Buffers::AUDIO_PARALLEL), 1U);
    EXPECT_EQ(units->get_audio_buffer_size(Buffers::AUDIO_PARALLEL), UNIT_SIZE);
}

TEST_F(TokenUnitManagerTest, EnsureWithZeroChannelsCreatesNoChannels)
{
    units->ensure_audio_channels(Buffers::AUDIO_BACKEND, 0);

    EXPECT_TRUE(units->has_audio_unit(Buffers::AUDIO_BACKEND));
    EXPECT_EQ(units->get_audio_channel_count(Buffers::AUDIO_BACKEND), 0U);
    EXPECT_TRUE(units->get_active_audio_tokens().empty());
}

TEST_F(TokenUnitManagerTest, EnsureAndGetOnAFreshTokenCreatesChannelZero)
{
    auto& unit = units->ensure_and_get_audio_unit(Buffers::AUDIO_PARALLEL, 0);

    EXPECT_EQ(unit.channel_count, 1U);
    ASSERT_EQ(unit.root_buffers.size(), 1U);
    ASSERT_NE(unit.root_buffers[0], nullptr);
    EXPECT_EQ(unit.root_buffers[0]->get_channel_id(), 0U);
}

TEST_F(TokenUnitManagerTest, ChannelsGrownAfterAResizeTakeTheResizedSize)
{
    units->ensure_audio_channels(Buffers::AUDIO_BACKEND, 2);
    units->resize_audio_buffers(Buffers::AUDIO_BACKEND, SMALL_SIZE);

    units->ensure_audio_channels(Buffers::AUDIO_BACKEND, 3);

    const auto& unit = units->get_audio_unit(Buffers::AUDIO_BACKEND);
    EXPECT_EQ(unit.buffer_size, SMALL_SIZE);
    EXPECT_EQ(unit.root_buffers[2]->get_num_samples(), SMALL_SIZE);
}

TEST_F(TokenUnitManagerTest, GraphicsUnitIsAbsentUntilCreatedAndQueriesDoNotCreateIt)
{
    const TokenUnitManager& view = *units;

    EXPECT_THROW(view.get_graphics_unit(Buffers::GRAPHICS_BACKEND), std::out_of_range);
    EXPECT_THROW(units->get_graphics_unit_mutable(Buffers::GRAPHICS_BACKEND), std::out_of_range);
    EXPECT_FALSE(units->has_graphics_unit(Buffers::GRAPHICS_BACKEND));
    EXPECT_TRUE(units->get_active_graphics_tokens().empty());
}

}
