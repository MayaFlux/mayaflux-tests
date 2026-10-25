#include "gtest/gtest.h"

#include "MayaFlux/Nodes/Graphics/PointCollectionNode.hpp"

namespace MayaFlux::Test {

namespace {

    using Nodes::PointVertex;
    using Nodes::GpuSync::PointCollectionNode;

    PointVertex make_point(float seed)
    {
        PointVertex p;
        p.position = { seed, seed + 1.0F, seed + 2.0F };
        p.color = { 0.1F * seed, 0.2F * seed, 0.3F * seed, 1.0F };
        p.size = 2.0F + seed;
        p.uv = { 0.5F * seed, 0.25F * seed };
        return p;
    }

    std::vector<PointVertex> make_points(uint32_t count, float first = 1.0F)
    {
        std::vector<PointVertex> v;
        v.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            v.push_back(make_point(first + static_cast<float>(i)));
        }
        return v;
    }

    bool same_point(const PointVertex& a, const PointVertex& b)
    {
        return a.position == b.position
            && a.color == b.color
            && a.size == b.size
            && a.uv == b.uv
            && a.normal == b.normal
            && a.tangent == b.tangent;
    }

    class PointCollectionNodeTest : public ::testing::Test {
    protected:
        static void expect_uploaded(const PointCollectionNode& node, const std::vector<PointVertex>& expected)
        {
            ASSERT_EQ(node.get_vertex_count(), expected.size());
            for (uint32_t i = 0; i < expected.size(); ++i) {
                EXPECT_TRUE(same_point(node.get_vertex_typed<PointVertex>(i), expected[i]))
                    << "uploaded vertex " << i;
            }
        }
    };

    TEST_F(PointCollectionNodeTest, CapacityConstructionStartsWithNoPoints)
    {
        PointCollectionNode node(16);

        EXPECT_EQ(node.get_point_count(), 0U);
        EXPECT_TRUE(node.get_points().empty());
    }

    TEST_F(PointCollectionNodeTest, CapacityConstructionReportsNoVerticesBeforeAnyFrame)
    {
        PointCollectionNode node(16);

        EXPECT_EQ(node.get_vertex_count(), 0U);
    }

    TEST_F(PointCollectionNodeTest, ConstructionUsesPointVertexStrideAndPointListTopology)
    {
        PointCollectionNode node(4);

        EXPECT_EQ(node.get_vertex_stride(), sizeof(PointVertex));
        EXPECT_EQ(node.get_primitive_topology(), Portal::Graphics::PrimitiveTopology::POINT_LIST);

        auto layout = node.get_vertex_layout();
        ASSERT_TRUE(layout.has_value());
        EXPECT_EQ(layout->stride_bytes, sizeof(PointVertex));
        EXPECT_EQ(layout->vertex_count, 0U);
    }

    TEST_F(PointCollectionNodeTest, VectorConstructionHoldsTheGivenPoints)
    {
        const auto pts = make_points(3);
        PointCollectionNode node(pts);

        ASSERT_EQ(node.get_point_count(), 3U);
        for (size_t i = 0; i < 3; ++i) {
            EXPECT_TRUE(same_point(node.get_point(i), pts[i]));
        }
        EXPECT_EQ(node.get_vertex_stride(), sizeof(PointVertex));
        ASSERT_TRUE(node.get_vertex_layout().has_value());
        EXPECT_EQ(node.get_vertex_layout()->vertex_count, 3U);
    }

    TEST_F(PointCollectionNodeTest, EmptyVectorConstructionIsEmpty)
    {
        PointCollectionNode node(std::vector<PointVertex> {});

        EXPECT_EQ(node.get_point_count(), 0U);
        node.compute_frame();
        EXPECT_EQ(node.get_vertex_count(), 0U);
    }

    TEST_F(PointCollectionNodeTest, ComputeFrameUploadsPointsInOrder)
    {
        const auto pts = make_points(4);
        PointCollectionNode node(pts);

        node.compute_frame();

        expect_uploaded(node, pts);
    }

    TEST_F(PointCollectionNodeTest, ComputeFrameUpdatesLayoutVertexCount)
    {
        PointCollectionNode node(make_points(5));
        node.compute_frame();

        ASSERT_TRUE(node.get_vertex_layout().has_value());
        EXPECT_EQ(node.get_vertex_layout()->vertex_count, 5U);
        EXPECT_EQ(node.get_vertex_layout()->stride_bytes, sizeof(PointVertex));
    }

    TEST_F(PointCollectionNodeTest, ComputeFrameMarksGeometryForGpuUpload)
    {
        PointCollectionNode node(make_points(2));
        node.clear_gpu_update_flag();
        ASSERT_FALSE(node.needs_gpu_update());

        node.compute_frame();

        EXPECT_TRUE(node.needs_gpu_update());
    }

    TEST_F(PointCollectionNodeTest, PointsBeyondInitialCapacityAreUploaded)
    {
        PointCollectionNode node(2);
        const auto pts = make_points(7);
        node.add_points(pts);

        node.compute_frame();

        expect_uploaded(node, pts);
    }

    TEST_F(PointCollectionNodeTest, AddPointAppendsToTheEnd)
    {
        PointCollectionNode node(make_points(2));
        node.add_point(make_point(10.0F));

        ASSERT_EQ(node.get_point_count(), 3U);
        EXPECT_TRUE(same_point(node.get_point(2), make_point(10.0F)));
        EXPECT_TRUE(same_point(node.get_point(0), make_point(1.0F)));
    }

    TEST_F(PointCollectionNodeTest, AddPointsAppendsInOrder)
    {
        PointCollectionNode node(make_points(2));
        node.add_points(make_points(3, 10.0F));

        ASSERT_EQ(node.get_point_count(), 5U);
        EXPECT_TRUE(same_point(node.get_point(2), make_point(10.0F)));
        EXPECT_TRUE(same_point(node.get_point(4), make_point(12.0F)));
    }

    TEST_F(PointCollectionNodeTest, SetPointsReplacesEverything)
    {
        PointCollectionNode node(make_points(5));
        const auto replacement = make_points(2, 20.0F);
        node.set_points(replacement);

        ASSERT_EQ(node.get_point_count(), 2U);
        node.compute_frame();
        expect_uploaded(node, replacement);
    }

    TEST_F(PointCollectionNodeTest, UpdatePointReplacesOnlyThatPoint)
    {
        PointCollectionNode node(make_points(3));
        node.update_point(1, make_point(50.0F));

        EXPECT_TRUE(same_point(node.get_point(0), make_point(1.0F)));
        EXPECT_TRUE(same_point(node.get_point(1), make_point(50.0F)));
        EXPECT_TRUE(same_point(node.get_point(2), make_point(3.0F)));
    }

    TEST_F(PointCollectionNodeTest, UpdatePointShowsUpInTheNextFrame)
    {
        auto pts = make_points(3);
        PointCollectionNode node(pts);
        node.compute_frame();

        node.update_point(2, make_point(50.0F));
        pts[2] = make_point(50.0F);
        node.compute_frame();

        expect_uploaded(node, pts);
    }

    TEST_F(PointCollectionNodeTest, UpdatePointOutOfRangeChangesNothing)
    {
        PointCollectionNode node(make_points(2));
        node.clear_gpu_update_flag();

        node.update_point(2, make_point(50.0F));
        node.update_point(100, make_point(50.0F));

        EXPECT_EQ(node.get_point_count(), 2U);
        EXPECT_FALSE(node.needs_gpu_update());
    }

    TEST_F(PointCollectionNodeTest, GetPointOutOfRangeReturnsOriginPosition)
    {
        PointCollectionNode node(make_points(2));

        EXPECT_EQ(node.get_point(2).position, glm::vec3(0.0F));
        EXPECT_EQ(node.get_point(999).position, glm::vec3(0.0F));
    }

    TEST_F(PointCollectionNodeTest, MutablePointsReferenceEditsReachTheNextFrame)
    {
        PointCollectionNode node(make_points(2));
        node.compute_frame();

        node.get_points()[0] = make_point(70.0F);
        node.compute_frame();

        EXPECT_TRUE(same_point(node.get_vertex_typed<PointVertex>(0), make_point(70.0F)));
        EXPECT_TRUE(same_point(node.get_vertex_typed<PointVertex>(1), make_point(2.0F)));
    }

    TEST_F(PointCollectionNodeTest, MutationFlagsFollowWhatChanged)
    {
        PointCollectionNode node(make_points(3));

        node.clear_gpu_update_flag();
        node.add_point(make_point(9.0F));
        EXPECT_TRUE(node.needs_vertex_data_update());
        EXPECT_TRUE(node.needs_layout_update());

        node.clear_gpu_update_flag();
        node.add_points(make_points(2, 9.0F));
        EXPECT_TRUE(node.needs_vertex_data_update());
        EXPECT_TRUE(node.needs_layout_update());

        node.clear_gpu_update_flag();
        node.set_points(make_points(2));
        EXPECT_TRUE(node.needs_vertex_data_update());
        EXPECT_TRUE(node.needs_layout_update());

        node.clear_gpu_update_flag();
        node.update_point(0, make_point(9.0F));
        EXPECT_TRUE(node.needs_vertex_data_update());
        EXPECT_FALSE(node.needs_layout_update());

        node.clear_gpu_update_flag();
        node.clear_points();
        EXPECT_TRUE(node.needs_vertex_data_update());
        EXPECT_TRUE(node.needs_layout_update());
    }

    TEST_F(PointCollectionNodeTest, ClearPointsThenFrameLeavesNothingToDraw)
    {
        PointCollectionNode node(make_points(4));
        node.compute_frame();
        ASSERT_EQ(node.get_vertex_count(), 4U);

        node.clear_points();
        node.compute_frame();

        EXPECT_EQ(node.get_point_count(), 0U);
        EXPECT_EQ(node.get_vertex_count(), 0U);
    }

    TEST_F(PointCollectionNodeTest, ClearPointsThenFrameResetsLayoutVertexCount)
    {
        PointCollectionNode node(make_points(4));
        node.compute_frame();

        node.clear_points();
        node.compute_frame();

        ASSERT_TRUE(node.get_vertex_layout().has_value());
        EXPECT_EQ(node.get_vertex_layout()->vertex_count, 0U);
    }

    TEST_F(PointCollectionNodeTest, ShrinkingThenFrameShrinksTheUpload)
    {
        PointCollectionNode node(make_points(6));
        node.compute_frame();

        const auto fewer = make_points(2, 30.0F);
        node.set_points(fewer);
        node.compute_frame();

        expect_uploaded(node, fewer);
        EXPECT_EQ(node.get_vertex_layout()->vertex_count, 2U);
    }

    TEST_F(PointCollectionNodeTest, ProcessSampleUploadsAFrameAndReturnsZero)
    {
        const auto pts = make_points(3);
        PointCollectionNode node(pts);

        EXPECT_DOUBLE_EQ(node.process_sample(0.0), 0.0);
        expect_uploaded(node, pts);
    }

    TEST_F(PointCollectionNodeTest, ContextDescribesTheUploadedPoints)
    {
        PointCollectionNode node(make_points(3));
        node.compute_frame();
        node.update_context(0.0);

        auto& ctx = dynamic_cast<Nodes::GpuSync::GeometryContext&>(node.get_last_context());
        EXPECT_EQ(ctx.vertex_count, 3U);
        EXPECT_EQ(ctx.vertex_stride, sizeof(PointVertex));
    }

    TEST_F(PointCollectionNodeTest, SaveAndRestoreReturnsTheUploadedVertices)
    {
        const auto pts = make_points(3);
        PointCollectionNode node(pts);
        node.compute_frame();
        node.save_state();

        node.set_points(make_points(1, 90.0F));
        node.compute_frame();
        ASSERT_EQ(node.get_vertex_count(), 1U);

        node.restore_state();

        expect_uploaded(node, pts);
    }

} // namespace

}
