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

}
}
