#include "gtest/gtest.h"

#include "MayaFlux/Buffers/BufferUtils.hpp"
#include "MayaFlux/Nodes/Conduit/Constant.hpp"
#include "MayaFlux/Nodes/Generators/Sine.hpp"
#include "MayaFlux/Nodes/Network/NodeNetwork.hpp"
#include "MayaFlux/Nodes/RootNode.hpp"

namespace MayaFlux::Test {
namespace {

    using Nodes::Generator::Sine;
    using Nodes::Node;
    using Nodes::NodeContext;
    using Nodes::NodeState;

    std::vector<double> run(Node& node, size_t count)
    {
        std::vector<double> samples;
        samples.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            samples.push_back(node.process_sample(0.0));
        }
        return samples;
    }

    class SinePairTest {
    protected:
        void prime()
        {
            a->process_sample(0.0);
            b->process_sample(0.0);
        }

        std::shared_ptr<Sine> a { std::make_shared<Sine>(440.0F, 0.5) };
        std::shared_ptr<Sine> b { std::make_shared<Sine>(440.0F, 0.5) };
    };

    struct ExtractCase {
        const char* name;
        NodeState state;
        uint32_t buffers;
        bool consumes;
    };

    class NodeExtractionTest : public SinePairTest, public ::testing::TestWithParam<ExtractCase> {
    protected:
        void SetUp() override
        {
            prime();
            Nodes::atomic_set_flag_strong(a->m_state, GetParam().state);
            for (uint32_t i = 0; i < GetParam().buffers; ++i) {
                a->add_buffer_reference();
            }
            reference = run(*b, 16);
        }

        [[nodiscard]] size_t offset() const { return GetParam().consumes ? 1 : 0; }

        std::vector<double> reference;
    };

    TEST_P(NodeExtractionTest, SingleSampleIsTheNodesNextSample)
    {
        EXPECT_DOUBLE_EQ(Buffers::extract_single_sample(a), reference[0]);
    }

    TEST_P(NodeExtractionTest, ExtractionConsumesOrPreservesTheNodeAsExpected)
    {
        Buffers::extract_single_sample(a);

        const auto following = run(*a, 3);

        for (size_t i = 0; i < following.size(); ++i) {
            EXPECT_DOUBLE_EQ(following[i], reference[offset() + i]) << "sample " << i;
        }
    }

    TEST_P(NodeExtractionTest, RepeatedSingleExtractionsFollowTheNodeOnlyWhenItIsConsumed)
    {
        for (size_t i = 0; i < 4; ++i) {
            const double expected = GetParam().consumes ? reference[i] : reference[0];
            EXPECT_DOUBLE_EQ(Buffers::extract_single_sample(a), expected) << "extraction " << i;
        }
    }

    TEST_P(NodeExtractionTest, MultipleSamplesMatchTheSequenceAndConsumeOrPreserve)
    {
        const auto extracted = Buffers::extract_multiple_samples(a, 8);

        ASSERT_EQ(extracted.size(), 8U);
        for (size_t i = 0; i < extracted.size(); ++i) {
            EXPECT_DOUBLE_EQ(extracted[i], reference[i]) << "sample " << i;
        }

        const auto following = run(*a, 3);
        const size_t skipped = GetParam().consumes ? 8 : 0;
        for (size_t i = 0; i < following.size(); ++i) {
            EXPECT_DOUBLE_EQ(following[i], reference[skipped + i]) << "sample " << i;
        }
    }

    TEST_P(NodeExtractionTest, NetworkExtractionMatchesTheSequenceAndConsumeOrPreserve)
    {
        std::vector<double> buffer;
        size_t position = 5;

        Nodes::Network::extract_node_samples(a, buffer, position, 8);

        EXPECT_EQ(position, 0U);
        ASSERT_EQ(buffer.size(), 8U);
        for (size_t i = 0; i < buffer.size(); ++i) {
            EXPECT_DOUBLE_EQ(buffer[i], reference[i]) << "sample " << i;
        }

        const auto following = run(*a, 3);
        const size_t skipped = GetParam().consumes ? 8 : 0;
        for (size_t i = 0; i < following.size(); ++i) {
            EXPECT_DOUBLE_EQ(following[i], reference[skipped + i]) << "sample " << i;
        }
    }

    TEST_P(NodeExtractionTest, TicksFireOnlyWhenTheNodeIsConsumed)
    {
        int ticks = 0;
        a->on_tick([&ticks](NodeContext&) { ++ticks; });

        Buffers::extract_single_sample(a);

        EXPECT_EQ(ticks, GetParam().consumes ? 1 : 0);
    }

    TEST_P(NodeExtractionTest, ExtractionLeavesTheStateFlagsAlone)
    {
        Buffers::extract_single_sample(a);
        Buffers::extract_multiple_samples(a, 4);

        EXPECT_EQ(a->m_state.load(), GetParam().state);
    }

    TEST_P(NodeExtractionTest, ClaimIsReleasedAfterEveryExtraction)
    {
        Buffers::extract_single_sample(a);
        EXPECT_FALSE(a->has_active_snapshot());

        Buffers::extract_multiple_samples(a, 4);
        EXPECT_FALSE(a->has_active_snapshot());

        std::vector<double> buffer;
        size_t position = 0;
        Nodes::Network::extract_node_samples(a, buffer, position, 4);
        EXPECT_FALSE(a->has_active_snapshot());
    }

    INSTANTIATE_TEST_SUITE_P(
        States,
        NodeExtractionTest,
        ::testing::Values(
            ExtractCase { "InactiveWithoutBuffers", NodeState::INACTIVE, 0, true },
            ExtractCase { "InactiveOneBuffer", NodeState::INACTIVE, 1, true },
            ExtractCase { "Active", NodeState::ACTIVE, 1, false },
            ExtractCase { "ActiveTwoBuffers", NodeState::ACTIVE, 2, false },
            ExtractCase { "ActiveProcessed", NodeState::ENGINE_PROCESSED, 1, false },
            ExtractCase { "InactiveProcessed", NodeState::PROCESSED, 1, false },
            ExtractCase { "InactiveOutputExcluded", NodeState::OUTPUT_EXCLUDED, 1, false }),
        [](const ::testing::TestParamInfo<ExtractCase>& info) { return std::string(info.param.name); });

    class NodeSharedBufferExtractionTest : public SinePairTest, public ::testing::TestWithParam<uint32_t> {
    protected:
        void SetUp() override
        {
            prime();
            for (uint32_t i = 0; i < GetParam(); ++i) {
                a->add_buffer_reference();
            }
            reference = run(*b, 8);
        }

        std::vector<double> reference;
    };

    TEST_P(NodeSharedBufferExtractionTest, FirstBufferConsumesAndTheRestPreviewTheNextSample)
    {
        EXPECT_DOUBLE_EQ(Buffers::extract_single_sample(a), reference[0]);
        EXPECT_TRUE(a->is_buffer_processed());

        for (uint32_t i = 1; i < GetParam(); ++i) {
            EXPECT_DOUBLE_EQ(Buffers::extract_single_sample(a), reference[1]) << "buffer " << i;
        }
    }

    TEST_P(NodeSharedBufferExtractionTest, NodeAdvancesOncePerCycleWhateverTheBufferCount)
    {
        for (uint32_t i = 0; i < GetParam(); ++i) {
            Buffers::extract_single_sample(a);
        }

        EXPECT_DOUBLE_EQ(a->process_sample(0.0), reference[1]);
    }

    TEST_P(NodeSharedBufferExtractionTest, FlagClearsOnceEveryBufferHasRead)
    {
        for (uint32_t i = 0; i < GetParam(); ++i) {
            Buffers::extract_single_sample(a);
        }

        EXPECT_FALSE(a->is_buffer_processed());
    }

    INSTANTIATE_TEST_SUITE_P(
        Buffers,
        NodeSharedBufferExtractionTest,
        ::testing::Values(2U, 3U, 4U));

    class NodeSaveRestoreTest : public SinePairTest, public ::testing::Test {
    protected:
        void SetUp() override { prime(); }
    };

    TEST_F(NodeSaveRestoreTest, RestoreRewindsTheNodeToTheSave)
    {
        const auto reference = run(*b, 5);

        a->save_state();
        run(*a, 5);
        a->restore_state();

        EXPECT_EQ(run(*a, 5), reference);
    }

    TEST_F(NodeSaveRestoreTest, SaveAndRestoreRepeatCleanly)
    {
        const auto reference = run(*b, 4);

        for (int round = 0; round < 3; ++round) {
            a->save_state();
            run(*a, 4);
            a->restore_state();
        }

        EXPECT_EQ(run(*a, 4), reference);
    }

    TEST_F(NodeSaveRestoreTest, RestoreRewindsTheLastOutputToo)
    {
        const double before = a->get_last_output();

        a->save_state();
        run(*a, 3);
        a->restore_state();

        EXPECT_DOUBLE_EQ(a->get_last_output(), before);
    }

    TEST_F(NodeSaveRestoreTest, SaveCascadesToTheModulators)
    {
        auto modulator_a = std::make_shared<Sine>(5.0F, 0.5);
        auto modulator_b = std::make_shared<Sine>(5.0F, 0.5);
        auto modulated_a = std::make_shared<Sine>(modulator_a, 880.0F, 0.5);
        auto modulated_b = std::make_shared<Sine>(modulator_b, 880.0F, 0.5);
        run(*modulated_a, 2);
        run(*modulated_b, 2);
        const auto reference = run(*modulated_b, 6);

        modulated_a->save_state();
        run(*modulated_a, 6);
        modulated_a->restore_state();

        EXPECT_EQ(run(*modulated_a, 6), reference);
    }

    TEST_F(NodeSaveRestoreTest, TicksAreSuppressedWhileSavedAndResumeAfterRestore)
    {
        int ticks = 0;
        a->on_tick([&ticks](NodeContext&) { ++ticks; });

        a->save_state();
        run(*a, 3);
        EXPECT_EQ(ticks, 0);

        a->restore_state();
        run(*a, 3);
        EXPECT_EQ(ticks, 3);
    }

    TEST_F(NodeSaveRestoreTest, TicksFireWhileSavedWhenSnapshotEventsAreEnabled)
    {
        int ticks = 0;
        a->on_tick([&ticks](NodeContext&) { ++ticks; });
        a->m_fire_events_during_snapshot = true;

        a->save_state();
        run(*a, 3);
        a->restore_state();

        EXPECT_EQ(ticks, 3);
    }

    TEST_F(NodeSaveRestoreTest, NetworkedNodeNeverFiresTicks)
    {
        int ticks = 0;
        a->on_tick([&ticks](NodeContext&) { ++ticks; });
        a->set_in_network(true);

        run(*a, 3);
        EXPECT_EQ(ticks, 0);

        a->set_in_network(false);
        run(*a, 3);
        EXPECT_EQ(ticks, 3);
    }

    class NodeClaimClashTest : public SinePairTest, public ::testing::Test {
    protected:
        void SetUp() override { prime(); }
    };

    TEST_F(NodeClaimClashTest, ExtractionBetweenRootCyclesDoesNotDisturbTheRootOutput)
    {
        Nodes::RootNode first;
        Nodes::RootNode second;
        a->register_channel_usage(0);
        b->register_channel_usage(0);
        first.register_node(a);
        second.register_node(b);

        for (int cycle = 0; cycle < 12; ++cycle) {
            EXPECT_DOUBLE_EQ(first.process_sample(), second.process_sample()) << "cycle " << cycle;
            if (cycle == 2 || cycle == 6 || cycle == 9) {
                Buffers::extract_single_sample(a);
                Buffers::extract_multiple_samples(a, 5);
                std::vector<double> buffer;
                size_t position = 0;
                Nodes::Network::extract_node_samples(a, buffer, position, 5);
            }
        }
    }

    TEST_F(NodeClaimClashTest, ExtractionFromARootNodeDoesNotMarkItProcessed)
    {
        Nodes::RootNode root;
        a->register_channel_usage(0);
        root.register_node(a);
        root.process_sample();
        ASSERT_FALSE((a->m_state.load() & NodeState::PROCESSED) != 0);

        Buffers::extract_single_sample(a);

        EXPECT_FALSE((a->m_state.load() & NodeState::PROCESSED) != 0);
    }

    TEST_F(NodeClaimClashTest, ContendedNodeTimesOutAndIsLeftUntouched)
    {
        Nodes::atomic_set_flag_strong(a->m_state, NodeState::ACTIVE);
        const auto reference = run(*b, 4);
        ASSERT_TRUE(a->try_claim_snapshot_context(777));

        EXPECT_DOUBLE_EQ(Buffers::extract_single_sample(a), 0.0);

        EXPECT_TRUE(a->is_in_snapshot_context(777));
        a->release_snapshot_context(777);
        EXPECT_EQ(run(*a, 4), reference);
    }

    TEST_F(NodeClaimClashTest, ConcurrentExtractionsLeaveAnActiveNodeUnchanged)
    {
        Nodes::atomic_set_flag_strong(a->m_state, NodeState::ACTIVE);
        const auto reference = run(*b, 4);

        std::vector<std::thread> threads;
        threads.reserve(4);
        for (int t = 0; t < 4; ++t) {
            threads.emplace_back([this] {
                for (int i = 0; i < 200; ++i) {
                    Buffers::extract_single_sample(a);
                }
            });
        }
        for (auto& thread : threads) {
            thread.join();
        }

        EXPECT_FALSE(a->has_active_snapshot());
        EXPECT_EQ(run(*a, 4), reference);
    }

    TEST_F(NodeClaimClashTest, ThrowingNodeReleasesTheClaimAndYieldsSilence)
    {
        Nodes::atomic_set_flag_strong(a->m_state, NodeState::ACTIVE);
        a->m_fire_events_during_snapshot = true;
        a->on_tick([](NodeContext&) { throw std::runtime_error("process failed"); });

        EXPECT_DOUBLE_EQ(Buffers::extract_single_sample(a), 0.0);
        EXPECT_FALSE(a->has_active_snapshot());

        EXPECT_TRUE(Buffers::extract_multiple_samples(a, 4).empty());
        EXPECT_FALSE(a->has_active_snapshot());

        std::vector<double> buffer;
        size_t position = 3;
        Nodes::Network::extract_node_samples(a, buffer, position, 4);
        EXPECT_FALSE(a->has_active_snapshot());
        ASSERT_EQ(buffer.size(), 4U);
        for (const double sample : buffer) {
            EXPECT_DOUBLE_EQ(sample, 0.0);
        }
    }

    TEST_F(NodeClaimClashTest, NullNodeYieldsSilence)
    {
        EXPECT_DOUBLE_EQ(Buffers::extract_single_sample(nullptr), 0.0);

        std::vector<double> buffer { 1.0, 2.0 };
        size_t position = 9;
        Nodes::Network::extract_node_samples(nullptr, buffer, position, 4);

        EXPECT_EQ(position, 0U);
        ASSERT_EQ(buffer.size(), 4U);
        for (const double sample : buffer) {
            EXPECT_DOUBLE_EQ(sample, 0.0);
        }
    }

}
}
