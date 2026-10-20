#include "gtest/gtest.h"

#include "MayaFlux/Nodes/Conduit/Constant.hpp"
#include "MayaFlux/Nodes/Filters/FIR.hpp"
#include "MayaFlux/Nodes/Generators/Sine.hpp"

namespace MayaFlux::Test {
namespace {

    using Nodes::Node;
    using Nodes::NodeState;

    bool processed(const Node& node)
    {
        return (node.m_state.load() & NodeState::PROCESSED) != 0;
    }

    class NodeModulatorCountTest : public ::testing::TestWithParam<int> {
    protected:
        Nodes::Constant node { 1.0 };
    };

    TEST_P(NodeModulatorCountTest, IncrementRaisesTheCountByTheAmount)
    {
        Nodes::atomic_inc_modulator_count(node.m_modulator_count, GetParam());

        EXPECT_EQ(node.m_modulator_count.load(), static_cast<uint32_t>(GetParam()));
    }

    TEST_P(NodeModulatorCountTest, DecrementLowersTheCountByTheAmount)
    {
        Nodes::atomic_inc_modulator_count(node.m_modulator_count, GetParam() * 2);

        Nodes::atomic_dec_modulator_count(node.m_modulator_count, GetParam());

        EXPECT_EQ(node.m_modulator_count.load(), static_cast<uint32_t>(GetParam()));
    }

    TEST_P(NodeModulatorCountTest, MatchingIncrementAndDecrementReturnToZero)
    {
        Nodes::atomic_inc_modulator_count(node.m_modulator_count, GetParam());
        Nodes::atomic_dec_modulator_count(node.m_modulator_count, GetParam());

        EXPECT_EQ(node.m_modulator_count.load(), 0U);
    }

    INSTANTIATE_TEST_SUITE_P(
        Amounts,
        NodeModulatorCountTest,
        ::testing::Values(1, 2, 5));

    struct TryResetCase {
        const char* name;
        uint32_t modulator_count;
        bool has_channel;
    };

    class NodeTryResetTest : public ::testing::TestWithParam<TryResetCase> {
    protected:
        void SetUp() override
        {
            node = std::make_shared<Nodes::Constant>(1.0);
            node->m_modulator_count.store(GetParam().modulator_count);
            if (GetParam().has_channel) {
                node->register_channel_usage(0);
            }
            Nodes::atomic_add_flag(node->m_state, NodeState::PROCESSED);
        }

        std::shared_ptr<Nodes::Constant> node;
    };

    TEST_P(NodeTryResetTest, ProcessedStateClearsOnlyWhenNoModulatorUsesTheNodeAndNoChannelOwnsIt)
    {
        Nodes::try_reset_processed_state(node);

        const bool expect_cleared = GetParam().modulator_count == 0 && !GetParam().has_channel;
        EXPECT_EQ(processed(*node), !expect_cleared);
    }

    INSTANTIATE_TEST_SUITE_P(
        States,
        NodeTryResetTest,
        ::testing::Values(
            TryResetCase { "Free", 0, false },
            TryResetCase { "OwnedByAChannel", 0, true },
            TryResetCase { "UsedByOneModulator", 1, false },
            TryResetCase { "UsedByThree", 3, false }),
        [](const ::testing::TestParamInfo<TryResetCase>& info) { return std::string(info.param.name); });

    using ConsumerFactory = std::function<std::shared_ptr<Node>(const std::shared_ptr<Node>&)>;

    struct ConsumerCase {
        const char* name;
        ConsumerFactory make;
    };

    class NodeConsumerResetTest : public ::testing::TestWithParam<ConsumerCase> {
    protected:
        std::shared_ptr<Nodes::Generator::Sine> modulator = std::make_shared<Nodes::Generator::Sine>(440.0F, 0.5);
    };

    TEST_P(NodeConsumerResetTest, ProcessingAConsumerClearsTheModulatorsProcessedFlag)
    {
        const auto consumer = GetParam().make(modulator);
        Nodes::atomic_add_flag(modulator->m_state, NodeState::PROCESSED);

        consumer->process_sample(0.0);

        EXPECT_FALSE(processed(*modulator));
    }

    TEST_P(NodeConsumerResetTest, ProcessingKeepsTheModulatorCountBalanced)
    {
        const auto consumer = GetParam().make(modulator);

        consumer->process_sample(0.0);
        consumer->process_sample(0.0);

        EXPECT_EQ(modulator->m_modulator_count.load(), 0U);
    }

    TEST_P(NodeConsumerResetTest, EverySharingConsumerClearsTheFlagInTurn)
    {
        std::vector<std::shared_ptr<Node>> consumers;
        for (int i = 0; i < 3; ++i) {
            consumers.push_back(GetParam().make(modulator));
        }

        for (const auto& consumer : consumers) {
            Nodes::atomic_add_flag(modulator->m_state, NodeState::PROCESSED);
            consumer->process_sample(0.0);
            EXPECT_FALSE(processed(*modulator));
        }
    }

    TEST_P(NodeConsumerResetTest, ChannelOwnedModulatorKeepsItsProcessedFlag)
    {
        const auto consumer = GetParam().make(modulator);
        modulator->register_channel_usage(0);
        Nodes::atomic_add_flag(modulator->m_state, NodeState::PROCESSED);

        consumer->process_sample(0.0);

        EXPECT_TRUE(processed(*modulator));
    }

    INSTANTIATE_TEST_SUITE_P(
        Consumers,
        NodeConsumerResetTest,
        ::testing::Values(
            ConsumerCase { "Filter", [](const std::shared_ptr<Node>& input) -> std::shared_ptr<Node> {
                              return std::make_shared<Nodes::Filters::FIR>(input, std::vector<double> { 0.5, 0.5 });
                          } },
            ConsumerCase { "FrequencyModulated", [](const std::shared_ptr<Node>& input) -> std::shared_ptr<Node> {
                              return std::make_shared<Nodes::Generator::Sine>(input, 880.0F, 0.5);
                          } }),
        [](const ::testing::TestParamInfo<ConsumerCase>& info) { return std::string(info.param.name); });

    class NodeOwnershipResetTest : public ::testing::TestWithParam<uint32_t> {
    protected:
        Nodes::Constant node { 1.0 };
    };

    TEST_P(NodeOwnershipResetTest, ChannelOwnershipClearsTheFlagWhateverTheModulatorCount)
    {
        node.register_channel_usage(0);
        node.m_modulator_count.store(GetParam());
        Nodes::atomic_add_flag(node.m_state, NodeState::PROCESSED);

        node.request_reset_from_channel(0);

        EXPECT_FALSE(processed(node));
    }

    INSTANTIATE_TEST_SUITE_P(
        Counts,
        NodeOwnershipResetTest,
        ::testing::Values(0U, 1U, 4U));

    struct FlagCase {
        const char* name;
        NodeState flag;
    };

    class NodeFlagHelperTest : public ::testing::TestWithParam<FlagCase> {
    protected:
        [[nodiscard]] uint32_t bits() const { return node.m_state.load(); }

        [[nodiscard]] NodeState other() const
        {
            return GetParam().flag == NodeState::ACTIVE ? NodeState::PROCESSED : NodeState::ACTIVE;
        }

        Nodes::Constant node { 1.0 };
    };

    TEST_P(NodeFlagHelperTest, AddSetsOnlyThatBit)
    {
        Nodes::atomic_add_flag(node.m_state, GetParam().flag);

        EXPECT_EQ(bits(), static_cast<uint32_t>(GetParam().flag));
    }

    TEST_P(NodeFlagHelperTest, AddIsIdempotent)
    {
        Nodes::atomic_add_flag(node.m_state, GetParam().flag);
        Nodes::atomic_add_flag(node.m_state, GetParam().flag);

        EXPECT_EQ(bits(), static_cast<uint32_t>(GetParam().flag));
    }

    TEST_P(NodeFlagHelperTest, AddKeepsTheOtherBits)
    {
        Nodes::atomic_add_flag(node.m_state, other());
        Nodes::atomic_add_flag(node.m_state, GetParam().flag);

        EXPECT_EQ(bits(), static_cast<uint32_t>(other()) | static_cast<uint32_t>(GetParam().flag));
    }

    TEST_P(NodeFlagHelperTest, RemoveClearsOnlyThatBit)
    {
        Nodes::atomic_add_flag(node.m_state, other());
        Nodes::atomic_add_flag(node.m_state, GetParam().flag);

        Nodes::atomic_remove_flag(node.m_state, GetParam().flag);

        EXPECT_EQ(bits(), static_cast<uint32_t>(other()));
    }

    TEST_P(NodeFlagHelperTest, RemovingAnAbsentBitChangesNothing)
    {
        Nodes::atomic_add_flag(node.m_state, other());

        Nodes::atomic_remove_flag(node.m_state, GetParam().flag);

        EXPECT_EQ(bits(), static_cast<uint32_t>(other()));
    }

    TEST_P(NodeFlagHelperTest, SetStrongReplacesTheWholeState)
    {
        Nodes::atomic_add_flag(node.m_state, other());

        Nodes::atomic_set_flag_strong(node.m_state, GetParam().flag);

        EXPECT_EQ(bits(), static_cast<uint32_t>(GetParam().flag));
    }

    INSTANTIATE_TEST_SUITE_P(
        Flags,
        NodeFlagHelperTest,
        ::testing::Values(
            FlagCase { "Active", NodeState::ACTIVE },
            FlagCase { "PendingRemoval", NodeState::PENDING_REMOVAL },
            FlagCase { "OutputExcluded", NodeState::OUTPUT_EXCLUDED },
            FlagCase { "Processed", NodeState::PROCESSED }),
        [](const ::testing::TestParamInfo<FlagCase>& info) { return std::string(info.param.name); });

    class NodeOutputExclusionTest : public ::testing::Test {
    protected:
        [[nodiscard]] bool has(NodeState flag) const { return (node.m_state.load() & flag) != 0; }

        Nodes::Constant node { 1.0 };
    };

    TEST_F(NodeOutputExclusionTest, ExclusionKeepsTheOtherStateBits)
    {
        Nodes::atomic_add_flag(node.m_state, NodeState::ACTIVE);
        Nodes::atomic_add_flag(node.m_state, NodeState::PROCESSED);

        node.exclude_output_from_graph(true);

        EXPECT_TRUE(has(NodeState::ACTIVE));
        EXPECT_TRUE(has(NodeState::PROCESSED));

        node.exclude_output_from_graph(false);

        EXPECT_TRUE(has(NodeState::ACTIVE));
        EXPECT_TRUE(has(NodeState::PROCESSED));
    }

    TEST_F(NodeOutputExclusionTest, ExcludingTwiceIsTheSameAsOnce)
    {
        node.exclude_output_from_graph(true);
        node.exclude_output_from_graph(true);
        node.exclude_output_from_graph(false);

        EXPECT_FALSE(node.is_output_excluded_from_graph());
    }

    TEST_F(NodeOutputExclusionTest, ResettingTheProcessedStateKeepsTheExclusion)
    {
        node.exclude_output_from_graph(true);
        Nodes::atomic_add_flag(node.m_state, NodeState::PROCESSED);

        node.reset_processed_state();

        EXPECT_FALSE(has(NodeState::PROCESSED));
        EXPECT_TRUE(node.is_output_excluded_from_graph());
    }

    struct ChannelSetCase {
        const char* name;
        std::vector<uint32_t> channels;
    };

    class NodeChannelResetTest : public ::testing::TestWithParam<ChannelSetCase> {
    protected:
        void SetUp() override
        {
            for (const uint32_t channel : GetParam().channels) {
                node.register_channel_usage(channel);
            }
            Nodes::atomic_add_flag(node.m_state, NodeState::PROCESSED);
        }

        Nodes::Constant node { 1.0 };
    };

    TEST_P(NodeChannelResetTest, ResetWaitsForEveryActiveChannel)
    {
        const auto& channels = GetParam().channels;

        for (size_t i = 0; i + 1 < channels.size(); ++i) {
            node.request_reset_from_channel(channels[i]);
            EXPECT_TRUE(processed(node)) << "after channel " << channels[i];
        }

        node.request_reset_from_channel(channels.back());

        EXPECT_FALSE(processed(node));
    }

    TEST_P(NodeChannelResetTest, RequestOrderDoesNotMatter)
    {
        const auto& channels = GetParam().channels;

        for (size_t i = channels.size(); i-- > 1;) {
            node.request_reset_from_channel(channels[i]);
            EXPECT_TRUE(processed(node));
        }

        node.request_reset_from_channel(channels.front());

        EXPECT_FALSE(processed(node));
    }

    TEST_P(NodeChannelResetTest, PendingRequestsClearAfterAReset)
    {
        const auto& channels = GetParam().channels;
        for (const uint32_t channel : channels) {
            node.request_reset_from_channel(channel);
        }
        ASSERT_FALSE(processed(node));

        Nodes::atomic_add_flag(node.m_state, NodeState::PROCESSED);
        node.request_reset_from_channel(channels.front());

        EXPECT_EQ(processed(node), channels.size() > 1);
    }

    TEST_P(NodeChannelResetTest, RepeatedRequestsFromOneChannelDoNotCompleteTheSet)
    {
        const auto& channels = GetParam().channels;
        if (channels.size() < 2) {
            GTEST_SKIP() << "needs two channels";
        }

        for (int i = 0; i < 3; ++i) {
            node.request_reset_from_channel(channels.front());
        }

        EXPECT_TRUE(processed(node));
    }

    TEST_P(NodeChannelResetTest, UnregisteringAChannelDropsItsPendingRequest)
    {
        const auto& channels = GetParam().channels;
        if (channels.size() < 2) {
            GTEST_SKIP() << "needs two channels";
        }

        node.request_reset_from_channel(channels.front());
        node.unregister_channel_usage(channels.front());

        for (size_t i = 1; i + 1 < channels.size(); ++i) {
            node.request_reset_from_channel(channels[i]);
            EXPECT_TRUE(processed(node));
        }
        node.request_reset_from_channel(channels.back());

        EXPECT_FALSE(processed(node));
    }

    INSTANTIATE_TEST_SUITE_P(
        Sets,
        NodeChannelResetTest,
        ::testing::Values(
            ChannelSetCase { "One", { 0 } },
            ChannelSetCase { "Two", { 0, 1 } },
            ChannelSetCase { "Three", { 0, 1, 2 } },
            ChannelSetCase { "Spread", { 3, 9, 31 } }),
        [](const ::testing::TestParamInfo<ChannelSetCase>& info) { return std::string(info.param.name); });

    class NodeChannelResetEdgeTest : public ::testing::Test {
    protected:
        Nodes::Constant node { 1.0 };
    };

    TEST_F(NodeChannelResetEdgeTest, NoActiveChannelNeverResetsOnRequest)
    {
        Nodes::atomic_add_flag(node.m_state, NodeState::PROCESSED);

        node.request_reset_from_channel(0);

        EXPECT_TRUE(processed(node));
    }

    TEST_F(NodeChannelResetEdgeTest, RequestFromAChannelThatDoesNotUseTheNodeDoesNotReset)
    {
        node.register_channel_usage(0);
        Nodes::atomic_add_flag(node.m_state, NodeState::PROCESSED);

        node.request_reset_from_channel(5);
        EXPECT_TRUE(processed(node));

        node.request_reset_from_channel(0);
        EXPECT_FALSE(processed(node));
    }

    TEST_F(NodeChannelResetEdgeTest, OutOfRangeRequestIsIgnored)
    {
        node.register_channel_usage(0);
        Nodes::atomic_add_flag(node.m_state, NodeState::PROCESSED);

        node.request_reset_from_channel(32);

        EXPECT_TRUE(processed(node));
    }

    class NodeBufferMarkTest : public ::testing::TestWithParam<uint32_t> {
    protected:
        void SetUp() override
        {
            for (uint32_t i = 0; i < GetParam(); ++i) {
                node.add_buffer_reference();
            }
        }

        Nodes::Constant node { 1.0 };
    };

    TEST_P(NodeBufferMarkTest, NodeIsClaimableAgainAfterAFullCycle)
    {
        ASSERT_TRUE(node.mark_buffer_processed());
        for (uint32_t i = 0; i + 1 < GetParam(); ++i) {
            node.request_buffer_reset();
        }
        ASSERT_FALSE(node.is_buffer_processed());

        EXPECT_TRUE(node.mark_buffer_processed());
        EXPECT_TRUE(node.is_buffer_processed());
    }

    INSTANTIATE_TEST_SUITE_P(
        Buffers,
        NodeBufferMarkTest,
        ::testing::Values(2U, 3U, 4U, 5U));

    class NodeBufferEdgeTest : public ::testing::Test {
    protected:
        Nodes::Constant node { 1.0 };
    };

    TEST_F(NodeBufferEdgeTest, SingleBufferAlwaysWinsWithoutSettingTheFlag)
    {
        node.add_buffer_reference();

        for (int i = 0; i < 3; ++i) {
            EXPECT_TRUE(node.mark_buffer_processed());
            EXPECT_FALSE(node.is_buffer_processed());
        }
    }

    TEST_F(NodeBufferEdgeTest, ResetOnAnUnmarkedNodeLeavesItUnmarked)
    {
        node.add_buffer_reference();
        node.add_buffer_reference();

        node.request_buffer_reset();

        EXPECT_FALSE(node.is_buffer_processed());
        EXPECT_TRUE(node.mark_buffer_processed());
    }

    struct BufferStateCase {
        const char* name;
        NodeState state;
    };

    class NodeBufferStateTest : public ::testing::TestWithParam<BufferStateCase> {
    protected:
        void SetUp() override
        {
            node.add_buffer_reference();
            node.add_buffer_reference();
            Nodes::atomic_set_flag_strong(node.m_state, GetParam().state);
        }

        Nodes::Constant node { 1.0 };
    };

    TEST_P(NodeBufferStateTest, OnlyABareInactiveNodeCanBeMarked)
    {
        EXPECT_FALSE(node.mark_buffer_processed());
        EXPECT_FALSE(node.is_buffer_processed());
    }

    INSTANTIATE_TEST_SUITE_P(
        States,
        NodeBufferStateTest,
        ::testing::Values(
            BufferStateCase { "Active", NodeState::ACTIVE },
            BufferStateCase { "ActiveProcessed", NodeState::ENGINE_PROCESSED },
            BufferStateCase { "Processed", NodeState::PROCESSED },
            BufferStateCase { "OutputExcluded", NodeState::OUTPUT_EXCLUDED },
            BufferStateCase { "PendingRemoval", NodeState::PENDING_REMOVAL },
            BufferStateCase { "ActiveOutputExcluded", NodeState::ENGINE_OUTPUT_EXCLUDED_PROCESSED }),
        [](const ::testing::TestParamInfo<BufferStateCase>& info) { return std::string(info.param.name); });

}
}
