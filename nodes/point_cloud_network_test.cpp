#include "gtest/gtest.h"

#include "MayaFlux/Nodes/Generators/Impulse.hpp"
#include "MayaFlux/Nodes/Network/Operators/FieldOperator.hpp"
#include "MayaFlux/Nodes/Network/Operators/PathOperator.hpp"
#include "MayaFlux/Nodes/Network/Operators/PhysicsOperator.hpp"
#include "MayaFlux/Nodes/Network/Operators/TopologyOperator.hpp"
#include "MayaFlux/Nodes/Network/PointCloudNetwork.hpp"

namespace MayaFlux::Test {

namespace {

    using Kinesis::InterpolationMode;
    using Kinesis::ProximityMode;
    using Nodes::LineVertex;
    using Nodes::Generator::Impulse;
    using Nodes::Network::FieldOperator;
    using Nodes::Network::MappingMode;
    using Nodes::Network::PathOperator;
    using Nodes::Network::PhysicsOperator;
    using Nodes::Network::PointCloudNetwork;
    using Nodes::Network::TopologyOperator;

    constexpr float k_eps = 1e-5F;

    std::vector<LineVertex> points(uint32_t count, float offset = 0.0F)
    {
        std::vector<LineVertex> out;
        out.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            LineVertex v;
            const auto x = static_cast<float>(i);
            v.position = { x + offset, (i % 2 == 0) ? 0.0F : 1.0F, offset };
            out.push_back(v);
        }
        return out;
    }

    LineVertex line_at(std::span<const uint8_t> bytes, size_t i)
    {
        LineVertex v;
        std::memcpy(&v, bytes.data() + i * sizeof(LineVertex), sizeof(LineVertex));
        return v;
    }

    size_t vertices_in(std::span<const uint8_t> bytes)
    {
        return bytes.size() / sizeof(LineVertex);
    }

    std::vector<uint8_t> copy_of(std::span<const uint8_t> bytes)
    {
        return { bytes.begin(), bytes.end() };
    }

    bool same_positions(const std::vector<LineVertex>& a, const std::vector<LineVertex>& b)
    {
        if (a.size() != b.size()) {
            return false;
        }
        for (size_t i = 0; i < a.size(); ++i) {
            if (a[i].position != b[i].position) {
                return false;
            }
        }
        return true;
    }

    bool near_color(const glm::vec3& a, const glm::vec3& b)
    {
        return std::abs(a.x - b.x) < k_eps && std::abs(a.y - b.y) < k_eps && std::abs(a.z - b.z) < k_eps;
    }

    std::vector<size_t> connections_per_topology(const TopologyOperator& op)
    {
        std::vector<size_t> out;
        for (size_t i = 0; i < op.get_topology_count(); ++i) {
            out.push_back(op.get_topology(i)->get_connection_count());
        }
        return out;
    }

    TEST(PathOperatorTest, InitializeBuildsOnePathFromTheControlPoints)
    {
        PathOperator op;

        op.initialize(points(6));

        ASSERT_EQ(op.get_path_count(), 1U);
        EXPECT_EQ(op.get_path(0)->get_control_point_count(), 6U);
        EXPECT_GT(op.get_vertex_count(), 0U);
    }

    TEST(PathOperatorTest, InitializeReplacesAndEmptyInitializeKeepsWhatIsThere)
    {
        PathOperator op;
        op.initialize(points(6));

        op.initialize(points(8));
        ASSERT_EQ(op.get_path_count(), 1U);
        EXPECT_EQ(op.get_path(0)->get_control_point_count(), 8U);

        op.initialize({});
        ASSERT_EQ(op.get_path_count(), 1U);
        EXPECT_EQ(op.get_path(0)->get_control_point_count(), 8U);
    }

    TEST(PathOperatorTest, InitializePathsKeepsListOrder)
    {
        PathOperator op;

        op.initialize_paths({ points(6), points(8), points(5) }, InterpolationMode::CATMULL_ROM);

        ASSERT_EQ(op.get_path_count(), 3U);
        EXPECT_EQ(op.get_path(0)->get_control_point_count(), 6U);
        EXPECT_EQ(op.get_path(1)->get_control_point_count(), 8U);
        EXPECT_EQ(op.get_path(2)->get_control_point_count(), 5U);
    }

    TEST(PathOperatorTest, AddPathAppendsAndIgnoresAnEmptyPath)
    {
        PathOperator op;
        op.initialize(points(6));

        op.add_path({}, InterpolationMode::CATMULL_ROM);
        EXPECT_EQ(op.get_path_count(), 1U);

        op.add_path(points(8), InterpolationMode::CATMULL_ROM);
        EXPECT_EQ(op.get_path_count(), 2U);
    }

    TEST(PathOperatorTest, AddNodeSharesTheCallersNodeAndIgnoresNull)
    {
        PathOperator op;
        auto node = std::make_shared<Nodes::GpuSync::PathGeneratorNode>(InterpolationMode::CATMULL_ROM, 16, 64, 0.5);
        node->set_control_points(points(6));

        op.add_node(nullptr);
        EXPECT_EQ(op.get_path_count(), 0U);

        op.add_node(node);
        ASSERT_EQ(op.get_path_count(), 1U);
        EXPECT_EQ(op.get_path(0), node);
    }

    TEST(PathOperatorTest, OutOfRangePathIsNull)
    {
        PathOperator op;
        op.initialize(points(6));

        EXPECT_EQ(op.get_path(1), nullptr);
    }

    TEST(PathOperatorTest, SamplesPerSegmentParameterReachesEveryPath)
    {
        PathOperator op;
        op.initialize_paths({ points(6), points(8) }, InterpolationMode::CATMULL_ROM);
        const auto first = op.get_path(0)->get_vertex_count();
        const auto second = op.get_path(1)->get_vertex_count();

        op.set_parameter("samples_per_segment", 4.0);
        op.process(0.0F);

        EXPECT_LT(op.get_path(0)->get_vertex_count(), first);
        EXPECT_LT(op.get_path(1)->get_vertex_count(), second);
    }

    TEST(PathOperatorTest, TensionParameterReachesEveryPath)
    {
        PathOperator op;
        op.initialize_paths({ points(6), points(8) }, InterpolationMode::CATMULL_ROM);
        const auto before = copy_of(op.get_vertex_data());

        op.set_parameter("tension", 0.0);
        op.process(0.0F);

        EXPECT_NE(copy_of(op.get_vertex_data()), before);
    }

    TEST(PathOperatorTest, ThicknessParameterReachesEveryVertexOfEveryPath)
    {
        PathOperator op;
        op.initialize_paths({ points(6), points(8) }, InterpolationMode::CATMULL_ROM);

        op.set_parameter("thickness", 6.0);
        op.process(0.0F);

        const auto bytes = op.get_vertex_data();
        for (size_t i = 0; i < vertices_in(bytes); ++i) {
            EXPECT_FLOAT_EQ(line_at(bytes, i).thickness, 6.0F) << "vertex " << i;
        }
    }

    TEST(PathOperatorTest, ThicknessSetBeforeAddingAPathIsInherited)
    {
        PathOperator op;
        op.initialize(points(6));
        op.set_global_thickness(6.0F);

        op.add_path(points(8, 20.0F), InterpolationMode::CATMULL_ROM);
        op.process(0.0F);

        const auto bytes = op.get_vertex_data_for_collection(1);
        ASSERT_GT(vertices_in(bytes), 0U);
        EXPECT_FLOAT_EQ(line_at(bytes, 0).thickness, 6.0F);
    }

    TEST(PathOperatorTest, GlobalColorReachesEveryVertex)
    {
        PathOperator op;
        op.initialize_paths({ points(6), points(8) }, InterpolationMode::CATMULL_ROM);

        op.set_global_color({ 0.2F, 0.4F, 0.6F });
        op.process(0.0F);

        const auto bytes = op.get_vertex_data();
        for (size_t i = 0; i < vertices_in(bytes); ++i) {
            EXPECT_TRUE(near_color(line_at(bytes, i).color, { 0.2F, 0.4F, 0.6F })) << "vertex " << i;
        }
    }

    TEST(PathOperatorTest, TypedSettersMatchTheirParameters)
    {
        PathOperator by_setter;
        PathOperator by_parameter;
        by_setter.initialize(points(6));
        by_parameter.initialize(points(6));

        by_setter.set_samples_per_segment(4);
        by_setter.set_tension(0.1);
        by_parameter.set_parameter("samples_per_segment", 4.0);
        by_parameter.set_parameter("tension", 0.1);
        by_setter.process(0.0F);
        by_parameter.process(0.0F);

        EXPECT_EQ(copy_of(by_setter.get_vertex_data()), copy_of(by_parameter.get_vertex_data()));
    }

    TEST(PathOperatorTest, UnknownParameterChangesNothing)
    {
        PathOperator op;
        op.initialize(points(6));
        const auto before = copy_of(op.get_vertex_data());

        op.set_parameter("no_such_parameter", 3.0);
        op.process(0.0F);

        EXPECT_EQ(copy_of(op.get_vertex_data()), before);
    }

    TEST(PathOperatorTest, QueryStateReportsCountsAndNothingElse)
    {
        PathOperator op;
        op.initialize_paths({ points(6), points(8) }, InterpolationMode::CATMULL_ROM);

        EXPECT_EQ(op.query_state("path_count"), 2.0);
        EXPECT_EQ(op.query_state("vertex_count"), static_cast<double>(op.get_vertex_count()));
        EXPECT_FALSE(op.query_state("no_such_state").has_value());
    }

    TEST(PathOperatorTest, ControlPointCountQueryCountsControlPoints)
    {
        PathOperator op;
        op.initialize_paths({ points(6), points(8) }, InterpolationMode::CATMULL_ROM);

        EXPECT_EQ(op.query_state("control_point_count"), 14.0);
    }

    TEST(PathOperatorTest, ExtractedVerticesAreTheControlPoints)
    {
        PathOperator op;
        op.initialize(points(6));

        EXPECT_EQ(op.extract_vertices().size(), 6U);
    }

    TEST(PathOperatorTest, SetVerticesKeepsEveryPathAndSplitsBySize)
    {
        PathOperator op;
        op.initialize_paths({ points(6), points(8) }, InterpolationMode::CATMULL_ROM);
        auto replacement = op.extract_vertices();
        for (auto& v : replacement) {
            v.position.x += 100.0F;
        }

        op.set_vertices(replacement);

        ASSERT_EQ(op.get_path_count(), 2U);
        EXPECT_EQ(op.get_path(0)->get_control_point_count(), 6U);
        EXPECT_EQ(op.get_path(1)->get_control_point_count(), 8U);
        EXPECT_TRUE(same_positions(op.extract_vertices(), replacement));
    }

    TEST(PathOperatorTest, SetVerticesWithADifferentCountBecomesOnePath)
    {
        PathOperator op;
        op.initialize_paths({ points(6), points(8) }, InterpolationMode::CATMULL_ROM);

        op.set_vertices(points(9));

        ASSERT_EQ(op.get_path_count(), 1U);
        EXPECT_EQ(op.get_path(0)->get_control_point_count(), 9U);
    }

    TEST(PathOperatorTest, SetVerticesOnAnEmptyOperatorInitializesIt)
    {
        PathOperator op;

        op.set_vertices(points(6));

        ASSERT_EQ(op.get_path_count(), 1U);
        EXPECT_EQ(op.get_path(0)->get_control_point_count(), 6U);
    }

    TEST(PathOperatorTest, ReportsItsTypeNames)
    {
        PathOperator op;

        EXPECT_EQ(op.get_type_name(), "Path");
        EXPECT_STREQ(op.get_vertex_type_name(), "PathVertex");
    }

    TEST(TopologyOperatorTest, InitializeBuildsOneTopologyFromThePoints)
    {
        TopologyOperator op;
        const auto input = points(8);

        op.initialize(input);

        ASSERT_EQ(op.get_topology_count(), 1U);
        EXPECT_EQ(op.get_topology(0)->get_point_count(), 8U);
        EXPECT_EQ(op.get_point_count(), 8U);
        EXPECT_TRUE(same_positions(op.extract_vertices(), input));
    }

    TEST(TopologyOperatorTest, InitializeReplacesAndEmptyInitializeKeepsWhatIsThere)
    {
        TopologyOperator op;
        op.initialize(points(8));

        op.initialize(points(5));
        ASSERT_EQ(op.get_topology_count(), 1U);
        EXPECT_EQ(op.get_topology(0)->get_point_count(), 5U);

        op.initialize({});
        ASSERT_EQ(op.get_topology_count(), 1U);
        EXPECT_EQ(op.get_topology(0)->get_point_count(), 5U);
    }

    TEST(TopologyOperatorTest, InitializeTopologiesKeepsListOrder)
    {
        TopologyOperator op;

        op.initialize_topologies({ points(8), points(5), points(6) }, ProximityMode::K_NEAREST);

        ASSERT_EQ(op.get_topology_count(), 3U);
        EXPECT_EQ(op.get_topology(0)->get_point_count(), 8U);
        EXPECT_EQ(op.get_topology(1)->get_point_count(), 5U);
        EXPECT_EQ(op.get_topology(2)->get_point_count(), 6U);
        EXPECT_EQ(op.get_point_count(), 19U);
    }

    TEST(TopologyOperatorTest, AddTopologyAppendsAndIgnoresAnEmptySet)
    {
        TopologyOperator op;
        op.initialize(points(8));

        op.add_topology({}, ProximityMode::K_NEAREST);
        EXPECT_EQ(op.get_topology_count(), 1U);

        op.add_topology(points(5), ProximityMode::K_NEAREST);
        EXPECT_EQ(op.get_topology_count(), 2U);
    }

    TEST(TopologyOperatorTest, ExtractedVerticesConcatenateEveryTopologyInOrder)
    {
        TopologyOperator op;
        const auto first = points(8);
        const auto second = points(5, 20.0F);
        op.initialize_topologies({ first, second }, ProximityMode::K_NEAREST);

        auto expected = first;
        expected.insert(expected.end(), second.begin(), second.end());

        EXPECT_TRUE(same_positions(op.extract_vertices(), expected));
    }

    TEST(TopologyOperatorTest, OutOfRangeTopologyIsNull)
    {
        TopologyOperator op;
        op.initialize(points(8));

        EXPECT_EQ(op.get_topology(1), nullptr);
    }

    TEST(TopologyOperatorTest, KNeighborsParameterReachesEveryTopology)
    {
        TopologyOperator op;
        op.initialize_topologies({ points(8), points(8, 20.0F) }, ProximityMode::K_NEAREST);

        op.set_parameter("k_neighbors", 1.0);
        op.process(0.0F);
        const auto sparse = connections_per_topology(op);

        op.set_parameter("k_neighbors", 4.0);
        op.process(0.0F);
        const auto dense = connections_per_topology(op);

        ASSERT_EQ(sparse.size(), 2U);
        EXPECT_GT(dense[0], sparse[0]);
        EXPECT_GT(dense[1], sparse[1]);
    }

    TEST(TopologyOperatorTest, ConnectionRadiusParameterReachesEveryTopology)
    {
        TopologyOperator op;
        op.initialize_topologies({ points(8), points(8, 20.0F) }, ProximityMode::RADIUS_THRESHOLD);

        op.set_parameter("connection_radius", 0.1);
        op.process(0.0F);
        const auto none = connections_per_topology(op);

        op.set_connection_radius(50.0F);
        op.process(0.0F);
        const auto many = connections_per_topology(op);

        EXPECT_EQ(none[0], 0U);
        EXPECT_EQ(none[1], 0U);
        EXPECT_GT(many[0], 0U);
        EXPECT_GT(many[1], 0U);
    }

    TEST(TopologyOperatorTest, LineThicknessAndColorReachEveryTopology)
    {
        TopologyOperator op;
        op.initialize_topologies({ points(8), points(8, 20.0F) }, ProximityMode::K_NEAREST);

        op.set_parameter("line_thickness", 6.0);
        op.set_global_line_color({ 0.2F, 0.4F, 0.6F });
        op.process(0.0F);

        for (uint32_t g = 0; g < 2; ++g) {
            const auto bytes = op.get_vertex_data_for_collection(g);
            ASSERT_GT(vertices_in(bytes), 0U) << "topology " << g;
            EXPECT_FLOAT_EQ(line_at(bytes, 0).thickness, 6.0F) << "topology " << g;
            EXPECT_TRUE(near_color(line_at(bytes, 0).color, { 0.2F, 0.4F, 0.6F })) << "topology " << g;
        }
    }

    TEST(TopologyOperatorTest, UnknownParameterChangesNothing)
    {
        TopologyOperator op;
        op.initialize(points(8));
        const auto before = copy_of(op.get_vertex_data());

        op.set_parameter("no_such_parameter", 3.0);
        op.process(0.0F);

        EXPECT_EQ(copy_of(op.get_vertex_data()), before);
    }

    TEST(TopologyOperatorTest, QueryStateReportsTotals)
    {
        TopologyOperator op;
        op.initialize_topologies({ points(8), points(5) }, ProximityMode::K_NEAREST);

        const auto per = connections_per_topology(op);

        EXPECT_EQ(op.query_state("point_count"), 13.0);
        EXPECT_EQ(op.query_state("topology_count"), 2.0);
        EXPECT_EQ(op.query_state("connection_count"), static_cast<double>(per[0] + per[1]));
        EXPECT_FALSE(op.query_state("no_such_state").has_value());
    }

    TEST(TopologyOperatorTest, SetVerticesKeepsEveryTopologyAndSplitsBySize)
    {
        TopologyOperator op;
        op.initialize_topologies({ points(8), points(5) }, ProximityMode::K_NEAREST);
        auto replacement = op.extract_vertices();
        for (auto& v : replacement) {
            v.position.x += 100.0F;
            v.color = { 0.1F, 0.2F, 0.3F };
        }

        op.set_vertices(replacement);

        ASSERT_EQ(op.get_topology_count(), 2U);
        EXPECT_EQ(op.get_topology(0)->get_point_count(), 8U);
        EXPECT_EQ(op.get_topology(1)->get_point_count(), 5U);
        EXPECT_TRUE(same_positions(op.extract_vertices(), replacement));
        EXPECT_TRUE(near_color(op.extract_vertices()[0].color, { 0.1F, 0.2F, 0.3F }));
    }

    TEST(TopologyOperatorTest, SetVerticesWithADifferentCountBecomesOneTopology)
    {
        TopologyOperator op;
        op.initialize_topologies({ points(8), points(5) }, ProximityMode::K_NEAREST);

        op.set_vertices(points(9));

        ASSERT_EQ(op.get_topology_count(), 1U);
        EXPECT_EQ(op.get_topology(0)->get_point_count(), 9U);
    }

    TEST(TopologyOperatorTest, SetVerticesOnAnEmptyOperatorInitializesIt)
    {
        TopologyOperator op;

        op.set_vertices(points(6));

        ASSERT_EQ(op.get_topology_count(), 1U);
        EXPECT_EQ(op.get_topology(0)->get_point_count(), 6U);
    }

    TEST(TopologyOperatorTest, ReportsItsTypeNames)
    {
        TopologyOperator op;

        EXPECT_EQ(op.get_type_name(), "Topology");
        EXPECT_STREQ(op.get_vertex_type_name(), "PathVertex");
    }

    class InitializedCloud : public PointCloudNetwork {
    public:
        explicit InitializedCloud(size_t count)
            : PointCloudNetwork(count)
        {
            initialize();
        }
    };

    TEST(PointCloudNetworkTest, DefaultConstructedHasNoOperatorAndNoPoints)
    {
        PointCloudNetwork net;

        net.initialize();
        EXPECT_NO_THROW(net.process_batch(4));

        EXPECT_FALSE(net.has_operator());
        EXPECT_EQ(net.get_node_count(), 0U);
        EXPECT_TRUE(net.get_vertices().empty());
        EXPECT_EQ(net.get_metadata().at("operator"), "none");
    }

    TEST(PointCloudNetworkTest, CountConstructorBuildsATopologyOperatorOnInitialize)
    {
        InitializedCloud net(12);

        ASSERT_TRUE(net.has_operator());
        EXPECT_EQ(net.get_operator()->get_type_name(), "Topology");
        EXPECT_EQ(net.get_node_count(), 12U);
        EXPECT_EQ(net.get_vertices().size(), 12U);
    }

    TEST(PointCloudNetworkTest, ConstructedButNotInitializedHasNoOperator)
    {
        PointCloudNetwork net(12);

        EXPECT_FALSE(net.has_operator());
        EXPECT_EQ(net.get_node_count(), 0U);
    }

    TEST(PointCloudNetworkTest, InitializeTwiceKeepsTheSameOperator)
    {
        InitializedCloud net(12);
        const auto* first = net.get_operator();

        net.initialize();

        EXPECT_EQ(net.get_operator(), first);
    }

    TEST(PointCloudNetworkTest, DefaultBoundsAreMinusOneToOne)
    {
        InitializedCloud net(12);

        EXPECT_EQ(net.get_bounds().min, glm::vec3(-1.0F));
        EXPECT_EQ(net.get_bounds().max, glm::vec3(1.0F));
    }

    TEST(PointCloudNetworkTest, GeneratedPointsStayInsideTheBounds)
    {
        const glm::vec3 lo(-2.0F, 0.0F, 1.0F);
        const glm::vec3 hi(2.0F, 3.0F, 4.0F);
        PointCloudNetwork net(64, lo, hi);
        net.initialize();

        for (const auto& v : net.get_vertices()) {
            EXPECT_GE(v.position.x, lo.x);
            EXPECT_LE(v.position.x, hi.x);
            EXPECT_GE(v.position.y, lo.y);
            EXPECT_LE(v.position.y, hi.y);
            EXPECT_GE(v.position.z, lo.z);
            EXPECT_LE(v.position.z, hi.z);
        }
    }

    TEST(PointCloudNetworkTest, NodeOutputIsTheIndexWithinRange)
    {
        InitializedCloud net(12);

        EXPECT_EQ(net.get_node_output(3), 3.0);
        EXPECT_EQ(net.get_node_output(11), 11.0);
        EXPECT_FALSE(net.get_node_output(12).has_value());
    }

    TEST(PointCloudNetworkTest, MetadataDescribesTheTopologyOperator)
    {
        InitializedCloud net(12);

        const auto meta = net.get_metadata();

        EXPECT_EQ(meta.at("point_count"), "12");
        EXPECT_EQ(meta.at("operator"), "Topology");
        EXPECT_TRUE(meta.contains("connection_count"));
        EXPECT_EQ(meta.at("topology_count"), "1");
    }

    TEST(PointCloudNetworkTest, MetadataDescribesThePathOperator)
    {
        InitializedCloud net(12);
        net.create_operator<PathOperator>();

        const auto meta = net.get_metadata();

        EXPECT_EQ(meta.at("operator"), "Path");
        EXPECT_TRUE(meta.contains("vertex_count"));
        EXPECT_EQ(meta.at("path_count"), "1");
    }

    TEST(PointCloudNetworkTest, NullOperatorIsIgnored)
    {
        InitializedCloud net(12);

        net.set_operator(nullptr);

        EXPECT_EQ(net.get_operator()->get_type_name(), "Topology");
        EXPECT_EQ(net.get_node_count(), 12U);
    }

    TEST(PointCloudNetworkTest, OperatorWithoutLineVertexStorageIsRejected)
    {
        InitializedCloud net(12);

        net.set_operator(std::make_unique<PhysicsOperator>());

        EXPECT_EQ(net.get_operator()->get_type_name(), "Topology");
        EXPECT_EQ(net.get_node_count(), 12U);
    }

    TEST(PointCloudNetworkTest, EmptyNetworkTakesItsPointsFromSetVertices)
    {
        PointCloudNetwork net;
        net.create_operator<TopologyOperator>();
        ASSERT_EQ(net.get_node_count(), 0U);

        net.set_vertices(points(7));

        EXPECT_EQ(net.get_node_count(), 7U);
        EXPECT_EQ(net.get_vertices().size(), 7U);
    }

    TEST(PointCloudNetworkTest, VerticesSetBeforeAnyOperatorSeedTheFirstOne)
    {
        PointCloudNetwork net;
        net.set_vertices(points(5));
        EXPECT_FALSE(net.has_operator());

        net.create_operator<TopologyOperator>();

        EXPECT_EQ(net.get_node_count(), 5U);
        EXPECT_TRUE(same_positions(net.get_vertices(), points(5)));
    }

    TEST(PointCloudNetworkTest, ResetKeepsTheCountAndTheOperator)
    {
        InitializedCloud net(12);
        const auto* op = net.get_operator();

        net.reset();

        EXPECT_EQ(net.get_operator(), op);
        EXPECT_EQ(net.get_node_count(), 12U);
    }

    TEST(PointCloudNetworkTest, ReinitializeChangesCountAndBounds)
    {
        InitializedCloud net(12);

        net.reinitialize(30, glm::vec3(-5.0F), glm::vec3(5.0F), Kinesis::SpatialDistribution::RANDOM_CUBE);

        EXPECT_EQ(net.get_node_count(), 30U);
        EXPECT_EQ(net.get_bounds().max, glm::vec3(5.0F));
    }

    TEST(PointCloudNetworkTest, ReinitializeBuildsAnOperatorWhenThereIsNone)
    {
        PointCloudNetwork net;

        net.reinitialize(9, glm::vec3(-1.0F), glm::vec3(1.0F), Kinesis::SpatialDistribution::RANDOM_CUBE);

        ASSERT_TRUE(net.has_operator());
        EXPECT_EQ(net.get_node_count(), 9U);
    }

    enum class Kind : uint8_t { Topology,
        Field,
        Path };

    struct Hop {
        const char* name;
        Kind from;
        Kind to;
    };

    void install(PointCloudNetwork& net, Kind kind)
    {
        switch (kind) {
        case Kind::Topology:
            net.create_operator<TopologyOperator>();
            break;
        case Kind::Field:
            net.create_operator<FieldOperator>();
            break;
        case Kind::Path:
            net.create_operator<PathOperator>();
            break;
        }
    }

    const char* type_name_of(Kind kind)
    {
        switch (kind) {
        case Kind::Topology:
            return "Topology";
        case Kind::Field:
            return "Field";
        case Kind::Path:
            return "Path";
        }
        return "";
    }

    class PointCloudOperatorSwapTest : public ::testing::TestWithParam<Hop> { };

    TEST_P(PointCloudOperatorSwapTest, SwapCarriesTheTwelvePointsAcross)
    {
        InitializedCloud net(12);
        install(net, GetParam().from);
        const auto before = net.get_vertices();
        ASSERT_EQ(before.size(), 12U);

        install(net, GetParam().to);

        EXPECT_EQ(net.get_operator()->get_type_name(), type_name_of(GetParam().to));
        if (GetParam().to == Kind::Path) {
            auto* path = dynamic_cast<PathOperator*>(net.get_operator());
            ASSERT_NE(path, nullptr);
            ASSERT_EQ(path->get_path_count(), 1U);
            EXPECT_EQ(path->get_path(0)->get_control_point_count(), 12U);
        } else {
            EXPECT_TRUE(same_positions(net.get_vertices(), before));
        }
    }

    INSTANTIATE_TEST_SUITE_P(
        PointCloudNetworks,
        PointCloudOperatorSwapTest,
        ::testing::Values(
            Hop { "TopologyToField", Kind::Topology, Kind::Field },
            Hop { "TopologyToPath", Kind::Topology, Kind::Path },
            Hop { "FieldToTopology", Kind::Field, Kind::Topology },
            Hop { "FieldToPath", Kind::Field, Kind::Path },
            Hop { "TopologyToTopology", Kind::Topology, Kind::Topology },
            Hop { "FieldToField", Kind::Field, Kind::Field }),
        [](const ::testing::TestParamInfo<Hop>& info) { return std::string(info.param.name); });

    TEST(PointCloudNetworkSwapTest, PathToTopologyCarriesTheTwelveControlPoints)
    {
        InitializedCloud net(12);
        net.create_operator<PathOperator>();

        net.create_operator<TopologyOperator>();

        EXPECT_EQ(net.get_vertices().size(), 12U);
    }

    TEST(PointCloudNetworkSwapTest, PathToFieldCarriesTheTwelveControlPoints)
    {
        InitializedCloud net(12);
        net.create_operator<PathOperator>();

        net.create_operator<FieldOperator>();

        EXPECT_EQ(net.get_vertices().size(), 12U);
    }

    TEST(PointCloudNetworkForwardingTest, KNeighborsAndRadiusReachTheTopologyOperator)
    {
        InitializedCloud net(20);
        auto* topo = dynamic_cast<TopologyOperator*>(net.get_operator());
        ASSERT_NE(topo, nullptr);

        net.set_k_neighbors(1);
        net.process_batch(1);
        const auto sparse = topo->get_topology(0)->get_connection_count();

        net.set_k_neighbors(6);
        net.process_batch(1);
        const auto dense = topo->get_topology(0)->get_connection_count();

        EXPECT_GT(dense, sparse);
    }

    TEST(PointCloudNetworkForwardingTest, LineThicknessAndColorReachThePathOperator)
    {
        InitializedCloud net(12);
        net.create_operator<PathOperator>();

        net.set_line_thickness(6.0F);
        net.set_line_color({ 0.2F, 0.4F, 0.6F });
        net.process_batch(1);

        auto* path = dynamic_cast<PathOperator*>(net.get_operator());
        const auto bytes = path->get_vertex_data();
        ASSERT_GT(vertices_in(bytes), 0U);
        for (size_t i = 0; i < vertices_in(bytes); ++i) {
            EXPECT_FLOAT_EQ(line_at(bytes, i).thickness, 6.0F) << "vertex " << i;
            EXPECT_TRUE(near_color(line_at(bytes, i).color, { 0.2F, 0.4F, 0.6F })) << "vertex " << i;
        }
    }

    TEST(PointCloudNetworkForwardingTest, LineThicknessAndColorReachTheTopologyOperator)
    {
        InitializedCloud net(12);

        net.set_line_thickness(6.0F);
        net.set_line_color({ 0.2F, 0.4F, 0.6F });
        net.process_batch(1);

        auto* topo = dynamic_cast<TopologyOperator*>(net.get_operator());
        const auto bytes = topo->get_vertex_data();
        ASSERT_GT(vertices_in(bytes), 0U);
        EXPECT_FLOAT_EQ(line_at(bytes, 0).thickness, 6.0F);
        EXPECT_TRUE(near_color(line_at(bytes, 0).color, { 0.2F, 0.4F, 0.6F }));
    }

    TEST(PointCloudNetworkForwardingTest, SamplesPerSegmentAndTensionReachThePathOperator)
    {
        InitializedCloud net(12);
        net.create_operator<PathOperator>();
        auto* path = dynamic_cast<PathOperator*>(net.get_operator());
        const auto dense = path->get_vertex_count();
        const auto shape = copy_of(path->get_vertex_data());

        net.set_samples_per_segment(4);
        net.process_batch(1);
        EXPECT_LT(path->get_vertex_count(), dense);

        net.set_samples_per_segment(32);
        net.set_tension(0.0);
        net.process_batch(1);
        EXPECT_NE(copy_of(path->get_vertex_data()), shape);
    }

    TEST(PointCloudNetworkForwardingTest, SettersForAnotherOperatorAreHarmless)
    {
        InitializedCloud net(12);
        net.create_operator<FieldOperator>();

        EXPECT_NO_THROW(net.set_connection_radius(1.0F));
        EXPECT_NO_THROW(net.set_k_neighbors(3));
        EXPECT_NO_THROW(net.set_line_thickness(4.0F));
        EXPECT_NO_THROW(net.set_line_color({ 1.0F, 0.0F, 0.0F }));
        EXPECT_NO_THROW(net.set_samples_per_segment(8));
        EXPECT_NO_THROW(net.set_tension(0.2));
        EXPECT_EQ(net.get_node_count(), 12U);
    }

    TEST(PointCloudNetworkGradientTest, LinearGradientRunsFromStartToEnd)
    {
        InitializedCloud net(5);
        net.set_vertices(points(5));

        net.apply_color_gradient({ 1.0F, 0.0F, 0.0F }, { 0.0F, 0.0F, 1.0F });

        const auto v = net.get_vertices();
        ASSERT_EQ(v.size(), 5U);
        EXPECT_TRUE(near_color(v[0].color, { 1.0F, 0.0F, 0.0F }));
        EXPECT_TRUE(near_color(v[2].color, { 0.5F, 0.0F, 0.5F }));
        EXPECT_TRUE(near_color(v[4].color, { 0.0F, 0.0F, 1.0F }));
    }

    TEST(PointCloudNetworkGradientTest, RadialGradientFollowsDistanceFromTheCentre)
    {
        InitializedCloud net(3);
        std::vector<LineVertex> line(3);
        line[0].position = { 0.0F, 0.0F, 0.0F };
        line[1].position = { 1.0F, 0.0F, 0.0F };
        line[2].position = { 3.0F, 0.0F, 0.0F };
        net.set_vertices(line);

        net.apply_radial_gradient({ 1.0F, 1.0F, 1.0F }, { 0.0F, 0.0F, 0.0F });

        const auto v = net.get_vertices();
        ASSERT_EQ(v.size(), 3U);
        EXPECT_TRUE(near_color(v[0].color, { 1.0F, 1.0F, 1.0F }));
        EXPECT_TRUE(near_color(v[1].color, glm::vec3(2.0F / 3.0F)));
        EXPECT_TRUE(near_color(v[2].color, { 0.0F, 0.0F, 0.0F }));
    }

    TEST(PointCloudNetworkGradientTest, GradientKeepsEveryTopology)
    {
        InitializedCloud net(8);
        auto* topo = dynamic_cast<TopologyOperator*>(net.get_operator());
        topo->add_topology(points(6, 20.0F), ProximityMode::K_NEAREST);
        ASSERT_EQ(topo->get_topology_count(), 2U);

        net.apply_color_gradient({ 1.0F, 0.0F, 0.0F }, { 0.0F, 0.0F, 1.0F });

        EXPECT_EQ(topo->get_topology_count(), 2U);
    }

    TEST(PointCloudNetworkGradientTest, GradientKeepsEveryPath)
    {
        InitializedCloud net(12);
        net.create_operator<PathOperator>();
        auto* path = dynamic_cast<PathOperator*>(net.get_operator());
        path->add_path(points(8, 20.0F), InterpolationMode::CATMULL_ROM);
        ASSERT_EQ(path->get_path_count(), 2U);

        net.apply_radial_gradient({ 1.0F, 0.0F, 0.0F }, { 0.0F, 0.0F, 1.0F });

        EXPECT_EQ(path->get_path_count(), 2U);
        EXPECT_EQ(path->get_path(0)->get_control_point_count(), 12U);
        EXPECT_EQ(path->get_path(1)->get_control_point_count(), 8U);
    }

    TEST(PointCloudNetworkGradientTest, GradientColorsAreVisibleInTheTopologyVertexData)
    {
        InitializedCloud net(12);

        net.apply_color_gradient({ 1.0F, 0.0F, 0.0F }, { 0.0F, 0.0F, 1.0F });
        net.process_batch(1);

        auto* topo = dynamic_cast<TopologyOperator*>(net.get_operator());
        const auto bytes = topo->get_vertex_data();
        ASSERT_GT(vertices_in(bytes), 1U);
        bool varied = false;
        for (size_t i = 1; i < vertices_in(bytes); ++i) {
            varied = varied || !near_color(line_at(bytes, i).color, line_at(bytes, 0).color);
        }
        EXPECT_TRUE(varied);
    }

    TEST(PointCloudNetworkGradientTest, GradientKeepsThePathControlPoints)
    {
        InitializedCloud net(12);
        net.create_operator<PathOperator>();
        auto* path = dynamic_cast<PathOperator*>(net.get_operator());
        ASSERT_EQ(path->get_path(0)->get_control_point_count(), 12U);

        net.apply_color_gradient({ 1.0F, 0.0F, 0.0F }, { 0.0F, 0.0F, 1.0F });

        EXPECT_EQ(path->get_path(0)->get_control_point_count(), 12U);
    }

    TEST(PointCloudNetworkGradientTest, SingleVertexTakesTheStartColour)
    {
        PointCloudNetwork net;
        net.create_operator<TopologyOperator>();
        net.set_vertices(points(1));

        net.apply_color_gradient({ 1.0F, 0.0F, 0.0F }, { 0.0F, 0.0F, 1.0F });

        ASSERT_EQ(net.get_vertices().size(), 1U);
        EXPECT_TRUE(near_color(net.get_vertices()[0].color, { 1.0F, 0.0F, 0.0F }));
    }

    TEST(PointCloudNetworkTest, UpdateVertexLeavesTheCloudUntouched)
    {
        InitializedCloud net(5);
        net.set_vertices(points(5));

        LineVertex moved;
        moved.position = { 9.0F, 9.0F, 9.0F };
        net.update_vertex(2, moved);

        EXPECT_TRUE(same_positions(net.get_vertices(), points(5)));
    }

    TEST(PointCloudNetworkMappingTest, BroadcastSourceDrivesAnOperatorParameter)
    {
        InitializedCloud net(12);
        net.create_operator<PathOperator>();
        auto source = std::make_shared<Impulse>(0.5);
        source->set_amplitude(3.0);
        source->process_sample(0.0);

        net.map_parameter("thickness", source, MappingMode::BROADCAST);
        net.process_batch(1);

        auto* path = dynamic_cast<PathOperator*>(net.get_operator());
        const auto bytes = path->get_vertex_data();
        ASSERT_GT(vertices_in(bytes), 0U);
        EXPECT_FLOAT_EQ(line_at(bytes, 0).thickness, 3.0F);
    }

    TEST(PointCloudNetworkMappingTest, UnmappingStopsTheSourceDrivingTheParameter)
    {
        InitializedCloud net(12);
        net.create_operator<PathOperator>();
        auto source = std::make_shared<Impulse>(0.5);
        source->set_amplitude(3.0);
        source->process_sample(0.0);
        net.map_parameter("thickness", source, MappingMode::BROADCAST);
        net.process_batch(1);

        net.unmap_parameter("thickness");
        net.set_line_thickness(2.0F);
        net.process_batch(1);

        auto* path = dynamic_cast<PathOperator*>(net.get_operator());
        EXPECT_FLOAT_EQ(line_at(path->get_vertex_data(), 0).thickness, 2.0F);
    }

    TEST(PointCloudNetworkMappingTest, OneToOneMappingLeavesATopologyOperatorUntouched)
    {
        InitializedCloud net(12);
        net.process_batch(1);
        auto* topo = dynamic_cast<TopologyOperator*>(net.get_operator());
        const auto bytes_before = copy_of(topo->get_vertex_data());
        const auto points_before = net.get_vertices();
        auto source = std::make_shared<InitializedCloud>(12);

        net.map_parameter("color", source);
        net.map_parameter("size", source);
        net.process_batch(1);

        EXPECT_EQ(copy_of(topo->get_vertex_data()), bytes_before);
        const auto points_after = net.get_vertices();
        for (size_t i = 0; i < points_before.size(); ++i) {
            EXPECT_TRUE(near_color(points_after[i].color, points_before[i].color)) << "point " << i;
            EXPECT_FLOAT_EQ(points_after[i].thickness, points_before[i].thickness) << "point " << i;
        }
    }

    TEST(PointCloudNetworkMappingTest, OneToOneMappingLeavesAPathOperatorUntouched)
    {
        InitializedCloud net(12);
        net.create_operator<PathOperator>();
        net.process_batch(1);
        auto* path = dynamic_cast<PathOperator*>(net.get_operator());
        const auto bytes_before = copy_of(path->get_vertex_data());
        auto source = std::make_shared<InitializedCloud>(path->get_point_count());

        net.map_parameter("color", source);
        net.process_batch(1);

        EXPECT_EQ(copy_of(path->get_vertex_data()), bytes_before);
    }

} // namespace

} // namespace MayaFlux::Test
