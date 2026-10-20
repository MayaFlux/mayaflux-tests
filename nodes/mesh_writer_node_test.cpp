#include "gtest/gtest.h"

#include "MayaFlux/Nodes/Graphics/MeshWriterNode.hpp"

namespace MayaFlux::Test {

namespace {

    using Nodes::MeshVertex;
    using Nodes::GpuSync::MeshWriterNode;

    MeshVertex make_vertex(float seed)
    {
        MeshVertex v;
        v.position = { seed, seed + 1.0F, seed + 2.0F };
        v.color = { 0.1F * seed, 0.2F * seed, 0.3F * seed };
        v.weight = 0.5F * seed;
        v.uv = { 0.25F * seed, 0.75F * seed };
        v.normal = { 0.0F, 1.0F, 0.0F };
        return v;
    }

    std::vector<MeshVertex> make_vertices(uint32_t count, float first = 1.0F)
    {
        std::vector<MeshVertex> v;
        v.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            v.push_back(make_vertex(first + static_cast<float>(i)));
        }
        return v;
    }

    bool same_vertex(const MeshVertex& a, const MeshVertex& b)
    {
        return a.position == b.position
            && a.color == b.color
            && a.weight == b.weight
            && a.uv == b.uv
            && a.normal == b.normal
            && a.tangent == b.tangent;
    }

    const std::vector<uint32_t> QUAD_INDICES { 0, 1, 2, 2, 3, 0 };

    class MeshWriterNodeTest : public ::testing::Test {
    protected:
        MeshWriterNode node { 16 };

        void load_quad()
        {
            node.set_mesh(make_vertices(4), QUAD_INDICES);
        }

        void expect_uploaded(const std::vector<MeshVertex>& vertices, const std::vector<uint32_t>& indices) const
        {
            ASSERT_EQ(node.get_vertex_count(), vertices.size());
            for (uint32_t i = 0; i < vertices.size(); ++i) {
                EXPECT_TRUE(same_vertex(node.get_vertex_typed<MeshVertex>(i), vertices[i])) << "uploaded vertex " << i;
            }
            const auto up = node.get_index_data();
            ASSERT_EQ(up.size(), indices.size());
            EXPECT_TRUE(std::equal(up.begin(), up.end(), indices.begin()));
        }
    };

    TEST_F(MeshWriterNodeTest, ConstructionStartsEmptyWithTriangleListTopology)
    {
        EXPECT_EQ(node.get_mesh_vertex_count(), 0U);
        EXPECT_EQ(node.get_mesh_face_count(), 0U);
        EXPECT_EQ(node.get_vertex_count(), 0U);
        EXPECT_FALSE(node.has_indices());
        EXPECT_EQ(node.get_vertex_stride(), sizeof(MeshVertex));
        EXPECT_EQ(node.get_primitive_topology(), Portal::Graphics::PrimitiveTopology::TRIANGLE_LIST);
        EXPECT_FALSE(node.get_mesh_data().has_value());
    }

    TEST_F(MeshWriterNodeTest, ConstructionLayoutDescribesNoVerticesYet)
    {
        auto layout = node.get_vertex_layout();

        ASSERT_TRUE(layout.has_value());
        EXPECT_EQ(layout->stride_bytes, sizeof(MeshVertex));
        EXPECT_EQ(layout->vertex_count, 0U);
    }

    TEST_F(MeshWriterNodeTest, SetMeshStoresVerticesAndIndices)
    {
        const auto verts = make_vertices(4);
        node.set_mesh(verts, QUAD_INDICES);

        ASSERT_EQ(node.get_mesh_vertex_count(), 4U);
        EXPECT_EQ(node.get_mesh_face_count(), 2U);
        for (size_t i = 0; i < 4; ++i) {
            EXPECT_TRUE(same_vertex(node.get_mesh_vertices()[i], verts[i]));
        }
        EXPECT_EQ(node.get_mesh_indices(), QUAD_INDICES);
    }

    TEST_F(MeshWriterNodeTest, SetMeshReplacesThePreviousMesh)
    {
        load_quad();
        node.set_mesh(make_vertices(3, 10.0F), std::vector<uint32_t> { 0, 1, 2 });

        EXPECT_EQ(node.get_mesh_vertex_count(), 3U);
        EXPECT_EQ(node.get_mesh_face_count(), 1U);
    }

    TEST_F(MeshWriterNodeTest, SettingVerticesLeavesTheIndicesAlone)
    {
        load_quad();
        node.set_mesh_vertices(make_vertices(6, 20.0F));

        EXPECT_EQ(node.get_mesh_vertex_count(), 6U);
        EXPECT_EQ(node.get_mesh_indices(), QUAD_INDICES);
    }

    TEST_F(MeshWriterNodeTest, SettingIndicesLeavesTheVerticesAlone)
    {
        load_quad();
        node.set_mesh_indices(std::vector<uint32_t> { 0, 1, 2 });

        EXPECT_EQ(node.get_mesh_vertex_count(), 4U);
        EXPECT_EQ(node.get_mesh_face_count(), 1U);
    }

    TEST_F(MeshWriterNodeTest, SettersFlagWhatChanged)
    {
        load_quad();
        node.compute_frame();

        node.clear_gpu_update_flag();
        node.set_mesh_vertices(make_vertices(4));
        EXPECT_TRUE(node.needs_vertex_data_update());
        EXPECT_TRUE(node.needs_layout_update());

        node.clear_gpu_update_flag();
        node.set_mesh_indices(QUAD_INDICES);
        EXPECT_TRUE(node.needs_vertex_data_update());
        EXPECT_FALSE(node.needs_layout_update());
    }

    TEST_F(MeshWriterNodeTest, ClearMeshEmptiesBothArrays)
    {
        load_quad();
        node.clear_mesh();

        EXPECT_EQ(node.get_mesh_vertex_count(), 0U);
        EXPECT_EQ(node.get_mesh_face_count(), 0U);
        EXPECT_TRUE(node.get_mesh_indices().empty());
    }

    TEST_F(MeshWriterNodeTest, MeshDataRoundTripsThroughAnotherNode)
    {
        const auto verts = make_vertices(4);
        node.set_mesh(verts, QUAD_INDICES);

        const auto data = node.get_mesh_data();
        ASSERT_TRUE(data.has_value());

        MeshWriterNode other { 4 };
        other.set_mesh(*data);

        ASSERT_EQ(other.get_mesh_vertex_count(), 4U);
        for (size_t i = 0; i < 4; ++i) {
            EXPECT_TRUE(same_vertex(other.get_mesh_vertices()[i], verts[i])) << "vertex " << i;
        }
        EXPECT_EQ(other.get_mesh_indices(), QUAD_INDICES);
    }

    TEST_F(MeshWriterNodeTest, MeshDataNeedsBothVerticesAndIndices)
    {
        node.set_mesh_vertices(make_vertices(3));
        EXPECT_FALSE(node.get_mesh_data().has_value());

        node.clear_mesh();
        node.set_mesh_indices(QUAD_INDICES);
        EXPECT_FALSE(node.get_mesh_data().has_value());
    }

    TEST_F(MeshWriterNodeTest, EmptyMeshDataIsRejectedAndKeepsTheCurrentMesh)
    {
        load_quad();

        node.set_mesh(Kakshya::MeshData::empty());

        EXPECT_EQ(node.get_mesh_vertex_count(), 4U);
        EXPECT_EQ(node.get_mesh_indices(), QUAD_INDICES);
    }

    TEST_F(MeshWriterNodeTest, ComputeFrameUploadsVerticesAndIndices)
    {
        const auto verts = make_vertices(4);
        node.set_mesh(verts, QUAD_INDICES);

        node.compute_frame();

        expect_uploaded(verts, QUAD_INDICES);
        EXPECT_TRUE(node.has_indices());
        EXPECT_EQ(node.get_index_count(), 6U);
    }

    TEST_F(MeshWriterNodeTest, ComputeFrameUpdatesLayoutVertexCount)
    {
        load_quad();
        node.compute_frame();

        ASSERT_TRUE(node.get_vertex_layout().has_value());
        EXPECT_EQ(node.get_vertex_layout()->vertex_count, 4U);
        EXPECT_EQ(node.get_vertex_layout()->stride_bytes, sizeof(MeshVertex));
    }

    TEST_F(MeshWriterNodeTest, ComputeFrameRequestsGpuUpload)
    {
        load_quad();
        node.clear_gpu_update_flag();

        node.compute_frame();

        EXPECT_TRUE(node.needs_gpu_update());
        EXPECT_TRUE(node.needs_vertex_data_update());
    }

    TEST_F(MeshWriterNodeTest, UnchangedMeshIsNotReuploaded)
    {
        load_quad();
        node.compute_frame();
        node.clear_gpu_update_flag();

        node.compute_frame();

        EXPECT_FALSE(node.needs_gpu_update());
        EXPECT_EQ(node.get_vertex_count(), 4U);
    }

    TEST_F(MeshWriterNodeTest, UploadBetweenSetAndFrameDoesNotLoseTheMesh)
    {
        node.compute_frame();

        load_quad();
        node.clear_gpu_update_flag();
        node.compute_frame();

        EXPECT_EQ(node.get_vertex_count(), 4U);
        EXPECT_EQ(node.get_index_count(), 6U);
    }

    TEST_F(MeshWriterNodeTest, ChangesAfterAFrameAppearInTheNext)
    {
        load_quad();
        node.compute_frame();

        const auto bigger = make_vertices(6, 30.0F);
        const std::vector<uint32_t> indices { 0, 1, 2, 3, 4, 5 };
        node.set_mesh(bigger, indices);
        node.compute_frame();

        expect_uploaded(bigger, indices);
        EXPECT_EQ(node.get_vertex_layout()->vertex_count, 6U);
    }

    TEST_F(MeshWriterNodeTest, VerticesWithoutIndicesStillUpload)
    {
        const auto verts = make_vertices(3);
        node.set_mesh_vertices(verts);
        node.compute_frame();

        EXPECT_EQ(node.get_vertex_count(), 3U);
        EXPECT_FALSE(node.has_indices());
    }

    TEST_F(MeshWriterNodeTest, EmptyNodeFramesAreHarmlessAndStaySettled)
    {
        node.compute_frame();
        EXPECT_EQ(node.get_vertex_count(), 0U);

        node.clear_gpu_update_flag();
        node.compute_frame();
        node.compute_frame();

        EXPECT_FALSE(node.needs_gpu_update());
    }

    TEST_F(MeshWriterNodeTest, MeshCanBeBuiltUpInteractivelyFromEmpty)
    {
        node.compute_frame();

        node.set_mesh_vertices(make_vertices(3));
        node.compute_frame();
        EXPECT_EQ(node.get_vertex_count(), 3U);
        EXPECT_FALSE(node.has_indices());

        node.set_mesh_indices(std::vector<uint32_t> { 0, 1, 2 });
        node.compute_frame();
        EXPECT_EQ(node.get_vertex_count(), 3U);
        EXPECT_EQ(node.get_index_count(), 3U);
    }

    TEST_F(MeshWriterNodeTest, ClearedMeshUploadsNoVertices)
    {
        load_quad();
        node.compute_frame();

        node.clear_mesh();
        node.compute_frame();

        EXPECT_EQ(node.get_vertex_count(), 0U);
    }

    TEST_F(MeshWriterNodeTest, ClearedMeshUploadsNoIndices)
    {
        load_quad();
        node.compute_frame();

        node.clear_mesh();
        node.compute_frame();

        EXPECT_FALSE(node.has_indices());
        EXPECT_EQ(node.get_index_count(), 0U);
    }

    TEST_F(MeshWriterNodeTest, ClearedMeshResetsLayoutVertexCount)
    {
        load_quad();
        node.compute_frame();

        node.clear_mesh();
        node.compute_frame();

        ASSERT_TRUE(node.get_vertex_layout().has_value());
        EXPECT_EQ(node.get_vertex_layout()->vertex_count, 0U);
    }

    TEST_F(MeshWriterNodeTest, ProcessSampleUploadsAFrameAndReturnsZero)
    {
        load_quad();

        EXPECT_DOUBLE_EQ(node.process_sample(0.0), 0.0);
        EXPECT_EQ(node.get_vertex_count(), 4U);
    }

    TEST_F(MeshWriterNodeTest, ContextDescribesTheUploadedMesh)
    {
        load_quad();
        node.compute_frame();
        node.update_context(0.0);

        auto& ctx = dynamic_cast<Nodes::GpuSync::GeometryContext&>(node.get_last_context());
        EXPECT_EQ(ctx.vertex_count, 4U);
        EXPECT_EQ(ctx.vertex_stride, sizeof(MeshVertex));
    }

} // namespace

}
