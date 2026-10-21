#include "gtest/gtest.h"

#include "MayaFlux/Nodes/Graphics/GeometryLeafNode.hpp"
#include "MayaFlux/Nodes/Network/InstanceNetwork.hpp"
#include "MayaFlux/Nodes/Network/Operators/InstanceFieldOperator.hpp"
#include "MayaFlux/Nodes/Network/Operators/OperatorChain.hpp"

namespace MayaFlux::Test {

namespace {

    using Nodes::GpuSync::GeometryLeafNode;
    using Nodes::Network::InstanceFieldOperator;
    using Nodes::Network::InstanceNetwork;
    using Nodes::Network::NetworkOperator;
    using Nodes::Network::OutputMode;

    std::vector<glm::vec3> positions(uint32_t count)
    {
        std::vector<glm::vec3> out;
        for (uint32_t i = 0; i < count; ++i) {
            out.emplace_back(static_cast<float>(i), 0.0F, 0.0F);
        }
        return out;
    }

    std::shared_ptr<GeometryLeafNode> leaf(uint32_t count = 3)
    {
        return std::make_shared<GeometryLeafNode>(positions(count));
    }

    Kinesis::VectorField step_up()
    {
        return Kinesis::VectorField { .fn = [](const glm::vec3& p) { return p + glm::vec3(0.0F, 1.0F, 0.0F); } };
    }

    class FrameCounter : public NetworkOperator {
    public:
        void process(float dt) override
        {
            ++calls;
            last_dt = dt;
        }

        void set_parameter(std::string_view, double) override { }

        [[nodiscard]] std::optional<double> query_state(std::string_view) const override
        {
            return std::nullopt;
        }

        [[nodiscard]] std::string_view get_type_name() const override { return "FrameCounter"; }

        int calls {};
        float last_dt {};
    };

    TEST(InstanceNetworkTest, StartsEmptyAndBoundToGraphics)
    {
        InstanceNetwork net;

        EXPECT_EQ(net.slot_count(), 0U);
        EXPECT_EQ(net.get_node_count(), 0U);
        EXPECT_FALSE(net.has_operator());
        EXPECT_EQ(net.get_operator(), nullptr);
        EXPECT_EQ(net.get_output_mode(), OutputMode::GRAPHICS_BIND);
        ASSERT_NE(net.get_operator_chain(), nullptr);
        EXPECT_TRUE(net.get_operator_chain()->empty());
    }

    TEST(InstanceNetworkTest, AddSlotReturnsSequentialIndicesAndKeepsTheNode)
    {
        InstanceNetwork net;
        auto first = leaf();
        auto second = leaf(5);

        EXPECT_EQ(net.add_slot("a", first), 0U);
        EXPECT_EQ(net.add_slot("b", second), 1U);

        EXPECT_EQ(net.slot_count(), 2U);
        EXPECT_EQ(net.get_node_count(), 2U);
        EXPECT_EQ(net.get_slot(0).node, first);
        EXPECT_EQ(net.get_slot(1).node, second);
        EXPECT_EQ(net.get_slot(1).index, 1U);
        EXPECT_EQ(net.get_slot(1).name, "b");
    }

    TEST(InstanceNetworkTest, NewSlotsStartAtIdentityAndDirty)
    {
        InstanceNetwork net;
        net.add_slot("a", leaf());

        EXPECT_EQ(net.get_slot(0).transform, glm::mat4(1.0F));
        EXPECT_TRUE(net.get_slot(0).dirty);
    }

    TEST(InstanceNetworkTest, NullNodeIsRejected)
    {
        InstanceNetwork net;

        EXPECT_THROW(net.add_slot("a", nullptr), std::invalid_argument);
        EXPECT_EQ(net.slot_count(), 0U);
    }

    TEST(InstanceNetworkTest, EmptyNameIsFilledFromTheIndexAndNodeType)
    {
        InstanceNetwork net;

        net.add_slot("", leaf());
        net.add_slot("", leaf());

        EXPECT_EQ(net.get_slot(0).name.rfind("slot_0_", 0), 0U);
        EXPECT_GT(net.get_slot(0).name.size(), std::string("slot_0_").size());
        EXPECT_EQ(net.get_slot(1).name.rfind("slot_1_", 0), 0U);
    }

    TEST(InstanceNetworkTest, OutOfRangeSlotThrows)
    {
        InstanceNetwork net;
        net.add_slot("a", leaf());

        EXPECT_THROW((void)net.get_slot(1), std::out_of_range);
        EXPECT_THROW((void)std::as_const(net).get_slot(5), std::out_of_range);
    }

    TEST(InstanceNetworkTest, FindSlotByName)
    {
        InstanceNetwork net;
        net.add_slot("a", leaf());
        net.add_slot("b", leaf());
        net.add_slot("b", leaf());

        ASSERT_NE(net.find_slot("a"), nullptr);
        EXPECT_EQ(net.find_slot("a")->index, 0U);
        EXPECT_EQ(net.find_slot("b")->index, 1U);
        EXPECT_EQ(net.find_slot("missing"), nullptr);
        EXPECT_EQ(std::as_const(net).find_slot("a")->index, 0U);
    }

    TEST(InstanceNetworkTest, SlotsAccessorExposesTheStoredSlots)
    {
        InstanceNetwork net;
        net.add_slot("a", leaf());

        net.slots()[0].transform[3] = glm::vec4(1.0F, 2.0F, 3.0F, 1.0F);

        EXPECT_EQ(std::as_const(net).slots()[0].transform[3], glm::vec4(1.0F, 2.0F, 3.0F, 1.0F));
    }

    TEST(InstanceNetworkTest, NodeOutputIsTheSlotIndexWithinRange)
    {
        InstanceNetwork net;
        net.add_slot("a", leaf());
        net.add_slot("b", leaf());

        EXPECT_EQ(net.get_node_output(0), 0.0);
        EXPECT_EQ(net.get_node_output(1), 1.0);
        EXPECT_FALSE(net.get_node_output(2).has_value());
    }

    TEST(InstanceNetworkTest, ResetRestoresIdentityAndMarksEverySlotDirty)
    {
        InstanceNetwork net;
        net.add_slot("a", leaf());
        net.add_slot("b", leaf());
        for (auto& slot : net.slots()) {
            slot.transform[3] = glm::vec4(4.0F, 5.0F, 6.0F, 1.0F);
            slot.dirty = false;
        }

        net.reset();

        for (const auto& slot : net.slots()) {
            EXPECT_EQ(slot.transform, glm::mat4(1.0F));
            EXPECT_TRUE(slot.dirty);
        }
    }

    TEST(InstanceNetworkTest, MetadataReportsSlotsOperatorAndChain)
    {
        InstanceNetwork net;
        net.add_slot("a", leaf());
        net.add_slot("b", leaf());

        auto meta = net.get_metadata();
        EXPECT_EQ(meta.at("slot_count"), "2");
        EXPECT_EQ(meta.at("operator"), "none");
        EXPECT_EQ(meta.at("chain_size"), "0");

        net.create_operator<InstanceFieldOperator>();
        net.get_operator_chain()->emplace<FrameCounter>();

        meta = net.get_metadata();
        EXPECT_EQ(meta.at("operator"), "InstanceField");
        EXPECT_EQ(meta.at("chain_size"), "1");
    }

    TEST(InstanceNetworkTest, CreateOperatorStoresAndReturnsTheOperator)
    {
        InstanceNetwork net;

        auto op = net.create_operator<InstanceFieldOperator>();

        ASSERT_TRUE(net.has_operator());
        EXPECT_EQ(net.get_operator(), op.get());
    }

    TEST(InstanceNetworkTest, ANullOperatorClearsTheCurrentOne)
    {
        InstanceNetwork net;
        net.create_operator<InstanceFieldOperator>();

        net.set_operator(nullptr);

        EXPECT_FALSE(net.has_operator());
        net.add_slot("a", leaf());
        EXPECT_NO_THROW(net.process_batch(2));
    }

    TEST(InstanceNetworkTest, ProcessBatchRefreshesEverySlotNode)
    {
        InstanceNetwork net;
        auto node = leaf(3);
        net.add_slot("a", node);
        node->set_positions(positions(7));

        net.process_batch(1);

        EXPECT_EQ(node->get_vertex_count(), 7U);
    }

    TEST(InstanceNetworkTest, DisabledNetworkDoesNothing)
    {
        InstanceNetwork net;
        auto node = leaf(3);
        node->compute_frame();
        net.add_slot("a", node);
        auto counter = net.get_operator_chain()->emplace<FrameCounter>();
        node->set_positions(positions(7));
        net.set_enabled(false);

        net.process_batch(2);

        EXPECT_EQ(node->get_vertex_count(), 3U);
        EXPECT_EQ(counter->calls, 0);
    }

    TEST(InstanceNetworkTest, NoSlotsMeansNothingRuns)
    {
        InstanceNetwork net;
        auto counter = net.get_operator_chain()->emplace<FrameCounter>();

        net.process_batch(4);

        EXPECT_EQ(counter->calls, 0);
    }

    TEST(InstanceNetworkTest, PrimaryOperatorRunsOncePerFrameWithZeroDt)
    {
        InstanceNetwork net;
        net.add_slot("a", leaf());
        auto counter = std::make_shared<FrameCounter>();
        net.set_operator(counter);

        net.process_batch(4);

        EXPECT_EQ(counter->calls, 4);
        EXPECT_EQ(counter->last_dt, 0.0F);
    }

    TEST(InstanceNetworkTest, ChainOperatorsRunOncePerFrameAfterThePrimary)
    {
        InstanceNetwork net;
        net.add_slot("a", leaf());
        auto counter = net.get_operator_chain()->emplace<FrameCounter>();

        net.process_batch(3);

        EXPECT_EQ(counter->calls, 3);
    }

    TEST(InstanceNetworkTest, ChainRunsEvenWithoutAPrimaryOperator)
    {
        InstanceNetwork net;
        net.add_slot("a", leaf());
        auto counter = net.get_operator_chain()->emplace<FrameCounter>();

        net.process_batch(2);

        EXPECT_FALSE(net.has_operator());
        EXPECT_EQ(counter->calls, 2);
    }

    TEST(InstanceNetworkTest, AFieldOperatorMovesItsSlotOnceForEveryFrame)
    {
        InstanceNetwork net;
        net.add_slot("a", leaf());
        net.add_slot("b", leaf());
        auto field = net.create_operator<InstanceFieldOperator>();
        field->bind_position(0, step_up());

        net.process_batch(3);

        EXPECT_EQ(glm::vec3(net.get_slot(0).transform[3]), glm::vec3(0.0F, 3.0F, 0.0F));
        EXPECT_EQ(glm::vec3(net.get_slot(1).transform[3]), glm::vec3(0.0F));
        EXPECT_TRUE(net.get_slot(0).dirty);
    }

    TEST(InstanceNetworkTest, ChainFieldOperatorsAreHandedTheSlotsToo)
    {
        InstanceNetwork net;
        net.add_slot("a", leaf());
        auto primary = net.create_operator<InstanceFieldOperator>();
        auto chained = net.get_operator_chain()->emplace<InstanceFieldOperator>();
        primary->bind_position(0, step_up());
        chained->bind_position(0, step_up());

        net.process_batch(1);

        EXPECT_EQ(glm::vec3(net.get_slot(0).transform[3]), glm::vec3(0.0F, 2.0F, 0.0F));
    }

    TEST(InstanceNetworkTest, ResetThenProcessStartsFromIdentityAgain)
    {
        InstanceNetwork net;
        net.add_slot("a", leaf());
        auto field = net.create_operator<InstanceFieldOperator>();
        field->bind_position(0, step_up());
        net.process_batch(2);

        net.reset();
        net.process_batch(1);

        EXPECT_EQ(glm::vec3(net.get_slot(0).transform[3]), glm::vec3(0.0F, 1.0F, 0.0F));
    }

} // namespace

} // namespace MayaFlux::Test
