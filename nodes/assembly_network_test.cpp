#include "gtest/gtest.h"

#include "MayaFlux/Nodes/Network/AssemblyNetwork.hpp"
#include "MayaFlux/Nodes/Network/Operators/FieldOperator.hpp"
#include "MayaFlux/Nodes/Network/Operators/OperatorChain.hpp"
#include "MayaFlux/Nodes/Network/Operators/PhysicsOperator.hpp"
#include "MayaFlux/Nodes/Network/PointCloudNetwork.hpp"

namespace MayaFlux::Test {

namespace {

    using Nodes::PointVertex;
    using Nodes::GpuSync::GeometryWriterNode;
    using Nodes::GpuSync::MeshWriterNode;
    using Nodes::Network::AssemblyNetwork;
    using Nodes::Network::AssemblyOperator;
    using Nodes::Network::FieldOperator;
    using Nodes::Network::NetworkOperator;
    using Nodes::Network::OperatorChain;
    using Nodes::Network::OutputMode;
    using Nodes::Network::PhysicsOperator;
    using Nodes::Network::PointCloudNetwork;
    using Nodes::Network::Topology;
    using Portal::Graphics::PrimitiveTopology;

    std::vector<glm::vec3> positions(uint32_t count, float offset = 0.0F)
    {
        std::vector<glm::vec3> out;
        out.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            out.emplace_back(static_cast<float>(i) + offset, offset, 0.0F);
        }
        return out;
    }

    std::vector<Kakshya::Vertex> vertices(uint32_t count)
    {
        std::vector<Kakshya::Vertex> out(count);
        for (uint32_t i = 0; i < count; ++i) {
            out[i].position = { static_cast<float>(i), 1.0F, 2.0F };
            out[i].color = { 0.25F, 0.5F, 0.75F };
        }
        return out;
    }

    std::vector<PointVertex> point_vertices(uint32_t count)
    {
        std::vector<PointVertex> out(count);
        for (uint32_t i = 0; i < count; ++i) {
            out[i].position = { static_cast<float>(i), 0.0F, 0.0F };
        }
        return out;
    }

    Kakshya::MeshData quad_mesh()
    {
        MeshWriterNode node(16);
        std::vector<Nodes::MeshVertex> quad(4);
        for (size_t i = 0; i < quad.size(); ++i) {
            quad[i].position = { static_cast<float>(i), 0.0F, 0.0F };
        }
        node.set_mesh(quad, std::vector<uint32_t> { 0, 1, 2, 2, 3, 0 });
        node.compute_frame();
        return node.get_mesh_data().value();
    }

    std::vector<uint8_t> copy_of(std::span<const uint8_t> bytes)
    {
        return { bytes.begin(), bytes.end() };
    }

    class RecordingOperator : public NetworkOperator {
    public:
        explicit RecordingOperator(std::vector<std::string>* log = nullptr, std::string name = "recording")
            : m_log(log)
            , m_name(std::move(name))
        {
        }

        void process(float dt) override
        {
            ++calls;
            last_dt = dt;
            if (m_log) {
                m_log->push_back(m_name);
            }
        }

        void set_parameter(std::string_view, double) override { }

        [[nodiscard]] std::optional<double> query_state(std::string_view) const override
        {
            return std::nullopt;
        }

        [[nodiscard]] std::string_view get_type_name() const override { return "Recording"; }

        int calls {};
        float last_dt {};

    private:
        std::vector<std::string>* m_log;
        std::string m_name;
    };

    TEST(AssemblyOperatorTest, NewOperatorIsEmpty)
    {
        AssemblyOperator op;

        EXPECT_EQ(op.item_count(), 0U);
        EXPECT_EQ(op.get_vertex_count(), 0U);
        EXPECT_EQ(op.query_state("item_count"), 0.0);
        EXPECT_EQ(op.get_vertex_layout().stride_bytes, sizeof(Kakshya::Vertex));
        EXPECT_FALSE(op.declared_topology().has_value());
    }

    TEST(AssemblyOperatorTest, PositionsBecomeAPointListItemWithThoseVertices)
    {
        AssemblyOperator op;

        auto node = op.add_geometry(positions(3));

        ASSERT_NE(node, nullptr);
        EXPECT_EQ(op.item_count(), 1U);
        EXPECT_EQ(node->get_vertex_count(), 3U);
        EXPECT_EQ(node->get_primitive_topology(), PrimitiveTopology::POINT_LIST);
        EXPECT_EQ(op.get_vertex_count(), 3U);
    }

    TEST(AssemblyOperatorTest, TopologyArgumentReachesTheItemAndTheOperator)
    {
        AssemblyOperator op;

        auto node = op.add_geometry(positions(4), PrimitiveTopology::LINE_LIST);

        EXPECT_EQ(node->get_primitive_topology(), PrimitiveTopology::LINE_LIST);
        EXPECT_EQ(op.declared_topology(), PrimitiveTopology::LINE_LIST);
    }

    TEST(AssemblyOperatorTest, FullVerticesKeepTheirAttributes)
    {
        AssemblyOperator op;

        auto node = op.add_geometry(vertices(3));

        ASSERT_EQ(node->get_vertex_count(), 3U);
        const auto v = node->get_vertex_typed<Kakshya::Vertex>(2);
        EXPECT_EQ(v.position, glm::vec3(2.0F, 1.0F, 2.0F));
        EXPECT_EQ(v.color, glm::vec3(0.25F, 0.5F, 0.75F));
    }

    TEST(AssemblyOperatorTest, MeshBecomesATriangleItem)
    {
        AssemblyOperator op;

        auto node = op.add_geometry(quad_mesh());

        ASSERT_NE(node, nullptr);
        EXPECT_EQ(op.item_count(), 1U);
        EXPECT_EQ(node->get_vertex_count(), 4U);
        EXPECT_EQ(op.declared_topology(), PrimitiveTopology::TRIANGLE_LIST);
    }

    TEST(AssemblyOperatorTest, AGivenNodeIsSharedAndNullIsIgnored)
    {
        AssemblyOperator op;
        auto node = std::make_shared<Nodes::GpuSync::GeometryLeafNode>(positions(3));

        op.add_geometry(std::shared_ptr<GeometryWriterNode> {});
        EXPECT_EQ(op.item_count(), 0U);

        op.add_geometry(std::shared_ptr<GeometryWriterNode>(node));
        EXPECT_EQ(op.item_count(), 1U);
        EXPECT_EQ(op.get_vertex_count(), 3U);
    }

    TEST(AssemblyOperatorTest, RemovingAnItemDropsItAndOnlyItOnce)
    {
        AssemblyOperator op;
        auto first = op.add_geometry(positions(3));
        auto second = op.add_geometry(positions(4, 10.0F));

        EXPECT_TRUE(op.remove_geometry(first));
        EXPECT_EQ(op.item_count(), 1U);
        EXPECT_EQ(op.get_vertex_count(), 4U);
        EXPECT_FALSE(op.remove_geometry(first));
        EXPECT_FALSE(op.remove_geometry(nullptr));
        EXPECT_TRUE(op.remove_geometry(second));
        EXPECT_EQ(op.item_count(), 0U);
    }

    TEST(AssemblyOperatorTest, ItemsKeepInsertionOrderInTheAggregate)
    {
        AssemblyOperator op;
        auto first = op.add_geometry(positions(3));
        auto second = op.add_geometry(positions(2, 10.0F));

        const auto all = op.get_vertex_data();
        const auto a = first->get_vertex_data();
        const auto b = second->get_vertex_data();

        ASSERT_EQ(all.size(), a.size() + b.size());
        EXPECT_TRUE(std::equal(a.begin(), a.end(), all.begin()));
        EXPECT_TRUE(std::equal(b.begin(), b.end(), all.begin() + static_cast<std::ptrdiff_t>(a.size())));
    }

    TEST(AssemblyOperatorTest, ProcessRecomputesAnEditedItem)
    {
        AssemblyOperator op;
        auto node = op.add_geometry(positions(3));
        op.mark_vertex_data_clean();

        node->set_positions(positions(5));
        EXPECT_EQ(op.get_vertex_count(), 3U);
        op.process(0.0F);

        EXPECT_EQ(op.get_vertex_count(), 5U);
        EXPECT_TRUE(op.is_vertex_data_dirty());
    }

    TEST(AssemblyOperatorTest, OnlyTheEditedItemReportsADirtyRange)
    {
        AssemblyOperator op;
        op.add_geometry(positions(3));
        auto second = op.add_geometry(positions(4, 10.0F));
        op.mark_vertex_data_clean();

        second->set_positions(positions(4, 20.0F));
        op.process(0.0F);

        const auto ranges = op.dirty_vertex_ranges();
        ASSERT_EQ(ranges.size(), 1U);
        EXPECT_EQ(ranges[0].group_index, 1U);
        EXPECT_EQ(ranges[0].vertex_offset, 3U);
        EXPECT_EQ(ranges[0].vertex_count, 4U);
    }

    TEST(AssemblyOperatorTest, LayoutFollowsTheFirstItemsTopology)
    {
        AssemblyOperator lines;
        lines.add_geometry(positions(4), PrimitiveTopology::LINE_LIST);
        AssemblyOperator points;
        points.add_geometry(positions(4), PrimitiveTopology::POINT_LIST);

        EXPECT_EQ(lines.get_vertex_layout().attributes[2].name, "thickness");
        EXPECT_EQ(points.get_vertex_layout().attributes[2].name, "size");
        EXPECT_EQ(lines.get_vertex_layout().vertex_count, 4U);
    }

    TEST(AssemblyOperatorTest, PointCountEqualsVertexCount)
    {
        AssemblyOperator op;
        op.add_geometry(positions(3));
        op.add_geometry(positions(4), PrimitiveTopology::LINE_LIST);

        EXPECT_EQ(op.get_point_count(), op.get_vertex_count());
    }

    TEST(AssemblyOperatorTest, QueryStateReportsCountsAndNothingElse)
    {
        AssemblyOperator op;
        op.add_geometry(positions(3));
        op.add_geometry(positions(4));

        EXPECT_EQ(op.query_state("item_count"), 2.0);
        EXPECT_EQ(op.query_state("vertex_count"), 7.0);
        EXPECT_FALSE(op.query_state("no_such_state").has_value());
    }

    TEST(AssemblyOperatorTest, ParametersAreIgnored)
    {
        AssemblyOperator op;
        op.add_geometry(positions(3));
        const auto before = copy_of(op.get_vertex_data());

        EXPECT_NO_THROW(op.set_parameter("anything", 4.0));
        op.process(0.0F);

        EXPECT_EQ(copy_of(op.get_vertex_data()), before);
    }

    TEST(AssemblyOperatorTest, OneToOneMappingLeavesTheItemsUntouched)
    {
        AssemblyOperator op;
        op.add_geometry(positions(4));
        const auto before = copy_of(op.get_vertex_data());
        auto source = std::make_shared<PointCloudNetwork>(op.get_point_count());
        source->initialize();

        op.apply_one_to_one("color", source);
        op.apply_one_to_one("size", source);

        EXPECT_EQ(copy_of(op.get_vertex_data()), before);
    }

    TEST(AssemblyOperatorTest, ReportsItsTypeNamesAndIncrementalUpload)
    {
        AssemblyOperator op;

        EXPECT_EQ(op.get_type_name(), "Assembly");
        EXPECT_STREQ(op.get_vertex_type_name(), "Vertex");
        EXPECT_TRUE(op.supports_incremental_upload());
    }

    TEST(AssemblyNetworkTest, FreshNetworkHoldsAnEmptyAssemblyOperator)
    {
        AssemblyNetwork net;

        ASSERT_TRUE(net.has_operator());
        ASSERT_NE(net.assembly(), nullptr);
        EXPECT_EQ(net.get_operator(), net.assembly());
        EXPECT_EQ(net.get_node_count(), 0U);
        EXPECT_EQ(net.get_topology(), Topology::INDEPENDENT);
        EXPECT_EQ(net.get_output_mode(), OutputMode::GRAPHICS_BIND);
    }

    TEST(AssemblyNetworkTest, EveryAddOverloadFeedsTheOperator)
    {
        AssemblyNetwork net;
        AssemblyNetwork::VertexInput input;
        input.values = vertices(3);
        input.topology = PrimitiveTopology::LINE_LIST;

        net.add_geometry(positions(3));
        EXPECT_EQ(net.get_node_count(), 1U);
        net.add_geometry(vertices(3));
        EXPECT_EQ(net.get_node_count(), 2U);
        auto from_input = net.add_geometry(input);
        EXPECT_EQ(net.get_node_count(), 3U);
        EXPECT_EQ(from_input->get_primitive_topology(), PrimitiveTopology::LINE_LIST);
        net.add_geometry(quad_mesh());
        EXPECT_EQ(net.get_node_count(), 4U);
        net.add_geometry(std::shared_ptr<GeometryWriterNode>(std::make_shared<Nodes::GpuSync::GeometryLeafNode>(positions(2))));
        EXPECT_EQ(net.get_node_count(), 5U);
        EXPECT_EQ(net.assembly()->item_count(), 5U);
    }

    TEST(AssemblyNetworkTest, SourceVariantDispatchesToTheMatchingOverload)
    {
        AssemblyNetwork net;
        AssemblyNetwork::VertexInput input;
        input.values = vertices(3);
        input.topology = PrimitiveTopology::LINE_STRIP;

        net.add_geometry(AssemblyNetwork::Source { input });
        net.add_geometry(AssemblyNetwork::Source { quad_mesh() });
        net.add_geometry(AssemblyNetwork::Source { std::shared_ptr<GeometryWriterNode>(std::make_shared<Nodes::GpuSync::GeometryLeafNode>(positions(2))) });

        EXPECT_EQ(net.get_node_count(), 3U);
        EXPECT_EQ(net.assembly()->declared_topology(), PrimitiveTopology::LINE_STRIP);
        EXPECT_EQ(net.assembly()->get_vertex_count(), 3U + 4U + 2U);
    }

    TEST(AssemblyNetworkTest, RemovingGeometryShrinksTheNetwork)
    {
        AssemblyNetwork net;
        auto first = net.add_geometry(positions(3));
        net.add_geometry(positions(4));

        EXPECT_TRUE(net.remove_geometry(first));
        EXPECT_EQ(net.get_node_count(), 1U);
        EXPECT_FALSE(net.remove_geometry(first));
    }

    TEST(AssemblyNetworkTest, ProcessBatchRecomputesEditedItems)
    {
        AssemblyNetwork net;
        auto node = net.add_geometry(positions(3));
        node->set_positions(positions(5));

        net.process_batch(1);

        EXPECT_EQ(net.assembly()->get_vertex_count(), 5U);
    }

    TEST(AssemblyNetworkTest, DisabledNetworkDoesNotProcess)
    {
        AssemblyNetwork net;
        auto node = net.add_geometry(positions(3));
        node->set_positions(positions(5));
        net.set_enabled(false);

        net.process_batch(2);
        EXPECT_EQ(net.assembly()->get_vertex_count(), 3U);

        net.set_enabled(true);
        net.process_batch(1);
        EXPECT_EQ(net.assembly()->get_vertex_count(), 5U);
    }

    TEST(AssemblyNetworkTest, ZeroFramesProcessNothing)
    {
        AssemblyNetwork net;
        auto node = net.add_geometry(positions(3));
        node->set_positions(positions(5));

        net.process_batch(0);

        EXPECT_EQ(net.assembly()->get_vertex_count(), 3U);
    }

    TEST(AssemblyNetworkTest, ChainOperatorsRunOncePerFrame)
    {
        AssemblyNetwork net;
        auto recorder = net.get_operator_chain()->emplace<RecordingOperator>();

        net.process_batch(4);
        EXPECT_EQ(recorder->calls, 4);
        EXPECT_EQ(recorder->last_dt, 0.0F);

        net.process_batch(0);
        EXPECT_EQ(recorder->calls, 4);
    }

    TEST(AssemblyNetworkTest, ChainDoesNotRunWhileDisabled)
    {
        AssemblyNetwork net;
        auto recorder = net.get_operator_chain()->emplace<RecordingOperator>();
        net.set_enabled(false);

        net.process_batch(3);

        EXPECT_EQ(recorder->calls, 0);
    }

    TEST(AssemblyNetworkTest, AChainOperatorThatConsumesUpstreamIsSeededFromTheAssembly)
    {
        AssemblyNetwork net;
        net.add_geometry(positions(6));
        auto field = net.get_operator_chain()->emplace<FieldOperator>();
        field->set_consumes_upstream(true);

        net.process_batch(1);

        EXPECT_EQ(field->get_vertex_count(), net.assembly()->get_vertex_count());
    }

    TEST(AssemblyNetworkTest, MetadataReportsTheItemCountAndEnabledState)
    {
        AssemblyNetwork net;
        net.add_geometry(positions(3));
        net.add_geometry(positions(2));

        EXPECT_EQ(net.get_metadata().at("node_count"), "2");
        EXPECT_EQ(net.get_metadata().at("enabled"), "true");

        net.set_enabled(false);
        EXPECT_EQ(net.get_metadata().at("enabled"), "false");
    }

    TEST(OperatorChainTest, StartsEmpty)
    {
        OperatorChain chain;

        EXPECT_TRUE(chain.empty());
        EXPECT_EQ(chain.size(), 0U);
        EXPECT_EQ(chain.get(0), nullptr);
    }

    TEST(OperatorChainTest, AddAppendsAndIgnoresNull)
    {
        OperatorChain chain;
        auto op = std::make_shared<RecordingOperator>();

        chain.add(nullptr);
        EXPECT_TRUE(chain.empty());

        chain.add(op);
        ASSERT_EQ(chain.size(), 1U);
        EXPECT_EQ(chain.get(0), op);
        EXPECT_EQ(chain.get(1), nullptr);
    }

    TEST(OperatorChainTest, EmplaceBuildsAndReturnsTheStoredOperator)
    {
        OperatorChain chain;

        auto op = chain.emplace<RecordingOperator>();

        ASSERT_EQ(chain.size(), 1U);
        EXPECT_EQ(chain.get(0), op);
        EXPECT_FALSE(chain.empty());
    }

    TEST(OperatorChainTest, FindReturnsTheFirstOperatorOfAType)
    {
        OperatorChain chain;
        auto first = chain.emplace<RecordingOperator>();
        chain.emplace<RecordingOperator>();
        auto field = chain.emplace<FieldOperator>();

        EXPECT_EQ(chain.find<RecordingOperator>(), first);
        EXPECT_EQ(chain.find<FieldOperator>(), field);
        EXPECT_EQ(chain.find<PhysicsOperator>(), nullptr);
    }

    TEST(OperatorChainTest, RemoveDropsOnlyThatOperatorAndClearDropsAll)
    {
        OperatorChain chain;
        auto a = chain.emplace<RecordingOperator>();
        auto b = chain.emplace<RecordingOperator>();

        chain.remove(a);
        ASSERT_EQ(chain.size(), 1U);
        EXPECT_EQ(chain.get(0), b);

        chain.remove(a);
        EXPECT_EQ(chain.size(), 1U);

        chain.clear();
        EXPECT_TRUE(chain.empty());
    }

    TEST(OperatorChainTest, ProcessRunsOperatorsInInsertionOrderWithTheGivenDt)
    {
        OperatorChain chain;
        std::vector<std::string> log;
        auto a = chain.emplace<RecordingOperator>(&log, "a");
        chain.emplace<RecordingOperator>(&log, "b");
        chain.emplace<RecordingOperator>(&log, "c");

        chain.process(0.5F);

        EXPECT_EQ(log, (std::vector<std::string> { "a", "b", "c" }));
        EXPECT_EQ(a->last_dt, 0.5F);
    }

    TEST(OperatorChainTest, EmptyChainProcessIsSafe)
    {
        OperatorChain chain;

        EXPECT_NO_THROW(chain.process(0.1F));
    }

    TEST(OperatorChainTest, ConsumingOperatorIsSeededFromTheGivenUpstream)
    {
        PhysicsOperator upstream;
        upstream.initialize(point_vertices(5));
        OperatorChain chain;
        auto field = chain.emplace<FieldOperator>();
        field->set_consumes_upstream(true);

        chain.process(0.0F, &upstream);

        EXPECT_EQ(field->get_vertex_count(), 5U);
    }

    TEST(OperatorChainTest, ConsumingOperatorIsSeededFromThePreviousGraphicsOperator)
    {
        OperatorChain chain;
        auto source = chain.emplace<FieldOperator>();
        source->initialize(point_vertices(3));
        auto consumer = chain.emplace<FieldOperator>();
        consumer->set_consumes_upstream(true);

        chain.process(0.0F);

        EXPECT_EQ(consumer->get_vertex_count(), 3U);
    }

    TEST(OperatorChainTest, NonConsumingOperatorIsNeverSeeded)
    {
        PhysicsOperator upstream;
        upstream.initialize(point_vertices(5));
        OperatorChain chain;
        auto field = chain.emplace<FieldOperator>();

        chain.process(0.0F, &upstream);

        EXPECT_EQ(field->get_vertex_count(), 0U);
    }

    TEST(OperatorChainTest, ConsumingOperatorWithoutAGraphicsUpstreamStaysEmpty)
    {
        OperatorChain chain;
        chain.emplace<RecordingOperator>();
        auto consumer = chain.emplace<FieldOperator>();
        consumer->set_consumes_upstream(true);

        EXPECT_NO_THROW(chain.process(0.0F));
        EXPECT_EQ(consumer->get_vertex_count(), 0U);

        EXPECT_NO_THROW(chain.process(0.0F, nullptr));
    }

} // namespace

} // namespace MayaFlux::Test
