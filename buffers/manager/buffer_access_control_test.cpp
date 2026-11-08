#include "gtest/gtest.h"

#include "MayaFlux/Buffers/AudioBuffer.hpp"
#include "MayaFlux/Buffers/BufferProcessingChain.hpp"
#include "MayaFlux/Buffers/BufferProcessor.hpp"
#include "MayaFlux/Buffers/Managers/BufferAccessControl.hpp"
#include "MayaFlux/Buffers/Managers/TokenUnitManager.hpp"

namespace MayaFlux::Test {

using Buffers::AudioBuffer;
using Buffers::BufferAccessControl;
using Buffers::TokenUnitManager;

namespace {
    constexpr uint32_t UNIT_SIZE = 512;
    constexpr uint32_t SMALL_SIZE = 256;
    constexpr uint32_t TINY_SIZE = 128;

    class TokenedProbe final : public Buffers::BufferProcessor {
    public:
        explicit TokenedProbe(Buffers::ProcessingToken token)
        {
            set_processing_token(token);
        }

        void processing_function(const std::shared_ptr<Buffers::Buffer>&) override { }
    };
}

class BufferAccessControlTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        units = std::make_unique<TokenUnitManager>(Buffers::AUDIO_BACKEND, Buffers::GRAPHICS_BACKEND, UNIT_SIZE);
        access = std::make_unique<BufferAccessControl>(*units);
    }

    std::unique_ptr<TokenUnitManager> units;
    std::unique_ptr<BufferAccessControl> access;
};

TEST_F(BufferAccessControlTest, UnknownTokenHasNoChannelsAndTheDefaultSize)
{
    EXPECT_EQ(access->get_num_audio_out_channels(Buffers::AUDIO_BACKEND), 0U);
    EXPECT_EQ(access->get_audio_buffer_size(Buffers::AUDIO_BACKEND), UNIT_SIZE);
}

TEST_F(BufferAccessControlTest, EnsureAudioChannelsGrowsButNeverShrinks)
{
    access->ensure_audio_channels(Buffers::AUDIO_BACKEND, 2);
    EXPECT_EQ(access->get_num_audio_out_channels(Buffers::AUDIO_BACKEND), 2U);

    access->ensure_audio_channels(Buffers::AUDIO_BACKEND, 1);
    EXPECT_EQ(access->get_num_audio_out_channels(Buffers::AUDIO_BACKEND), 2U);
}

TEST_F(BufferAccessControlTest, RootAudioBufferHasTheUnitSizeAndIsStableAcrossCalls)
{
    access->ensure_audio_channels(Buffers::AUDIO_BACKEND, 2);

    auto root = access->get_root_audio_buffer(Buffers::AUDIO_BACKEND, 1);

    ASSERT_NE(root, nullptr);
    EXPECT_EQ(access->get_root_audio_buffer(Buffers::AUDIO_BACKEND, 1), root);
    EXPECT_EQ(access->get_audio_buffer_data(Buffers::AUDIO_BACKEND, 1).size(), UNIT_SIZE);
}

TEST_F(BufferAccessControlTest, ConstAccessToAMissingTokenThrows)
{
    const BufferAccessControl& view = *access;

    EXPECT_THROW(view.get_root_audio_buffer(Buffers::AUDIO_PARALLEL, 0), std::out_of_range);
}

TEST_F(BufferAccessControlTest, ChannelChainIsSharedAndDistinctPerChannel)
{
    auto chain = access->get_audio_processing_chain(Buffers::AUDIO_BACKEND, 1);

    ASSERT_NE(chain, nullptr);
    EXPECT_EQ(access->get_num_audio_out_channels(Buffers::AUDIO_BACKEND), 2U);
    EXPECT_EQ(access->get_audio_processing_chain(Buffers::AUDIO_BACKEND, 1), chain);
    EXPECT_NE(access->get_audio_processing_chain(Buffers::AUDIO_BACKEND, 0), chain);
}

TEST_F(BufferAccessControlTest, AddBufferAttachesToTheRequestedChannelAndWidensTheUnit)
{
    auto child = std::make_shared<AudioBuffer>();

    access->add_buffer(child, Buffers::AUDIO_BACKEND, 2);

    EXPECT_EQ(access->get_num_audio_out_channels(Buffers::AUDIO_BACKEND), 3U);
    EXPECT_EQ(child->get_channel_id(), 2U);
    EXPECT_EQ(access->get_audio_buffers(Buffers::AUDIO_BACKEND, 2), (std::vector<std::shared_ptr<AudioBuffer>> { child }));
    EXPECT_TRUE(access->get_audio_buffers(Buffers::AUDIO_BACKEND, 0).empty());
    EXPECT_EQ(access->get_root_audio_buffer(Buffers::AUDIO_BACKEND, 2)->get_num_children(), 1U);
}

TEST_F(BufferAccessControlTest, AddBufferResizesAChildToTheCurrentUnitSize)
{
    access->ensure_audio_channels(Buffers::AUDIO_BACKEND, 1);
    access->resize_audio_buffers(Buffers::AUDIO_BACKEND, SMALL_SIZE);
    auto child = std::make_shared<AudioBuffer>();
    ASSERT_NE(child->get_num_samples(), SMALL_SIZE);

    access->add_buffer(child, Buffers::AUDIO_BACKEND, 0);

    EXPECT_EQ(access->get_audio_buffer_size(Buffers::AUDIO_BACKEND), SMALL_SIZE);
    EXPECT_EQ(child->get_num_samples(), SMALL_SIZE);
}

TEST_F(BufferAccessControlTest, ResizeAudioBuffersResizesEveryExistingRoot)
{
    access->ensure_audio_channels(Buffers::AUDIO_BACKEND, 2);

    access->resize_audio_buffers(Buffers::AUDIO_BACKEND, TINY_SIZE);

    EXPECT_EQ(access->get_audio_buffer_size(Buffers::AUDIO_BACKEND), TINY_SIZE);
    EXPECT_EQ(access->get_audio_buffer_data(Buffers::AUDIO_BACKEND, 0).size(), TINY_SIZE);
    EXPECT_EQ(access->get_audio_buffer_data(Buffers::AUDIO_BACKEND, 1).size(), TINY_SIZE);
}

TEST_F(BufferAccessControlTest, RemoveBufferDetachesOnlyThatChild)
{
    auto kept = std::make_shared<AudioBuffer>();
    auto dropped = std::make_shared<AudioBuffer>();
    access->add_buffer(kept, Buffers::AUDIO_BACKEND, 0);
    access->add_buffer(dropped, Buffers::AUDIO_BACKEND, 0);

    access->remove_buffer(dropped, Buffers::AUDIO_BACKEND, 0);

    EXPECT_EQ(access->get_audio_buffers(Buffers::AUDIO_BACKEND, 0), (std::vector<std::shared_ptr<AudioBuffer>> { kept }));
}

TEST_F(BufferAccessControlTest, RemoveBufferFromAMissingTokenOrChannelIsHarmless)
{
    auto child = std::make_shared<AudioBuffer>();

    EXPECT_NO_THROW(access->remove_buffer(child, Buffers::AUDIO_BACKEND, 0));

    access->ensure_audio_channels(Buffers::AUDIO_BACKEND, 1);
    EXPECT_NO_THROW(access->remove_buffer(child, Buffers::AUDIO_BACKEND, 3));
    EXPECT_EQ(access->get_num_audio_out_channels(Buffers::AUDIO_BACKEND), 1U);
}

TEST_F(BufferAccessControlTest, GetAudioBuffersThrowsForAChannelOutOfRange)
{
    access->ensure_audio_channels(Buffers::AUDIO_BACKEND, 1);

    EXPECT_THROW(access->get_audio_buffers(Buffers::AUDIO_BACKEND, 1), std::out_of_range);
}

TEST_F(BufferAccessControlTest, AddBufferAcceptsAChildWhoseTokenMatchesTheRoot)
{
    auto child = std::make_shared<AudioBuffer>();
    child->set_default_processor(std::make_shared<TokenedProbe>(Buffers::AUDIO_BACKEND));

    access->add_buffer(child, Buffers::AUDIO_BACKEND, 0);

    EXPECT_EQ(access->get_audio_buffers(Buffers::AUDIO_BACKEND, 0), (std::vector<std::shared_ptr<AudioBuffer>> { child }));
}

TEST_F(BufferAccessControlTest, AddBufferRefusesAChildWhoseTokenTheStrictRootRejects)
{
    auto child = std::make_shared<AudioBuffer>();
    child->set_default_processor(std::make_shared<TokenedProbe>(Buffers::AUDIO_PARALLEL));

    EXPECT_THROW(access->add_buffer(child, Buffers::AUDIO_BACKEND, 0), std::runtime_error);

    EXPECT_TRUE(access->get_audio_buffers(Buffers::AUDIO_BACKEND, 0).empty());
}

TEST_F(BufferAccessControlTest, TerminateZeroesAttachedChildren)
{
    auto child = std::make_shared<AudioBuffer>();
    access->add_buffer(child, Buffers::AUDIO_BACKEND, 0);
    child->get_data()[0] = 0.5;

    access->terminate_active_buffers();

    EXPECT_EQ(child->get_data()[0], 0.0);
}

}
