#include "gtest/gtest.h"
#include "../../support/test_metadata.hpp"

#include "MayaFlux/Buffers/AudioBuffer.hpp"
#include "MayaFlux/Buffers/BufferProcessor.hpp"
#include "MayaFlux/Buffers/Root/RootAudioBuffer.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {

using Buffers::AudioBuffer;
using Buffers::BufferProcessor;
using Buffers::BufferRoutingState;
using Buffers::ChannelProcessor;
using Buffers::FinalLimiterProcessor;
using Buffers::RootAudioBuffer;

namespace {

    class TokenedProbe final : public BufferProcessor {
    public:
        explicit TokenedProbe(Buffers::ProcessingToken token)
        {
            set_processing_token(token);
        }

        void processing_function(const std::shared_ptr<Buffers::Buffer>&) override { }
    };

    class RootAudioBufferFixture : public ::testing::Test {
    protected:
        void SetUp() override
        {
            root = std::make_shared<RootAudioBuffer>(0);
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

        std::shared_ptr<RootAudioBuffer> root;
        uint32_t size { 0 };
    };

}

using RootAudioBufferTest = RootAudioBufferFixture;

TEST_F(RootAudioBufferTest, NothingIsInstalledUntilInitialize)
{
    const auto fresh = std::make_shared<RootAudioBuffer>(0);

    EXPECT_EQ(fresh->get_default_processor(), nullptr);
    EXPECT_NE(std::dynamic_pointer_cast<ChannelProcessor>(root->get_default_processor()), nullptr);
}

TEST_F(RootAudioBufferTest, AChildOfAnotherAudioTokenIsRefused)
{
    auto parallel = std::make_shared<AudioBuffer>();
    parallel->set_default_processor(std::make_shared<TokenedProbe>(Buffers::AUDIO_PARALLEL));
    auto native = std::make_shared<AudioBuffer>();
    native->set_default_processor(std::make_shared<TokenedProbe>(Buffers::AUDIO_BACKEND));

    EXPECT_THROW(root->add_child_buffer(parallel), std::runtime_error);
    EXPECT_NO_THROW(root->add_child_buffer(native));
    EXPECT_EQ(root->get_num_children(), 1U);
}

TEST_F(RootAudioBufferTest, ResizeReachesTheDataTheNodeOutputAndTheChildren)
{
    const auto child = make_child(0.5);
    root->set_node_output(std::vector<double>(size, 0.2));

    root->resize(128);

    EXPECT_EQ(root->get_data().size(), 128U);
    EXPECT_EQ(root->get_node_output().size(), 128U);
    EXPECT_EQ(child->get_num_samples(), 128U);
}

TEST_F(RootAudioBufferTest, SetNodeOutputCopiesTheDataAndFlagsIt)
{
    const std::vector<double> node { 0.1, 0.2, 0.3 };
    EXPECT_FALSE(root->has_node_output());

    root->set_node_output(node);

    EXPECT_TRUE(root->has_node_output());
    EXPECT_EQ(root->get_node_output(), node);
}

TEST_F(RootAudioBufferTest, ChildrenAreSummedIntoTheOutputWithoutNormalization)
{
    make_child(0.5);
    make_child(0.25);

    root->process_default();

    EXPECT_DOUBLE_EQ(root->get_data().front(), 0.75);
    EXPECT_DOUBLE_EQ(root->get_data().back(), 0.75);
}

TEST_F(RootAudioBufferTest, NormalizationDividesTheSumByTheActiveChildCount)
{
    root->set_mix_normalization(true);
    make_child(0.5);
    make_child(0.25);

    root->process_default();

    EXPECT_DOUBLE_EQ(root->get_data().front(), 0.375);
}

TEST_F(RootAudioBufferTest, AChildWithoutDataIsLeftOutOfTheSumAndTheCount)
{
    root->set_mix_normalization(true);
    make_child(0.5);
    const auto silent = make_child(0.25);
    silent->mark_for_processing(false);

    root->process_default();

    EXPECT_DOUBLE_EQ(root->get_data().front(), 0.5);
}

TEST_F(RootAudioBufferTest, AnInternalOnlyChildIsNotSummed)
{
    const auto internal = make_child(0.5);
    internal->force_internal_usage(true);

    root->process_default();

    EXPECT_EQ(root->get_data(), std::vector<double>(size, 0.0));
}

TEST_F(RootAudioBufferTest, AChildMarkedForRemovalIsNotSummedAndLeavesAfterTwoCycles)
{
    const auto doomed = make_child(0.5);
    doomed->mark_for_removal();

    root->process_default();
    EXPECT_EQ(root->get_data(), std::vector<double>(size, 0.0));

    root->process_default();
    EXPECT_EQ(root->get_num_children(), 0U);
}

TEST_F(RootAudioBufferTest, ARoutedChildLeavingThisChannelIsScaledByItsFromAmount)
{
    const auto child = make_child(1.0);
    auto& state = child->get_routing_state();
    state.phase = BufferRoutingState::ACTIVE;
    state.from_channel = root->get_channel_id();
    state.to_channel = 1;
    state.from_amount = 0.5;
    state.fade_cycles = 4;

    root->process_default();

    EXPECT_DOUBLE_EQ(root->get_data().front(), 0.5);
}

TEST_F(RootAudioBufferTest, ARoutedChildFromAnotherChannelIsNotScaled)
{
    const auto child = make_child(1.0);
    auto& state = child->get_routing_state();
    state.phase = BufferRoutingState::ACTIVE;
    state.from_channel = root->get_channel_id() + 3;
    state.to_channel = 1;
    state.from_amount = 0.5;
    state.fade_cycles = 4;

    root->process_default();

    EXPECT_DOUBLE_EQ(root->get_data().front(), 1.0);
}

TEST_F(RootAudioBufferTest, TheNodeOutputIsAddedBeforeTheChildren)
{
    make_child(0.5);
    root->set_node_output(std::vector<double>(size, 0.1));

    root->process_default();

    EXPECT_DOUBLE_EQ(root->get_data().front(), 0.6);
}

TEST_F(RootAudioBufferTest, ANodeOutputLongerThanTheRootIsCutToIt)
{
    root->set_node_output(std::vector<double>(size + 512, 1.0));

    EXPECT_NO_THROW(root->process_default());

    EXPECT_EQ(root->get_data().size(), size);
    EXPECT_EQ(root->get_data().back(), 1.0);
}

TEST_F(RootAudioBufferTest, AnEmptyCycleClearsTheStaleOutput)
{
    std::ranges::fill(root->get_data(), 0.7);

    root->process_default();

    EXPECT_EQ(root->get_data(), std::vector<double>(size, 0.0));
}

TEST_F(RootAudioBufferTest, DisabledDefaultProcessingLeavesTheOutputAlone)
{
    make_child(0.5);
    std::ranges::fill(root->get_data(), 0.7);
    root->enforce_default_processing(false);

    root->process_default();

    EXPECT_EQ(root->get_data(), std::vector<double>(size, 0.7));
}

TEST_F(RootAudioBufferTest, AChannelProcessorLeavesAForeignRootAlone)
{
    const auto other = std::make_shared<RootAudioBuffer>(1);
    other->resize(size);
    std::ranges::fill(other->get_data(), 0.7);
    make_child(0.5);
    ChannelProcessor processor(root);

    processor.processing_function(other);

    EXPECT_EQ(other->get_data(), std::vector<double>(size, 0.7));
}

TEST_F(RootAudioBufferTest, AChannelProcessorAttachesOnlyToARootAudioBuffer)
{
    ChannelProcessor processor(root);
    const auto plain = std::make_shared<AudioBuffer>();

    EXPECT_NO_THROW(processor.on_attach(root));
    EXPECT_THROW(processor.on_attach(plain), std::runtime_error);
    EXPECT_TRUE(processor.is_compatible_with(root));
    EXPECT_FALSE(processor.is_compatible_with(plain));
}

TEST(FinalLimiterTest, SamplesWithinTheThresholdPassUnchanged)
{
    FinalLimiterProcessor limiter;
    const auto buffer = std::make_shared<AudioBuffer>();
    buffer->get_data() = { 0.0, 0.5, -0.9, 0.95 };

    limiter.processing_function(buffer);

    EXPECT_EQ(buffer->get_data(), (std::vector<double> { 0.0, 0.5, -0.9, 0.95 }));
}

TEST(FinalLimiterTest, SamplesAboveTheThresholdAreCompressedAndKeepTheirSign)
{
    FinalLimiterProcessor limiter;
    const auto buffer = std::make_shared<AudioBuffer>();
    buffer->get_data() = { 1.5, -1.5 };

    limiter.processing_function(buffer);

    EXPECT_GT(buffer->get_data()[0], 0.95);
    EXPECT_LT(buffer->get_data()[1], -0.95);
}

TEST(FinalLimiterTest, LargerInputsGiveLargerOutputsUntilTheCeiling)
{
    FinalLimiterProcessor limiter;
    const auto buffer = std::make_shared<AudioBuffer>();
    buffer->get_data() = { 1.0, 1.2, 4.0 };

    limiter.processing_function(buffer);

    EXPECT_LT(buffer->get_data()[0], buffer->get_data()[1]);
    EXPECT_LE(buffer->get_data()[1], buffer->get_data()[2]);
}

TEST(FinalLimiterTest, OutputsStayWithinFullScale)
{
    FinalLimiterProcessor limiter;
    const auto buffer = std::make_shared<AudioBuffer>();
    buffer->get_data() = { 10.0, -10.0, 1.2 };

    limiter.processing_function(buffer);

    for (const double sample : buffer->get_data()) {
        EXPECT_LE(std::abs(sample), 1.0);
    }
}

TEST(FinalLimiterTest, AcceptsAudioBuffersOnly)
{
    FinalLimiterProcessor limiter;
    const auto buffer = std::make_shared<AudioBuffer>();

    EXPECT_TRUE(limiter.is_compatible_with(buffer));
    EXPECT_NO_THROW(limiter.on_attach(buffer));
}

}
