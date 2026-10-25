#include "gtest/gtest.h"

#include "MayaFlux/Nodes/Graphics/TopologyGeneratorNode.hpp"

namespace MayaFlux::Test {

namespace {

    using Kinesis::InterpolationMode;
    using Kinesis::ProximityMode;
    using Nodes::LineVertex;
    using Nodes::GpuSync::GeometryWriterNode;
    using Nodes::GpuSync::TopologyGeneratorNode;

    LineVertex make_vertex(const glm::vec3& position, float thickness = 2.0F, const glm::vec3& color = glm::vec3(1.0F))
    {
        LineVertex v;
        v.position = position;
        v.color = glm::vec4(color, 1.0F);
        v.thickness = thickness;
        return v;
    }

    std::vector<LineVertex> collinear_points(uint32_t count, float spacing = 1.0F)
    {
        std::vector<LineVertex> pts;
        pts.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            const auto f = static_cast<float>(i);
            pts.push_back(make_vertex({ f * spacing, 0.0F, 0.0F }, 1.0F + f, { 0.1F * (f + 1.0F), 0.0F, 0.0F }));
        }
        return pts;
    }

    std::vector<LineVertex> grid_points()
    {
        std::vector<LineVertex> pts;
        for (int y = 0; y < 2; ++y) {
            for (int x = 0; x < 3; ++x) {
                pts.push_back(make_vertex({ static_cast<float>(x), static_cast<float>(y), 0.0F }));
            }
        }
        return pts;
    }

    void add_in_order(TopologyGeneratorNode& node, const std::vector<LineVertex>& pts)
    {
        for (const auto& p : pts) {
            node.add_point(p);
        }
    }

    uint32_t uploaded_count(const TopologyGeneratorNode& node)
    {
        return node.GeometryWriterNode::get_vertex_count();
    }

    std::vector<LineVertex> uploaded_vertices(const TopologyGeneratorNode& node)
    {
        std::vector<LineVertex> out;
        out.reserve(uploaded_count(node));
        for (uint32_t i = 0; i < uploaded_count(node); ++i) {
            out.push_back(node.get_vertex_typed<LineVertex>(i));
        }
        return out;
    }

    ::testing::AssertionResult near_vec(const glm::vec3& a, const glm::vec3& b, float tol = 1e-4F)
    {
        if (glm::length(a - b) <= tol) {
            return ::testing::AssertionSuccess();
        }
        return ::testing::AssertionFailure()
            << "(" << a.x << ", " << a.y << ", " << a.z << ") != ("
            << b.x << ", " << b.y << ", " << b.z << ")";
    }

    class LineTopology : public TopologyGeneratorNode {
    public:
        explicit LineTopology(size_t max_points = 16)
            : TopologyGeneratorNode(ProximityMode::SEQUENTIAL, true, max_points)
        {
            set_path_interpolation_mode(InterpolationMode::LINEAR);
        }
    };

    class TopologyGeneratorNodeTest : public ::testing::Test {
    };

    TEST_F(TopologyGeneratorNodeTest, ConstructionStartsEmptyWithLineListTopology)
    {
        TopologyGeneratorNode node;

        EXPECT_EQ(node.get_point_count(), 0U);
        EXPECT_EQ(node.get_connection_count(), 0U);
        EXPECT_EQ(node.get_vertex_count(), 0U);
        EXPECT_EQ(uploaded_count(node), 0U);
        EXPECT_EQ(node.get_vertex_stride(), sizeof(LineVertex));
        EXPECT_EQ(node.get_primitive_topology(), Portal::Graphics::PrimitiveTopology::LINE_LIST);
    }

    TEST_F(TopologyGeneratorNodeTest, ConstructionLayoutDescribesNoVerticesYet)
    {
        TopologyGeneratorNode node;
        auto layout = node.get_vertex_layout();

        ASSERT_TRUE(layout.has_value());
        EXPECT_EQ(layout->stride_bytes, sizeof(LineVertex));
        EXPECT_EQ(layout->vertex_count, 0U);
    }

    TEST_F(TopologyGeneratorNodeTest, DefaultLineColourIsWhiteAndNotForced)
    {
        TopologyGeneratorNode node;

        EXPECT_EQ(node.get_line_color(), glm::vec4(glm::vec3(1.0F), 1.F));
        EXPECT_FALSE(node.should_force_uniform_color());
    }

    TEST_F(TopologyGeneratorNodeTest, PointsAreHeldNewestFirst)
    {
        LineTopology node;
        add_in_order(node, collinear_points(3));

        ASSERT_EQ(node.get_point_count(), 3U);
        EXPECT_TRUE(near_vec(node.get_point(0).position, { 2, 0, 0 }));
        EXPECT_TRUE(near_vec(node.get_point(2).position, { 0, 0, 0 }));
        auto all = node.get_points();
        ASSERT_EQ(all.size(), 3U);
        EXPECT_TRUE(near_vec(all[1].position, { 1, 0, 0 }));
    }

    TEST_F(TopologyGeneratorNodeTest, ExceedingMaxPointsDropsTheOldest)
    {
        LineTopology node(3);
        add_in_order(node, collinear_points(5));

        ASSERT_EQ(node.get_point_count(), 3U);
        EXPECT_TRUE(near_vec(node.get_point(0).position, { 4, 0, 0 }));
        EXPECT_TRUE(near_vec(node.get_point(2).position, { 2, 0, 0 }));
    }

    TEST_F(TopologyGeneratorNodeTest, AddPointsMatchesAddingOneByOne)
    {
        LineTopology one_by_one;
        add_in_order(one_by_one, collinear_points(4));

        LineTopology spanned;
        const auto pts = collinear_points(4);
        spanned.add_points(pts);

        ASSERT_EQ(spanned.get_point_count(), 4U);
        for (size_t i = 0; i < 4; ++i) {
            EXPECT_TRUE(near_vec(spanned.get_point(i).position, one_by_one.get_point(i).position));
        }
    }

    TEST_F(TopologyGeneratorNodeTest, AddingNoPointsChangesNothing)
    {
        LineTopology node;
        add_in_order(node, collinear_points(2));
        node.compute_frame();
        node.clear_gpu_update_flag();

        node.add_points(std::span<const LineVertex> {});
        node.compute_frame();

        EXPECT_EQ(node.get_point_count(), 2U);
        EXPECT_FALSE(node.needs_gpu_update());
    }

    TEST_F(TopologyGeneratorNodeTest, SetPointsMatchesAddingInOrder)
    {
        LineTopology added;
        add_in_order(added, collinear_points(4));

        LineTopology set;
        set.set_points(collinear_points(4));

        ASSERT_EQ(set.get_point_count(), 4U);
        for (size_t i = 0; i < 4; ++i) {
            EXPECT_TRUE(near_vec(set.get_point(i).position, added.get_point(i).position));
        }
        EXPECT_EQ(set.get_connections(), added.get_connections());
    }

    TEST_F(TopologyGeneratorNodeTest, SetPointsBeyondMaxKeepsTheNewest)
    {
        LineTopology node(3);
        node.set_points(collinear_points(6));

        ASSERT_EQ(node.get_point_count(), 3U);
        EXPECT_TRUE(near_vec(node.get_point(0).position, { 5, 0, 0 }));
        EXPECT_TRUE(near_vec(node.get_point(2).position, { 3, 0, 0 }));
    }

    TEST_F(TopologyGeneratorNodeTest, RemovePointDropsThatPointAndReconnects)
    {
        LineTopology node;
        add_in_order(node, collinear_points(4));

        node.remove_point(1);

        ASSERT_EQ(node.get_point_count(), 3U);
        EXPECT_TRUE(near_vec(node.get_point(0).position, { 3, 0, 0 }));
        EXPECT_TRUE(near_vec(node.get_point(1).position, { 1, 0, 0 }));
        EXPECT_EQ(node.get_connection_count(), 2U);
    }

    TEST_F(TopologyGeneratorNodeTest, RemoveAndUpdateOutOfRangeChangeNothing)
    {
        LineTopology node;
        add_in_order(node, collinear_points(2));
        node.compute_frame();
        node.clear_gpu_update_flag();

        node.remove_point(2);
        node.update_point(9, make_vertex({ 9, 9, 9 }));
        node.compute_frame();

        EXPECT_EQ(node.get_point_count(), 2U);
        EXPECT_FALSE(node.needs_gpu_update());
    }

    TEST_F(TopologyGeneratorNodeTest, UpdatePointMovesItsConnectedVertices)
    {
        LineTopology node;
        add_in_order(node, collinear_points(3));
        node.compute_frame();

        node.update_point(1, make_vertex({ 1, 7, 0 }));
        node.compute_frame();

        const auto up = uploaded_vertices(node);
        ASSERT_EQ(up.size(), 4U);
        EXPECT_TRUE(near_vec(up[1].position, { 1, 7, 0 }));
        EXPECT_TRUE(near_vec(up[2].position, { 1, 7, 0 }));
    }

    TEST_F(TopologyGeneratorNodeTest, GetPointOutOfRangeReturnsAnOriginVertex)
    {
        LineTopology node;
        add_in_order(node, collinear_points(2));

        EXPECT_TRUE(near_vec(node.get_point(5).position, { 0, 0, 0 }));
    }

    TEST_F(TopologyGeneratorNodeTest, ClearEmptiesPointsConnectionsAndVertices)
    {
        LineTopology node;
        add_in_order(node, collinear_points(3));
        node.compute_frame();

        node.clear();
        node.compute_frame();

        EXPECT_EQ(node.get_point_count(), 0U);
        EXPECT_EQ(node.get_connection_count(), 0U);
        EXPECT_EQ(node.get_vertex_count(), 0U);
        EXPECT_EQ(uploaded_count(node), 0U);
    }

    TEST_F(TopologyGeneratorNodeTest, ClearResetsLayoutVertexCount)
    {
        LineTopology node;
        add_in_order(node, collinear_points(3));
        node.compute_frame();

        node.clear();
        node.compute_frame();

        ASSERT_TRUE(node.get_vertex_layout().has_value());
        EXPECT_EQ(node.get_vertex_layout()->vertex_count, 0U);
    }

    TEST_F(TopologyGeneratorNodeTest, SequentialConnectsConsecutiveStoredPoints)
    {
        LineTopology node;
        add_in_order(node, collinear_points(4));

        const std::vector<std::pair<size_t, size_t>> expected { { 0, 1 }, { 1, 2 }, { 2, 3 } };
        EXPECT_EQ(node.get_connections(), expected);
    }

    TEST_F(TopologyGeneratorNodeTest, FewerThanTwoPointsHaveNoConnections)
    {
        LineTopology node;
        EXPECT_EQ(node.get_connection_count(), 0U);

        node.add_point(make_vertex({ 1, 1, 1 }));
        node.compute_frame();

        EXPECT_EQ(node.get_connection_count(), 0U);
        EXPECT_EQ(uploaded_count(node), 0U);
    }

    TEST_F(TopologyGeneratorNodeTest, DirectEdgesUploadTwoVerticesPerConnectionAtTheirPoints)
    {
        LineTopology node;
        add_in_order(node, collinear_points(3));
        node.compute_frame();

        const auto up = uploaded_vertices(node);
        ASSERT_EQ(up.size(), 4U);
        EXPECT_TRUE(near_vec(up[0].position, { 2, 0, 0 }));
        EXPECT_TRUE(near_vec(up[1].position, { 1, 0, 0 }));
        EXPECT_TRUE(near_vec(up[2].position, { 1, 0, 0 }));
        EXPECT_TRUE(near_vec(up[3].position, { 0, 0, 0 }));
    }

    TEST_F(TopologyGeneratorNodeTest, DirectEdgesCarryTheEdgeVectorAsTangent)
    {
        LineTopology node;
        node.add_point(make_vertex({ 0, 0, 0 }));
        node.add_point(make_vertex({ 3, 4, 0 }));
        node.compute_frame();

        const auto up = uploaded_vertices(node);
        ASSERT_EQ(up.size(), 2U);
        EXPECT_TRUE(near_vec(up[0].tangent, { -3, -4, 0 }));
        EXPECT_TRUE(near_vec(up[1].tangent, { -3, -4, 0 }));
    }

    TEST_F(TopologyGeneratorNodeTest, DirectEdgesKeepPerPointColourAndThickness)
    {
        LineTopology node;
        add_in_order(node, collinear_points(2));
        node.compute_frame();

        const auto up = uploaded_vertices(node);
        EXPECT_TRUE(near_vec(up[0].color, { 0.2F, 0, 0 }));
        EXPECT_FLOAT_EQ(up[0].thickness, 2.0F);
        EXPECT_TRUE(near_vec(up[1].color, { 0.1F, 0, 0 }));
        EXPECT_FLOAT_EQ(up[1].thickness, 1.0F);
    }

    TEST_F(TopologyGeneratorNodeTest, DefaultSequentialModeDrawsASmoothPathThroughTheEndPoints)
    {
        TopologyGeneratorNode node;
        add_in_order(node, collinear_points(5));
        node.compute_frame();

        const auto up = uploaded_vertices(node);
        ASSERT_EQ(up.size(), 152U);

        float min_x = up.front().position.x;
        float max_x = min_x;
        for (const auto& v : up) {
            min_x = std::min(min_x, v.position.x);
            max_x = std::max(max_x, v.position.x);
        }
        SCOPED_TRACE(::testing::Message()
            << "x range [" << min_x << ", " << max_x << "], first x "
            << up[0].position.x << " " << up[1].position.x << " " << up[2].position.x
            << ", last x " << up[up.size() - 3].position.x << " "
            << up[up.size() - 2].position.x << " " << up[up.size() - 1].position.x
            << ", stored x " << node.get_point(0).position.x << " ... "
            << node.get_point(4).position.x);

        EXPECT_TRUE(near_vec(up.front().position, node.get_point(0).position));
        EXPECT_TRUE(near_vec(up.back().position, node.get_point(4).position));
        for (const auto& v : up) {
            ASSERT_TRUE(std::isfinite(v.position.x));
            EXPECT_NEAR(v.position.y, 0.0F, 1e-4F);
            EXPECT_NEAR(v.position.z, 0.0F, 1e-4F);
        }
    }

    TEST_F(TopologyGeneratorNodeTest, EvaluatorReferenceForTheSameFivePointPath)
    {
        Kinesis::CurveEvaluator evaluator(InterpolationMode::CATMULL_ROM, 0.5);
        const std::vector<double> controls {
            4, 0, 0,
            3, 0, 0,
            2, 0, 0,
            1, 0, 0,
            0, 0, 0
        };
        std::vector<double> out;
        std::vector<double> tangents;

        evaluator.evaluate_planar(controls, 3, 77, out, tangents);

        ASSERT_EQ(out.size(), 3U * 77U);
        double min_x = out[0];
        double max_x = out[0];
        for (size_t i = 0; i < 77; ++i) {
            min_x = std::min(min_x, out[i]);
            max_x = std::max(max_x, out[i]);
        }
        SCOPED_TRACE(::testing::Message()
            << "x range [" << min_x << ", " << max_x << "], x[0] " << out[0]
            << " x[1] " << out[1] << " x[38] " << out[38] << " x[75] " << out[75]
            << " x[76] " << out[76]);

        EXPECT_NEAR(out[0], 4.0, 1e-9);
        EXPECT_NEAR(out[76], 0.0, 1e-9);
        for (size_t i = 1; i < 77; ++i) {
            EXPECT_LE(out[i], out[i - 1] + 1e-9) << "sample " << i;
        }
    }

    TEST_F(TopologyGeneratorNodeTest, FewPointsFallBackToStraightEdgesInsteadOfFailing)
    {
        for (uint32_t n = 2; n <= 4; ++n) {
            SCOPED_TRACE(n);

            TopologyGeneratorNode node;
            add_in_order(node, collinear_points(n));

            EXPECT_NO_THROW(node.compute_frame());
            EXPECT_EQ(uploaded_count(node), 2U * (n - 1));
        }
    }

    TEST_F(TopologyGeneratorNodeTest, SmoothPathTakesOverOnceThereAreEnoughPoints)
    {
        TopologyGeneratorNode node;
        add_in_order(node, collinear_points(4));
        node.compute_frame();
        ASSERT_EQ(uploaded_count(node), 6U);

        node.add_point(make_vertex({ 4, 0, 0 }));
        node.compute_frame();

        EXPECT_GT(uploaded_count(node), 8U);
    }

    TEST_F(TopologyGeneratorNodeTest, SmoothPathSegmentsJoinEndToEnd)
    {
        TopologyGeneratorNode node;
        node.set_samples_per_segment(5);
        add_in_order(node, collinear_points(5));
        node.compute_frame();

        const auto up = uploaded_vertices(node);
        ASSERT_EQ(up.size(), 32U);
        for (size_t i = 1; i + 1 < up.size(); i += 2) {
            EXPECT_TRUE(near_vec(up[i].position, up[i + 1].position)) << "junction " << i;
        }
    }

    TEST_F(TopologyGeneratorNodeTest, SamplesPerSegmentSetsThePathResolution)
    {
        TopologyGeneratorNode node;
        add_in_order(node, collinear_points(5));
        node.set_samples_per_segment(5);
        node.compute_frame();

        EXPECT_EQ(uploaded_count(node), 32U);
    }

    TEST_F(TopologyGeneratorNodeTest, SamplesPerSegmentBelowTwoIsIgnored)
    {
        TopologyGeneratorNode node;
        add_in_order(node, collinear_points(5));
        node.set_samples_per_segment(5);
        node.set_samples_per_segment(1);
        node.compute_frame();

        EXPECT_EQ(uploaded_count(node), 32U);
    }

    TEST_F(TopologyGeneratorNodeTest, LinearPathInterpolationDrawsTheConnectionsDirectly)
    {
        TopologyGeneratorNode node;
        add_in_order(node, collinear_points(5));
        node.compute_frame();

        node.set_path_interpolation_mode(InterpolationMode::LINEAR);
        node.compute_frame();

        EXPECT_EQ(uploaded_count(node), 8U);
    }

    TEST_F(TopologyGeneratorNodeTest, ArcLengthReparameterizationKeepsTheCountAndStaysFinite)
    {
        TopologyGeneratorNode node;
        node.set_samples_per_segment(8);
        node.add_point(make_vertex({ 0, 0, 0 }));
        node.add_point(make_vertex({ 1, 1, 0 }));
        node.add_point(make_vertex({ 2, 0, 0 }));
        node.add_point(make_vertex({ 3, 1, 0 }));
        node.add_point(make_vertex({ 4, 0, 0 }));
        node.compute_frame();
        const auto count = uploaded_count(node);

        node.set_arc_length_reparameterization(true);
        node.compute_frame();

        ASSERT_EQ(uploaded_count(node), count);
        for (const auto& v : uploaded_vertices(node)) {
            EXPECT_TRUE(std::isfinite(v.position.x) && std::isfinite(v.position.y) && std::isfinite(v.position.z));
        }
    }

    TEST_F(TopologyGeneratorNodeTest, RadiusThresholdConnectsOnlyPointsWithinRadius)
    {
        TopologyGeneratorNode node(ProximityMode::RADIUS_THRESHOLD, true, 16);
        node.set_connection_radius(2.0F);
        node.add_point(make_vertex({ 0, 0, 0 }));
        node.add_point(make_vertex({ 1, 0, 0 }));
        node.add_point(make_vertex({ 10, 0, 0 }));

        ASSERT_EQ(node.get_connection_count(), 1U);
        const auto [a, b] = node.get_connections().front();
        const float xa = node.get_point(a).position.x;
        const float xb = node.get_point(b).position.x;
        EXPECT_FLOAT_EQ(std::min(xa, xb), 0.0F);
        EXPECT_FLOAT_EQ(std::max(xa, xb), 1.0F);
    }

    TEST_F(TopologyGeneratorNodeTest, ChangingTheRadiusReconnects)
    {
        TopologyGeneratorNode node(ProximityMode::RADIUS_THRESHOLD, true, 16);
        node.add_point(make_vertex({ 0, 0, 0 }));
        node.add_point(make_vertex({ 1, 0, 0 }));
        node.add_point(make_vertex({ 10, 0, 0 }));

        node.set_connection_radius(0.5F);
        EXPECT_EQ(node.get_connection_count(), 0U);

        node.set_connection_radius(2.0F);
        EXPECT_EQ(node.get_connection_count(), 1U);

        node.set_connection_radius(20.0F);
        EXPECT_EQ(node.get_connection_count(), 3U);
    }

    TEST_F(TopologyGeneratorNodeTest, RadiusIsIgnoredOutsideRadiusMode)
    {
        LineTopology node;
        add_in_order(node, collinear_points(4));
        const auto before = node.get_connections();

        node.set_connection_radius(0.1F);
        node.set_k_neighbors(1);

        EXPECT_EQ(node.get_connections(), before);
    }

    TEST_F(TopologyGeneratorNodeTest, MoreNeighboursNeverGiveFewerConnections)
    {
        TopologyGeneratorNode node(ProximityMode::K_NEAREST, true, 16);
        add_in_order(node, collinear_points(6));

        node.set_k_neighbors(1);
        const auto one = node.get_connection_count();
        node.set_k_neighbors(3);
        const auto three = node.get_connection_count();

        EXPECT_GT(one, 0U);
        EXPECT_GE(three, one);
    }

    TEST_F(TopologyGeneratorNodeTest, MinimumSpanningTreeHasOneFewerEdgeThanPoints)
    {
        TopologyGeneratorNode node(ProximityMode::MINIMUM_SPANNING_TREE, true, 16);
        add_in_order(node, collinear_points(5));

        EXPECT_EQ(node.get_connection_count(), 4U);
    }

    TEST_F(TopologyGeneratorNodeTest, SwitchingModeRebuildsTheConnections)
    {
        TopologyGeneratorNode node(ProximityMode::RADIUS_THRESHOLD, true, 16);
        node.set_connection_radius(0.5F);
        add_in_order(node, collinear_points(4));
        ASSERT_EQ(node.get_connection_count(), 0U);

        node.set_connection_mode(ProximityMode::SEQUENTIAL);

        EXPECT_EQ(node.get_connection_count(), 3U);
    }

    TEST_F(TopologyGeneratorNodeTest, CustomFunctionReceivesPositionsAsColumnsAndSuppliesTheEdges)
    {
        Eigen::Index rows = 0;
        Eigen::Index cols = 0;
        TopologyGeneratorNode node(
            [&](const Eigen::MatrixXd& positions) {
                rows = positions.rows();
                cols = positions.cols();
                return std::vector<std::pair<size_t, size_t>> { { 0, 2 } };
            },
            true, 16);
        add_in_order(node, collinear_points(3));
        node.compute_frame();

        EXPECT_EQ(rows, 3);
        EXPECT_EQ(cols, 3);
        ASSERT_EQ(node.get_connection_count(), 1U);
        const auto up = uploaded_vertices(node);
        ASSERT_EQ(up.size(), 2U);
        EXPECT_TRUE(near_vec(up[0].position, node.get_point(0).position));
        EXPECT_TRUE(near_vec(up[1].position, node.get_point(2).position));
    }

    TEST_F(TopologyGeneratorNodeTest, ConnectionsStayEmptyUntilRegeneratedWhenAutoConnectIsOff)
    {
        TopologyGeneratorNode node(ProximityMode::SEQUENTIAL, false, 16);
        node.set_path_interpolation_mode(InterpolationMode::LINEAR);
        add_in_order(node, collinear_points(3));
        node.compute_frame();

        EXPECT_EQ(node.get_connection_count(), 0U);
        EXPECT_EQ(uploaded_count(node), 0U);

        node.regenerate_topology();
        node.compute_frame();

        EXPECT_EQ(node.get_connection_count(), 2U);
        EXPECT_EQ(uploaded_count(node), 4U);
    }

    TEST_F(TopologyGeneratorNodeTest, StaleConnectionsToRemovedPointsAreNotDrawn)
    {
        TopologyGeneratorNode node(ProximityMode::SEQUENTIAL, false, 16);
        node.set_path_interpolation_mode(InterpolationMode::LINEAR);
        add_in_order(node, collinear_points(3));
        node.regenerate_topology();

        node.remove_point(2);
        node.compute_frame();

        const auto up = uploaded_vertices(node);
        ASSERT_EQ(up.size(), 2U);
        EXPECT_TRUE(near_vec(up[0].position, node.get_point(0).position));
        EXPECT_TRUE(near_vec(up[1].position, node.get_point(1).position));
    }

    TEST_F(TopologyGeneratorNodeTest, ComputeFrameUpdatesLayoutVertexCount)
    {
        LineTopology node;
        add_in_order(node, collinear_points(3));
        node.compute_frame();

        ASSERT_TRUE(node.get_vertex_layout().has_value());
        EXPECT_EQ(node.get_vertex_layout()->vertex_count, 4U);
    }

    TEST_F(TopologyGeneratorNodeTest, ComputeFrameRequestsGpuUpload)
    {
        LineTopology node;
        add_in_order(node, collinear_points(3));
        node.compute_frame();
        node.clear_gpu_update_flag();
        node.add_point(make_vertex({ 3, 0, 0 }));

        node.compute_frame();

        EXPECT_TRUE(node.needs_gpu_update());
    }

    TEST_F(TopologyGeneratorNodeTest, ComputeFrameMarksVertexDataAsNeedingUpdate)
    {
        LineTopology node;
        add_in_order(node, collinear_points(3));
        node.compute_frame();
        node.clear_gpu_update_flag();
        node.add_point(make_vertex({ 3, 0, 0 }));

        node.compute_frame();

        EXPECT_TRUE(node.needs_vertex_data_update());
    }

    TEST_F(TopologyGeneratorNodeTest, UploadBetweenAddAndFrameDoesNotLoseTheTopology)
    {
        LineTopology node;
        add_in_order(node, collinear_points(2));
        node.compute_frame();

        node.add_point(make_vertex({ 2, 0, 0 }));
        node.clear_gpu_update_flag();
        node.compute_frame();

        EXPECT_EQ(uploaded_count(node), 4U);
    }

    TEST_F(TopologyGeneratorNodeTest, UnchangedTopologyIsNotReuploaded)
    {
        LineTopology node;
        add_in_order(node, collinear_points(3));
        node.compute_frame();
        node.clear_gpu_update_flag();

        node.compute_frame();

        EXPECT_FALSE(node.needs_gpu_update());
        EXPECT_EQ(uploaded_count(node), 4U);
    }

    TEST_F(TopologyGeneratorNodeTest, EmptyNodeFramesAreHarmlessAndStaySettled)
    {
        LineTopology node;
        node.compute_frame();
        EXPECT_EQ(uploaded_count(node), 0U);

        node.clear_gpu_update_flag();
        node.compute_frame();
        node.compute_frame();

        EXPECT_FALSE(node.needs_gpu_update());
    }

    TEST_F(TopologyGeneratorNodeTest, PointsAddedInteractivelyAppearInTheNextFrame)
    {
        LineTopology node;
        node.compute_frame();

        const auto pts = collinear_points(4);
        const std::array<uint32_t, 4> expected { 0, 2, 4, 6 };
        for (size_t i = 0; i < pts.size(); ++i) {
            node.add_point(pts[i]);
            node.compute_frame();
            EXPECT_EQ(uploaded_count(node), expected[i]) << "after point " << i;
        }
    }

    TEST_F(TopologyGeneratorNodeTest, ProcessSampleUploadsAFrameAndReturnsZero)
    {
        LineTopology node;
        add_in_order(node, collinear_points(3));

        EXPECT_DOUBLE_EQ(node.process_sample(0.0), 0.0);
        EXPECT_EQ(uploaded_count(node), 4U);
    }

    TEST_F(TopologyGeneratorNodeTest, GeneratedVertexCountMatchesTheUploadAfterAFrame)
    {
        LineTopology node;
        add_in_order(node, collinear_points(4));
        node.compute_frame();

        EXPECT_EQ(node.get_vertex_count(), uploaded_count(node));
    }

    TEST_F(TopologyGeneratorNodeTest, UniformColourOverridesEveryDirectVertex)
    {
        LineTopology node;
        add_in_order(node, collinear_points(3));
        node.compute_frame();

        node.set_line_color({ 0, 1, 0, 1 });
        node.compute_frame();

        ASSERT_EQ(uploaded_count(node), 4U);
        for (const auto& v : uploaded_vertices(node)) {
            EXPECT_TRUE(near_vec(v.color, { 0, 1, 0 }));
        }
        EXPECT_TRUE(node.should_force_uniform_color());
        EXPECT_EQ(node.get_line_color(), glm::vec4(glm::vec3(0, 1, 0), 1.F));
    }

    TEST_F(TopologyGeneratorNodeTest, UniformColourOverridesEverySmoothPathVertex)
    {
        TopologyGeneratorNode node;
        node.set_samples_per_segment(5);
        add_in_order(node, collinear_points(5));
        node.compute_frame();

        node.set_line_color({ 0, 1, 0, 1 });
        node.compute_frame();

        ASSERT_EQ(uploaded_count(node), 32U);
        for (const auto& v : uploaded_vertices(node)) {
            EXPECT_TRUE(near_vec(v.color, { 0, 1, 0 }));
        }
    }

    TEST_F(TopologyGeneratorNodeTest, DisablingUniformColourRestoresPerPointColour)
    {
        LineTopology node;
        add_in_order(node, collinear_points(3));
        node.set_line_color({ 0, 1, 0, 1 });
        node.compute_frame();

        node.force_uniform_color(false);
        node.compute_frame();

        const auto up = uploaded_vertices(node);
        EXPECT_TRUE(near_vec(up[0].color, { 0.3F, 0, 0 }));
        EXPECT_TRUE(near_vec(up[3].color, { 0.1F, 0, 0 }));
        EXPECT_FALSE(node.should_force_uniform_color());
    }

    TEST_F(TopologyGeneratorNodeTest, SettingColourWithoutForcingOnlyStoresIt)
    {
        LineTopology node;
        add_in_order(node, collinear_points(3));
        node.set_line_color({ 0, 0, 1, 1 }, false);
        node.compute_frame();

        EXPECT_EQ(node.get_line_color(), glm::vec4(glm::vec3(0, 0, 1), 1.F));
        EXPECT_TRUE(near_vec(uploaded_vertices(node)[0].color, { 0.3F, 0, 0 }));
    }

    TEST_F(TopologyGeneratorNodeTest, UniformThicknessOverridesEveryVertexAndCanBeLiftedAgain)
    {
        LineTopology node;
        add_in_order(node, collinear_points(3));
        node.compute_frame();

        node.set_line_thickness(6.0F);
        node.compute_frame();
        for (const auto& v : uploaded_vertices(node)) {
            EXPECT_FLOAT_EQ(v.thickness, 6.0F);
        }

        node.force_uniform_thickness(false);
        node.compute_frame();
        const auto up = uploaded_vertices(node);
        EXPECT_FLOAT_EQ(up[0].thickness, 3.0F);
        EXPECT_FLOAT_EQ(up[3].thickness, 1.0F);
    }

    class TopologyModeTest : public ::testing::TestWithParam<ProximityMode> {
    protected:
        static void populate(TopologyGeneratorNode& node)
        {
            node.set_path_interpolation_mode(InterpolationMode::LINEAR);
            node.set_connection_radius(1.5F);
            add_in_order(node, grid_points());
            node.compute_frame();
        }
    };

    TEST_P(TopologyModeTest, EveryConnectionJoinsTwoDistinctExistingPoints)
    {
        TopologyGeneratorNode node(GetParam(), true, 16);
        populate(node);

        ASSERT_GT(node.get_connection_count(), 0U);
        for (const auto& [a, b] : node.get_connections()) {
            EXPECT_LT(a, node.get_point_count());
            EXPECT_LT(b, node.get_point_count());
            EXPECT_NE(a, b);
        }
    }

    TEST_P(TopologyModeTest, UploadedVerticesAreTheConnectedPointsInConnectionOrder)
    {
        TopologyGeneratorNode node(GetParam(), true, 16);
        populate(node);

        const auto& edges = node.get_connections();
        const auto up = uploaded_vertices(node);
        ASSERT_EQ(up.size(), edges.size() * 2);
        for (size_t i = 0; i < edges.size(); ++i) {
            EXPECT_TRUE(near_vec(up[2 * i].position, node.get_point(edges[i].first).position)) << "edge " << i;
            EXPECT_TRUE(near_vec(up[2 * i + 1].position, node.get_point(edges[i].second).position)) << "edge " << i;
        }
    }

    TEST_P(TopologyModeTest, LayoutTracksTheUploadedVertexCount)
    {
        TopologyGeneratorNode node(GetParam(), true, 16);
        populate(node);

        ASSERT_TRUE(node.get_vertex_layout().has_value());
        EXPECT_EQ(node.get_vertex_layout()->vertex_count, uploaded_count(node));
    }

    class TopologyPathModeTest : public ::testing::TestWithParam<InterpolationMode> {
    };

    TEST_P(TopologyPathModeTest, AnyPointCountDrawsWithoutFailingAndKeepsPairs)
    {
        for (uint32_t n = 2; n <= 8; ++n) {
            SCOPED_TRACE(n);

            TopologyGeneratorNode node(ProximityMode::SEQUENTIAL, true, 16);
            node.set_path_interpolation_mode(GetParam());
            node.set_samples_per_segment(6);
            add_in_order(node, collinear_points(n));

            ASSERT_NO_THROW(node.compute_frame());
            EXPECT_GT(uploaded_count(node), 0U);
            EXPECT_EQ(uploaded_count(node) % 2, 0U);
            for (const auto& v : uploaded_vertices(node)) {
                ASSERT_TRUE(std::isfinite(v.position.x));
                EXPECT_NEAR(v.position.y, 0.0F, 1e-4F);
            }
        }
    }

    INSTANTIATE_TEST_SUITE_P(
        TopologyPathModes,
        TopologyPathModeTest,
        ::testing::Values(
            InterpolationMode::LINEAR,
            InterpolationMode::COSINE,
            InterpolationMode::CATMULL_ROM,
            InterpolationMode::CUBIC_HERMITE,
            InterpolationMode::CUBIC_BEZIER,
            InterpolationMode::QUADRATIC_BEZIER,
            InterpolationMode::BSPLINE));

    INSTANTIATE_TEST_SUITE_P(
        TopologyModes,
        TopologyModeTest,
        ::testing::Values(
            ProximityMode::K_NEAREST,
            ProximityMode::RADIUS_THRESHOLD,
            ProximityMode::MINIMUM_SPANNING_TREE,
            ProximityMode::GABRIEL_GRAPH,
            ProximityMode::RELATIVE_NEIGHBORHOOD_GRAPH,
            ProximityMode::SEQUENTIAL,
            ProximityMode::NEAREST_NEIGHBOR));

} // namespace

}
