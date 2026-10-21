#include "gtest/gtest.h"

#include "MayaFlux/Nodes/Network/Operators/AssemblyOperator.hpp"
#include "MayaFlux/Nodes/Network/Operators/FieldOperator.hpp"
#include "MayaFlux/Nodes/Network/Operators/PathOperator.hpp"
#include "MayaFlux/Nodes/Network/Operators/PhysicsOperator.hpp"
#include "MayaFlux/Nodes/Network/Operators/TopologyOperator.hpp"

namespace MayaFlux::Test {

namespace {

    using Nodes::LineVertex;
    using Nodes::PointVertex;
    using Nodes::Network::AssemblyOperator;
    using Nodes::Network::FieldOperator;
    using Nodes::Network::GraphicsOperator;
    using Nodes::Network::PathOperator;
    using Nodes::Network::PhysicsOperator;
    using Nodes::Network::TopologyOperator;

    std::vector<LineVertex> line_points(uint32_t count, float offset)
    {
        std::vector<LineVertex> pts;
        pts.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            LineVertex v;
            const auto x = static_cast<float>(i);
            v.position = { x + offset, (i % 2 == 0) ? 0.0F : 1.0F, offset };
            pts.push_back(v);
        }
        return pts;
    }

    std::vector<PointVertex> point_vertices(uint32_t count, float offset)
    {
        std::vector<PointVertex> pts;
        pts.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            PointVertex v;
            v.position = { static_cast<float>(i) + offset, offset, 0.0F };
            pts.push_back(v);
        }
        return pts;
    }

    std::vector<glm::vec3> positions(uint32_t count, float offset)
    {
        std::vector<glm::vec3> pts;
        pts.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            pts.emplace_back(static_cast<float>(i) + offset, offset, 0.0F);
        }
        return pts;
    }

    struct Subject {
        const char* name;
        size_t groups;
        std::function<std::unique_ptr<GraphicsOperator>()> make;
        std::function<std::unique_ptr<GraphicsOperator>()> make_empty;
    };

    std::vector<Subject> subjects()
    {
        return {
            { .name = "Path", .groups = 2,
                .make = [] {
                    auto op = std::make_unique<PathOperator>();
                    op->initialize_paths(
                        { line_points(6, 0.0F), line_points(8, 10.0F) },
                        Kinesis::InterpolationMode::CATMULL_ROM);
                    return op;
                },
                .make_empty = [] { return std::make_unique<PathOperator>(); } },
            { .name = "Topology", .groups = 2,
                .make = [] {
                    auto op = std::make_unique<TopologyOperator>();
                    op->initialize_topologies(
                        { line_points(6, 0.0F), line_points(8, 10.0F) },
                        Kinesis::ProximityMode::K_NEAREST);
                    return op;
                },
                .make_empty = [] { return std::make_unique<TopologyOperator>(); } },
            { .name = "Assembly", .groups = 2,
                .make = [] {
                    auto op = std::make_unique<AssemblyOperator>();
                    op->add_geometry(positions(5, 0.0F), Portal::Graphics::PrimitiveTopology::POINT_LIST);
                    op->add_geometry(positions(7, 10.0F), Portal::Graphics::PrimitiveTopology::POINT_LIST);
                    return op;
                },
                .make_empty = [] { return std::make_unique<AssemblyOperator>(); } },
            { .name = "Physics", .groups = 2,
                .make = [] {
                    auto op = std::make_unique<PhysicsOperator>();
                    op->initialize_collections({ point_vertices(5, 0.0F), point_vertices(7, 10.0F) });
                    return op;
                },
                .make_empty = [] { return std::make_unique<PhysicsOperator>(); } },
            { .name = "Field", .groups = 1,
                .make = [] {
                    auto op = std::make_unique<FieldOperator>();
                    op->initialize(point_vertices(9, 0.0F));
                    return op;
                },
                .make_empty = [] { return std::make_unique<FieldOperator>(); } },
        };
    }

    std::string subject_name(const ::testing::TestParamInfo<Subject>& info)
    {
        return info.param.name;
    }

    size_t run_total(const std::vector<Portal::Graphics::DrawRun>& runs)
    {
        size_t total = 0;
        for (const auto& r : runs) {
            total += r.vertex_count;
        }
        return total;
    }

    class GraphicsOperatorContractTest : public ::testing::TestWithParam<Subject> {
    protected:
        [[nodiscard]] std::unique_ptr<GraphicsOperator> populated() const
        {
            auto op = GetParam().make();
            EXPECT_GT(op->get_vertex_count(), 0U) << "subject must be populated";
            return op;
        }
    };

    TEST_P(GraphicsOperatorContractTest, VertexBytesAreCountTimesStride)
    {
        auto op = populated();

        const auto layout = op->get_vertex_layout();
        ASSERT_GT(layout.stride_bytes, 0U);
        EXPECT_EQ(op->get_vertex_data().size(), op->get_vertex_count() * layout.stride_bytes);
    }

    TEST_P(GraphicsOperatorContractTest, LayoutVertexCountMatchesOperatorCount)
    {
        auto op = populated();

        EXPECT_EQ(op->get_vertex_layout().vertex_count, op->get_vertex_count());
    }

    TEST_P(GraphicsOperatorContractTest, ProcessKeepsBytesLayoutAndCountConsistent)
    {
        auto op = populated();

        op->process(0.016F);

        const auto layout = op->get_vertex_layout();
        EXPECT_EQ(layout.vertex_count, op->get_vertex_count());
        EXPECT_EQ(op->get_vertex_data().size(), op->get_vertex_count() * layout.stride_bytes);
    }

    TEST_P(GraphicsOperatorContractTest, PerGroupBytesConcatenateToAggregate)
    {
        auto op = populated();

        std::vector<uint8_t> joined;
        for (uint32_t g = 0; g < GetParam().groups; ++g) {
            const auto group = op->get_vertex_data_for_collection(g);
            joined.insert(joined.end(), group.begin(), group.end());
        }

        const auto whole = op->get_vertex_data();
        ASSERT_EQ(joined.size(), whole.size());
        EXPECT_TRUE(std::equal(joined.begin(), joined.end(), whole.begin()));
    }

    TEST_P(GraphicsOperatorContractTest, OutOfRangeGroupYieldsNoBytes)
    {
        auto op = populated();
        const auto beyond = static_cast<uint32_t>(GetParam().groups);

        if (GetParam().groups == 1) {
            GTEST_SKIP() << "single-pack operator serves the whole buffer for any index";
        }

        EXPECT_TRUE(op->get_vertex_data_for_collection(beyond).empty());
    }

    TEST_P(GraphicsOperatorContractTest, DrawRunsCoverEveryVertexContiguously)
    {
        auto op = populated();

        const auto runs = op->topology_runs();
        ASSERT_FALSE(runs.empty());

        EXPECT_EQ(runs.front().vertex_offset, 0U);
        for (size_t i = 1; i < runs.size(); ++i) {
            EXPECT_EQ(runs[i].vertex_offset, runs[i - 1].vertex_offset + runs[i - 1].vertex_count)
                << "run " << i;
        }
        EXPECT_EQ(run_total(runs), op->get_vertex_count());
    }

    TEST_P(GraphicsOperatorContractTest, DeclaredTopologyLeadsTheDrawRuns)
    {
        auto op = populated();

        const auto declared = op->declared_topology();
        if (!declared.has_value()) {
            GTEST_SKIP() << "operator declares no topology";
        }

        const auto runs = op->topology_runs();
        ASSERT_FALSE(runs.empty());
        EXPECT_EQ(runs.front().topology, *declared);
    }

    TEST_P(GraphicsOperatorContractTest, FreshOperatorReportsDirty)
    {
        auto op = populated();

        EXPECT_TRUE(op->is_vertex_data_dirty());
    }

    TEST_P(GraphicsOperatorContractTest, FreshDirtyRangesCoverEveryVertexInOrder)
    {
        auto op = populated();

        const auto ranges = op->dirty_vertex_ranges();
        ASSERT_FALSE(ranges.empty());

        const auto total = static_cast<uint32_t>(op->get_vertex_count());
        uint32_t covered = 0;
        uint32_t cursor = 0;
        for (const auto& r : ranges) {
            EXPECT_GE(r.vertex_offset, cursor);
            EXPECT_LE(r.vertex_offset + r.vertex_count, total);
            EXPECT_LT(r.group_index, std::max<size_t>(GetParam().groups, 1U));
            cursor = r.vertex_offset + r.vertex_count;
            covered += r.vertex_count;
        }
        EXPECT_EQ(covered, total);
    }

    TEST_P(GraphicsOperatorContractTest, MarkingCleanClearsDirtyFlagAndRanges)
    {
        auto op = populated();

        op->mark_vertex_data_clean();

        EXPECT_FALSE(op->is_vertex_data_dirty());
        EXPECT_TRUE(op->dirty_vertex_ranges().empty());
    }

    TEST_P(GraphicsOperatorContractTest, ClusterIdsSizedPerVertexAndGroupedInOrder)
    {
        auto op = populated();

        const auto ids = op->build_cluster_ids();
        ASSERT_EQ(ids.size(), op->get_vertex_count());

        EXPECT_TRUE(std::ranges::is_sorted(ids));
        EXPECT_EQ(ids.front(), 0U);
        EXPECT_EQ(ids.back(), static_cast<uint32_t>(GetParam().groups - 1));
    }

    TEST_P(GraphicsOperatorContractTest, EmptyOperatorHasNoVertices)
    {
        auto op = GetParam().make_empty();

        EXPECT_EQ(op->get_vertex_count(), 0U);
        EXPECT_TRUE(op->get_vertex_data().empty());
        EXPECT_TRUE(op->build_cluster_ids().empty());
        EXPECT_EQ(run_total(op->topology_runs()), 0U);
    }

    TEST_P(GraphicsOperatorContractTest, EmptyOperatorIsNotDirty)
    {
        auto op = GetParam().make_empty();

        EXPECT_FALSE(op->is_vertex_data_dirty());
        EXPECT_TRUE(op->dirty_vertex_ranges().empty());
    }

    TEST_P(GraphicsOperatorContractTest, EmptyOperatorSurvivesProcessAndCleanAndLayoutQueries)
    {
        auto op = GetParam().make_empty();

        EXPECT_NO_THROW(op->process(0.016F));
        EXPECT_NO_THROW(op->mark_vertex_data_clean());
        EXPECT_NO_THROW((void)op->get_vertex_layout());
        EXPECT_NO_THROW((void)op->get_vertex_data_for_collection(0));
        EXPECT_EQ(op->get_vertex_count(), 0U);
    }

    INSTANTIATE_TEST_SUITE_P(
        GraphicsOperators,
        GraphicsOperatorContractTest,
        ::testing::ValuesIn(subjects()),
        subject_name);

    TEST(AssemblyOperatorContractTest, TopologyRunsSplitWhereTopologyChanges)
    {
        using Portal::Graphics::PrimitiveTopology;

        AssemblyOperator op;
        op.add_geometry(positions(3, 0.0F), PrimitiveTopology::POINT_LIST);
        op.add_geometry(positions(4, 5.0F), PrimitiveTopology::LINE_STRIP);
        op.add_geometry(positions(2, 9.0F), PrimitiveTopology::POINT_LIST);

        const auto runs = op.topology_runs();

        ASSERT_EQ(runs.size(), 3U);
        EXPECT_EQ(runs[0].topology, PrimitiveTopology::POINT_LIST);
        EXPECT_EQ(runs[1].topology, PrimitiveTopology::LINE_STRIP);
        EXPECT_EQ(runs[2].topology, PrimitiveTopology::POINT_LIST);
        EXPECT_EQ(runs[1].vertex_offset, 3U);
        EXPECT_EQ(runs[2].vertex_offset, 7U);
        EXPECT_EQ(run_total(runs), 9U);
    }

    TEST(AssemblyOperatorContractTest, AdjacentConcatenableTopologiesMergeIntoOneRun)
    {
        using Portal::Graphics::PrimitiveTopology;

        AssemblyOperator op;
        op.add_geometry(positions(3, 0.0F), PrimitiveTopology::POINT_LIST);
        op.add_geometry(positions(4, 5.0F), PrimitiveTopology::POINT_LIST);

        const auto runs = op.topology_runs();

        ASSERT_EQ(runs.size(), 1U);
        EXPECT_EQ(runs[0].vertex_count, 7U);
    }

    TEST(AssemblyOperatorContractTest, StripTopologiesNeverMerge)
    {
        using Portal::Graphics::PrimitiveTopology;

        AssemblyOperator op;
        op.add_geometry(positions(3, 0.0F), PrimitiveTopology::LINE_STRIP);
        op.add_geometry(positions(4, 5.0F), PrimitiveTopology::LINE_STRIP);

        EXPECT_EQ(op.topology_runs().size(), 2U);
    }

    TEST(AssemblyOperatorContractTest, RemovingAnItemDropsItsVertices)
    {
        using Portal::Graphics::PrimitiveTopology;

        AssemblyOperator op;
        auto first = op.add_geometry(positions(3, 0.0F), PrimitiveTopology::POINT_LIST);
        op.add_geometry(positions(4, 5.0F), PrimitiveTopology::POINT_LIST);

        EXPECT_TRUE(op.remove_geometry(first));
        EXPECT_EQ(op.get_vertex_count(), 4U);
        EXPECT_FALSE(op.remove_geometry(first));
    }

    TEST(PathOperatorContractTest, DirtyRangeNamesOnlyTheEditedPath)
    {
        PathOperator op;
        op.initialize_paths(
            { line_points(6, 0.0F), line_points(8, 10.0F) },
            Kinesis::InterpolationMode::CATMULL_ROM);
        op.mark_vertex_data_clean();

        op.add_path(line_points(6, 20.0F), Kinesis::InterpolationMode::CATMULL_ROM);

        const auto ranges = op.dirty_vertex_ranges();
        ASSERT_EQ(ranges.size(), 1U);
        EXPECT_EQ(ranges[0].group_index, 2U);
    }

    TEST(TopologyOperatorContractTest, DirtyRangeNamesOnlyTheEditedTopology)
    {
        TopologyOperator op;
        op.initialize_topologies(
            { line_points(6, 0.0F), line_points(8, 10.0F) },
            Kinesis::ProximityMode::K_NEAREST);
        op.mark_vertex_data_clean();

        op.add_topology(line_points(6, 20.0F), Kinesis::ProximityMode::K_NEAREST);

        const auto ranges = op.dirty_vertex_ranges();
        ASSERT_EQ(ranges.size(), 1U);
        EXPECT_EQ(ranges[0].group_index, 2U);
    }

} // namespace

} // namespace MayaFlux::Test
