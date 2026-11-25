#include "network_buffer_fixture.hpp"
#include "../../support/test_metadata.hpp"

#include "MayaFlux/Buffers/Network/AssembledGeometryBuffer.hpp"
#include "MayaFlux/Buffers/Shaders/VertexFieldProcessor.hpp"
#include "MayaFlux/Kinesis/Tendency/DualField.hpp"
#include "MayaFlux/Kinesis/Tendency/ShaderCompat.hpp"
#include "MayaFlux/Nodes/Graphics/MeshWriterNode.hpp"
#include "MayaFlux/Nodes/Network/AssemblyNetwork.hpp"
#include "MayaFlux/Nodes/Network/Operators/GpuFieldOperator.hpp"
#include "MayaFlux/Portal/Graphics/PrimitiveMill.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::vulkan>() }))

namespace MayaFlux::Test {
namespace {

    using Buffers::AssembledGeometryBuffer;
    using Kinesis::FieldTarget;
    using Nodes::Network::AssemblyNetwork;
    using Nodes::Network::GpuFieldOperator;
    using Nodes::Network::SpatialFieldConfig;
    using Portal::Graphics::DrawRun;
    using Portal::Graphics::PrimitiveTopology;

    namespace assembled_fields {

        using namespace MayaFlux::ShaderCompat;

        const auto push_x = MF_FIELD(push_x, [](vec3 p) -> vec3 {
            return p * 0.0f + vec3(0.25f, 0.0f, 0.0f);
        });

    } // namespace assembled_fields

    std::vector<glm::vec3> line_of(uint32_t count, float y = 0.0F)
    {
        std::vector<glm::vec3> out;
        out.reserve(count);
        for (uint32_t i = 0; i < count; ++i)
            out.emplace_back(0.1F * static_cast<float>(i), y, 0.0F);
        return out;
    }

    Kakshya::MeshData indexed_quad()
    {
        std::vector<Nodes::MeshVertex> corners(4);
        corners[0].position = { -0.5F, -0.5F, 0.0F };
        corners[1].position = { 0.5F, -0.5F, 0.0F };
        corners[2].position = { 0.5F, 0.5F, 0.0F };
        corners[3].position = { -0.5F, 0.5F, 0.0F };

        Nodes::GpuSync::MeshWriterNode node(16);
        node.set_mesh(corners, std::vector<uint32_t> { 0, 1, 2, 2, 3, 0 });
        node.compute_frame();
        return node.get_mesh_data().value();
    }

    class AssembledGeometryBufferTest : public NetworkBufferDeviceTest {
    protected:
        void SetUp() override
        {
            NetworkBufferDeviceTest::SetUp();
            if (IsSkipped())
                return;
            assembly = std::make_shared<AssemblyNetwork>();
        }

        void TearDown() override
        {
            if (assembled) {
                if (auto chain = assembled->get_processing_chain()) {
                    if (auto render_processor = assembled->get_render_processor())
                        chain->remove_processor_anywhere(render_processor, assembled);
                }
                assembled->set_default_processor(nullptr);
                if (assembled->is_initialized() && service)
                    service->destroy_buffer(assembled);
            }
            assembled.reset();
            assembly.reset();
            NetworkBufferDeviceTest::TearDown();
        }

        void attach_assembled(float over_allocate_factor = 1.5F)
        {
            assembled = std::make_shared<AssembledGeometryBuffer>(assembly, over_allocate_factor);
            service->initialize_buffer(assembled);
            ASSERT_TRUE(assembled->is_initialized());
            assembled->setup_processors(Buffers::ProcessingToken::GRAPHICS_BACKEND);
            assembled->process_default();
        }

        void render_assembled()
        {
            window = std::make_shared<ScriptedWindow>(320, 240);
            assembled->setup_rendering({ .target_window = window });
            ASSERT_NE(assembled->get_render_processor(), nullptr);
        }

        [[nodiscard]] uint32_t drawn_vertices() const
        {
            const auto layout = assembled->get_vertex_layout();
            return layout ? layout->vertex_count : 0U;
        }

        std::vector<Kakshya::Vertex> uploaded_vertices(size_t count)
        {
            std::vector<uint8_t> bytes(assembled->get_size_bytes());
            Buffers::download_from_gpu(assembled, bytes.data(), bytes.size());
            std::vector<Kakshya::Vertex> out(count);
            std::memcpy(out.data(), bytes.data(), count * sizeof(Kakshya::Vertex));
            return out;
        }

        std::vector<uint32_t> uploaded_indices()
        {
            const size_t byte_count = assembled->get_index_buffer_size();
            if (!assembled->has_index_buffer() || byte_count == 0)
                return {};

            RawVulkanBuffer destination(
                backend(), byte_count, vk::BufferUsageFlagBits::eTransferDst, true);
            service->copy_buffer(
                static_cast<void*>(static_cast<VkBuffer>(assembled->get_index_buffer())),
                static_cast<void*>(static_cast<VkBuffer>(destination.buffer())),
                byte_count, 0, 0);

            std::vector<uint32_t> indices(byte_count / sizeof(uint32_t));
            std::memcpy(indices.data(), destination.mapped(), byte_count);
            return indices;
        }

        [[nodiscard]] std::vector<DrawRun> runs() const
        {
            return assembled->get_render_processor()->get_runs();
        }

        static void expect_run(const DrawRun& run, PrimitiveTopology topology, uint32_t offset, uint32_t count)
        {
            EXPECT_EQ(run.topology, topology);
            EXPECT_EQ(run.vertex_offset, offset);
            EXPECT_EQ(run.vertex_count, count);
        }

        [[nodiscard]] std::shared_ptr<Buffers::VertexFieldProcessor> field_stage() const
        {
            for (const auto& processor : assembled->get_processing_chain()->get_processors(assembled)) {
                if (auto pass = std::dynamic_pointer_cast<Buffers::VertexFieldProcessor>(processor))
                    return pass;
            }
            return nullptr;
        }

        std::shared_ptr<AssemblyNetwork> assembly;
        std::shared_ptr<AssembledGeometryBuffer> assembled;
    };

    // Construction

    TEST_F(AssembledGeometryBufferTest, ANullNetworkIsRejected)
    {
        EXPECT_ANY_THROW(std::make_shared<AssembledGeometryBuffer>(std::shared_ptr<AssemblyNetwork> {}));
    }

    TEST_F(AssembledGeometryBufferTest, AnEmptyAssemblyDrawsNothingAndLinksNoIndices)
    {
        attach_assembled();

        EXPECT_EQ(drawn_vertices(), 0U);
        EXPECT_FALSE(assembled->has_index_buffer());
    }

    // Streams

    TEST_F(AssembledGeometryBufferTest, UnindexedItemsUploadNoIndexStream)
    {
        assembly->add_geometry(line_of(3));
        assembly->add_geometry(line_of(3, 0.5F), PrimitiveTopology::TRIANGLE_LIST);
        attach_assembled();

        EXPECT_EQ(drawn_vertices(), 6U);
        EXPECT_FALSE(assembled->has_index_buffer());
    }

    TEST_F(AssembledGeometryBufferTest, MeshIndicesAreOffsetByTheVerticesBeforeThem)
    {
        assembly->add_geometry(line_of(3));
        assembly->add_geometry(indexed_quad());
        attach_assembled();

        EXPECT_EQ(drawn_vertices(), 7U);
        EXPECT_EQ(uploaded_indices(), (std::vector<uint32_t> { 3, 4, 5, 5, 6, 3 }));

        const auto vertices = uploaded_vertices(7);
        EXPECT_FLOAT_EQ(vertices[3].position.x, -0.5F);
        EXPECT_FLOAT_EQ(vertices[5].position.y, 0.5F);
    }

    TEST_F(AssembledGeometryBufferTest, UnindexedTrianglesBesideAMeshAreNumberedInOrder)
    {
        assembly->add_geometry(indexed_quad());
        assembly->add_geometry(line_of(3), PrimitiveTopology::TRIANGLE_LIST);
        attach_assembled();

        EXPECT_EQ(uploaded_indices(), (std::vector<uint32_t> { 0, 1, 2, 2, 3, 0, 4, 5, 6 }));
    }

    // Runs

    TEST_F(AssembledGeometryBufferTest, TriangleRunsCountIndicesAndOtherRunsCountVertices)
    {
        assembly->add_geometry(indexed_quad());
        assembly->add_geometry(line_of(2));
        assembly->add_geometry(line_of(2, 0.5F), PrimitiveTopology::LINE_LIST);
        assembly->add_geometry(indexed_quad());
        attach_assembled();
        render_assembled();

        const auto drawn = runs();
        ASSERT_EQ(drawn.size(), 4U);
        expect_run(drawn[0], PrimitiveTopology::TRIANGLE_LIST, 0, 6);
        expect_run(drawn[1], PrimitiveTopology::POINT_LIST, 4, 2);
        expect_run(drawn[2], PrimitiveTopology::LINE_LIST, 6, 2);
        expect_run(drawn[3], PrimitiveTopology::TRIANGLE_LIST, 6, 6);
    }

    TEST_F(AssembledGeometryBufferTest, NeighbouringMeshesShareOneRun)
    {
        assembly->add_geometry(indexed_quad());
        assembly->add_geometry(indexed_quad());
        attach_assembled();
        render_assembled();

        const auto drawn = runs();
        ASSERT_EQ(drawn.size(), 1U);
        expect_run(drawn[0], PrimitiveTopology::TRIANGLE_LIST, 0, 12);
    }

    TEST_F(AssembledGeometryBufferTest, TheDrawIsMilledAndTheMillAcceptsTheMixedRuns)
    {
        assembly->add_geometry(indexed_quad());
        assembly->add_geometry(line_of(2));
        assembly->add_geometry(line_of(2, 0.5F), PrimitiveTopology::LINE_LIST);
        attach_assembled();
        render_assembled();
        ASSERT_TRUE(assembled->get_render_processor()->is_triangulate());

        Portal::Graphics::PrimitiveMill mill;
        const auto drawn = runs();

        EXPECT_EQ(mill.mill(assembled, drawn, {}), 6U + 12U + 6U);
    }

    // Changes while attached

    TEST_F(AssembledGeometryBufferTest, AnItemAddedLaterGrowsBothStreams)
    {
        assembly->add_geometry(indexed_quad());
        attach_assembled(1.0F);
        const size_t first_size = assembled->get_size_bytes();

        assembly->add_geometry(line_of(200));
        assembly->add_geometry(indexed_quad());
        assembled->process_default();

        EXPECT_GT(assembled->get_size_bytes(), first_size);
        EXPECT_EQ(drawn_vertices(), 208U);
        EXPECT_EQ(uploaded_indices(),
            (std::vector<uint32_t> { 0, 1, 2, 2, 3, 0, 204, 205, 206, 206, 207, 204 }));
    }

    TEST_F(AssembledGeometryBufferTest, RemovingTheLastMeshUnlinksTheIndexStream)
    {
        auto quad = assembly->add_geometry(indexed_quad());
        assembly->add_geometry(line_of(3));
        attach_assembled();
        ASSERT_TRUE(assembled->has_index_buffer());

        ASSERT_TRUE(assembly->remove_geometry(quad));
        assembled->process_default();

        EXPECT_FALSE(assembled->has_index_buffer());
        EXPECT_EQ(drawn_vertices(), 3U);
    }

    TEST_F(AssembledGeometryBufferTest, ADisabledNetworkIsNotUploaded)
    {
        assembly->add_geometry(line_of(3));
        attach_assembled();

        assembly->set_enabled(false);
        assembly->add_geometry(line_of(3));
        assembled->process_default();

        EXPECT_EQ(drawn_vertices(), 3U);
    }

    // Field pass

    TEST_F(AssembledGeometryBufferTest, ABoundFieldMovesEveryItemWithoutAccumulating)
    {
        assembly->add_geometry(indexed_quad());
        assembly->add_geometry(line_of(3));
        auto field_op = assembly->get_operator_chain()->emplace<GpuFieldOperator>(
            Kakshya::VertexLayout::for_meshes(sizeof(Kakshya::Vertex)));
        field_op->bind(FieldTarget::POSITION, assembled_fields::push_x);
        attach_assembled();
        ASSERT_NE(field_stage(), nullptr);

        for (int cycle = 0; cycle < 2; ++cycle) {
            assembled->process_default();
            assembled->get_processing_chain()->process_complete(assembled);

            const auto vertices = uploaded_vertices(7);
            EXPECT_NEAR(vertices[0].position.x, -0.25F, 1e-5F) << "cycle " << cycle;
            EXPECT_NEAR(vertices[4].position.x, 0.25F, 1e-5F) << "cycle " << cycle;
            EXPECT_NEAR(vertices[6].position.x, 0.45F, 1e-5F) << "cycle " << cycle;
        }
    }

    TEST_F(AssembledGeometryBufferTest, SpatialStagesAreNotWiredButTheFieldPassIs)
    {
        assembly->add_geometry(line_of(3));
        auto field_op = assembly->get_operator_chain()->emplace<GpuFieldOperator>(
            Kakshya::VertexLayout::for_meshes(sizeof(Kakshya::Vertex)),
            SpatialFieldConfig { .cell_size = 0.5F, .density_color = true });
        field_op->bind(FieldTarget::POSITION, assembled_fields::push_x);
        attach_assembled();

        EXPECT_EQ(assembled->get_processing_chain()->get_processors(assembled).size(), 1U);
        EXPECT_NE(field_stage(), nullptr);
    }

} // namespace
} // namespace MayaFlux::Test
