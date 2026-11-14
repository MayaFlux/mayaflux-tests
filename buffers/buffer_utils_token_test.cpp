#include "gtest/gtest.h"
#include "../support/test_metadata.hpp"

#include "MayaFlux/Buffers/BufferUtils.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {

namespace {

    using Buffers::ProcessingToken;

    struct NamedToken {
        const char* name;
        ProcessingToken token;
    };

    constexpr NamedToken k_tokens[] = {
        { "AUDIO_BACKEND", ProcessingToken::AUDIO_BACKEND },
        { "AUDIO_PARALLEL", ProcessingToken::AUDIO_PARALLEL },
        { "GRAPHICS_BACKEND", ProcessingToken::GRAPHICS_BACKEND },
        { "WINDOW_EVENTS", ProcessingToken::WINDOW_EVENTS },
        { "INPUT_BACKEND", ProcessingToken::INPUT_BACKEND },
        { "CONTROL_DATA", ProcessingToken::CONTROL_DATA },
    };

    constexpr size_t k_count = std::size(k_tokens);

    constexpr bool k_expected[k_count][k_count] = {
        { true, false, false, true, true, false },
        { false, true, true, false, false, false },
        { false, false, true, true, false, false },
        { false, false, true, true, false, false },
        { true, false, false, true, true, false },
        { false, false, true, false, false, true },
    };

}

TEST(TokenCompatibilityTest, EveryNamedPairKeepsItsPinnedResult)
{
    for (size_t preferred = 0; preferred < k_count; ++preferred) {
        for (size_t current = 0; current < k_count; ++current) {
            EXPECT_EQ(
                Buffers::are_tokens_compatible(k_tokens[preferred].token, k_tokens[current].token),
                k_expected[preferred][current])
                << "preferred " << k_tokens[preferred].name << ", current " << k_tokens[current].name;
        }
    }
}

TEST(TokenCompatibilityTest, EveryNamedTokenIsCompatibleWithItself)
{
    for (const auto& named : k_tokens) {
        EXPECT_TRUE(Buffers::are_tokens_compatible(named.token, named.token)) << named.name;
    }
}

TEST(TokenCompatibilityTest, AnAudioParallelChainAcceptsNoCpuAudioProcessor)
{
    EXPECT_FALSE(Buffers::are_tokens_compatible(ProcessingToken::AUDIO_PARALLEL, ProcessingToken::AUDIO_BACKEND));
    EXPECT_FALSE(Buffers::are_tokens_compatible(ProcessingToken::AUDIO_BACKEND, ProcessingToken::AUDIO_PARALLEL));
}

TEST(TokenValidationTest, EveryNamedTokenIsValid)
{
    for (const auto& named : k_tokens) {
        EXPECT_NO_THROW(Buffers::validate_token(named.token)) << named.name;
    }
}

TEST(TokenValidationTest, CpuAndGpuTogetherAreRejected)
{
    const auto token = static_cast<ProcessingToken>(
        static_cast<uint32_t>(ProcessingToken::CPU_PROCESS) | static_cast<uint32_t>(ProcessingToken::GPU_PROCESS));

    EXPECT_THROW(Buffers::validate_token(token), std::invalid_argument);
}

TEST(TokenValidationTest, SequentialAndParallelTogetherAreRejected)
{
    const auto token = static_cast<ProcessingToken>(
        static_cast<uint32_t>(ProcessingToken::SEQUENTIAL) | static_cast<uint32_t>(ProcessingToken::PARALLEL));

    EXPECT_THROW(Buffers::validate_token(token), std::invalid_argument);
}

}
