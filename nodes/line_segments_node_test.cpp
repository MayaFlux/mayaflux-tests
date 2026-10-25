#include "gtest/gtest.h"

#include "MayaFlux/Nodes/Graphics/LineSegmentsNode.hpp"

namespace MayaFlux::Test {

namespace {

    using Nodes::LineVertex;
    using Nodes::GpuSync::LineSegmentsNode;

    LineVertex make_vertex(const glm::vec3& position, float thickness = 2.0F, const glm::vec3& color = glm::vec3(1.0F))
    {
        LineVertex v;
        v.position = position;
        v.color = glm::vec4(color, 1.0F);
        v.thickness = thickness;
        return v;
    }

    ::testing::AssertionResult near_vec(const glm::vec3& a, const glm::vec3& b, float tol = 1e-5F)
    {
        if (glm::length(a - b) <= tol) {
            return ::testing::AssertionSuccess();
        }
        return ::testing::AssertionFailure()
            << "(" << a.x << ", " << a.y << ", " << a.z << ") != ("
            << b.x << ", " << b.y << ", " << b.z << ")";
    }

    std::vector<LineVertex> straight_path(uint32_t count, float spacing = 1.0F)
    {
        std::vector<LineVertex> path;
        path.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            path.push_back(make_vertex({ spacing * static_cast<float>(i), 0.0F, 0.0F },
                3.0F + static_cast<float>(i), { 0.5F, 0.25F, 0.125F }));
        }
        return path;
    }

    class LineSegmentsNodeTest : public ::testing::Test {
    protected:
        LineSegmentsNode node { 8 };

        [[nodiscard]] LineVertex uploaded(uint32_t i) const
        {
            return node.get_vertex_typed<LineVertex>(i);
        }
    };

    TEST_F(LineSegmentsNodeTest, ConstructionStartsEmptyWithLineListTopology)
    {
        EXPECT_EQ(node.get_segment_count(), 0U);
        EXPECT_EQ(node.get_vertex_count(), 0U);
        EXPECT_EQ(node.get_vertex_stride(), sizeof(LineVertex));
        EXPECT_EQ(node.get_primitive_topology(), Portal::Graphics::PrimitiveTopology::LINE_LIST);
    }

    TEST_F(LineSegmentsNodeTest, ConstructionLayoutDescribesNoVerticesYet)
    {
        auto layout = node.get_vertex_layout();

        ASSERT_TRUE(layout.has_value());
        EXPECT_EQ(layout->stride_bytes, sizeof(LineVertex));
        EXPECT_EQ(layout->vertex_count, 0U);
    }

    TEST_F(LineSegmentsNodeTest, AddLineCountsOneSegment)
    {
        node.add_line(make_vertex({ 0, 0, 0 }), make_vertex({ 1, 0, 0 }));

        EXPECT_EQ(node.get_segment_count(), 1U);
    }

    TEST_F(LineSegmentsNodeTest, ComputeFrameUploadsTwoVerticesPerSegmentInOrder)
    {
        node.add_line(make_vertex({ 0, 0, 0 }, 1.0F), make_vertex({ 1, 0, 0 }, 2.0F));
        node.add_line(make_vertex({ 0, 1, 0 }, 3.0F), make_vertex({ 0, 2, 0 }, 4.0F));

        node.compute_frame();

        ASSERT_EQ(node.get_vertex_count(), 4U);
        EXPECT_TRUE(near_vec(uploaded(0).position, { 0, 0, 0 }));
        EXPECT_TRUE(near_vec(uploaded(1).position, { 1, 0, 0 }));
        EXPECT_TRUE(near_vec(uploaded(2).position, { 0, 1, 0 }));
        EXPECT_TRUE(near_vec(uploaded(3).position, { 0, 2, 0 }));
        EXPECT_FLOAT_EQ(uploaded(0).thickness, 1.0F);
        EXPECT_FLOAT_EQ(uploaded(3).thickness, 4.0F);
    }

    TEST_F(LineSegmentsNodeTest, ComputeFrameKeepsPerVertexColour)
    {
        node.add_line(make_vertex({ 0, 0, 0 }, 2.0F, { 1, 0, 0 }),
            make_vertex({ 1, 0, 0 }, 2.0F, { 0, 0, 1 }));
        node.compute_frame();

        EXPECT_TRUE(near_vec(uploaded(0).color, { 1, 0, 0 }));
        EXPECT_TRUE(near_vec(uploaded(1).color, { 0, 0, 1 }));
    }

    TEST_F(LineSegmentsNodeTest, ComputeFrameUpdatesLayoutVertexCount)
    {
        node.add_line(make_vertex({ 0, 0, 0 }), make_vertex({ 1, 0, 0 }));
        node.add_line(make_vertex({ 0, 1, 0 }), make_vertex({ 1, 1, 0 }));
        node.compute_frame();

        ASSERT_TRUE(node.get_vertex_layout().has_value());
        EXPECT_EQ(node.get_vertex_layout()->vertex_count, 4U);
    }

    TEST_F(LineSegmentsNodeTest, ComputeFrameRequestsGpuUpload)
    {
        node.add_line(make_vertex({ 0, 0, 0 }), make_vertex({ 1, 0, 0 }));
        node.clear_gpu_update_flag();

        node.compute_frame();

        EXPECT_TRUE(node.needs_gpu_update());
    }

    TEST_F(LineSegmentsNodeTest, ComputeFrameMarksVertexDataAsNeedingUpdate)
    {
        node.add_line(make_vertex({ 0, 0, 0 }), make_vertex({ 1, 0, 0 }));
        node.clear_gpu_update_flag();

        node.compute_frame();

        EXPECT_TRUE(node.needs_vertex_data_update());
    }

    TEST_F(LineSegmentsNodeTest, UnchangedSegmentsAreNotReuploaded)
    {
        node.add_line(make_vertex({ 0, 0, 0 }), make_vertex({ 1, 0, 0 }));
        node.compute_frame();
        node.clear_gpu_update_flag();

        node.compute_frame();

        EXPECT_FALSE(node.needs_gpu_update());
        EXPECT_EQ(node.get_vertex_count(), 2U);
    }

    TEST_F(LineSegmentsNodeTest, SegmentsAddedAfterAFrameAppearInTheNext)
    {
        node.add_line(make_vertex({ 0, 0, 0 }), make_vertex({ 1, 0, 0 }));
        node.compute_frame();

        node.add_line(make_vertex({ 5, 0, 0 }), make_vertex({ 6, 0, 0 }));
        node.compute_frame();

        ASSERT_EQ(node.get_vertex_count(), 4U);
        EXPECT_TRUE(near_vec(uploaded(0).position, { 0, 0, 0 }));
        EXPECT_TRUE(near_vec(uploaded(3).position, { 6, 0, 0 }));
    }

    TEST_F(LineSegmentsNodeTest, MoreSegmentsThanInitialCapacityAreUploaded)
    {
        for (int i = 0; i < 30; ++i) {
            const auto f = static_cast<float>(i);
            node.add_line(make_vertex({ f, 0, 0 }), make_vertex({ f, 1, 0 }));
        }
        node.compute_frame();

        ASSERT_EQ(node.get_vertex_count(), 60U);
        EXPECT_TRUE(near_vec(uploaded(58).position, { 29, 0, 0 }));
        EXPECT_TRUE(near_vec(uploaded(59).position, { 29, 1, 0 }));
    }

    TEST_F(LineSegmentsNodeTest, AddAxisRunsFromOriginAlongDirectionScaledByLength)
    {
        node.add_axis(make_vertex({ 1, 2, 3 }, 5.0F, { 0, 1, 0 }), { 0, 1, 0 }, 4.0F);
        node.compute_frame();

        ASSERT_EQ(node.get_vertex_count(), 2U);
        EXPECT_TRUE(near_vec(uploaded(0).position, { 1, 2, 3 }));
        EXPECT_TRUE(near_vec(uploaded(1).position, { 1, 6, 3 }));
    }

    TEST_F(LineSegmentsNodeTest, AddAxisTipInheritsOriginColourAndThickness)
    {
        node.add_axis(make_vertex({ 0, 0, 0 }, 5.0F, { 0, 1, 0 }), { 1, 0, 0 }, 2.0F);
        node.compute_frame();

        EXPECT_TRUE(near_vec(uploaded(1).color, { 0, 1, 0 }));
        EXPECT_FLOAT_EQ(uploaded(1).thickness, 5.0F);
    }

    TEST_F(LineSegmentsNodeTest, AddAxisMultipliesTheDirectionByLength)
    {
        node.add_axis(make_vertex({ 0, 0, 0 }), { 2, 0, 0 }, 3.0F);
        node.compute_frame();

        EXPECT_TRUE(near_vec(uploaded(1).position, { 6, 0, 0 }));
    }

    TEST_F(LineSegmentsNodeTest, ClearSegmentsEmptiesTheNode)
    {
        node.add_line(make_vertex({ 0, 0, 0 }), make_vertex({ 1, 0, 0 }));
        node.compute_frame();

        node.clear_segments();

        EXPECT_EQ(node.get_segment_count(), 0U);
    }

    TEST_F(LineSegmentsNodeTest, ClearedNodeUploadsNothingOnTheNextFrame)
    {
        node.add_line(make_vertex({ 0, 0, 0 }), make_vertex({ 1, 0, 0 }));
        node.compute_frame();
        ASSERT_EQ(node.get_vertex_count(), 2U);

        node.clear_segments();
        node.compute_frame();

        EXPECT_EQ(node.get_vertex_count(), 0U);
    }

    TEST_F(LineSegmentsNodeTest, ClearedNodeResetsLayoutVertexCount)
    {
        node.add_line(make_vertex({ 0, 0, 0 }), make_vertex({ 1, 0, 0 }));
        node.compute_frame();

        node.clear_segments();
        node.compute_frame();

        ASSERT_TRUE(node.get_vertex_layout().has_value());
        EXPECT_EQ(node.get_vertex_layout()->vertex_count, 0U);
    }

    TEST_F(LineSegmentsNodeTest, EmptyNodeFramesAreHarmlessAndStaySettled)
    {
        node.compute_frame();
        EXPECT_EQ(node.get_vertex_count(), 0U);

        node.clear_gpu_update_flag();
        node.compute_frame();
        node.compute_frame();

        EXPECT_FALSE(node.needs_gpu_update());
    }

    TEST_F(LineSegmentsNodeTest, SegmentsCanBeAddedInteractivelyAcrossFramesToAnEmptyNode)
    {
        node.compute_frame();

        for (int i = 1; i <= 3; ++i) {
            const auto f = static_cast<float>(i);
            node.add_line(make_vertex({ f, 0, 0 }), make_vertex({ f, 1, 0 }));
            node.compute_frame();

            ASSERT_EQ(node.get_vertex_count(), static_cast<uint32_t>(2 * i));
            EXPECT_EQ(node.get_vertex_layout()->vertex_count, static_cast<uint32_t>(2 * i));
        }
    }

    TEST_F(LineSegmentsNodeTest, UploadBetweenAddAndFrameDoesNotLoseTheSegments)
    {
        node.add_line(make_vertex({ 0, 0, 0 }), make_vertex({ 1, 0, 0 }));
        node.clear_gpu_update_flag();

        node.compute_frame();

        ASSERT_EQ(node.get_vertex_count(), 2U);
        EXPECT_TRUE(near_vec(uploaded(1).position, { 1, 0, 0 }));
    }

    TEST_F(LineSegmentsNodeTest, ProcessSampleUploadsAFrameAndReturnsZero)
    {
        node.add_line(make_vertex({ 0, 0, 0 }), make_vertex({ 1, 0, 0 }));

        EXPECT_DOUBLE_EQ(node.process_sample(0.0), 0.0);
        EXPECT_EQ(node.get_vertex_count(), 2U);
    }

    TEST_F(LineSegmentsNodeTest, TangentsSitCentredOnEachPathVertexAlongThePath)
    {
        node.add_tangent(straight_path(3), 0.4F);
        node.compute_frame();

        ASSERT_EQ(node.get_segment_count(), 2U);
        EXPECT_TRUE(near_vec(uploaded(0).position, { -0.2F, 0, 0 }));
        EXPECT_TRUE(near_vec(uploaded(1).position, { 0.2F, 0, 0 }));
        EXPECT_TRUE(near_vec(uploaded(2).position, { 0.8F, 0, 0 }));
        EXPECT_TRUE(near_vec(uploaded(3).position, { 1.2F, 0, 0 }));
    }

    TEST_F(LineSegmentsNodeTest, TangentsTakeTheColourAndThicknessOfTheirPathVertex)
    {
        node.add_tangent(straight_path(3), 0.4F);
        node.compute_frame();

        EXPECT_TRUE(near_vec(uploaded(0).color, { 0.5F, 0.25F, 0.125F }));
        EXPECT_FLOAT_EQ(uploaded(0).thickness, 3.0F);
        EXPECT_FLOAT_EQ(uploaded(2).thickness, 4.0F);
    }

    TEST_F(LineSegmentsNodeTest, TangentStrideSkipsPathVertices)
    {
        node.add_tangent(straight_path(5), 0.4F, 2);

        EXPECT_EQ(node.get_segment_count(), 2U);
    }

    TEST_F(LineSegmentsNodeTest, NormalsAreCentredOnSegmentMidpointsAndPerpendicular)
    {
        node.add_normal(straight_path(2), 2.0F);
        node.compute_frame();

        ASSERT_EQ(node.get_segment_count(), 1U);
        const glm::vec3 a = uploaded(0).position;
        const glm::vec3 b = uploaded(1).position;
        EXPECT_TRUE(near_vec((a + b) * 0.5F, { 0.5F, 0, 0 }));
        EXPECT_NEAR(glm::dot(b - a, glm::vec3(1, 0, 0)), 0.0F, 1e-5F);
    }

    TEST_F(LineSegmentsNodeTest, NormalLengthScalesTheDrawnSegment)
    {
        node.add_normal(straight_path(2), 2.0F);
        node.compute_frame();

        EXPECT_NEAR(glm::length(uploaded(1).position - uploaded(0).position), 2.0F, 1e-5F);
    }

    TEST_F(LineSegmentsNodeTest, CurvatureRunsFromThePathVertexByTheScaledSecondDifference)
    {
        const std::vector<LineVertex> path {
            make_vertex({ 0, 0, 0 }),
            make_vertex({ 1, 1, 0 }),
            make_vertex({ 2, 0, 0 }),
        };
        node.add_curvature(path, 0.5F);
        node.compute_frame();

        ASSERT_EQ(node.get_segment_count(), 1U);
        EXPECT_TRUE(near_vec(uploaded(0).position, { 1, 1, 0 }));
        EXPECT_TRUE(near_vec(uploaded(1).position, { 1, 0, 0 }));
    }

    TEST_F(LineSegmentsNodeTest, StraightPathHasZeroLengthCurvature)
    {
        node.add_curvature(straight_path(4), 1.0F);
        node.compute_frame();

        ASSERT_EQ(node.get_segment_count(), 2U);
        EXPECT_TRUE(near_vec(uploaded(0).position, uploaded(1).position));
    }

    TEST_F(LineSegmentsNodeTest, AnnotationsOnPathsTooShortAddNothing)
    {
        node.add_tangent(straight_path(1), 1.0F);
        node.add_normal(straight_path(1), 1.0F);
        node.add_curvature(straight_path(2), 1.0F);
        node.add_tangent({}, 1.0F);

        EXPECT_EQ(node.get_segment_count(), 0U);
    }

    TEST_F(LineSegmentsNodeTest, ZeroStrideAddsNothing)
    {
        node.add_tangent(straight_path(4), 1.0F, 0);
        node.add_normal(straight_path(4), 1.0F, 0);
        node.add_curvature(straight_path(4), 1.0F, 0);

        EXPECT_EQ(node.get_segment_count(), 0U);
    }

    TEST_F(LineSegmentsNodeTest, CoincidentPathVerticesAreSkipped)
    {
        const std::vector<LineVertex> path {
            make_vertex({ 0, 0, 0 }),
            make_vertex({ 0, 0, 0 }),
            make_vertex({ 1, 0, 0 }),
        };
        node.add_tangent(path, 1.0F);

        EXPECT_EQ(node.get_segment_count(), 1U);
    }

    TEST_F(LineSegmentsNodeTest, AnnotationsAppendAfterExistingSegments)
    {
        node.add_line(make_vertex({ 9, 9, 9 }), make_vertex({ 8, 8, 8 }));
        node.add_tangent(straight_path(2), 1.0F);
        node.compute_frame();

        ASSERT_EQ(node.get_segment_count(), 2U);
        EXPECT_TRUE(near_vec(uploaded(0).position, { 9, 9, 9 }));
        EXPECT_TRUE(near_vec(uploaded(2).position, { -0.5F, 0, 0 }));
    }

    TEST_F(LineSegmentsNodeTest, AnnotationMarksNodeForUploadOnNextFrame)
    {
        node.add_line(make_vertex({ 0, 0, 0 }), make_vertex({ 1, 0, 0 }));
        node.compute_frame();
        node.clear_gpu_update_flag();

        node.add_tangent(straight_path(2), 1.0F);
        node.compute_frame();

        EXPECT_TRUE(node.needs_gpu_update());
        EXPECT_EQ(node.get_vertex_count(), 4U);
    }

} // namespace

}
