#include "gtest/gtest.h"

#include "MayaFlux/Nodes/Network/MeshNetwork.hpp"
#include "MayaFlux/Nodes/Network/Operators/MeshFieldOperator.hpp"
#include "MayaFlux/Nodes/Network/Operators/MeshTransformOperator.hpp"
#include "MayaFlux/Nodes/Network/Operators/OperatorChain.hpp"

namespace MayaFlux::Test {

namespace {

    using Nodes::MeshVertex;
    using Nodes::GpuSync::MeshWriterNode;
    using Nodes::Network::MeshFieldOperator;
    using Nodes::Network::MeshNetwork;
    using Nodes::Network::MeshOperator;
    using Nodes::Network::MeshSlot;
    using Nodes::Network::MeshTransformOperator;
    using Nodes::Network::OutputMode;
    using Nodes::Network::Topology;

    glm::mat4 translation(float x, float y, float z)
    {
        glm::mat4 m(1.0F);
        m[3] = glm::vec4(x, y, z, 1.0F);
        return m;
    }

    glm::vec3 origin(const glm::mat4& m)
    {
        return glm::vec3(m[3]);
    }

    std::shared_ptr<MeshWriterNode> quad_node(uint32_t vertex_count = 4)
    {
        auto node = std::make_shared<MeshWriterNode>(16);
        std::vector<MeshVertex> v(vertex_count);
        for (size_t i = 0; i < v.size(); ++i) {
            v[i].position = { static_cast<float>(i), 0.0F, 0.0F };
        }
        node->set_mesh(v, std::vector<uint32_t> { 0, 1, 2, 2, 3, 0 });
        node->compute_frame();
        return node;
    }

    class MeshFrameCounter : public MeshOperator {
    public:
        void process_slot(MeshSlot&, float) override { ++slot_calls; }

        void process(float dt) override
        {
            ++frames;
            last_dt = dt;
            MeshOperator::process(dt);
        }

        [[nodiscard]] std::string_view get_type_name() const override { return "MeshFrameCounter"; }

        int frames {};
        int slot_calls {};
        float last_dt {};
    };

    TEST(MeshNetworkTest, StartsEmptyAndBoundToGraphics)
    {
        MeshNetwork net;

        EXPECT_EQ(net.slot_count(), 0U);
        EXPECT_EQ(net.get_node_count(), 0U);
        EXPECT_FALSE(net.has_operator());
        EXPECT_EQ(net.get_output_mode(), OutputMode::GRAPHICS_BIND);
        EXPECT_EQ(net.get_topology(), Topology::CUSTOM);
        ASSERT_NE(net.get_operator_chain(), nullptr);
        EXPECT_TRUE(net.get_operator_chain()->empty());
    }

    TEST(MeshNetworkTest, AddSlotReturnsSequentialIndicesAndRecordsTheParent)
    {
        MeshNetwork net;
        auto body = quad_node();
        auto arm = quad_node();

        EXPECT_EQ(net.add_slot("body", body), 0U);
        EXPECT_EQ(net.add_slot("arm", arm, 0U), 1U);

        EXPECT_EQ(net.slot_count(), 2U);
        EXPECT_EQ(net.get_slot(0).node, body);
        EXPECT_EQ(net.get_slot(1).name, "arm");
        EXPECT_EQ(net.get_slot(1).parent_index, 0U);
        EXPECT_FALSE(net.get_slot(0).parent_index.has_value());
        EXPECT_EQ(net.get_slot(0).child_indices, std::vector<uint32_t> { 1U });
        EXPECT_TRUE(net.get_slot(1).dirty);
    }

    TEST(MeshNetworkTest, NullNodeAndMissingParentAreRejected)
    {
        MeshNetwork net;
        net.add_slot("body", quad_node());

        EXPECT_THROW(net.add_slot("x", nullptr), std::invalid_argument);
        EXPECT_THROW(net.add_slot("x", quad_node(), 7U), std::out_of_range);
        EXPECT_EQ(net.slot_count(), 1U);
        EXPECT_TRUE(net.get_slot(0).child_indices.empty());
    }

    TEST(MeshNetworkTest, SlotLookup)
    {
        MeshNetwork net;
        net.add_slot("a", quad_node());
        net.add_slot("b", quad_node());

        EXPECT_THROW((void)net.get_slot(2), std::out_of_range);
        ASSERT_NE(net.find_slot("b"), nullptr);
        EXPECT_EQ(net.find_slot("b")->index, 1U);
        EXPECT_EQ(net.find_slot("missing"), nullptr);
        EXPECT_EQ(net.find_slot_index("a"), 0U);
        EXPECT_FALSE(net.find_slot_index("missing").has_value());
        EXPECT_EQ(std::as_const(net).find_slot("a")->index, 0U);
    }

    TEST(MeshNetworkTest, SortPutsEveryParentBeforeItsChildren)
    {
        MeshNetwork net;
        net.add_slot("r0", quad_node());
        net.add_slot("c0", quad_node(), 0U);
        net.add_slot("r1", quad_node());
        net.add_slot("c1", quad_node(), 1U);
        net.add_slot("c2", quad_node(), 2U);

        net.ensure_sorted();

        const auto& order = net.sorted_indices();
        ASSERT_EQ(order.size(), 5U);
        auto position_of = [&order](uint32_t index) {
            return static_cast<size_t>(std::ranges::find(order, index) - order.begin());
        };
        for (uint32_t i = 0; i < 5; ++i) {
            ASSERT_NE(std::ranges::find(order, i), order.end()) << "slot " << i;
            if (auto parent = net.get_slot(i).parent_index) {
                EXPECT_LT(position_of(*parent), position_of(i)) << "slot " << i;
            }
        }
    }

    TEST(MeshNetworkTest, ASlotAddedLaterJoinsTheNextBatchsOrder)
    {
        MeshNetwork net;
        net.add_slot("a", quad_node());
        net.process_batch(1);
        ASSERT_EQ(net.sorted_indices().size(), 1U);

        net.add_slot("b", quad_node(), 0U);
        net.process_batch(1);

        EXPECT_EQ(net.sorted_indices().size(), 2U);
    }

    TEST(MeshNetworkTest, WorldTransformsComposeDownTheHierarchy)
    {
        MeshNetwork net;
        net.add_slot("root", quad_node());
        net.add_slot("child", quad_node(), 0U);
        net.add_slot("grandchild", quad_node(), 1U);
        net.get_slot(0).local_transform = translation(1.0F, 0.0F, 0.0F);
        net.get_slot(1).local_transform = translation(0.0F, 2.0F, 0.0F);
        net.get_slot(2).local_transform = translation(0.0F, 0.0F, 3.0F);

        net.process_batch(1);

        EXPECT_EQ(origin(net.get_slot(0).world_transform), glm::vec3(1.0F, 0.0F, 0.0F));
        EXPECT_EQ(origin(net.get_slot(1).world_transform), glm::vec3(1.0F, 2.0F, 0.0F));
        EXPECT_EQ(origin(net.get_slot(2).world_transform), glm::vec3(1.0F, 2.0F, 3.0F));
    }

    TEST(MeshNetworkTest, WorldTransformsAlsoPropagateWhenOnlyTheChainHasOperators)
    {
        MeshNetwork net;
        net.add_slot("root", quad_node());
        net.add_slot("child", quad_node(), 0U);
        net.get_slot(0).local_transform = translation(1.0F, 0.0F, 0.0F);
        net.get_slot(1).local_transform = translation(0.0F, 2.0F, 0.0F);
        net.get_operator_chain()->emplace<MeshFieldOperator>();

        net.process_batch(1);

        EXPECT_EQ(origin(net.get_slot(1).world_transform), glm::vec3(1.0F, 2.0F, 0.0F));
    }

    TEST(MeshNetworkTest, WorldTransformsPropagateWithAFieldOperatorAsPrimary)
    {
        MeshNetwork net;
        net.add_slot("root", quad_node());
        net.add_slot("child", quad_node(), 0U);
        net.get_slot(0).local_transform = translation(1.0F, 0.0F, 0.0F);
        net.get_slot(1).local_transform = translation(0.0F, 2.0F, 0.0F);
        net.create_operator<MeshFieldOperator>();

        net.process_batch(1);

        EXPECT_EQ(origin(net.get_slot(1).world_transform), glm::vec3(1.0F, 2.0F, 0.0F));
    }

    TEST(MeshNetworkTest, ATransformOperatorAsPrimaryDrivesLocalAndWorld)
    {
        MeshNetwork net;
        net.add_slot("root", quad_node());
        net.add_slot("child", quad_node(), 0U);
        net.get_slot(1).local_transform = translation(0.0F, 2.0F, 0.0F);
        auto op = net.create_operator<MeshTransformOperator>();
        op->bind(0, [](float) { return translation(5.0F, 0.0F, 0.0F); });

        net.process_batch(1);

        EXPECT_EQ(origin(net.get_slot(0).local_transform), glm::vec3(5.0F, 0.0F, 0.0F));
        EXPECT_EQ(origin(net.get_slot(0).world_transform), glm::vec3(5.0F, 0.0F, 0.0F));
        EXPECT_EQ(origin(net.get_slot(1).world_transform), glm::vec3(5.0F, 2.0F, 0.0F));
    }

    TEST(MeshNetworkTest, PrimaryAndChainOperatorsSeeEverySlotOncePerFrame)
    {
        MeshNetwork net;
        net.add_slot("a", quad_node());
        net.add_slot("b", quad_node(), 0U);
        auto primary = net.create_operator<MeshFrameCounter>();
        auto chained = net.get_operator_chain()->emplace<MeshFrameCounter>();

        net.process_batch(3);

        EXPECT_EQ(primary->frames, 3);
        EXPECT_EQ(primary->slot_calls, 6);
        EXPECT_EQ(chained->frames, 3);
        EXPECT_EQ(chained->slot_calls, 6);
    }

    TEST(MeshNetworkTest, DisabledNetworkOrNoSlotsRunsNothing)
    {
        MeshNetwork empty;
        auto idle = empty.create_operator<MeshFrameCounter>();
        empty.process_batch(2);
        EXPECT_EQ(idle->frames, 0);

        MeshNetwork net;
        net.add_slot("a", quad_node());
        auto counter = net.create_operator<MeshFrameCounter>();
        net.set_enabled(false);
        net.process_batch(2);
        EXPECT_EQ(counter->frames, 0);
    }

    TEST(MeshNetworkTest, ProcessBatchRefreshesEverySlotNode)
    {
        MeshNetwork net;
        auto node = quad_node(4);
        net.add_slot("a", node);
        std::vector<MeshVertex> bigger(6);
        node->set_mesh_vertices(bigger);

        net.process_batch(1);

        EXPECT_EQ(node->get_vertex_count(), 6U);
    }

    TEST(MeshNetworkTest, OperatorsReceiveTheFramePeriodAsDt)
    {
        MeshNetwork net;
        net.add_slot("a", quad_node());
        auto counter = net.create_operator<MeshFrameCounter>();

        net.process_batch(2);
        EXPECT_NEAR(counter->last_dt, 1.0F / 60.0F, 1e-6F);

        net.set_frame_rate(50);
        net.process_batch(1);
        EXPECT_NEAR(counter->last_dt, 0.02F, 1e-6F);
    }

    TEST(MeshNetworkTest, ATimestepOverridesTheFramePeriodUntilCleared)
    {
        MeshNetwork net;
        net.add_slot("a", quad_node());
        auto counter = net.create_operator<MeshFrameCounter>();

        net.set_timestep(0.5F);
        net.process_batch(1);
        EXPECT_FLOAT_EQ(counter->last_dt, 0.5F);

        net.clear_timestep();
        net.process_batch(1);
        EXPECT_NEAR(counter->last_dt, 1.0F / 60.0F, 1e-6F);
    }

    TEST(MeshNetworkTest, ChainOperatorsReceiveTheSameDt)
    {
        MeshNetwork net;
        net.add_slot("a", quad_node());
        auto chained = net.get_operator_chain()->emplace<MeshFrameCounter>();
        net.set_timestep(0.25F);

        net.process_batch(1);

        EXPECT_FLOAT_EQ(chained->last_dt, 0.25F);
    }

    TEST(MeshNetworkTest, ATransformOperatorFollowsTheNetworkTime)
    {
        MeshNetwork net;
        net.add_slot("a", quad_node());
        auto op = net.create_operator<MeshTransformOperator>();
        float seen = -1.0F;
        op->bind(0, [&seen](float t) {
            seen = t;
            return glm::mat4(1.0F);
        });
        net.set_timestep(0.1F);

        net.process_batch(3);

        EXPECT_NEAR(seen, 0.3F, 1e-5F);
    }

    TEST(MeshNetworkTest, ANullOperatorIsIgnoredAndKeepsTheCurrentOne)
    {
        MeshNetwork net;
        auto op = net.create_operator<MeshFrameCounter>();

        net.set_operator(nullptr);

        ASSERT_TRUE(net.has_operator());
        EXPECT_EQ(net.get_operator(), op.get());
    }

    TEST(MeshNetworkTest, ResetRestoresIdentityAndMarksEverySlotDirty)
    {
        MeshNetwork net;
        net.add_slot("a", quad_node());
        net.add_slot("b", quad_node(), 0U);
        for (auto& slot : net.slots()) {
            slot.local_transform = translation(1.0F, 2.0F, 3.0F);
            slot.world_transform = translation(4.0F, 5.0F, 6.0F);
            slot.dirty = false;
        }

        net.reset();

        for (const auto& slot : net.slots()) {
            EXPECT_EQ(slot.local_transform, glm::mat4(1.0F));
            EXPECT_EQ(slot.world_transform, glm::mat4(1.0F));
            EXPECT_TRUE(slot.dirty);
        }
    }

    TEST(MeshNetworkTest, NodeOutputIsTheSlotIndexWithinRange)
    {
        MeshNetwork net;
        net.add_slot("a", quad_node());

        EXPECT_EQ(net.get_node_output(0), 0.0);
        EXPECT_FALSE(net.get_node_output(1).has_value());
    }

    TEST(MeshNetworkTest, MetadataReportsSlotsOperatorAndChain)
    {
        MeshNetwork net;
        net.add_slot("a", quad_node());

        auto meta = net.get_metadata();
        EXPECT_EQ(meta.at("slot_count"), "1");
        EXPECT_EQ(meta.at("operator"), "none");
        EXPECT_EQ(meta.at("chain_size"), "0");

        net.create_operator<MeshTransformOperator>();
        net.get_operator_chain()->emplace<MeshFieldOperator>();

        meta = net.get_metadata();
        EXPECT_EQ(meta.at("operator"), "MeshTransform");
        EXPECT_EQ(meta.at("chain_size"), "1");
    }

    TEST(MeshNetworkTest, MeshDataMergesEverySlotAsASubmesh)
    {
        MeshNetwork net;
        net.add_slot("a", quad_node());
        net.add_slot("b", quad_node());

        const auto mesh = net.get_mesh_data();

        ASSERT_TRUE(mesh.has_value());
        ASSERT_TRUE(mesh->submeshes.has_value());
        EXPECT_EQ(mesh->submeshes->regions.size(), 2U);
    }

    TEST(MeshNetworkTest, MeshDataSkipsASlotWithoutANode)
    {
        MeshNetwork net;
        net.add_slot("a", quad_node());
        net.add_slot("b", quad_node());
        net.slots()[1].node.reset();

        const auto mesh = net.get_mesh_data();

        ASSERT_TRUE(mesh.has_value());
        ASSERT_TRUE(mesh->submeshes.has_value());
        EXPECT_EQ(mesh->submeshes->regions.size(), 1U);
    }

    TEST(MeshNetworkTest, MeshDataOfAnEmptyNetworkIsSafe)
    {
        MeshNetwork net;

        EXPECT_NO_THROW((void)net.get_mesh_data());
    }

    class MeshTransformOperatorTest : public ::testing::Test {
    protected:
        MeshTransformOperator op;
        MeshSlot slot;

        void SetUp() override { slot.index = 0; }
    };

    TEST_F(MeshTransformOperatorTest, AnUnboundSlotKeepsItsLocalAndMirrorsItIntoWorld)
    {
        slot.local_transform = translation(1.0F, 2.0F, 3.0F);
        slot.dirty = false;

        op.process_slot(slot, 0.5F);

        EXPECT_EQ(slot.local_transform, translation(1.0F, 2.0F, 3.0F));
        EXPECT_EQ(slot.world_transform, translation(1.0F, 2.0F, 3.0F));
        EXPECT_TRUE(slot.dirty);
    }

    TEST_F(MeshTransformOperatorTest, TheBoundFieldReceivesAccumulatedTimeAndSetsTheLocal)
    {
        std::vector<float> seen;
        op.bind(0, [&seen](float t) {
            seen.push_back(t);
            return translation(t, 0.0F, 0.0F);
        });

        op.process_slot(slot, 0.25F);
        op.process_slot(slot, 0.25F);

        ASSERT_EQ(seen.size(), 2U);
        EXPECT_FLOAT_EQ(seen[0], 0.25F);
        EXPECT_FLOAT_EQ(seen[1], 0.5F);
        EXPECT_EQ(origin(slot.local_transform), glm::vec3(0.5F, 0.0F, 0.0F));
        EXPECT_EQ(origin(slot.world_transform), glm::vec3(0.5F, 0.0F, 0.0F));
    }

    TEST_F(MeshTransformOperatorTest, ADifferentSlotIndexIsNotDriven)
    {
        op.bind(3, [](float) { return translation(9.0F, 0.0F, 0.0F); });

        op.process_slot(slot, 0.5F);

        EXPECT_EQ(slot.local_transform, glm::mat4(1.0F));
    }

    TEST_F(MeshTransformOperatorTest, ParentWorldIsAppliedThroughSetSlots)
    {
        std::vector<MeshSlot> slots(2);
        slots[0].index = 0;
        slots[0].world_transform = translation(10.0F, 0.0F, 0.0F);
        slots[1].index = 1;
        slots[1].parent_index = 0U;
        slots[1].local_transform = translation(0.0F, 2.0F, 0.0F);
        const std::vector<uint32_t> order { 0, 1 };
        op.set_slots(slots, order);

        op.process_slot(slots[1], 0.5F);

        EXPECT_EQ(origin(slots[1].world_transform), glm::vec3(10.0F, 2.0F, 0.0F));
    }

    TEST_F(MeshTransformOperatorTest, AMissingParentIndexFallsBackToIdentity)
    {
        std::vector<MeshSlot> slots(1);
        slots[0].index = 0;
        slots[0].parent_index = 5U;
        slots[0].local_transform = translation(0.0F, 2.0F, 0.0F);
        const std::vector<uint32_t> order { 0 };
        op.set_slots(slots, order);

        op.process_slot(slots[0], 0.5F);

        EXPECT_EQ(origin(slots[0].world_transform), glm::vec3(0.0F, 2.0F, 0.0F));
    }

    TEST_F(MeshTransformOperatorTest, UnbindStopsTheFieldAndRestartsItsClock)
    {
        float last = -1.0F;
        auto field = [&last](float t) {
            last = t;
            return translation(t, 0.0F, 0.0F);
        };
        op.bind(0, field);
        op.process_slot(slot, 0.5F);

        op.unbind(0);
        op.process_slot(slot, 0.5F);
        EXPECT_FLOAT_EQ(last, 0.5F);
        EXPECT_EQ(origin(slot.local_transform), glm::vec3(0.5F, 0.0F, 0.0F));

        op.bind(0, field);
        op.process_slot(slot, 0.25F);
        EXPECT_FLOAT_EQ(last, 0.25F);
    }

    TEST_F(MeshTransformOperatorTest, RebindingWithoutUnbindKeepsTheClock)
    {
        float last = -1.0F;
        op.bind(0, [](float t) { return translation(t, 0.0F, 0.0F); });
        op.process_slot(slot, 0.5F);

        op.bind(0, [&last](float t) {
            last = t;
            return translation(t, 0.0F, 0.0F);
        });
        op.process_slot(slot, 0.25F);

        EXPECT_FLOAT_EQ(last, 0.75F);
    }

    TEST_F(MeshTransformOperatorTest, UnbindAllStopsEverySlot)
    {
        op.bind(0, [](float) { return translation(9.0F, 0.0F, 0.0F); });
        op.bind(1, [](float) { return translation(9.0F, 0.0F, 0.0F); });

        op.unbind_all();
        op.process_slot(slot, 0.5F);

        EXPECT_EQ(slot.local_transform, glm::mat4(1.0F));
    }

    TEST_F(MeshTransformOperatorTest, ProcessWithoutSlotsIsSafe)
    {
        EXPECT_NO_THROW(op.process(0.0F));
    }

    TEST_F(MeshTransformOperatorTest, ProcessDrivesEverySlotInTheGivenOrder)
    {
        std::vector<MeshSlot> slots(2);
        slots[0].index = 0;
        slots[1].index = 1;
        const std::vector<uint32_t> order { 1, 0 };
        op.set_slots(slots, order);
        std::vector<uint32_t> visited;
        op.bind(0, [&visited](float) {
            visited.push_back(0);
            return glm::mat4(1.0F);
        });
        op.bind(1, [&visited](float) {
            visited.push_back(1);
            return glm::mat4(1.0F);
        });

        op.process(0.0F);

        EXPECT_EQ(visited, (std::vector<uint32_t> { 1, 0 }));
    }

    TEST_F(MeshTransformOperatorTest, ProcessAccumulatesTheDtItIsGiven)
    {
        std::vector<MeshSlot> slots(1);
        slots[0].index = 0;
        const std::vector<uint32_t> order { 0 };
        op.set_slots(slots, order);
        std::vector<float> seen;
        op.bind(0, [&seen](float t) {
            seen.push_back(t);
            return glm::mat4(1.0F);
        });

        op.process(0.25F);
        op.process(0.25F);

        ASSERT_EQ(seen.size(), 2U);
        EXPECT_FLOAT_EQ(seen[0], 0.25F);
        EXPECT_FLOAT_EQ(seen[1], 0.5F);
    }

    TEST_F(MeshTransformOperatorTest, ReportsItsTypeAndNoState)
    {
        EXPECT_EQ(op.get_type_name(), "MeshTransform");
        EXPECT_FALSE(op.query_state("anything").has_value());
        EXPECT_NO_THROW(op.set_parameter("anything", 1.0));
    }

} // namespace

} // namespace MayaFlux::Test
