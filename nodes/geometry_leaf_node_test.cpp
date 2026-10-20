#include "gtest/gtest.h"

#include "MayaFlux/Nodes/Graphics/GeometryLeafNode.hpp"

namespace MayaFlux::Test {

namespace {

    using Nodes::Vertex;
    using Nodes::GpuSync::GeometryLeafNode;
    using Portal::Graphics::PrimitiveTopology;

    Vertex make_vertex(float seed)
    {
        Vertex v;
        v.position = { seed, seed + 1.0F, seed + 2.0F };
        v.color = { 0.1F * seed, 0.2F * seed, 0.3F * seed };
        v.scalar = 0.5F * seed;
        v.uv = { 0.25F * seed, 0.75F * seed };
        v.normal = { 0.0F, 1.0F, 0.0F };
        v.tangent = { 0.0F, 0.0F, 1.0F };
        return v;
    }

    std::vector<Vertex> make_vertices(uint32_t count, float first = 1.0F)
    {
        std::vector<Vertex> v;
        v.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            v.push_back(make_vertex(first + static_cast<float>(i)));
        }
        return v;
    }

    bool same_vertex(const Vertex& a, const Vertex& b)
    {
        return a.position == b.position
            && a.color == b.color
            && a.scalar == b.scalar
            && a.uv == b.uv
            && a.normal == b.normal
            && a.tangent == b.tangent;
    }

    class GeometryLeafNodeTest : public ::testing::Test {
    protected:
        static void expect_uploaded(const GeometryLeafNode& node, const std::vector<Vertex>& expected)
        {
            ASSERT_EQ(node.get_vertex_count(), expected.size());
            for (uint32_t i = 0; i < expected.size(); ++i) {
                EXPECT_TRUE(same_vertex(node.get_vertex_typed<Vertex>(i), expected[i])) << "uploaded vertex " << i;
            }
        }

        static std::string name_of_third_attribute(const GeometryLeafNode& node)
        {
            const auto layout = node.get_vertex_layout();
            if (!layout || layout->attributes.size() < 3) {
                return {};
            }
            return layout->attributes[2].name;
        }
    };

    TEST_F(GeometryLeafNodeTest, CapacityConstructionStartsEmptyWithVertexStride)
    {
        GeometryLeafNode node(16);

        EXPECT_EQ(node.size(), 0U);
        EXPECT_TRUE(node.get_geometry().empty());
        EXPECT_EQ(node.get_vertex_stride(), sizeof(Vertex));
    }

    TEST_F(GeometryLeafNodeTest, CapacityConstructionReportsNoVerticesBeforeAnyFrame)
    {
        GeometryLeafNode node(16);

        EXPECT_EQ(node.get_vertex_count(), 0U);
    }

    TEST_F(GeometryLeafNodeTest, ConstructionLayoutDescribesNoVerticesYet)
    {
        GeometryLeafNode node(16);
        auto layout = node.get_vertex_layout();

        ASSERT_TRUE(layout.has_value());
        EXPECT_EQ(layout->stride_bytes, sizeof(Vertex));
        EXPECT_EQ(layout->vertex_count, 0U);
    }

    TEST_F(GeometryLeafNodeTest, DefaultTopologyIsPointList)
    {
        GeometryLeafNode node;

        EXPECT_EQ(node.get_primitive_topology(), PrimitiveTopology::POINT_LIST);
    }

    TEST_F(GeometryLeafNodeTest, VertexConstructionHoldsTheGivenVertices)
    {
        const auto verts = make_vertices(3);
        GeometryLeafNode node(verts);

        ASSERT_EQ(node.size(), 3U);
        for (size_t i = 0; i < 3; ++i) {
            EXPECT_TRUE(same_vertex(node.get_geometry()[i], verts[i]));
        }
        ASSERT_TRUE(node.get_vertex_layout().has_value());
        EXPECT_EQ(node.get_vertex_layout()->vertex_count, 3U);
    }

    TEST_F(GeometryLeafNodeTest, PositionConstructionFillsDefaultAttributes)
    {
        GeometryLeafNode node(std::vector<glm::vec3> { { 1, 2, 3 }, { 4, 5, 6 } });

        ASSERT_EQ(node.size(), 2U);
        EXPECT_EQ(node.get_geometry()[0].position, glm::vec3(1, 2, 3));
        EXPECT_EQ(node.get_geometry()[1].position, glm::vec3(4, 5, 6));
        EXPECT_EQ(node.get_geometry()[1].color, glm::vec3(1.0F));
        EXPECT_FLOAT_EQ(node.get_geometry()[1].scalar, 1.0F);
        EXPECT_EQ(node.get_geometry()[1].normal, glm::vec3(0, 0, 1));
    }

    TEST_F(GeometryLeafNodeTest, SetPositionsReplacesEverythingWithDefaultAttributes)
    {
        GeometryLeafNode node(make_vertices(5));
        node.set_positions({ { 7, 8, 9 } });

        ASSERT_EQ(node.size(), 1U);
        EXPECT_EQ(node.get_geometry()[0].position, glm::vec3(7, 8, 9));
        EXPECT_EQ(node.get_geometry()[0].color, glm::vec3(1.0F));
    }

    TEST_F(GeometryLeafNodeTest, SetGeometryReplacesEverything)
    {
        GeometryLeafNode node(make_vertices(5));
        const auto replacement = make_vertices(2, 30.0F);
        node.set_geometry(replacement);

        ASSERT_EQ(node.size(), 2U);
        EXPECT_TRUE(same_vertex(node.get_geometry()[1], replacement[1]));
    }

    TEST_F(GeometryLeafNodeTest, UpdateVertexReplacesOnlyThatVertex)
    {
        GeometryLeafNode node(make_vertices(3));
        node.update_vertex(1, make_vertex(50.0F));

        EXPECT_TRUE(same_vertex(node.get_geometry()[0], make_vertex(1.0F)));
        EXPECT_TRUE(same_vertex(node.get_geometry()[1], make_vertex(50.0F)));
        EXPECT_TRUE(same_vertex(node.get_geometry()[2], make_vertex(3.0F)));
    }

    TEST_F(GeometryLeafNodeTest, UpdateVertexOutOfRangeChangesNothing)
    {
        GeometryLeafNode node(make_vertices(2));
        node.compute_frame();
        node.clear_gpu_update_flag();

        node.update_vertex(2, make_vertex(50.0F));
        node.update_vertex(100, make_vertex(50.0F));
        node.compute_frame();

        EXPECT_EQ(node.size(), 2U);
        EXPECT_FALSE(node.needs_gpu_update());
    }

    TEST_F(GeometryLeafNodeTest, ClearGeometryEmptiesTheNode)
    {
        GeometryLeafNode node(make_vertices(3));
        node.clear_geometry();

        EXPECT_EQ(node.size(), 0U);
    }

    TEST_F(GeometryLeafNodeTest, ComputeFrameUploadsEveryFieldOfEveryVertexInOrder)
    {
        const auto verts = make_vertices(4);
        GeometryLeafNode node(verts);

        node.compute_frame();

        expect_uploaded(node, verts);
    }

    TEST_F(GeometryLeafNodeTest, ComputeFrameUpdatesLayoutVertexCount)
    {
        GeometryLeafNode node(make_vertices(5));
        node.compute_frame();

        ASSERT_TRUE(node.get_vertex_layout().has_value());
        EXPECT_EQ(node.get_vertex_layout()->vertex_count, 5U);
        EXPECT_EQ(node.get_vertex_layout()->stride_bytes, sizeof(Vertex));
    }

    TEST_F(GeometryLeafNodeTest, ComputeFrameRequestsGpuUpload)
    {
        GeometryLeafNode node(make_vertices(2));
        node.compute_frame();
        node.clear_gpu_update_flag();
        node.set_geometry(make_vertices(3));

        node.compute_frame();

        EXPECT_TRUE(node.needs_gpu_update());
        EXPECT_TRUE(node.needs_vertex_data_update());
    }

    TEST_F(GeometryLeafNodeTest, UnchangedGeometryIsNotReuploaded)
    {
        GeometryLeafNode node(make_vertices(3));
        node.compute_frame();
        node.clear_gpu_update_flag();

        node.compute_frame();

        EXPECT_FALSE(node.needs_gpu_update());
        EXPECT_EQ(node.get_vertex_count(), 3U);
    }

    TEST_F(GeometryLeafNodeTest, UploadBetweenAChangeAndTheFrameDoesNotLoseIt)
    {
        GeometryLeafNode node(make_vertices(2));
        node.compute_frame();

        const auto replacement = make_vertices(4, 10.0F);
        node.set_geometry(replacement);
        node.clear_gpu_update_flag();
        node.compute_frame();

        expect_uploaded(node, replacement);
    }

    TEST_F(GeometryLeafNodeTest, VerticesBeyondTheInitialCapacityAreUploaded)
    {
        GeometryLeafNode node(2);
        const auto verts = make_vertices(9);
        node.set_geometry(verts);

        node.compute_frame();

        expect_uploaded(node, verts);
    }

    TEST_F(GeometryLeafNodeTest, UpdatedVertexShowsUpInTheNextFrame)
    {
        auto verts = make_vertices(3);
        GeometryLeafNode node(verts);
        node.compute_frame();

        node.update_vertex(2, make_vertex(50.0F));
        verts[2] = make_vertex(50.0F);
        node.compute_frame();

        expect_uploaded(node, verts);
    }

    TEST_F(GeometryLeafNodeTest, EmptyNodeFramesAreHarmlessAndStaySettled)
    {
        GeometryLeafNode node(8);
        node.compute_frame();
        EXPECT_EQ(node.get_vertex_count(), 0U);

        node.clear_gpu_update_flag();
        node.compute_frame();
        node.compute_frame();

        EXPECT_FALSE(node.needs_gpu_update());
    }

    TEST_F(GeometryLeafNodeTest, VerticesCanBeAddedInteractivelyToAnEmptyNode)
    {
        GeometryLeafNode node(8);
        node.compute_frame();

        auto verts = make_vertices(1);
        for (uint32_t n = 1; n <= 3; ++n) {
            node.set_geometry(verts);
            node.compute_frame();
            expect_uploaded(node, verts);
            verts.push_back(make_vertex(10.0F + static_cast<float>(n)));
        }
    }

    TEST_F(GeometryLeafNodeTest, ClearedGeometryUploadsNothing)
    {
        GeometryLeafNode node(make_vertices(4));
        node.compute_frame();

        node.clear_geometry();
        node.compute_frame();

        EXPECT_EQ(node.get_vertex_count(), 0U);
    }

    TEST_F(GeometryLeafNodeTest, ClearedGeometryResetsLayoutVertexCount)
    {
        GeometryLeafNode node(make_vertices(4));
        node.compute_frame();

        node.clear_geometry();
        node.compute_frame();

        ASSERT_TRUE(node.get_vertex_layout().has_value());
        EXPECT_EQ(node.get_vertex_layout()->vertex_count, 0U);
    }

    TEST_F(GeometryLeafNodeTest, LayoutNamesTheScalarAfterTheTopologyItWasUploadedWith)
    {
        GeometryLeafNode points(make_vertices(2));
        points.compute_frame();
        EXPECT_EQ(name_of_third_attribute(points), "size");

        GeometryLeafNode lines(make_vertices(2));
        lines.set_primitive_topology(PrimitiveTopology::LINE_LIST);
        lines.compute_frame();
        EXPECT_EQ(name_of_third_attribute(lines), "thickness");

        GeometryLeafNode triangles(make_vertices(3));
        triangles.set_primitive_topology(PrimitiveTopology::TRIANGLE_LIST);
        triangles.compute_frame();
        EXPECT_EQ(name_of_third_attribute(triangles), "weight");
    }

    TEST_F(GeometryLeafNodeTest, TopologyChangedAfterAFrameUpdatesTheLayoutOnTheNext)
    {
        GeometryLeafNode node(make_vertices(2));
        node.compute_frame();
        ASSERT_EQ(name_of_third_attribute(node), "size");

        node.set_primitive_topology(PrimitiveTopology::LINE_STRIP);
        node.compute_frame();

        EXPECT_EQ(name_of_third_attribute(node), "thickness");
    }

    TEST_F(GeometryLeafNodeTest, TopologyChangedThroughTheBaseInterfaceUpdatesTheLayout)
    {
        GeometryLeafNode node(make_vertices(3));
        node.compute_frame();
        Nodes::GpuSync::GeometryWriterNode& base = node;

        base.set_primitive_topology(PrimitiveTopology::TRIANGLE_LIST);
        node.compute_frame();

        EXPECT_EQ(name_of_third_attribute(node), "weight");
    }

    TEST_F(GeometryLeafNodeTest, TopologyChangeKeepsTheVerticesAndRequestsUpload)
    {
        const auto verts = make_vertices(3);
        GeometryLeafNode node(verts);
        node.compute_frame();
        node.clear_gpu_update_flag();

        node.set_primitive_topology(PrimitiveTopology::LINE_STRIP);
        node.compute_frame();

        expect_uploaded(node, verts);
        EXPECT_TRUE(node.needs_gpu_update());
    }

    TEST_F(GeometryLeafNodeTest, SettingTheSameTopologyDoesNotRequestAnUpload)
    {
        GeometryLeafNode node(make_vertices(3));
        node.compute_frame();
        node.clear_gpu_update_flag();

        node.set_primitive_topology(PrimitiveTopology::POINT_LIST);
        node.compute_frame();

        EXPECT_FALSE(node.needs_gpu_update());
    }

    TEST_F(GeometryLeafNodeTest, ProcessSampleUploadsAFrameAndReturnsZero)
    {
        const auto verts = make_vertices(3);
        GeometryLeafNode node(verts);

        EXPECT_DOUBLE_EQ(node.process_sample(0.0), 0.0);
        expect_uploaded(node, verts);
    }

    TEST_F(GeometryLeafNodeTest, ContextDescribesTheUploadedVertices)
    {
        GeometryLeafNode node(make_vertices(3));
        node.compute_frame();
        node.update_context(0.0);

        auto& ctx = dynamic_cast<Nodes::GpuSync::GeometryContext&>(node.get_last_context());
        EXPECT_EQ(ctx.vertex_count, 3U);
        EXPECT_EQ(ctx.vertex_stride, sizeof(Vertex));
    }

} // namespace

}
