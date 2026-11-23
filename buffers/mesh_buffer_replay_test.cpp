#include "gtest/gtest.h"
#include "../support/test_metadata.hpp"

#include <thread>

#include <glm/gtc/matrix_transform.hpp>

#include "network/network_buffer_fixture.hpp"

#include "MayaFlux/Buffers/Geometry/GeometryBuffer.hpp"
#include "MayaFlux/Buffers/Geometry/MeshBuffer.hpp"
#include "MayaFlux/Buffers/Shaders/SkinProcessor.hpp"
#include "MayaFlux/Buffers/Shaders/VertexFieldProcessor.hpp"
#include "MayaFlux/Kakshya/NDData/MeshData.hpp"
#include "MayaFlux/Kakshya/NDData/MeshSkin.hpp"
#include "MayaFlux/Kinesis/Tendency/DualField.hpp"
#include "MayaFlux/Kinesis/Tendency/ShaderCompat.hpp"
#include "MayaFlux/Kriya/MeshReflow.hpp"
#include "MayaFlux/Nodes/Graphics/MeshWriterNode.hpp"
#include "MayaFlux/Nodes/Network/Operators/GpuFieldOperator.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::vulkan>() }))

namespace MayaFlux::Test {
namespace {

    using Buffers::MeshBuffer;
    using Kakshya::MeshData;
    using Kinesis::FieldTarget;
    using Kriya::FrameContext;
    using Nodes::MeshVertex;
    using Nodes::Network::GpuFieldOperator;

    namespace replay_fields {

        using namespace MayaFlux::ShaderCompat;

        const auto replay_shift = MF_FIELD(replay_shift, [](vec3 p) -> vec3 {
            return p * 0.0f + vec3(0.25f, 0.0f, 0.0f);
        });

    }


    namespace skinning {

        /** Every vertex follows the first joint of the skin, whole. */
        std::shared_ptr<const Kakshya::MeshSkin> follow_first_joint(size_t vertices, size_t joints = 1)
        {
            Kakshya::MeshSkin skin;
            for (size_t j = 0; j < joints; ++j) {
                skin.joints.push_back(j);
                skin.inverse_bind.push_back(glm::mat4(1.0F));
            }
            skin.influence_joints.assign(vertices, glm::uvec4(0));
            skin.influence_weights.assign(vertices, glm::vec4(1.0F, 0.0F, 0.0F, 0.0F));
            return std::make_shared<const Kakshya::MeshSkin>(std::move(skin));
        }

        /** The skinned record a vertex should become, from the definition of the blend. */
        MeshVertex skin_oracle(const MeshVertex& vertex, const Kakshya::MeshSkin& skin, size_t index,
            const std::vector<glm::mat4>& palette)
        {
            glm::mat4 blended(0.0F);
            float total = 0.0F;
            for (glm::length_t c = 0; c < 4; ++c) {
                const float weight = skin.influence_weights[index][c];
                blended += weight * palette[skin.influence_joints[index][c]];
                total += weight;
            }
            blended += (1.0F - total) * glm::mat4(1.0F);

            const glm::vec3 a0(blended[0]);
            const glm::vec3 a1(blended[1]);
            const glm::vec3 a2(blended[2]);
            const glm::vec3 c0 = glm::cross(a1, a2);
            const glm::vec3 c1 = glm::cross(a2, a0);
            const glm::vec3 c2 = glm::cross(a0, a1);

            MeshVertex result = vertex;
            result.position = glm::vec3(blended * glm::vec4(vertex.position, 1.0F));
            result.normal = glm::normalize(c0 * vertex.normal.x + c1 * vertex.normal.y + c2 * vertex.normal.z);
            const glm::vec3 tangent = glm::normalize(a0 * vertex.tangent.x + a1 * vertex.tangent.y + a2 * vertex.tangent.z);
            result.tangent = glm::vec4(tangent, vertex.tangent.w * (glm::dot(a0, c0) < 0.0F ? -1.0F : 1.0F));
            return result;
        }

    }

    MeshVertex replay_vertex(glm::vec3 position)
    {
        MeshVertex result;
        result.position = position;
        return result;
    }

    std::shared_ptr<const MeshData> replay_frame(
        const std::vector<MeshVertex>& vertices, const std::vector<uint32_t>& indices)
    {
        auto data = MeshData::from_vertices(vertices, std::span<const uint32_t>(indices));
        return std::make_shared<const MeshData>(std::move(data.value()));
    }

    std::vector<MeshVertex> replay_triangles(size_t count, float offset = 0.0F)
    {
        std::vector<MeshVertex> vertices;
        for (size_t i = 0; i < count; ++i) {
            const auto base = static_cast<float>(i);
            vertices.push_back(replay_vertex({ base + offset, 0.0F, 0.0F }));
            vertices.push_back(replay_vertex({ base + offset + 0.5F, 0.0F, 0.0F }));
            vertices.push_back(replay_vertex({ base + offset, 0.5F, 0.0F }));
        }
        return vertices;
    }

    std::vector<uint32_t> replay_indices(size_t triangles, bool reversed = false)
    {
        std::vector<uint32_t> indices;
        for (uint32_t i = 0; i < triangles; ++i) {
            if (reversed) {
                indices.insert(indices.end(), { i * 3 + 2, i * 3 + 1, i * 3 });
            } else {
                indices.insert(indices.end(), { i * 3, i * 3 + 1, i * 3 + 2 });
            }
        }
        return indices;
    }

    void* replay_handle(vk::Buffer buffer)
    {
        return static_cast<void*>(static_cast<VkBuffer>(buffer));
    }

    class MeshBufferReplayDeviceTest : public NetworkBufferDeviceTest {
    protected:
        void TearDown() override
        {
            if (mesh) {
                if (auto chain = mesh->get_processing_chain()) {
                    if (auto render = mesh->get_render_processor())
                        chain->remove_processor_anywhere(render, mesh);
                }
                mesh->set_default_processor(nullptr);
                if (mesh->is_initialized() && service)
                    service->destroy_buffer(mesh);
            }
            mesh.reset();
            field_op.reset();
            if (geometry) {
                geometry->set_default_processor(nullptr);
                if (geometry->is_initialized() && service)
                    service->destroy_buffer(geometry);
                geometry.reset();
            }
            NetworkBufferDeviceTest::TearDown();
        }

        void make(MeshData data)
        {
            mesh = std::make_shared<MeshBuffer>(std::move(data));
        }

        void attach_mesh()
        {
            service->initialize_buffer(mesh);
            ASSERT_TRUE(mesh->is_initialized());
            mesh->setup_processors(Buffers::ProcessingToken::GRAPHICS_BACKEND);
            ASSERT_NE(mesh->get_mesh_processor(), nullptr);
            mesh->process_default();
        }

        void show(const std::shared_ptr<const MeshData>& frame)
        {
            mesh->write_frame(frame);
            mesh->process_default();
        }

        [[nodiscard]] std::vector<MeshVertex> read_mesh_vertices(size_t count)
        {
            std::vector<uint8_t> bytes(mesh->get_size_bytes());
            Buffers::download_from_gpu(mesh, bytes.data(), bytes.size());
            std::vector<MeshVertex> vertices(count);
            std::memcpy(vertices.data(), bytes.data(), count * sizeof(MeshVertex));
            return vertices;
        }

        [[nodiscard]] std::vector<uint32_t> read_mesh_indices(size_t count)
        {
            const size_t byte_count = count * sizeof(uint32_t);
            RawVulkanBuffer destination(
                backend(), byte_count, vk::BufferUsageFlagBits::eTransferDst, true);
            service->copy_buffer(
                replay_handle(mesh->get_index_buffer()), replay_handle(destination.buffer()), byte_count, 0, 0);
            std::vector<uint32_t> indices(count);
            std::memcpy(indices.data(), destination.mapped(), byte_count);
            return indices;
        }

        void expect_shown(const std::vector<MeshVertex>& vertices, const std::vector<uint32_t>& indices)
        {
            const auto read_vertices = read_mesh_vertices(vertices.size());
            for (size_t i = 0; i < vertices.size(); ++i) {
                EXPECT_EQ(read_vertices[i].position, vertices[i].position) << "vertex " << i;
            }
            EXPECT_EQ(mesh->get_index_buffer_size(), indices.size() * sizeof(uint32_t));
            EXPECT_EQ(read_mesh_indices(indices.size()), indices);
            ASSERT_TRUE(mesh->get_vertex_layout().has_value());
            EXPECT_EQ(mesh->get_vertex_layout()->vertex_count, vertices.size());
        }

        void render_mesh()
        {
            window = std::make_shared<ScriptedWindow>(320, 240);
            mesh->setup_rendering(Portal::Graphics::RenderConfig { .target_window = window });
        }

        [[nodiscard]] bool has_field_pass() const
        {
            const auto processors = mesh->get_processing_chain()->get_processors(mesh);
            return std::ranges::any_of(processors, [](const auto& processor) {
                return std::dynamic_pointer_cast<Buffers::VertexFieldProcessor>(processor) != nullptr;
            });
        }

        [[nodiscard]] bool has_skin_pass() const
        {
            const auto processors = mesh->get_processing_chain()->get_processors(mesh);
            return std::ranges::any_of(processors, [](const auto& processor) {
                return std::dynamic_pointer_cast<Buffers::SkinProcessor>(processor) != nullptr;
            });
        }

        void run_cycles(int cycles)
        {
            for (int cycle = 0; cycle < cycles; ++cycle) {
                mesh->process_default();
                mesh->get_processing_chain()->process(mesh);
            }
        }

        void expect_matches_oracle(const std::vector<MeshVertex>& rest,
            const Kakshya::MeshSkin& skin, const std::vector<glm::mat4>& palette)
        {
            const auto read_vertices = read_mesh_vertices(rest.size());
            for (size_t i = 0; i < rest.size(); ++i) {
                const auto expected = skinning::skin_oracle(rest[i], skin, i, palette);
                EXPECT_NEAR(glm::length(read_vertices[i].position - expected.position), 0.0F, 1e-4F) << "position " << i;
                EXPECT_NEAR(glm::length(read_vertices[i].normal - expected.normal), 0.0F, 1e-4F) << "normal " << i;
                EXPECT_NEAR(glm::length(glm::vec3(read_vertices[i].tangent) - glm::vec3(expected.tangent)), 0.0F, 1e-4F) << "tangent " << i;
                EXPECT_FLOAT_EQ(read_vertices[i].tangent.w, expected.tangent.w) << "handedness " << i;
                EXPECT_EQ(read_vertices[i].uv, rest[i].uv) << "uv " << i;
                EXPECT_EQ(read_vertices[i].color, rest[i].color) << "colour " << i;
            }
        }

        std::shared_ptr<MeshBuffer> mesh;
        std::shared_ptr<GpuFieldOperator> field_op;
        std::shared_ptr<Buffers::GeometryBuffer> geometry;
    };

    TEST_F(MeshBufferReplayDeviceTest, ABufferMadeEmptyDrawsNothingUntilItsFirstFrame)
    {
        make(MeshData::empty());
        attach_mesh();

        EXPECT_FALSE(mesh->has_index_buffer());
        ASSERT_TRUE(mesh->get_vertex_layout().has_value());
        EXPECT_EQ(mesh->get_vertex_layout()->vertex_count, 0U);

        const auto vertices = replay_triangles(1);
        const auto indices = replay_indices(1);
        show(replay_frame(vertices, indices));

        ASSERT_TRUE(mesh->has_index_buffer());
        expect_shown(vertices, indices);
    }

    TEST_F(MeshBufferReplayDeviceTest, AFrameReplacesTheVerticesAndTheIndices)
    {
        make(*replay_frame(replay_triangles(1), replay_indices(1)));
        attach_mesh();

        const auto vertices = replay_triangles(1, 4.0F);
        const auto indices = replay_indices(1, true);
        show(replay_frame(vertices, indices));

        expect_shown(vertices, indices);
        EXPECT_EQ(mesh->get_mesh_data().vertex_count(), 3U);
    }

    TEST_F(MeshBufferReplayDeviceTest, AFrameWithTheSameIndicesStillMovesTheVertices)
    {
        make(*replay_frame(replay_triangles(2), replay_indices(2)));
        attach_mesh();

        const auto vertices = replay_triangles(2, 3.0F);
        const auto indices = replay_indices(2);
        show(replay_frame(vertices, indices));

        expect_shown(vertices, indices);
    }

    TEST_F(MeshBufferReplayDeviceTest, ASmallerFrameDrawsOnlyItsOwnIndices)
    {
        make(*replay_frame(replay_triangles(3), replay_indices(3)));
        attach_mesh();
        ASSERT_EQ(mesh->get_index_buffer_size(), 9 * sizeof(uint32_t));

        const auto vertices = replay_triangles(1, 2.0F);
        const auto indices = replay_indices(1);
        show(replay_frame(vertices, indices));

        expect_shown(vertices, indices);
    }

    TEST_F(MeshBufferReplayDeviceTest, ASmallerMeshSetOnTheOwnerThreadDrawsOnlyItsOwnIndices)
    {
        make(*replay_frame(replay_triangles(2), replay_indices(2)));
        attach_mesh();

        const auto vertices = replay_triangles(1);
        const auto indices = replay_indices(1);
        mesh->set_mesh_data(*replay_frame(vertices, indices));
        mesh->process_default();

        expect_shown(vertices, indices);
    }

    TEST_F(MeshBufferReplayDeviceTest, AGrowingFrameResizesTheVertexAndIndexBuffers)
    {
        make(*replay_frame(replay_triangles(1), replay_indices(1)));
        attach_mesh();

        const auto vertices = replay_triangles(40, 1.0F);
        const auto indices = replay_indices(40);
        show(replay_frame(vertices, indices));

        expect_shown(vertices, indices);
    }

    TEST_F(MeshBufferReplayDeviceTest, ANullFrameHidesTheMesh)
    {
        make(*replay_frame(replay_triangles(1), replay_indices(1)));
        attach_mesh();
        show(replay_frame(replay_triangles(2), replay_indices(2)));

        show(nullptr);

        ASSERT_TRUE(mesh->get_vertex_layout().has_value());
        EXPECT_EQ(mesh->get_vertex_layout()->vertex_count, 0U);
        EXPECT_EQ(mesh->get_index_buffer_size(), 0U);
    }

    TEST_F(MeshBufferReplayDeviceTest, AFrameWithVerticesButNoIndicesHidesTheMesh)
    {
        make(*replay_frame(replay_triangles(1), replay_indices(1)));
        attach_mesh();

        auto bare = MeshData::empty();
        bare.vertex_variant = std::get<std::vector<uint8_t>>(replay_frame(replay_triangles(1), replay_indices(1))->vertex_variant);
        bare.layout.vertex_count = 3;
        show(std::make_shared<const MeshData>(std::move(bare)));

        ASSERT_TRUE(mesh->get_vertex_layout().has_value());
        EXPECT_EQ(mesh->get_vertex_layout()->vertex_count, 0U);
        EXPECT_EQ(mesh->get_index_buffer_size(), 0U);
    }

    TEST_F(MeshBufferReplayDeviceTest, AFrameWrittenFromAnotherThreadIsShownOnTheNextCycle)
    {
        make(MeshData::empty());
        attach_mesh();

        const auto vertices = replay_triangles(2, 5.0F);
        const auto indices = replay_indices(2);
        const auto frame = replay_frame(vertices, indices);
        std::thread writer([&] { mesh->write_frame(frame); });
        writer.join();
        mesh->process_default();

        expect_shown(vertices, indices);
    }

    TEST_F(MeshBufferReplayDeviceTest, OnlyTheNewestOfSeveralFramesIsShown)
    {
        make(MeshData::empty());
        attach_mesh();

        mesh->write_frame(replay_frame(replay_triangles(1), replay_indices(1)));
        const auto vertices = replay_triangles(2, 7.0F);
        const auto indices = replay_indices(2);
        mesh->write_frame(replay_frame(vertices, indices));
        mesh->process_default();

        expect_shown(vertices, indices);
    }

    TEST_F(MeshBufferReplayDeviceTest, TheMeshSinkShowsEachFrameAndIgnoresABufferThatIsGone)
    {
        make(MeshData::empty());
        attach_mesh();
        const auto sink = Kriya::mesh_sink(mesh);

        const auto vertices = replay_triangles(1, 2.0F);
        const auto indices = replay_indices(1);
        sink(replay_frame(vertices, indices), FrameContext {});
        mesh->process_default();
        expect_shown(vertices, indices);

        sink(nullptr, FrameContext {});
        mesh->process_default();
        expect_shown(vertices, indices);

        const auto orphan = Kriya::mesh_sink(std::make_shared<MeshBuffer>(MeshData::empty()));
        EXPECT_NO_THROW(orphan(replay_frame(vertices, indices), FrameContext {}));
    }

    TEST_F(MeshBufferReplayDeviceTest, AGpuFieldDisplacesTheStoredMeshEveryCycleWithoutAccumulating)
    {
        const auto vertices = replay_triangles(1);
        make(*replay_frame(vertices, replay_indices(1)));
        field_op = std::make_shared<GpuFieldOperator>(Kakshya::VertexLayout::for_meshes(sizeof(MeshVertex)));
        field_op->bind(FieldTarget::POSITION, replay_fields::replay_shift);
        mesh->set_field_operator(field_op);
        attach_mesh();
        render_mesh();
        ASSERT_TRUE(has_field_pass());
        EXPECT_TRUE(mesh->restores_vertices());

        for (int cycle = 0; cycle < 3; ++cycle) {
            mesh->process_default();
            mesh->get_processing_chain()->process(mesh);
        }

        const auto read_vertices = read_mesh_vertices(vertices.size());
        for (size_t i = 0; i < vertices.size(); ++i) {
            EXPECT_NEAR(read_vertices[i].position.x, vertices[i].position.x + 0.25F, 1e-5F) << "vertex " << i;
            EXPECT_EQ(read_vertices[i].position.y, vertices[i].position.y) << "vertex " << i;
        }
    }

    TEST_F(MeshBufferReplayDeviceTest, AFieldFollowsTheFrameThatReplacesTheMesh)
    {
        make(*replay_frame(replay_triangles(1), replay_indices(1)));
        field_op = std::make_shared<GpuFieldOperator>(Kakshya::VertexLayout::for_meshes(sizeof(MeshVertex)));
        field_op->bind(FieldTarget::POSITION, replay_fields::replay_shift);
        mesh->set_field_operator(field_op);
        attach_mesh();
        render_mesh();

        const auto vertices = replay_triangles(1, 4.0F);
        mesh->write_frame(replay_frame(vertices, replay_indices(1)));
        for (int cycle = 0; cycle < 2; ++cycle) {
            mesh->process_default();
            mesh->get_processing_chain()->process(mesh);
        }

        const auto read_vertices = read_mesh_vertices(vertices.size());
        for (size_t i = 0; i < vertices.size(); ++i) {
            EXPECT_NEAR(read_vertices[i].position.x, vertices[i].position.x + 0.25F, 1e-5F) << "vertex " << i;
        }
    }

    TEST_F(MeshBufferReplayDeviceTest, AFieldOperatorWithTheWrongLayoutSizeWiresNoPass)
    {
        const auto vertices = replay_triangles(1);
        make(*replay_frame(vertices, replay_indices(1)));
        field_op = std::make_shared<GpuFieldOperator>(
            Kakshya::VertexLayout::for_meshes(sizeof(MeshVertex) + 2 * sizeof(float)));
        field_op->bind(FieldTarget::POSITION, replay_fields::replay_shift);
        mesh->set_field_operator(field_op);
        attach_mesh();
        render_mesh();

        EXPECT_FALSE(has_field_pass());
        EXPECT_FALSE(mesh->restores_vertices());
        mesh->process_default();
        mesh->get_processing_chain()->process(mesh);

        const auto read_vertices = read_mesh_vertices(vertices.size());
        for (size_t i = 0; i < vertices.size(); ++i) {
            EXPECT_EQ(read_vertices[i].position, vertices[i].position) << "vertex " << i;
        }
    }

    TEST_F(MeshBufferReplayDeviceTest, ASkinPassSkinsPositionNormalAndTangentFromAPalette)
    {
        const auto vertices = replay_triangles(1);
        make(*replay_frame(vertices, replay_indices(1)));
        const auto skin = skinning::follow_first_joint(vertices.size());
        mesh->set_skin(skin);
        attach_mesh();
        render_mesh();
        ASSERT_TRUE(has_skin_pass());

        const std::vector<glm::mat4> palette {
            glm::translate(glm::mat4(1.0F), glm::vec3(1.0F, 0.0F, 2.0F))
            * glm::rotate(glm::mat4(1.0F), glm::radians(90.0F), glm::vec3(1.0F, 0.0F, 0.0F))
        };
        ASSERT_TRUE(mesh->set_palette(palette));
        run_cycles(2);

        expect_matches_oracle(vertices, *skin, palette);
    }

    TEST_F(MeshBufferReplayDeviceTest, AVertexBlendsItsJointsAndKeepsTheRestOfItsWeightInPlace)
    {
        const auto vertices = replay_triangles(1);
        make(*replay_frame(vertices, replay_indices(1)));
        Kakshya::MeshSkin blend;
        blend.joints = { 0, 1 };
        blend.inverse_bind = { glm::mat4(1.0F), glm::mat4(1.0F) };
        blend.influence_joints = { glm::uvec4(0, 1, 0, 0), glm::uvec4(0), glm::uvec4(0) };
        blend.influence_weights = { glm::vec4(0.5F, 0.5F, 0.0F, 0.0F), glm::vec4(0.25F, 0.0F, 0.0F, 0.0F), glm::vec4(0.0F) };
        const auto skin = std::make_shared<const Kakshya::MeshSkin>(std::move(blend));
        mesh->set_skin(skin);
        attach_mesh();
        render_mesh();

        const std::vector<glm::mat4> palette {
            glm::translate(glm::mat4(1.0F), glm::vec3(2.0F, 0.0F, 0.0F)),
            glm::translate(glm::mat4(1.0F), glm::vec3(-2.0F, 0.0F, 0.0F))
        };
        ASSERT_TRUE(mesh->set_palette(palette));
        run_cycles(1);

        expect_matches_oracle(vertices, *skin, palette);
        const auto read_vertices = read_mesh_vertices(vertices.size());
        EXPECT_NEAR(read_vertices[0].position.x, vertices[0].position.x, 1e-5F);
        EXPECT_NEAR(read_vertices[1].position.x, vertices[1].position.x + 0.5F, 1e-5F);
        EXPECT_NEAR(read_vertices[2].position.x, vertices[2].position.x, 1e-5F);
    }

    TEST_F(MeshBufferReplayDeviceTest, ANormalFollowsTheCofactorSoANonUniformScaleKeepsItPerpendicular)
    {
        std::vector<MeshVertex> vertices = replay_triangles(1);
        for (auto& vertex : vertices) {
            vertex.normal = glm::normalize(glm::vec3(1.0F, 1.0F, 0.0F));
        }
        make(*replay_frame(vertices, replay_indices(1)));
        const auto skin = skinning::follow_first_joint(vertices.size());
        mesh->set_skin(skin);
        attach_mesh();
        render_mesh();

        const std::vector<glm::mat4> palette { glm::scale(glm::mat4(1.0F), glm::vec3(2.0F, 1.0F, 1.0F)) };
        ASSERT_TRUE(mesh->set_palette(palette));
        run_cycles(1);

        const auto read_vertices = read_mesh_vertices(vertices.size());
        const glm::vec3 expected = glm::normalize(glm::vec3(1.0F, 2.0F, 0.0F));
        EXPECT_NEAR(glm::length(read_vertices[0].normal - expected), 0.0F, 1e-4F);
        expect_matches_oracle(vertices, *skin, palette);
    }

    TEST_F(MeshBufferReplayDeviceTest, ASkinnedBufferStaysInItsRestPoseUntilAPaletteArrives)
    {
        const auto vertices = replay_triangles(1);
        make(*replay_frame(vertices, replay_indices(1)));
        mesh->set_skin(skinning::follow_first_joint(vertices.size()));
        attach_mesh();
        render_mesh();

        run_cycles(2);

        const auto read_vertices = read_mesh_vertices(vertices.size());
        for (size_t i = 0; i < vertices.size(); ++i) {
            EXPECT_EQ(read_vertices[i].position, vertices[i].position) << "vertex " << i;
        }
    }

    TEST_F(MeshBufferReplayDeviceTest, APaletteOfTheWrongSizeOrBeforeRenderingIsRefused)
    {
        const auto vertices = replay_triangles(1);
        make(*replay_frame(vertices, replay_indices(1)));
        mesh->set_skin(skinning::follow_first_joint(vertices.size()));
        attach_mesh();

        EXPECT_FALSE(mesh->set_palette({ glm::mat4(1.0F) }));

        render_mesh();

        EXPECT_FALSE(mesh->set_palette({ glm::mat4(1.0F), glm::mat4(1.0F) }));
        EXPECT_TRUE(mesh->set_palette({ glm::mat4(1.0F) }));
    }

    TEST_F(MeshBufferReplayDeviceTest, ASkinThatDoesNotCoverTheVerticesWiresNoPass)
    {
        const auto vertices = replay_triangles(1);
        make(*replay_frame(vertices, replay_indices(1)));
        mesh->set_skin(skinning::follow_first_joint(vertices.size() + 1));
        attach_mesh();
        render_mesh();

        EXPECT_FALSE(has_skin_pass());
        EXPECT_FALSE(mesh->set_palette({ glm::mat4(1.0F) }));
    }

    TEST_F(MeshBufferReplayDeviceTest, AFieldDisplacesTheSkinnedSurfaceEveryCycleWithoutAccumulating)
    {
        const auto vertices = replay_triangles(1);
        make(*replay_frame(vertices, replay_indices(1)));
        const auto skin = skinning::follow_first_joint(vertices.size());
        mesh->set_skin(skin);
        field_op = std::make_shared<GpuFieldOperator>(Kakshya::VertexLayout::for_meshes(sizeof(MeshVertex)));
        field_op->bind(FieldTarget::POSITION, replay_fields::replay_shift);
        mesh->set_field_operator(field_op);
        attach_mesh();
        render_mesh();
        ASSERT_TRUE(has_skin_pass());
        ASSERT_TRUE(has_field_pass());

        ASSERT_TRUE(mesh->set_palette({ glm::translate(glm::mat4(1.0F), glm::vec3(0.0F, 0.0F, 2.0F)) }));
        run_cycles(3);

        const auto read_vertices = read_mesh_vertices(vertices.size());
        for (size_t i = 0; i < vertices.size(); ++i) {
            EXPECT_NEAR(read_vertices[i].position.x, vertices[i].position.x + 0.25F, 1e-5F) << "vertex " << i;
            EXPECT_NEAR(read_vertices[i].position.z, 2.0F, 1e-5F) << "vertex " << i;
        }
    }

    TEST_F(MeshBufferReplayDeviceTest, AGeometryBufferWhoseNodeShrinksDrawsOnlyItsOwnIndices)
    {
        auto node = std::make_shared<Nodes::GpuSync::MeshWriterNode>(6);
        node->set_mesh(replay_triangles(2), replay_indices(2));
        node->compute_frame();
        geometry = std::make_shared<Buffers::GeometryBuffer>(node);
        service->initialize_buffer(geometry);
        ASSERT_TRUE(geometry->is_initialized());
        geometry->setup_processors(Buffers::ProcessingToken::GRAPHICS_BACKEND);
        geometry->process_default();
        ASSERT_EQ(geometry->get_index_buffer_size(), 6 * sizeof(uint32_t));

        node->set_mesh(replay_triangles(1), replay_indices(1));
        node->compute_frame();
        geometry->process_default();

        EXPECT_EQ(geometry->get_index_buffer_size(), 3 * sizeof(uint32_t));
    }

    TEST_F(MeshBufferReplayDeviceTest, ABufferFedByANodeKeepsTheNodesPolygonFacesAndSubmeshes)
    {
        const std::vector<MeshVertex> corners {
            replay_vertex({ 0.0F, 0.0F, 0.0F }), replay_vertex({ 1.0F, 0.0F, 0.0F }),
            replay_vertex({ 1.0F, 1.0F, 0.0F }), replay_vertex({ 0.0F, 1.0F, 0.0F })
        };
        auto data = MeshData::from_vertices(corners, Kakshya::FaceList { .sizes = { 4 }, .corners = { 0, 1, 2, 3 } }).value();
        Kakshya::MeshSubrange part;
        part.index_start = 0;
        part.index_count = 6;
        part.name = "front";
        Kakshya::RegionGroup group("submeshes");
        group.add_region(part.to_region());
        data.submeshes = std::move(group);

        auto node = std::make_shared<Nodes::GpuSync::MeshWriterNode>(4);
        node->set_mesh(data);
        node->compute_frame();
        mesh = std::make_shared<MeshBuffer>(node);
        attach_mesh();

        mesh->process_default();

        EXPECT_EQ(mesh->get_mesh_data().polygon_count(), 1U);
        ASSERT_TRUE(mesh->get_submeshes().has_value());
        EXPECT_EQ(mesh->get_submeshes()->regions.size(), 1U);
    }

    TEST_F(MeshBufferReplayDeviceTest, ABufferWithoutAFieldOperatorWiresNoPass)
    {
        make(*replay_frame(replay_triangles(1), replay_indices(1)));
        attach_mesh();
        render_mesh();

        EXPECT_FALSE(has_field_pass());
        EXPECT_FALSE(mesh->restores_vertices());
    }

} // namespace
} // namespace MayaFlux::Test
