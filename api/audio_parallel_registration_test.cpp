#include "gtest/gtest.h"

#include "MayaFlux/Buffers/BufferManager.hpp"

#include "MayaFlux/MayaFlux.hpp"

namespace MayaFlux::Test {
namespace {

    constexpr unsigned int SAMPLE_RATE = 48000;
    constexpr unsigned int BUFFER_SIZE = 512;
    constexpr unsigned int NUM_CHANNELS = 2;

    class ApiAudioParallelRegistrationTest : public ::testing::Test {
    protected:
        void SetUp() override
        {
            MayaFlux::End();
            MayaFlux::Init(SAMPLE_RATE, BUFFER_SIZE, NUM_CHANNELS, 0);
        }

        void TearDown() override { MayaFlux::End(); }

        [[nodiscard]] static bool registered_under(
            const std::shared_ptr<Buffers::AudioBuffer>& buffer,
            Buffers::ProcessingToken token,
            uint32_t channel)
        {
            const auto buffers = MayaFlux::get_buffer_manager()->get_buffers(token, channel);
            return std::ranges::find(buffers, buffer) != buffers.end();
        }
    };

}

TEST_F(ApiAudioParallelRegistrationTest, RegisterAudioBufferHonorsTheRequestedToken)
{
    const auto buffer = std::make_shared<Buffers::AudioBuffer>();

    MayaFlux::register_audio_buffer(buffer, 0, Buffers::ProcessingToken::AUDIO_PARALLEL);

    EXPECT_TRUE(registered_under(buffer, Buffers::ProcessingToken::AUDIO_PARALLEL, 0));
    EXPECT_FALSE(registered_under(buffer, Buffers::ProcessingToken::AUDIO_BACKEND, 0));
}

TEST_F(ApiAudioParallelRegistrationTest, RegisterAudioBufferWithoutATokenStillUsesTheBackendToken)
{
    const auto buffer = std::make_shared<Buffers::AudioBuffer>();

    MayaFlux::register_audio_buffer(buffer, 1);

    EXPECT_TRUE(registered_under(buffer, Buffers::ProcessingToken::AUDIO_BACKEND, 1));
}

TEST_F(ApiAudioParallelRegistrationTest, UnregisterAudioBufferHonorsTheRequestedToken)
{
    const auto buffer = std::make_shared<Buffers::AudioBuffer>();
    MayaFlux::register_audio_buffer(buffer, 0, Buffers::ProcessingToken::AUDIO_PARALLEL);
    ASSERT_TRUE(registered_under(buffer, Buffers::ProcessingToken::AUDIO_PARALLEL, 0));

    MayaFlux::unregister_audio_buffer(buffer, 0, Buffers::ProcessingToken::AUDIO_PARALLEL);

    EXPECT_FALSE(registered_under(buffer, Buffers::ProcessingToken::AUDIO_PARALLEL, 0));
}

TEST_F(ApiAudioParallelRegistrationTest, TheAudioParallelPipeRegistersInTheParallelUnit)
{
    const auto buffer = vega.AudioBuffer() | AudioParallel[0];

    EXPECT_TRUE(registered_under(buffer, Buffers::ProcessingToken::AUDIO_PARALLEL, 0));
    EXPECT_FALSE(registered_under(buffer, Buffers::ProcessingToken::AUDIO_BACKEND, 0));
}

TEST_F(ApiAudioParallelRegistrationTest, TheAudioPipeStillRegistersInTheBackendUnit)
{
    const auto buffer = vega.AudioBuffer() | Audio[0];

    EXPECT_TRUE(registered_under(buffer, Buffers::ProcessingToken::AUDIO_BACKEND, 0));
}

}
