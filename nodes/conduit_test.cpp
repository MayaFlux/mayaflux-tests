#include "gtest/gtest.h"

#include "MayaFlux/Nodes/Conduit/Constant.hpp"
#include "MayaFlux/Nodes/Conduit/NodeChain.hpp"
#include "MayaFlux/Nodes/Conduit/NodeCombine.hpp"
#include "MayaFlux/Nodes/Conduit/StreamReaderNode.hpp"
#include "MayaFlux/Nodes/Generators/Sine.hpp"
#include "MayaFlux/Nodes/NodeGraphManager.hpp"

namespace MayaFlux::Test {
namespace {

    using Nodes::BinaryOpContext;
    using Nodes::BinaryOpNode;
    using Nodes::ChainNode;
    using Nodes::CompositeOpContext;
    using Nodes::CompositeOpNode;
    using Nodes::Constant;
    using Nodes::ModulatorRole;
    using Nodes::Node;
    using Nodes::NodeBinaryOpSemantics;
    using Nodes::NodeChainSemantics;
    using Nodes::NodeContext;
    using Nodes::NodeGraphManager;
    using Nodes::NodeState;
    using Nodes::ProcessingToken;
    using Nodes::StreamReaderNode;
    using Nodes::Generator::Sine;

    constexpr double k_tolerance = 1e-12;

    std::vector<double> run(Node& node, size_t count)
    {
        std::vector<double> samples;
        samples.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            samples.push_back(node.process_sample(0.0));
        }
        return samples;
    }

    bool has(const Node& node, NodeState state)
    {
        return (node.m_state.load() & state) != 0;
    }

    bool in_root(Nodes::RootNode& root, const std::shared_ptr<Node>& node)
    {
        return std::ranges::find(root.nodes(), node) != root.nodes().end();
    }

    class ConduitConstantTest : public ::testing::TestWithParam<double> {
    protected:
        Constant node { GetParam() };
    };

    TEST_P(ConduitConstantTest, ReportsItsValueBeforeAnyProcessing)
    {
        EXPECT_DOUBLE_EQ(node.get_constant(), GetParam());
        EXPECT_DOUBLE_EQ(node.get_last_output(), GetParam());
    }

    TEST_P(ConduitConstantTest, ProcessingReturnsTheValueAndIgnoresInput)
    {
        EXPECT_DOUBLE_EQ(node.process_sample(0.0), GetParam());
        EXPECT_DOUBLE_EQ(node.process_sample(123.0), GetParam());
    }

    TEST_P(ConduitConstantTest, BatchRepeatsTheValue)
    {
        const auto batch = node.process_batch(17);

        ASSERT_EQ(batch.size(), 17U);
        for (const double sample : batch) {
            EXPECT_DOUBLE_EQ(sample, GetParam());
        }
    }

    TEST_P(ConduitConstantTest, SetConstantChangesTheValueAndTheLastOutputImmediately)
    {
        node.set_constant(GetParam() + 5.0);

        EXPECT_DOUBLE_EQ(node.get_constant(), GetParam() + 5.0);
        EXPECT_DOUBLE_EQ(node.get_last_output(), GetParam() + 5.0);
        EXPECT_DOUBLE_EQ(node.process_sample(0.0), GetParam() + 5.0);
    }

    TEST_P(ConduitConstantTest, RestoreBringsBackTheSavedValue)
    {
        node.save_state();
        node.set_constant(99.0);

        node.restore_state();

        EXPECT_DOUBLE_EQ(node.get_constant(), GetParam());
        EXPECT_DOUBLE_EQ(node.get_last_output(), GetParam());
    }

    TEST_P(ConduitConstantTest, ContextCarriesTheValueOnEachTick)
    {
        double seen = -1000.0;
        node.on_tick([&seen](NodeContext& ctx) { seen = ctx.value; });

        node.process_sample(0.0);

        EXPECT_DOUBLE_EQ(seen, GetParam());
    }

    TEST_P(ConduitConstantTest, TicksFireOncePerSampleAndStopWhileSaved)
    {
        int ticks = 0;
        node.on_tick([&ticks](NodeContext&) { ++ticks; });

        run(node, 4);
        EXPECT_EQ(ticks, 4);

        node.save_state();
        run(node, 4);
        EXPECT_EQ(ticks, 4);

        node.restore_state();
        run(node, 4);
        EXPECT_EQ(ticks, 8);
    }

    TEST_P(ConduitConstantTest, NetworkedNodeNeverTicks)
    {
        int ticks = 0;
        node.on_tick([&ticks](NodeContext&) { ++ticks; });
        node.set_in_network(true);

        run(node, 4);

        EXPECT_EQ(ticks, 0);
    }

    INSTANTIATE_TEST_SUITE_P(
        Values,
        ConduitConstantTest,
        ::testing::Values(0.0, 1.0, -2.5, 1.0e6));

    TEST(ConduitConstantDefaultTest, DefaultsToZero)
    {
        Constant node;

        EXPECT_DOUBLE_EQ(node.process_sample(0.0), 0.0);
    }

    class ConduitStreamReaderTest : public ::testing::Test {
    protected:
        StreamReaderNode node;
        std::vector<double> data { 1.0, 2.0, 3.0, 4.0 };
    };

    TEST_F(ConduitStreamReaderTest, EmptyNodeOutputsZero)
    {
        EXPECT_EQ(node.remaining(), 0U);
        EXPECT_DOUBLE_EQ(node.process_sample(0.0), 0.0);
        EXPECT_DOUBLE_EQ(node.get_last_output(), 0.0);
    }

    TEST_F(ConduitStreamReaderTest, ReadsTheDataInOrderAndCountsDown)
    {
        ASSERT_TRUE(node.set_data(data));
        EXPECT_EQ(node.remaining(), 4U);

        for (size_t i = 0; i < data.size(); ++i) {
            EXPECT_DOUBLE_EQ(node.process_sample(0.0), data[i]);
            EXPECT_EQ(node.remaining(), data.size() - i - 1);
        }
    }

    TEST_F(ConduitStreamReaderTest, HoldsTheLastValueOnceTheDataIsExhausted)
    {
        node.set_data(data);
        run(node, 4);

        for (int i = 0; i < 5; ++i) {
            EXPECT_DOUBLE_EQ(node.process_sample(0.0), 4.0);
        }
        EXPECT_EQ(node.remaining(), 0U);
    }

    TEST_F(ConduitStreamReaderTest, RewindRestartsReadingFromTheFirstSample)
    {
        node.set_data(data);
        run(node, 3);

        node.rewind();

        EXPECT_EQ(node.remaining(), 4U);
        EXPECT_DOUBLE_EQ(node.process_sample(0.0), 1.0);
    }

    TEST_F(ConduitStreamReaderTest, SettingNewDataRestartsTheReadHead)
    {
        node.set_data(data);
        run(node, 3);

        const std::vector<double> other { 7.0, 8.0 };
        node.set_data(other);

        EXPECT_EQ(node.remaining(), 2U);
        EXPECT_DOUBLE_EQ(node.process_sample(0.0), 7.0);
    }

    TEST_F(ConduitStreamReaderTest, BatchReadsConsecutiveSamples)
    {
        node.set_data(data);

        EXPECT_EQ(node.process_batch(6), (std::vector<double> { 1.0, 2.0, 3.0, 4.0, 4.0, 4.0 }));
    }

    TEST_F(ConduitStreamReaderTest, FirstOwnerKeepsTheDataAgainstOthers)
    {
        int first = 0;
        int second = 0;

        EXPECT_TRUE(node.set_data(data, &first));
        const std::vector<double> other { 9.0 };
        EXPECT_FALSE(node.set_data(other, &second));

        EXPECT_EQ(node.remaining(), 4U);
        EXPECT_DOUBLE_EQ(node.process_sample(0.0), 1.0);
    }

    TEST_F(ConduitStreamReaderTest, OwnerCanReplaceItsOwnData)
    {
        int owner = 0;
        node.set_data(data, &owner);

        const std::vector<double> other { 9.0, 8.0 };
        EXPECT_TRUE(node.set_data(other, &owner));
        EXPECT_EQ(node.remaining(), 2U);
    }

    TEST_F(ConduitStreamReaderTest, ReleasingTheOwnerLetsAnotherTakeOver)
    {
        int first = 0;
        int second = 0;
        node.set_data(data, &first);

        node.release_owner(&second);
        EXPECT_FALSE(node.set_data(data, &second));

        node.release_owner(&first);
        EXPECT_TRUE(node.set_data(data, &second));
    }

    TEST_F(ConduitStreamReaderTest, UnownedDataCanBeClaimedLater)
    {
        int owner = 0;
        node.set_data(data);

        EXPECT_TRUE(node.set_data(data, &owner));
    }

    TEST_F(ConduitStreamReaderTest, RestoreRewindsDataAndReadHead)
    {
        node.set_data(data);
        run(node, 2);

        node.save_state();
        const std::vector<double> other { 50.0 };
        node.set_data(other);
        run(node, 3);
        node.restore_state();

        EXPECT_EQ(node.remaining(), 2U);
        EXPECT_DOUBLE_EQ(node.process_sample(0.0), 3.0);
        EXPECT_DOUBLE_EQ(node.process_sample(0.0), 4.0);
    }

    TEST_F(ConduitStreamReaderTest, TicksFireOncePerSampleAndStopWhileSaved)
    {
        node.set_data(data);
        int ticks = 0;
        node.on_tick([&ticks](NodeContext&) { ++ticks; });

        run(node, 2);
        EXPECT_EQ(ticks, 2);

        node.save_state();
        run(node, 2);
        EXPECT_EQ(ticks, 2);

        node.restore_state();
        run(node, 2);
        EXPECT_EQ(ticks, 4);
    }

    class ConduitChainTest : public ::testing::Test {
    protected:
        std::shared_ptr<Constant> one { std::make_shared<Constant>(1.0) };
        std::shared_ptr<Constant> two { std::make_shared<Constant>(2.0) };
        std::shared_ptr<Constant> three { std::make_shared<Constant>(3.0) };
    };

    TEST_F(ConduitChainTest, RequiresAtLeastTwoNodes)
    {
        EXPECT_THROW(ChainNode(std::vector<std::shared_ptr<Node>> { one }), std::invalid_argument);
        EXPECT_THROW(ChainNode(std::vector<std::shared_ptr<Node>> {}), std::invalid_argument);
    }

    TEST_F(ConduitChainTest, RejectsNullNodes)
    {
        EXPECT_THROW(ChainNode(std::vector<std::shared_ptr<Node>> { one, nullptr }), std::invalid_argument);
        EXPECT_THROW(ChainNode(one, nullptr), std::invalid_argument);
    }

    TEST_F(ConduitChainTest, AppendRejectsNull)
    {
        ChainNode chain(one, two);

        EXPECT_THROW(chain.append(nullptr), std::invalid_argument);
        EXPECT_EQ(chain.size(), 2U);
    }

    TEST_F(ConduitChainTest, OutputIsTheLastNodesOutput)
    {
        ChainNode chain(one, two);

        EXPECT_DOUBLE_EQ(chain.process_sample(0.0), 2.0);
    }

    TEST_F(ConduitChainTest, AppendExtendsTheChainAtTheEnd)
    {
        ChainNode chain(one, two);

        chain.append(three);

        ASSERT_EQ(chain.size(), 3U);
        EXPECT_EQ(chain.nodes()[2], three);
        EXPECT_DOUBLE_EQ(chain.process_sample(0.0), 3.0);
    }

    TEST_F(ConduitChainTest, AppendChainMovesTheOtherChainsNodesOver)
    {
        ChainNode chain(one, two);
        auto other = std::make_shared<ChainNode>(three, std::make_shared<Constant>(4.0));

        chain.append_chain(other);

        EXPECT_EQ(chain.size(), 4U);
        EXPECT_DOUBLE_EQ(chain.process_sample(0.0), 4.0);
    }

    TEST_F(ConduitChainTest, EachNodeReceivesThePreviousNodesOutputAsInput)
    {
        auto sine = std::make_shared<Sine>(12000.0F, 1.0);
        ChainNode chain(one, sine);

        const auto samples = run(chain, 4);

        const std::vector<double> expected { 0.5, 1.0, 0.5, 0.0 };
        for (size_t i = 0; i < expected.size(); ++i) {
            EXPECT_NEAR(samples[i], expected[i], 1e-9) << "sample " << i;
        }
    }

    TEST_F(ConduitChainTest, AlreadyProcessedNodeAddsItsLastOutput)
    {
        ChainNode chain(one, two);
        Nodes::atomic_add_flag(two->m_state, NodeState::PROCESSED);

        EXPECT_DOUBLE_EQ(chain.process_sample(0.0), 1.0 + 2.0);
    }

    TEST_F(ConduitChainTest, ProcessingLeavesTheModulatorCountsBalancedAndFlagsCleared)
    {
        ChainNode chain(one, two);

        run(chain, 3);

        EXPECT_EQ(one->m_modulator_count.load(), 0U);
        EXPECT_EQ(two->m_modulator_count.load(), 0U);
        EXPECT_FALSE(has(*one, NodeState::PROCESSED));
        EXPECT_FALSE(has(*two, NodeState::PROCESSED));
    }

    TEST_F(ConduitChainTest, ChannelOwnedNodeKeepsItsProcessedFlag)
    {
        ChainNode chain(one, two);
        two->register_channel_usage(0);

        chain.process_sample(0.0);

        EXPECT_TRUE(has(*two, NodeState::PROCESSED));
        EXPECT_FALSE(has(*one, NodeState::PROCESSED));
    }

    TEST_F(ConduitChainTest, ResetClearsTheChainAndItsNodes)
    {
        ChainNode chain(one, two);
        Nodes::atomic_add_flag(chain.m_state, NodeState::PROCESSED);
        Nodes::atomic_add_flag(one->m_state, NodeState::PROCESSED);
        Nodes::atomic_add_flag(two->m_state, NodeState::PROCESSED);

        chain.reset_processed_state();

        EXPECT_FALSE(has(chain, NodeState::PROCESSED));
        EXPECT_FALSE(has(*one, NodeState::PROCESSED));
        EXPECT_FALSE(has(*two, NodeState::PROCESSED));
    }

    TEST_F(ConduitChainTest, HooksAreForwardedToTheLastNode)
    {
        ChainNode chain(one, two);
        int ticks = 0;
        const auto id = chain.on_tick([&ticks](NodeContext&) { ++ticks; });

        run(chain, 3);
        EXPECT_NE(id, 0U);
        EXPECT_EQ(ticks, 3);

        EXPECT_TRUE(chain.unhook(id));
        run(chain, 3);
        EXPECT_EQ(ticks, 3);
    }

    TEST_F(ConduitChainTest, ConditionalHooksAndRemoveAllForwardToo)
    {
        ChainNode chain(one, two);
        int ticks = 0;
        chain.on_tick_if([](NodeContext&) { return true; }, [&ticks](NodeContext&) { ++ticks; });
        run(chain, 2);
        ASSERT_EQ(ticks, 2);

        chain.remove_all_hooks();
        run(chain, 2);

        EXPECT_EQ(ticks, 2);
    }

    TEST_F(ConduitChainTest, ContextIsTheLastNodesContext)
    {
        ChainNode chain(one, two);

        EXPECT_EQ(&chain.get_last_context(), &two->get_last_context());
    }

    TEST_F(ConduitChainTest, ModulatorsAreTheLinksInOrder)
    {
        ChainNode chain(one, two);
        chain.append(three);

        const auto links = chain.get_modulators();

        ASSERT_EQ(links.size(), 3U);
        EXPECT_EQ(links[0].second, one);
        EXPECT_EQ(links[1].second, two);
        EXPECT_EQ(links[2].second, three);
        for (const auto& link : links) {
            EXPECT_EQ(link.first, ModulatorRole::ChainLink);
        }
    }

    TEST_F(ConduitChainTest, BatchMatchesConsecutiveSamples)
    {
        ChainNode first(std::make_shared<Constant>(1.0), std::make_shared<Sine>(12000.0F, 1.0));
        ChainNode second(std::make_shared<Constant>(1.0), std::make_shared<Sine>(12000.0F, 1.0));

        EXPECT_EQ(first.process_batch(8), run(second, 8));
    }

    TEST_F(ConduitChainTest, RestoreRewindsEveryLink)
    {
        auto make = [] {
            return std::make_shared<ChainNode>(std::make_shared<Constant>(1.0), std::make_shared<Sine>(12000.0F, 1.0));
        };
        auto a = make();
        auto b = make();
        run(*a, 3);
        run(*b, 3);
        const auto reference = run(*b, 5);

        a->save_state();
        run(*a, 5);
        a->restore_state();

        EXPECT_EQ(run(*a, 5), reference);
    }

    struct ChainSemanticsCase {
        const char* name;
        NodeChainSemantics semantics;
        bool keeps_source;
        bool keeps_target;
    };

    class ConduitChainManagerTest : public ::testing::TestWithParam<ChainSemanticsCase> {
    protected:
        void SetUp() override
        {
            Nodes::NodeConfig config;
            config.chain_semantics = GetParam().semantics;
            manager.set_node_config(config);

            manager.add_to_root(source, token, 0);
            manager.add_to_root(target, token, 0);
            root(0).process_sample();
            ASSERT_EQ(root(0).get_node_size(), 2U);
        }

        Nodes::RootNode& root(uint32_t channel) { return manager.get_root_node(token, channel); }

        NodeGraphManager manager;
        ProcessingToken token = ProcessingToken::AUDIO_RATE;
        std::shared_ptr<Constant> source { std::make_shared<Constant>(1.0) };
        std::shared_ptr<Constant> target { std::make_shared<Constant>(2.0) };
    };

    TEST_P(ConduitChainManagerTest, ChainRegistersItselfAndAppliesTheSemanticsOnFirstProcess)
    {
        auto chain = std::make_shared<ChainNode>(source, target, manager, token);

        chain->process_sample(0.0);
        root(0).process_sample();

        EXPECT_TRUE(in_root(root(0), chain));
        EXPECT_EQ(in_root(root(0), source), GetParam().keeps_source);
        EXPECT_EQ(in_root(root(0), target), GetParam().keeps_target);
        EXPECT_EQ(root(0).get_node_size(), 1U + (GetParam().keeps_source ? 1U : 0U) + (GetParam().keeps_target ? 1U : 0U));
    }

    TEST_P(ConduitChainManagerTest, ChainNeverRegistersTwice)
    {
        auto chain = std::make_shared<ChainNode>(source, target, manager, token);

        for (int i = 0; i < 4; ++i) {
            chain->process_sample(0.0);
            root(0).process_sample();
        }

        EXPECT_EQ(std::ranges::count(root(0).nodes(), std::static_pointer_cast<Node>(chain)), 1);
    }

    TEST_P(ConduitChainManagerTest, ChainJoinsEveryChannelOfItsTarget)
    {
        manager.add_to_root(target, token, 2);
        root(2).process_sample();
        auto chain = std::make_shared<ChainNode>(source, target, manager, token);

        chain->process_sample(0.0);
        root(0).process_sample();
        root(2).process_sample();

        EXPECT_TRUE(in_root(root(0), chain));
        EXPECT_TRUE(in_root(root(2), chain));
        EXPECT_EQ(in_root(root(2), target), GetParam().keeps_target);
    }

    INSTANTIATE_TEST_SUITE_P(
        Semantics,
        ConduitChainManagerTest,
        ::testing::Values(
            ChainSemanticsCase { "ReplaceTarget", NodeChainSemantics::REPLACE_TARGET, true, false },
            ChainSemanticsCase { "OnlyChain", NodeChainSemantics::ONLY_CHAIN, false, false },
            ChainSemanticsCase { "PreserveBoth", NodeChainSemantics::PRESERVE_BOTH, true, true }),
        [](const ::testing::TestParamInfo<ChainSemanticsCase>& info) { return std::string(info.param.name); });

    TEST(ConduitChainFallbackTest, TargetOutsideEveryRootFallsBackToChannelZero)
    {
        NodeGraphManager manager;
        auto chain = std::make_shared<ChainNode>(std::make_shared<Constant>(1.0), std::make_shared<Constant>(2.0), manager);

        chain->initialize();
        manager.get_root_node(ProcessingToken::AUDIO_RATE, 0).process_sample();

        EXPECT_TRUE(in_root(manager.get_root_node(ProcessingToken::AUDIO_RATE, 0), chain));
    }

    struct BinaryCase {
        const char* name;
        BinaryOpNode::CombineFunc func;
        double lhs;
        double rhs;
        double expected;
    };

    class ConduitBinaryOpTest : public ::testing::TestWithParam<BinaryCase> {
    protected:
        std::shared_ptr<Constant> lhs { std::make_shared<Constant>(GetParam().lhs) };
        std::shared_ptr<Constant> rhs { std::make_shared<Constant>(GetParam().rhs) };
        BinaryOpNode node { lhs, rhs, GetParam().func };
    };

    TEST_P(ConduitBinaryOpTest, OutputIsTheFunctionOfTheTwoInputs)
    {
        EXPECT_DOUBLE_EQ(node.process_sample(0.0), GetParam().expected);
        EXPECT_DOUBLE_EQ(node.get_last_output(), GetParam().expected);
    }

    TEST_P(ConduitBinaryOpTest, BatchRepeatsTheResult)
    {
        for (const double sample : node.process_batch(5)) {
            EXPECT_DOUBLE_EQ(sample, GetParam().expected);
        }
    }

    TEST_P(ConduitBinaryOpTest, ContextCarriesTheInputsAndTheResult)
    {
        double value = 0.0;
        double left = 0.0;
        double right = 0.0;
        node.on_tick([&](NodeContext& ctx) {
            const auto* binary = dynamic_cast<const BinaryOpContext*>(&ctx);
            ASSERT_NE(binary, nullptr);
            value = binary->value;
            left = binary->lhs_value;
            right = binary->rhs_value;
        });

        node.process_sample(0.0);

        EXPECT_DOUBLE_EQ(value, GetParam().expected);
        EXPECT_DOUBLE_EQ(left, GetParam().lhs);
        EXPECT_DOUBLE_EQ(right, GetParam().rhs);
    }

    TEST_P(ConduitBinaryOpTest, GpuCompatibleNodeReportsTheSameContext)
    {
        node.set_gpu_compatible(true);
        double left = 0.0;
        double right = 0.0;
        node.on_tick([&](NodeContext& ctx) {
            const auto* binary = dynamic_cast<const BinaryOpContext*>(&ctx);
            ASSERT_NE(binary, nullptr);
            left = binary->lhs_value;
            right = binary->rhs_value;
        });

        node.process_sample(0.0);

        EXPECT_DOUBLE_EQ(left, GetParam().lhs);
        EXPECT_DOUBLE_EQ(right, GetParam().rhs);
    }

    TEST_P(ConduitBinaryOpTest, ProcessingLeavesTheModulatorCountsBalanced)
    {
        run(node, 3);

        EXPECT_EQ(lhs->m_modulator_count.load(), 0U);
        EXPECT_EQ(rhs->m_modulator_count.load(), 0U);
        EXPECT_FALSE(has(*lhs, NodeState::PROCESSED));
        EXPECT_FALSE(has(*rhs, NodeState::PROCESSED));
    }

    TEST_P(ConduitBinaryOpTest, TicksFireOncePerSampleAndStopWhileSaved)
    {
        int ticks = 0;
        node.on_tick([&ticks](NodeContext&) { ++ticks; });

        run(node, 3);
        EXPECT_EQ(ticks, 3);

        node.save_state();
        run(node, 3);
        EXPECT_EQ(ticks, 3);

        node.restore_state();
        run(node, 3);
        EXPECT_EQ(ticks, 6);
    }

    TEST_P(ConduitBinaryOpTest, ModulatorsAreLeftThenRight)
    {
        const auto modulators = node.get_modulators();

        ASSERT_EQ(modulators.size(), 2U);
        EXPECT_EQ(modulators[0].first, ModulatorRole::Lhs);
        EXPECT_EQ(modulators[0].second, lhs);
        EXPECT_EQ(modulators[1].first, ModulatorRole::Rhs);
        EXPECT_EQ(modulators[1].second, rhs);
    }

    INSTANTIATE_TEST_SUITE_P(
        Functions,
        ConduitBinaryOpTest,
        ::testing::Values(
            BinaryCase { "Add", [](double a, double b) { return a + b; }, 1.5, 2.0, 3.5 },
            BinaryCase { "Subtract", [](double a, double b) { return a - b; }, 1.5, 2.0, -0.5 },
            BinaryCase { "Multiply", [](double a, double b) { return a * b; }, 3.0, -2.0, -6.0 },
            BinaryCase { "Max", [](double a, double b) { return std::max(a, b); }, -1.0, -3.0, -1.0 },
            BinaryCase { "Left", [](double a, double) { return a; }, 4.0, 9.0, 4.0 }),
        [](const ::testing::TestParamInfo<BinaryCase>& info) { return std::string(info.param.name); });

    class ConduitBinaryOpBehaviourTest : public ::testing::Test {
    protected:
        static double add(double a, double b) { return a + b; }

        std::shared_ptr<Constant> lhs { std::make_shared<Constant>(1.0) };
        std::shared_ptr<Constant> rhs { std::make_shared<Constant>(2.0) };
    };

    TEST_F(ConduitBinaryOpBehaviourTest, RejectsNullInputs)
    {
        EXPECT_THROW(BinaryOpNode(nullptr, rhs, add), std::invalid_argument);
        EXPECT_THROW(BinaryOpNode(lhs, nullptr, add), std::invalid_argument);
    }

    TEST_F(ConduitBinaryOpBehaviourTest, AlreadyProcessedInputsAddTheCallersInput)
    {
        BinaryOpNode node(lhs, rhs, add);
        Nodes::atomic_add_flag(lhs->m_state, NodeState::PROCESSED);
        Nodes::atomic_add_flag(rhs->m_state, NodeState::PROCESSED);

        EXPECT_DOUBLE_EQ(node.process_sample(10.0), (10.0 + 1.0) + (10.0 + 2.0));
    }

    TEST_F(ConduitBinaryOpBehaviourTest, ResetClearsTheNodeAndItsInputs)
    {
        BinaryOpNode node(lhs, rhs, add);
        Nodes::atomic_add_flag(node.m_state, NodeState::PROCESSED);
        Nodes::atomic_add_flag(lhs->m_state, NodeState::PROCESSED);
        Nodes::atomic_add_flag(rhs->m_state, NodeState::PROCESSED);

        node.reset_processed_state();

        EXPECT_FALSE(has(node, NodeState::PROCESSED));
        EXPECT_FALSE(has(*lhs, NodeState::PROCESSED));
        EXPECT_FALSE(has(*rhs, NodeState::PROCESSED));
    }

    TEST_F(ConduitBinaryOpBehaviourTest, RestoreRewindsBothInputs)
    {
        auto make = [] {
            return std::make_shared<BinaryOpNode>(
                std::make_shared<Sine>(1000.0F, 1.0),
                std::make_shared<Sine>(3000.0F, 0.5),
                add);
        };
        auto a = make();
        auto b = make();
        run(*a, 4);
        run(*b, 4);
        const auto reference = run(*b, 6);

        a->save_state();
        run(*a, 6);
        a->restore_state();

        EXPECT_EQ(run(*a, 6), reference);
    }

    TEST_F(ConduitBinaryOpBehaviourTest, DirectInitialiseRegistersOnChannelZeroWhenInputsAreInNoRoot)
    {
        NodeGraphManager manager;
        auto node = std::make_shared<BinaryOpNode>(lhs, rhs, add, manager);

        node->initialize();
        manager.get_root_node(ProcessingToken::AUDIO_RATE, 0).process_sample();

        EXPECT_TRUE(in_root(manager.get_root_node(ProcessingToken::AUDIO_RATE, 0), node));
    }

    struct BinarySemanticsCase {
        const char* name;
        NodeBinaryOpSemantics semantics;
        bool keeps_inputs;
    };

    class ConduitBinaryOpManagerTest : public ::testing::TestWithParam<BinarySemanticsCase> {
    protected:
        void SetUp() override
        {
            Nodes::NodeConfig config;
            config.binary_op_semantics = GetParam().semantics;
            manager.set_node_config(config);

            manager.add_to_root(lhs, token, 0);
            manager.add_to_root(rhs, token, 1);
            root(0).process_sample();
            root(1).process_sample();
        }

        Nodes::RootNode& root(uint32_t channel) { return manager.get_root_node(token, channel); }

        NodeGraphManager manager;
        ProcessingToken token = ProcessingToken::AUDIO_RATE;
        std::shared_ptr<Constant> lhs { std::make_shared<Constant>(1.0) };
        std::shared_ptr<Constant> rhs { std::make_shared<Constant>(2.0) };
    };

    TEST_P(ConduitBinaryOpManagerTest, RegistersOnTheUnionOfItsInputsChannels)
    {
        auto node = std::make_shared<BinaryOpNode>(lhs, rhs, [](double a, double b) { return a + b; }, manager, token);

        node->process_sample(0.0);
        root(0).process_sample();
        root(1).process_sample();

        EXPECT_TRUE(in_root(root(0), node));
        EXPECT_TRUE(in_root(root(1), node));
    }

    TEST_P(ConduitBinaryOpManagerTest, InputsFollowTheBinaryOpSemantics)
    {
        auto node = std::make_shared<BinaryOpNode>(lhs, rhs, [](double a, double b) { return a + b; }, manager, token);

        node->process_sample(0.0);
        root(0).process_sample();
        root(1).process_sample();

        EXPECT_EQ(in_root(root(0), lhs), GetParam().keeps_inputs);
        EXPECT_EQ(in_root(root(1), rhs), GetParam().keeps_inputs);
    }

    INSTANTIATE_TEST_SUITE_P(
        Semantics,
        ConduitBinaryOpManagerTest,
        ::testing::Values(
            BinarySemanticsCase { "Replace", NodeBinaryOpSemantics::REPLACE, false },
            BinarySemanticsCase { "Keep", NodeBinaryOpSemantics::KEEP, true }),
        [](const ::testing::TestParamInfo<BinarySemanticsCase>& info) { return std::string(info.param.name); });

    double sum_of(std::span<const double> values)
    {
        return std::accumulate(values.begin(), values.end(), 0.0);
    }

    double largest_of(std::span<const double> values)
    {
        return *std::ranges::max_element(values);
    }

    std::vector<std::shared_ptr<Node>> constants(std::initializer_list<double> values)
    {
        std::vector<std::shared_ptr<Node>> nodes;
        nodes.reserve(values.size());
        for (const double value : values) {
            nodes.push_back(std::make_shared<Constant>(value));
        }
        return nodes;
    }

    TEST(ConduitCompositeTest, CombinesEveryInputThroughTheFunction)
    {
        CompositeOpNode<> sum(constants({ 1.0, 2.0, 3.0, 4.0 }), sum_of);
        CompositeOpNode<> largest(constants({ 1.0, 7.0, 3.0 }), largest_of);

        EXPECT_DOUBLE_EQ(sum.process_sample(0.0), 10.0);
        EXPECT_DOUBLE_EQ(largest.process_sample(0.0), 7.0);
    }

    TEST(ConduitCompositeTest, InputsArePassedInOrder)
    {
        std::vector<double> seen;
        CompositeOpNode<3> node(constants({ 5.0, 6.0, 7.0 }), [&seen](std::span<const double> values) {
            seen.assign(values.begin(), values.end());
            return 0.0;
        });

        node.process_sample(0.0);

        EXPECT_EQ(seen, (std::vector<double> { 5.0, 6.0, 7.0 }));
    }

    TEST(ConduitCompositeTest, ReportsItsInputs)
    {
        auto nodes = constants({ 1.0, 2.0, 3.0 });
        CompositeOpNode<> node(nodes, sum_of);

        EXPECT_EQ(node.input_count(), 3U);
        EXPECT_EQ(node.inputs(), nodes);
    }

    TEST(ConduitCompositeTest, FixedSizeRejectsAMismatchedInputCount)
    {
        EXPECT_THROW(CompositeOpNode<3>(constants({ 1.0, 2.0 }), sum_of), std::invalid_argument);
        EXPECT_THROW(CompositeOpNode<2>(constants({ 1.0, 2.0, 3.0 }), sum_of), std::invalid_argument);
        EXPECT_NO_THROW(CompositeOpNode<3>(constants({ 1.0, 2.0, 3.0 }), sum_of));
    }

    TEST(ConduitCompositeTest, RequiresAtLeastTwoInputs)
    {
        EXPECT_THROW(CompositeOpNode<>(constants({ 1.0 }), sum_of), std::invalid_argument);
        EXPECT_THROW(CompositeOpNode<>(constants({}), sum_of), std::invalid_argument);
    }

    TEST(ConduitCompositeTest, RejectsNullInputs)
    {
        auto nodes = constants({ 1.0, 2.0 });
        nodes.push_back(nullptr);

        EXPECT_THROW(CompositeOpNode<>(nodes, sum_of), std::invalid_argument);
    }

    TEST(ConduitCompositeTest, ContextCarriesEveryInputValue)
    {
        CompositeOpNode<> node(constants({ 1.0, 2.0, 3.0 }), sum_of);
        std::vector<double> inputs;
        double value = 0.0;
        node.on_tick([&](NodeContext& ctx) {
            const auto* composite = dynamic_cast<const CompositeOpContext*>(&ctx);
            ASSERT_NE(composite, nullptr);
            inputs = composite->input_values;
            value = composite->value;
        });

        node.process_sample(0.0);

        EXPECT_EQ(inputs, (std::vector<double> { 1.0, 2.0, 3.0 }));
        EXPECT_DOUBLE_EQ(value, 6.0);
    }

    TEST(ConduitCompositeTest, ModulatorsAreSignalInputsInOrder)
    {
        auto nodes = constants({ 1.0, 2.0, 3.0 });
        CompositeOpNode<> node(nodes, sum_of);

        const auto modulators = node.get_modulators();

        ASSERT_EQ(modulators.size(), 3U);
        for (size_t i = 0; i < modulators.size(); ++i) {
            EXPECT_EQ(modulators[i].first, ModulatorRole::SignalMod);
            EXPECT_EQ(modulators[i].second, nodes[i]);
        }
    }

    TEST(ConduitCompositeTest, AlreadyProcessedInputsAddTheCallersInput)
    {
        auto nodes = constants({ 1.0, 2.0 });
        CompositeOpNode<> node(nodes, sum_of);
        Nodes::atomic_add_flag(nodes[1]->m_state, NodeState::PROCESSED);

        EXPECT_DOUBLE_EQ(node.process_sample(10.0), 1.0 + (10.0 + 2.0));
    }

    TEST(ConduitCompositeTest, ProcessingLeavesTheInputsBalancedAndCleared)
    {
        auto nodes = constants({ 1.0, 2.0, 3.0 });
        CompositeOpNode<> node(nodes, sum_of);

        run(node, 3);

        for (const auto& input : nodes) {
            EXPECT_EQ(input->m_modulator_count.load(), 0U);
            EXPECT_FALSE(has(*input, NodeState::PROCESSED));
        }
    }

    TEST(ConduitCompositeTest, ResetClearsTheNodeAndItsInputs)
    {
        auto nodes = constants({ 1.0, 2.0 });
        CompositeOpNode<> node(nodes, sum_of);
        Nodes::atomic_add_flag(node.m_state, NodeState::PROCESSED);
        for (const auto& input : nodes) {
            Nodes::atomic_add_flag(input->m_state, NodeState::PROCESSED);
        }

        node.reset_processed_state();

        EXPECT_FALSE(has(node, NodeState::PROCESSED));
        for (const auto& input : nodes) {
            EXPECT_FALSE(has(*input, NodeState::PROCESSED));
        }
    }

    TEST(ConduitCompositeTest, TicksFireOncePerSampleAndStopWhileSaved)
    {
        CompositeOpNode<> node(constants({ 1.0, 2.0 }), sum_of);
        int ticks = 0;
        node.on_tick([&ticks](NodeContext&) { ++ticks; });

        run(node, 3);
        EXPECT_EQ(ticks, 3);

        node.save_state();
        run(node, 3);
        EXPECT_EQ(ticks, 3);

        node.restore_state();
        run(node, 3);
        EXPECT_EQ(ticks, 6);
    }

    TEST(ConduitCompositeTest, RestoreRewindsEveryInput)
    {
        auto make = [] {
            std::vector<std::shared_ptr<Node>> nodes {
                std::make_shared<Sine>(1000.0F, 1.0),
                std::make_shared<Sine>(3000.0F, 0.5),
                std::make_shared<Sine>(500.0F, 0.25)
            };
            return std::make_shared<CompositeOpNode<>>(nodes, sum_of);
        };
        auto a = make();
        auto b = make();
        run(*a, 4);
        run(*b, 4);
        const auto reference = run(*b, 6);

        a->save_state();
        run(*a, 6);
        a->restore_state();

        EXPECT_EQ(run(*a, 6), reference);
    }

    TEST(ConduitCompositeTest, BatchMatchesConsecutiveSamples)
    {
        CompositeOpNode<> first(constants({ 1.0, 2.0 }), sum_of);
        CompositeOpNode<> second(constants({ 1.0, 2.0 }), sum_of);

        EXPECT_EQ(first.process_batch(6), run(second, 6));
    }

    class ConduitCompositeManagerTest : public ::testing::TestWithParam<BinarySemanticsCase> {
    protected:
        void SetUp() override
        {
            Nodes::NodeConfig config;
            config.binary_op_semantics = GetParam().semantics;
            manager.set_node_config(config);

            inputs = constants({ 1.0, 2.0, 3.0 });
            manager.add_to_root(inputs[0], token, 0);
            manager.add_to_root(inputs[1], token, 1);
            root(0).process_sample();
            root(1).process_sample();
        }

        Nodes::RootNode& root(uint32_t channel) { return manager.get_root_node(token, channel); }

        NodeGraphManager manager;
        ProcessingToken token = ProcessingToken::AUDIO_RATE;
        std::vector<std::shared_ptr<Node>> inputs;
    };

    TEST_P(ConduitCompositeManagerTest, RegistersOnTheUnionOfItsInputsChannels)
    {
        auto node = std::make_shared<CompositeOpNode<>>(inputs, sum_of, manager, token);

        node->process_sample(0.0);
        root(0).process_sample();
        root(1).process_sample();

        EXPECT_TRUE(in_root(root(0), node));
        EXPECT_TRUE(in_root(root(1), node));
    }

    TEST_P(ConduitCompositeManagerTest, InputsFollowTheBinaryOpSemantics)
    {
        auto node = std::make_shared<CompositeOpNode<>>(inputs, sum_of, manager, token);

        node->process_sample(0.0);
        root(0).process_sample();
        root(1).process_sample();

        EXPECT_EQ(in_root(root(0), inputs[0]), GetParam().keeps_inputs);
        EXPECT_EQ(in_root(root(1), inputs[1]), GetParam().keeps_inputs);
    }

    TEST_P(ConduitCompositeManagerTest, InputsOutsideEveryRootStillRegisterTheNodeOnChannelZero)
    {
        NodeGraphManager other;
        auto node = std::make_shared<CompositeOpNode<>>(constants({ 1.0, 2.0 }), sum_of, other, token);

        node->process_sample(0.0);
        other.get_root_node(token, 0).process_sample();

        EXPECT_TRUE(in_root(other.get_root_node(token, 0), node));
    }

    INSTANTIATE_TEST_SUITE_P(
        Semantics,
        ConduitCompositeManagerTest,
        ::testing::Values(
            BinarySemanticsCase { "Replace", NodeBinaryOpSemantics::REPLACE, false },
            BinarySemanticsCase { "Keep", NodeBinaryOpSemantics::KEEP, true }),
        [](const ::testing::TestParamInfo<BinarySemanticsCase>& info) { return std::string(info.param.name); });

}
}
