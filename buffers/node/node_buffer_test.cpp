#include "gtest/gtest.h"

#include "MayaFlux/Buffers/AudioBuffer.hpp"
#include "MayaFlux/Buffers/Node/NodeBuffer.hpp"
#include "MayaFlux/Nodes/Conduit/Constant.hpp"
#include "MayaFlux/Nodes/Generators/Phasor.hpp"

namespace MayaFlux::Test {

using Buffers::AudioBuffer;
using Buffers::NodeBuffer;
using Buffers::NodeSourceProcessor;
using Buffers::ProcessingToken;
using Nodes::Constant;
using Nodes::NodeState;
using Nodes::Generator::Phasor;

namespace {

    constexpr uint32_t k_block = 32;
    constexpr float k_mix = 0.5F;

    std::vector<double> phasor_reference(size_t count, float mix)
    {
        Phasor reference(1.0F, 1.0);
        std::vector<double> out(count);
        for (double& sample : out) {
            sample = reference.process_sample(0.0) * mix;
        }
        return out;
    }

    class NodeSourceProcessorTest : public ::testing::Test {
    protected:
        std::shared_ptr<AudioBuffer> buffer { std::make_shared<AudioBuffer>(0, k_block) };
        std::shared_ptr<Constant> constant { std::make_shared<Constant>(0.5) };
        std::shared_ptr<Phasor> phasor { std::make_shared<Phasor>(1.0F, 1.0) };

        void fill(double value)
        {
            buffer->get_data().assign(k_block, value);
        }
    };

    class NodeBufferTest : public ::testing::Test {
    protected:
        std::shared_ptr<Constant> constant { std::make_shared<Constant>(0.5) };
        std::shared_ptr<NodeBuffer> node_buffer { std::make_shared<NodeBuffer>(0, k_block, constant) };

        void SetUp() override
        {
            node_buffer->setup_processors(ProcessingToken::AUDIO_BACKEND);
        }
    };

}

TEST_F(NodeSourceProcessorTest, WritesNodeOutputScaledByMix)
{
    fill(0.0);
    NodeSourceProcessor processor(constant, k_mix);

    processor.process(buffer);

    for (double sample : buffer->get_data()) {
        EXPECT_DOUBLE_EQ(sample, 0.5 * k_mix);
    }
}

TEST_F(NodeSourceProcessorTest, ClearsExistingDataWhenAskedTo)
{
    fill(1.0);
    NodeSourceProcessor processor(constant, k_mix, true);

    processor.process(buffer);

    for (double sample : buffer->get_data()) {
        EXPECT_DOUBLE_EQ(sample, 0.5 * k_mix);
    }
}

TEST_F(NodeSourceProcessorTest, AddsToExistingDataWhenNotClearing)
{
    fill(1.0);
    NodeSourceProcessor processor(constant, k_mix, false);

    processor.process(buffer);

    for (double sample : buffer->get_data()) {
        EXPECT_DOUBLE_EQ(sample, 1.0 + 0.5 * k_mix);
    }
}

TEST_F(NodeSourceProcessorTest, ZeroMixPreservesExistingData)
{
    fill(1.0);
    NodeSourceProcessor processor(constant, 0.0F, false);

    processor.process(buffer);

    for (double sample : buffer->get_data()) {
        EXPECT_DOUBLE_EQ(sample, 1.0);
    }
}

TEST_F(NodeSourceProcessorTest, MixRoundTrips)
{
    NodeSourceProcessor processor(constant, k_mix);
    EXPECT_FLOAT_EQ(processor.get_mix(), k_mix);

    processor.set_mix(0.25F);
    EXPECT_FLOAT_EQ(processor.get_mix(), 0.25F);
}

TEST_F(NodeSourceProcessorTest, NullNodeLeavesTheBufferUntouched)
{
    fill(1.0);
    NodeSourceProcessor processor(nullptr, k_mix, false);

    processor.process(buffer);

    for (double sample : buffer->get_data()) {
        EXPECT_DOUBLE_EQ(sample, 1.0);
    }
}

TEST_F(NodeSourceProcessorTest, InactiveNodeContinuesItsSequenceAcrossBlocks)
{
    fill(0.0);
    NodeSourceProcessor processor(phasor, 1.0F);
    const auto expected = phasor_reference(static_cast<size_t>(k_block) * 2, 1.0F);

    processor.process(buffer);
    const std::vector<double> first = buffer->get_data();
    processor.process(buffer);
    const std::vector<double> second = buffer->get_data();

    for (uint32_t i = 0; i < k_block; ++i) {
        EXPECT_DOUBLE_EQ(first[i], expected[i]);
        EXPECT_DOUBLE_EQ(second[i], expected[k_block + i]);
    }
}

TEST_F(NodeSourceProcessorTest, ActiveNodeIsSampledWithoutAdvancing)
{
    phasor->m_state.store(NodeState::ACTIVE);
    fill(0.0);
    NodeSourceProcessor processor(phasor, 1.0F);
    const auto expected = phasor_reference(k_block, 1.0F);

    processor.process(buffer);
    const std::vector<double> first = buffer->get_data();
    processor.process(buffer);
    const std::vector<double> second = buffer->get_data();

    EXPECT_DOUBLE_EQ(phasor->get_phase(), 0.0);
    for (uint32_t i = 0; i < k_block; ++i) {
        EXPECT_DOUBLE_EQ(first[i], expected[i]);
        EXPECT_DOUBLE_EQ(second[i], expected[i]);
    }
}

TEST_F(NodeSourceProcessorTest, AttachTakesABufferReferenceAndDetachReleasesIt)
{
    NodeSourceProcessor processor(constant);
    EXPECT_FALSE(constant->mark_buffer_processed());

    processor.on_attach(buffer);
    EXPECT_TRUE(constant->mark_buffer_processed());

    processor.on_detach(buffer);
    EXPECT_FALSE(constant->mark_buffer_processed());
}

TEST_F(NodeBufferTest, DefaultProcessorIsANodeSourceProcessor)
{
    EXPECT_NE(std::dynamic_pointer_cast<NodeSourceProcessor>(node_buffer->get_default_processor()), nullptr);
}

TEST_F(NodeBufferTest, SetupTakesAReferenceOnTheSourceNode)
{
    EXPECT_TRUE(constant->mark_buffer_processed());
}

TEST_F(NodeBufferTest, ProcessDefaultCapturesTheSourceNode)
{
    node_buffer->process_default();

    ASSERT_EQ(node_buffer->get_data().size(), node_buffer->get_num_samples());
    for (double sample : node_buffer->get_data()) {
        EXPECT_DOUBLE_EQ(sample, 0.5 * k_mix);
    }
}

TEST_F(NodeBufferTest, ClearFlagRoundTrips)
{
    EXPECT_TRUE(node_buffer->get_clear_before_process());

    node_buffer->set_clear_before_process(false);
    EXPECT_FALSE(node_buffer->get_clear_before_process());
}

TEST_F(NodeBufferTest, ProcessingDoesNotAccumulateWhileClearing)
{
    node_buffer->process_default();
    node_buffer->process_default();

    for (double sample : node_buffer->get_data()) {
        EXPECT_DOUBLE_EQ(sample, 0.5 * k_mix);
    }
}

TEST_F(NodeBufferTest, ProcessingAccumulatesWhenNotClearing)
{
    node_buffer->set_clear_before_process(false);

    node_buffer->process_default();
    node_buffer->process_default();

    for (double sample : node_buffer->get_data()) {
        EXPECT_DOUBLE_EQ(sample, 2.0 * 0.5 * k_mix);
    }
}

}
