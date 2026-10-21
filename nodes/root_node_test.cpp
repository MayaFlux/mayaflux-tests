#include "gtest/gtest.h"

#include "MayaFlux/Nodes/Conduit/Constant.hpp"
#include "MayaFlux/Nodes/RootNode.hpp"

namespace MayaFlux::Test {
namespace {

    using Nodes::Constant;
    using Nodes::Node;
    using Nodes::NodeState;
    using Nodes::RootNode;

    uint32_t flags(const Node& node)
    {
        return node.m_state.load();
    }

    bool has(const Node& node, NodeState state)
    {
        return (flags(node) & state) != 0;
    }

    class RootNodeQueueTest : public ::testing::Test {
    protected:
        RootNode root;
        std::shared_ptr<Constant> node { std::make_shared<Constant>(1.0) };
    };

    TEST_F(RootNodeQueueTest, ConstructionRecordsTokenAndChannel)
    {
        RootNode other(Nodes::ProcessingToken::VISUAL_RATE, 3);
        EXPECT_EQ(other.get_token(), Nodes::ProcessingToken::VISUAL_RATE);
        EXPECT_EQ(other.get_channel(), 3U);
        EXPECT_EQ(other.get_node_size(), 0U);
    }

    TEST_F(RootNodeQueueTest, NullNodeIsIgnored)
    {
        root.register_node(nullptr);
        root.unregister_node(nullptr);
        root.process_sample();
        EXPECT_EQ(root.get_node_size(), 0U);
    }

    TEST_F(RootNodeQueueTest, RegistrationIsQueuedUntilTheRootProcesses)
    {
        root.register_node(node);

        EXPECT_EQ(root.get_node_size(), 0U);
        EXPECT_FALSE(has(*node, NodeState::ACTIVE));

        root.process_sample();

        EXPECT_EQ(root.get_node_size(), 1U);
        EXPECT_TRUE(has(*node, NodeState::ACTIVE));
    }

    TEST_F(RootNodeQueueTest, RegisteringTheSameNodeTwiceCommitsOnce)
    {
        root.register_node(node);
        root.register_node(node);
        root.process_sample();

        EXPECT_EQ(root.get_node_size(), 1U);
    }

    TEST_F(RootNodeQueueTest, RegistrationAcrossCyclesDoesNotDuplicate)
    {
        root.register_node(node);
        root.process_sample();
        root.register_node(node);
        root.process_sample();

        EXPECT_EQ(root.get_node_size(), 1U);
    }

    TEST_F(RootNodeQueueTest, RemovalIsQueuedUntilTheRootProcesses)
    {
        root.register_node(node);
        root.process_sample();

        root.unregister_node(node);

        EXPECT_EQ(root.get_node_size(), 1U);
        EXPECT_TRUE(has(*node, NodeState::PENDING_REMOVAL));

        root.process_sample();

        EXPECT_EQ(root.get_node_size(), 0U);
        EXPECT_FALSE(has(*node, NodeState::PENDING_REMOVAL));
        EXPECT_FALSE(has(*node, NodeState::ACTIVE));
        EXPECT_FALSE(has(*node, NodeState::ACTIVE));
    }

    TEST_F(RootNodeQueueTest, RemovingAnUnregisteredNodeLeavesItInactive)
    {
        root.unregister_node(node);
        root.process_sample();

        EXPECT_EQ(root.get_node_size(), 0U);
        EXPECT_FALSE(has(*node, NodeState::PENDING_REMOVAL));
        EXPECT_FALSE(has(*node, NodeState::ACTIVE));
    }

    TEST_F(RootNodeQueueTest, RegisterThenUnregisterInOneCycleLeavesNothing)
    {
        root.register_node(node);
        root.unregister_node(node);
        root.process_sample();

        EXPECT_EQ(root.get_node_size(), 0U);
        EXPECT_FALSE(has(*node, NodeState::ACTIVE));
    }

    TEST_F(RootNodeQueueTest, UnregisterThenRegisterInOneCycleLeavesNodeRegistered)
    {
        root.unregister_node(node);
        root.register_node(node);
        root.process_sample();

        EXPECT_EQ(root.get_node_size(), 1U);
    }

    TEST_F(RootNodeQueueTest, OperationsOnDifferentNodesAreIndependent)
    {
        auto other = std::make_shared<Constant>(2.0);
        root.register_node(node);
        root.register_node(other);
        root.process_sample();
        ASSERT_EQ(root.get_node_size(), 2U);

        root.unregister_node(node);
        root.process_sample();

        EXPECT_EQ(root.get_node_size(), 1U);
        EXPECT_TRUE(has(*other, NodeState::ACTIVE));
        EXPECT_FALSE(has(*node, NodeState::ACTIVE));
    }

    TEST_F(RootNodeQueueTest, QueuedNodeIsHeldAliveUntilCommitted)
    {
        std::weak_ptr<Constant> weak = node;
        root.register_node(node);
        node.reset();

        EXPECT_FALSE(weak.expired());

        root.process_sample();
        EXPECT_FALSE(weak.expired());
        EXPECT_EQ(root.get_node_size(), 1U);
    }

    TEST_F(RootNodeQueueTest, DrainedRemovalReleasesTheNode)
    {
        std::weak_ptr<Constant> weak = node;
        root.register_node(node);
        root.process_sample();
        root.unregister_node(node);
        node.reset();
        root.process_sample();

        EXPECT_EQ(root.get_node_size(), 0U);
        EXPECT_TRUE(weak.expired());
    }

    TEST_F(RootNodeQueueTest, ProcessSampleSumsRegisteredNodes)
    {
        root.register_node(std::make_shared<Constant>(1.0));
        root.register_node(std::make_shared<Constant>(2.0));

        EXPECT_DOUBLE_EQ(root.process_sample(), 3.0);
        EXPECT_DOUBLE_EQ(root.process_sample(), 3.0);
    }

    TEST_F(RootNodeQueueTest, RemovedNodeNoLongerContributes)
    {
        auto other = std::make_shared<Constant>(2.0);
        root.register_node(node);
        root.register_node(other);
        root.process_sample();

        root.unregister_node(node);
        root.process_sample();

        EXPECT_DOUBLE_EQ(root.process_sample(), 2.0);
    }

    TEST_F(RootNodeQueueTest, ProcessBatchProducesOneSamplePerStep)
    {
        root.register_node(node);

        const auto batch = root.process_batch(16);

        ASSERT_EQ(batch.size(), 16U);
        for (const double sample : batch) {
            EXPECT_DOUBLE_EQ(sample, 1.0);
        }
    }

    TEST_F(RootNodeQueueTest, ProcessFrameCommitsPendingOperations)
    {
        root.register_node(node);
        root.process_frame();

        EXPECT_EQ(root.get_node_size(), 1U);
    }

    TEST_F(RootNodeQueueTest, ClearAllNodesEmptiesCommittedNodes)
    {
        root.register_node(node);
        root.process_sample();
        root.clear_all_nodes();

        EXPECT_EQ(root.get_node_size(), 0U);
    }

    TEST_F(RootNodeQueueTest, TerminateAllNodesCommitsRemovalsImmediately)
    {
        root.register_node(node);
        root.register_node(std::make_shared<Constant>(2.0));
        root.process_sample();
        ASSERT_EQ(root.get_node_size(), 2U);

        root.terminate_all_nodes();

        EXPECT_EQ(root.get_node_size(), 0U);
        EXPECT_FALSE(has(*node, NodeState::ACTIVE));
        EXPECT_FALSE(has(*node, NodeState::ACTIVE));
    }

    TEST_F(RootNodeQueueTest, ProcessingStopsAfterTerminate)
    {
        root.register_node(node);
        root.process_sample();
        root.terminate_all_nodes();

        root.register_node(node);

        EXPECT_DOUBLE_EQ(root.process_sample(), 0.0);
        EXPECT_EQ(root.get_node_size(), 0U);
    }

    class RootNodeProcessTest : public ::testing::Test {
    protected:
        std::shared_ptr<Constant> make_counted(double value, int& ticks)
        {
            auto entry = std::make_shared<Constant>(value);
            entry->on_tick([&ticks](Nodes::NodeContext&) { ++ticks; });
            return entry;
        }

        void commit(RootNode& target, const std::shared_ptr<Constant>& entry)
        {
            entry->register_channel_usage(target.get_channel());
            target.register_node(entry);
            target.preprocess();
            target.postprocess();
        }

        RootNode root;
        int ticks {};
    };

    TEST_F(RootNodeProcessTest, PreprocessThenPostprocessCompletesACycle)
    {
        EXPECT_TRUE(root.preprocess());
        root.postprocess();
        EXPECT_TRUE(root.preprocess());
        root.postprocess();
    }

    TEST_F(RootNodeProcessTest, PreprocessRefusesWhileACycleIsInFlight)
    {
        ASSERT_TRUE(root.preprocess());

        EXPECT_FALSE(root.preprocess());
        EXPECT_DOUBLE_EQ(root.process_sample(), 0.0);

        root.postprocess();
        EXPECT_TRUE(root.preprocess());
        root.postprocess();
    }

    TEST_F(RootNodeProcessTest, RegistrationDuringACycleCommitsAtPostprocess)
    {
        auto entry = std::make_shared<Constant>(1.0);
        ASSERT_TRUE(root.preprocess());

        root.register_node(entry);
        EXPECT_EQ(root.get_node_size(), 0U);

        root.postprocess();
        EXPECT_EQ(root.get_node_size(), 1U);
    }

    TEST_F(RootNodeProcessTest, PreprocessCommitsPendingRegistrations)
    {
        root.register_node(std::make_shared<Constant>(1.0));

        ASSERT_TRUE(root.preprocess());
        EXPECT_EQ(root.get_node_size(), 1U);
        root.postprocess();
    }

    TEST_F(RootNodeProcessTest, PreprocessFailsAfterTerminate)
    {
        root.terminate_all_nodes();

        EXPECT_FALSE(root.preprocess());
    }

    TEST_F(RootNodeProcessTest, NodesAccessorListsCommittedNodesInRegistrationOrder)
    {
        auto first = std::make_shared<Constant>(1.0);
        auto second = std::make_shared<Constant>(2.0);
        auto third = std::make_shared<Constant>(3.0);
        root.register_node(first);
        root.register_node(second);
        root.register_node(third);
        root.process_sample();

        const auto& listed = root.nodes();
        ASSERT_EQ(listed.size(), 3U);
        EXPECT_EQ(listed[0], first);
        EXPECT_EQ(listed[1], second);
        EXPECT_EQ(listed[2], third);
    }

    TEST_F(RootNodeProcessTest, NodeWithChannelUsageProcessesOncePerCycle)
    {
        commit(root, make_counted(1.0, ticks));
        ticks = 0;

        for (int i = 0; i < 5; ++i) {
            root.process_sample();
        }

        EXPECT_EQ(ticks, 5);
    }

    TEST_F(RootNodeProcessTest, NodeIsResetForTheNextCycleWhenAllItsChannelsFinished)
    {
        auto entry = make_counted(1.0, ticks);
        commit(root, entry);

        root.process_sample();

        EXPECT_FALSE(has(*entry, NodeState::PROCESSED));
    }

    TEST_F(RootNodeProcessTest, NodeWithoutChannelUsageStaysProcessedAfterTheCycle)
    {
        auto entry = make_counted(1.0, ticks);
        root.register_node(entry);

        root.process_sample();

        EXPECT_TRUE(has(*entry, NodeState::PROCESSED));
    }

    TEST_F(RootNodeProcessTest, AlreadyProcessedNodeIsNotProcessedAgain)
    {
        auto entry = make_counted(4.0, ticks);
        root.register_node(entry);
        root.process_sample();
        ASSERT_EQ(ticks, 1);

        const double sample = root.process_sample();

        EXPECT_EQ(ticks, 1);
        EXPECT_DOUBLE_EQ(sample, 4.0);
    }

    TEST_F(RootNodeProcessTest, ProcessFrameProcessesNodesWithoutSumming)
    {
        commit(root, make_counted(1.0, ticks));
        ticks = 0;

        root.process_frame();

        EXPECT_EQ(ticks, 1);
    }

    TEST_F(RootNodeProcessTest, ProcessBatchFrameRunsEveryCycle)
    {
        commit(root, make_counted(1.0, ticks));
        ticks = 0;

        root.process_batch_frame(8);

        EXPECT_EQ(ticks, 8);
    }

    TEST_F(RootNodeProcessTest, ProcessBatchRunsEveryCycle)
    {
        commit(root, make_counted(1.0, ticks));
        ticks = 0;

        const auto batch = root.process_batch(6);

        EXPECT_EQ(batch.size(), 6U);
        EXPECT_EQ(ticks, 6);
    }

    TEST_F(RootNodeProcessTest, OutputExcludedNodeRunsButDoesNotContribute)
    {
        auto audible = std::make_shared<Constant>(1.0);
        auto hidden = make_counted(5.0, ticks);
        commit(root, audible);
        commit(root, hidden);
        Nodes::atomic_add_flag(hidden->m_state, NodeState::OUTPUT_EXCLUDED);
        ticks = 0;

        EXPECT_DOUBLE_EQ(root.process_sample(), 1.0);
        EXPECT_EQ(ticks, 1);
    }

    TEST_F(RootNodeProcessTest, ChannelRoutingScalesOutputByTheRootChannelAmount)
    {
        RootNode second(Nodes::ProcessingToken::AUDIO_RATE, 1);
        auto entry = std::make_shared<Constant>(2.0);
        entry->register_channel_usage(0);
        entry->register_channel_usage(1);
        entry->get_routing_state().phase = Nodes::RoutingState::ACTIVE;
        entry->get_routing_state().amount[0] = 0.5;
        entry->get_routing_state().amount[1] = 0.25;
        root.register_node(entry);
        second.register_node(entry);

        EXPECT_DOUBLE_EQ(root.process_sample(), 1.0);
        EXPECT_DOUBLE_EQ(second.process_sample(), 0.5);
    }

    TEST_F(RootNodeProcessTest, NodeSharedByTwoRootsIsProcessedOncePerCycle)
    {
        RootNode second(Nodes::ProcessingToken::AUDIO_RATE, 1);
        auto entry = make_counted(1.0, ticks);
        entry->register_channel_usage(0);
        entry->register_channel_usage(1);
        root.register_node(entry);
        second.register_node(entry);

        root.process_sample();
        second.process_sample();

        EXPECT_EQ(ticks, 1);
        EXPECT_FALSE(has(*entry, NodeState::PROCESSED));

        root.process_sample();
        second.process_sample();

        EXPECT_EQ(ticks, 2);
    }

    TEST_F(RootNodeProcessTest, SharedNodeStaysProcessedUntilEveryChannelHasFinished)
    {
        RootNode second(Nodes::ProcessingToken::AUDIO_RATE, 1);
        auto entry = std::make_shared<Constant>(1.0);
        entry->register_channel_usage(0);
        entry->register_channel_usage(1);
        root.register_node(entry);
        second.register_node(entry);

        root.process_sample();

        EXPECT_TRUE(has(*entry, NodeState::PROCESSED));

        second.process_sample();

        EXPECT_FALSE(has(*entry, NodeState::PROCESSED));
    }

    TEST_F(RootNodeProcessTest, RemovalClearsTheProcessedFlag)
    {
        auto entry = std::make_shared<Constant>(1.0);
        root.register_node(entry);
        root.process_sample();
        ASSERT_TRUE(has(*entry, NodeState::PROCESSED));

        root.unregister_node(entry);
        root.process_sample();

        EXPECT_FALSE(has(*entry, NodeState::PROCESSED));
    }

    TEST_F(RootNodeProcessTest, TerminateAllNodesSkipsFurtherCycles)
    {
        commit(root, make_counted(1.0, ticks));
        root.terminate_all_nodes();
        ticks = 0;

        EXPECT_DOUBLE_EQ(root.process_sample(), 0.0);
        root.process_frame();
        EXPECT_EQ(ticks, 0);
    }

    class RootNodeBulkTest : public ::testing::TestWithParam<size_t> {
    protected:
        std::vector<std::shared_ptr<Constant>> make_nodes(size_t count)
        {
            std::vector<std::shared_ptr<Constant>> nodes;
            nodes.reserve(count);
            for (size_t i = 0; i < count; ++i) {
                nodes.push_back(std::make_shared<Constant>(1.0));
            }
            return nodes;
        }

        RootNode root;
    };

    TEST_P(RootNodeBulkTest, EveryQueuedRegistrationCommitsOnce)
    {
        const auto nodes = make_nodes(GetParam());
        for (const auto& entry : nodes) {
            root.register_node(entry);
        }

        EXPECT_EQ(root.get_node_size(), 0U);

        root.process_sample();

        EXPECT_EQ(root.get_node_size(), GetParam());
        for (const auto& entry : nodes) {
            EXPECT_TRUE(has(*entry, NodeState::ACTIVE));
        }
    }

    TEST_P(RootNodeBulkTest, EveryQueuedRemovalCommitsOnce)
    {
        const auto nodes = make_nodes(GetParam());
        for (const auto& entry : nodes) {
            root.register_node(entry);
        }
        root.process_sample();

        for (const auto& entry : nodes) {
            root.unregister_node(entry);
        }

        EXPECT_EQ(root.get_node_size(), GetParam());

        root.process_sample();

        EXPECT_EQ(root.get_node_size(), 0U);
        for (const auto& entry : nodes) {
            EXPECT_FALSE(has(*entry, NodeState::ACTIVE));
        }
    }

    TEST_P(RootNodeBulkTest, SumScalesWithRegisteredCount)
    {
        const auto nodes = make_nodes(GetParam());
        for (const auto& entry : nodes) {
            root.register_node(entry);
        }

        EXPECT_DOUBLE_EQ(root.process_sample(), static_cast<double>(GetParam()));
    }

    TEST_P(RootNodeBulkTest, QueueIsReusableAfterAFullDrain)
    {
        for (int round = 0; round < 3; ++round) {
            const auto nodes = make_nodes(GetParam());
            for (const auto& entry : nodes) {
                root.register_node(entry);
            }
            root.process_sample();
            ASSERT_EQ(root.get_node_size(), GetParam());

            for (const auto& entry : nodes) {
                root.unregister_node(entry);
            }
            root.process_sample();
            ASSERT_EQ(root.get_node_size(), 0U);
        }
    }

    INSTANTIATE_TEST_SUITE_P(
        Counts,
        RootNodeBulkTest,
        ::testing::Values(size_t { 1 }, size_t { 7 }, size_t { 64 }, size_t { 1000 }, size_t { 2048 }),
        [](const ::testing::TestParamInfo<size_t>& info) { return "N" + std::to_string(info.param); });

    struct ConcurrentCase {
        const char* name;
        size_t producers;
        size_t per_producer;
    };

    class RootNodeConcurrentTest : public ::testing::TestWithParam<ConcurrentCase> {
    protected:
        RootNode root;
    };

    TEST_P(RootNodeConcurrentTest, ConcurrentRegistrationsAllCommitWhileProcessing)
    {
        const auto& param = GetParam();
        std::atomic<bool> done { false };
        std::atomic<size_t> started { 0 };

        std::vector<std::vector<std::shared_ptr<Constant>>> owned(param.producers);
        for (auto& list : owned) {
            for (size_t i = 0; i < param.per_producer; ++i) {
                list.push_back(std::make_shared<Constant>(1.0));
            }
        }

        std::thread consumer([&] {
            while (!done.load(std::memory_order_acquire)) {
                root.process_sample();
            }
        });

        std::vector<std::thread> producers;
        producers.reserve(param.producers);
        for (size_t p = 0; p < param.producers; ++p) {
            producers.emplace_back([&, p] {
                started.fetch_add(1);
                while (started.load() < param.producers) {
                    std::this_thread::yield();
                }
                for (const auto& entry : owned[p]) {
                    root.register_node(entry);
                }
            });
        }
        for (auto& thread : producers) {
            thread.join();
        }

        done.store(true, std::memory_order_release);
        consumer.join();
        root.process_sample();

        EXPECT_EQ(root.get_node_size(), param.producers * param.per_producer);
    }

    TEST_P(RootNodeConcurrentTest, ConcurrentRegisterThenUnregisterLeavesNothing)
    {
        const auto& param = GetParam();
        std::atomic<bool> done { false };

        std::vector<std::vector<std::shared_ptr<Constant>>> owned(param.producers);
        for (auto& list : owned) {
            for (size_t i = 0; i < param.per_producer; ++i) {
                list.push_back(std::make_shared<Constant>(1.0));
            }
        }

        std::thread consumer([&] {
            while (!done.load(std::memory_order_acquire)) {
                root.process_sample();
            }
        });

        std::vector<std::thread> producers;
        producers.reserve(param.producers);
        for (size_t p = 0; p < param.producers; ++p) {
            producers.emplace_back([&, p] {
                for (const auto& entry : owned[p]) {
                    root.register_node(entry);
                    root.unregister_node(entry);
                }
            });
        }
        for (auto& thread : producers) {
            thread.join();
        }

        done.store(true, std::memory_order_release);
        consumer.join();
        root.process_sample();

        EXPECT_EQ(root.get_node_size(), 0U);
    }

    INSTANTIATE_TEST_SUITE_P(
        Producers,
        RootNodeConcurrentTest,
        ::testing::Values(
            ConcurrentCase { "OneProducer", 1, 500 },
            ConcurrentCase { "FourProducers", 4, 300 },
            ConcurrentCase { "EightProducers", 8, 200 }),
        [](const ::testing::TestParamInfo<ConcurrentCase>& info) { return std::string(info.param.name); });

    struct TokenCase {
        const char* name;
        Nodes::ProcessingToken token;
    };

    class RootNodeTokenTest : public ::testing::TestWithParam<TokenCase> {
    protected:
        std::shared_ptr<Constant> make_member(RootNode& root, double value, int* ticks = nullptr)
        {
            auto entry = std::make_shared<Constant>(value);
            if (ticks != nullptr) {
                entry->on_tick([ticks](Nodes::NodeContext&) { ++*ticks; });
            }
            entry->register_channel_usage(root.get_channel());
            root.register_node(entry);
            return entry;
        }
    };

    TEST_P(RootNodeTokenTest, ARegisterProcessAndRemoveCycleIsTheSameUnderEveryToken)
    {
        RootNode root(GetParam().token, 0);
        const auto first = make_member(root, 1.0);
        make_member(root, 2.0);

        EXPECT_EQ(root.get_token(), GetParam().token);
        EXPECT_DOUBLE_EQ(root.process_sample(), 3.0);
        EXPECT_EQ(root.get_node_size(), 2U);

        root.unregister_node(first);
        EXPECT_DOUBLE_EQ(root.process_sample(), 2.0);
        EXPECT_EQ(root.get_node_size(), 1U);

        root.terminate_all_nodes();
        EXPECT_EQ(root.get_node_size(), 0U);
        EXPECT_DOUBLE_EQ(root.process_sample(), 0.0);
    }

    TEST_P(RootNodeTokenTest, FrameAndBatchProcessingRunEveryNodeUnderEveryToken)
    {
        RootNode root(GetParam().token, 0);
        int ticks = 0;
        make_member(root, 1.0, &ticks);

        root.process_frame();
        EXPECT_EQ(ticks, 1);

        root.process_batch_frame(4);
        EXPECT_EQ(ticks, 5);

        EXPECT_EQ(root.process_batch(3).size(), 3U);
        EXPECT_EQ(ticks, 8);
    }

    TEST_P(RootNodeTokenTest, AnOutputExcludedNodeStillRunsUnderEveryToken)
    {
        RootNode root(GetParam().token, 0);
        int ticks = 0;
        make_member(root, 1.0);
        const auto hidden = make_member(root, 5.0, &ticks);
        Nodes::atomic_add_flag(hidden->m_state, NodeState::OUTPUT_EXCLUDED);

        EXPECT_DOUBLE_EQ(root.process_sample(), 1.0);
        EXPECT_EQ(ticks, 1);
    }

    INSTANTIATE_TEST_SUITE_P(
        Tokens,
        RootNodeTokenTest,
        ::testing::Values(
            TokenCase { "Audio", Nodes::ProcessingToken::AUDIO_RATE },
            TokenCase { "Visual", Nodes::ProcessingToken::VISUAL_RATE },
            TokenCase { "Event", Nodes::ProcessingToken::EVENT_RATE },
            TokenCase { "Custom", Nodes::ProcessingToken::CUSTOM_RATE }),
        [](const ::testing::TestParamInfo<TokenCase>& info) { return std::string(info.param.name); });

}
}
