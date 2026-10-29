#include "network_buffer_fixture.hpp"

#include "MayaFlux/Kakshya/NDData/VertexFormats.hpp"
#include "MayaFlux/Nodes/Network/PointCloudNetwork.hpp"

namespace MayaFlux::Test {
namespace {

    using Buffers::NetworkGeometryBuffer;
    using Nodes::Network::ParticleNetwork;
    using Nodes::Network::PhysicsOperator;
    using Nodes::Network::PointCloudNetwork;
    using Portal::Graphics::PolygonMode;
    using Portal::Graphics::PrimitiveTopology;
    using Portal::Graphics::RenderConfig;

    std::shared_ptr<ParticleNetwork> physics_particles(size_t count)
    {
        auto particles = std::make_shared<ParticleNetwork>(
            count, glm::vec3(-1.0F), glm::vec3(1.0F), Kinesis::SpatialDistribution::RANDOM_VOLUME);
        particles->create_operator<PhysicsOperator>();
        return particles;
    }

    constexpr size_t k_point_bytes = sizeof(Kakshya::PointVertex);

    // Sizing and construction (no device needed)

    TEST(NetworkGeometryBufferSizingTest, NullNetworkIsRejected)
    {
        EXPECT_ANY_THROW(NetworkGeometryBuffer(nullptr));
    }

    TEST(NetworkGeometryBufferSizingTest, OperatorLayoutSizesTheBufferTimesTheOverAllocateFactor)
    {
        const NetworkGeometryBuffer exact(physics_particles(100), "exact", 1.0F);
        const NetworkGeometryBuffer doubled(physics_particles(100), "doubled", 2.0F);

        EXPECT_EQ(exact.get_size_bytes(), 100 * k_point_bytes);
        EXPECT_EQ(doubled.get_size_bytes(), 200 * k_point_bytes);
    }

    TEST(NetworkGeometryBufferSizingTest, VertexCountComesFromThePrimaryOperator)
    {
        const NetworkGeometryBuffer buffer(physics_particles(64));
        EXPECT_EQ(buffer.get_vertex_count(), 64U);
    }

    TEST(NetworkGeometryBufferSizingTest, EmptyNetworkGetsTheMinimumSize)
    {
        const NetworkGeometryBuffer buffer(std::make_shared<PointCloudNetwork>());
        EXPECT_EQ(buffer.get_size_bytes(), 4096U);
        EXPECT_EQ(buffer.get_vertex_count(), 0U);
    }

    TEST(NetworkGeometryBufferSizingTest, BindingNameAndNetworkAreKept)
    {
        auto particles = physics_particles(8);
        const NetworkGeometryBuffer buffer(particles, "swarm");

        EXPECT_EQ(buffer.get_binding_name(), "swarm");
        EXPECT_EQ(buffer.get_network(), particles);
    }

    // Processor wiring

    TEST_F(NetworkBufferDeviceTest, SetupProcessorsBindsTheNetworkUnderItsName)
    {
        attach(physics_particles(32));

        const auto processor = buffer->get_processor();
        ASSERT_NE(processor, nullptr);
        EXPECT_EQ(processor->get_binding_count(), 1U);
        EXPECT_TRUE(processor->has_binding(buffer->get_binding_name()));
        EXPECT_EQ(buffer->get_default_processor(), processor);
    }

    TEST_F(NetworkBufferDeviceTest, UpdateNetworkRespectsTheNetworkEnabledFlag)
    {
        auto particles = physics_particles(16);
        attach(particles);

        EXPECT_NO_THROW(buffer->update_network(4));
        particles->set_enabled(false);
        EXPECT_NO_THROW(buffer->update_network(4));
    }

    // Auxiliary state fields

    TEST_F(NetworkBufferDeviceTest, DeclareStateRejectsEmptyNamesZeroSizesAndDuplicates)
    {
        attach(physics_particles(16));

        EXPECT_FALSE(buffer->declare_state("", 4, 4));
        EXPECT_FALSE(buffer->declare_state("no_elements", 0, 4));
        EXPECT_FALSE(buffer->declare_state("no_stride", 4, 0));
        EXPECT_TRUE(buffer->declare_state("field", 4, 4));
        EXPECT_FALSE(buffer->declare_state("field", 8, 4));
        EXPECT_EQ(buffer->get_state_bytes("field"), 16U);
    }

    TEST_F(NetworkBufferDeviceTest, UndeclaredStateResolvesToNothing)
    {
        attach(physics_particles(16));

        EXPECT_FALSE(buffer->has_state("missing"));
        EXPECT_EQ(buffer->get_state_bytes("missing"), 0U);
        EXPECT_FALSE(buffer->read_state_handle("missing"));
        EXPECT_FALSE(buffer->write_state_handle("missing"));
        EXPECT_FALSE(buffer->read_state_slot("missing").buffer);
        EXPECT_FALSE(buffer->write_state_slot("missing").buffer);
        EXPECT_NO_THROW(buffer->swap_state("missing"));
    }

    TEST_F(NetworkBufferDeviceTest, SingleSlotStateReadsAndWritesTheSameHandleAndIgnoresSwap)
    {
        attach(physics_particles(16));
        ASSERT_TRUE(buffer->declare_state("single", 4, 4, false));

        const auto handle = buffer->read_state_handle("single");
        EXPECT_TRUE(handle);
        EXPECT_EQ(handle, buffer->write_state_handle("single"));

        buffer->swap_state("single");
        EXPECT_EQ(handle, buffer->read_state_handle("single"));
        EXPECT_EQ(handle, buffer->write_state_handle("single"));
    }

    TEST_F(NetworkBufferDeviceTest, DoubleBufferedStateExchangesReadAndWriteOnSwap)
    {
        attach(physics_particles(16));
        ASSERT_TRUE(buffer->declare_state("ping", 4, 4, true));

        const auto read_before = buffer->read_state_handle("ping");
        const auto write_before = buffer->write_state_handle("ping");
        EXPECT_NE(read_before, write_before);

        buffer->swap_state("ping");
        EXPECT_EQ(buffer->read_state_handle("ping"), write_before);
        EXPECT_EQ(buffer->write_state_handle("ping"), read_before);

        buffer->swap_state("ping");
        EXPECT_EQ(buffer->read_state_handle("ping"), read_before);
    }

    TEST_F(NetworkBufferDeviceTest, StateFieldsKeepIndependentStorage)
    {
        attach(physics_particles(16));
        ASSERT_TRUE(buffer->declare_state("first", 4, sizeof(uint32_t), false));
        ASSERT_TRUE(buffer->declare_state("second", 4, sizeof(uint32_t), false));

        write_state<uint32_t>("first", { 1, 2, 3, 4 });
        write_state<uint32_t>("second", { 9, 8, 7, 6 });

        EXPECT_EQ(read_state<uint32_t>("first"), (std::vector<uint32_t> { 1, 2, 3, 4 }));
        EXPECT_EQ(read_state<uint32_t>("second"), (std::vector<uint32_t> { 9, 8, 7, 6 }));
    }

    // Cluster ids

    TEST_F(NetworkBufferDeviceTest, EnsureClusterIdsDeclaresOneZeroedEntryPerVertex)
    {
        attach(physics_particles(48));

        ASSERT_TRUE(buffer->ensure_cluster_ids());

        EXPECT_TRUE(buffer->has_state("hash_cluster_id"));
        EXPECT_EQ(buffer->get_state_bytes("hash_cluster_id"), 48 * sizeof(uint32_t));
        EXPECT_EQ(read_state<uint32_t>("hash_cluster_id"), std::vector<uint32_t>(48, 0U));
    }

    TEST_F(NetworkBufferDeviceTest, EnsureClusterIdsIsIdempotent)
    {
        attach(physics_particles(48));
        ASSERT_TRUE(buffer->ensure_cluster_ids());
        const auto handle = buffer->read_state_handle("hash_cluster_id");

        EXPECT_TRUE(buffer->ensure_cluster_ids());
        EXPECT_EQ(buffer->read_state_handle("hash_cluster_id"), handle);
    }

    TEST_F(NetworkBufferDeviceTest, EnsureClusterIdsFailsWithoutAGraphicsOperator)
    {
        attach(std::make_shared<PointCloudNetwork>());

        EXPECT_FALSE(buffer->ensure_cluster_ids());
        EXPECT_FALSE(buffer->has_state("hash_cluster_id"));
    }

    // Render configuration

    TEST_F(NetworkBufferDeviceTest, PointTopologyGetsThePointShaders)
    {
        attach(physics_particles(16));
        render();

        const auto config = buffer->get_render_config();
        EXPECT_EQ(config.vertex_shader, "point.vert.spv");
        EXPECT_EQ(config.fragment_shader, "point.frag.spv");
        EXPECT_TRUE(config.geometry_shader.empty());
        ASSERT_NE(buffer->get_render_processor(), nullptr);
    }

    TEST_F(NetworkBufferDeviceTest, LineTopologyGetsTheLineShadersAndGeometryStage)
    {
        attach(physics_particles(16));
        render(RenderConfig { .topology = PrimitiveTopology::LINE_LIST });

        const bool has_geometry_stage = Portal::Graphics::get_shader_foundry()
                                            .get_physical_device()
                                            .getFeatures()
                                            .geometryShader;

        const auto config = buffer->get_render_config();
        EXPECT_EQ(config.vertex_shader, "line.vert.spv");
        EXPECT_EQ(config.fragment_shader, "line.frag.spv");
        EXPECT_EQ(config.geometry_shader, has_geometry_stage ? "line.geom.spv" : "");
        EXPECT_EQ(buffer->get_render_processor()->is_triangulate(), !has_geometry_stage);
    }

    TEST_F(NetworkBufferDeviceTest, TriangulatedLinesDropTheGeometryStage)
    {
        attach(physics_particles(16));
        render(RenderConfig { .topology = PrimitiveTopology::LINE_LIST, .triangulate = true });

        const auto config = buffer->get_render_config();
        EXPECT_EQ(config.vertex_shader, "line.vert.spv");
        EXPECT_TRUE(config.geometry_shader.empty());
        EXPECT_TRUE(buffer->get_render_processor()->is_triangulate());
    }

    TEST_F(NetworkBufferDeviceTest, TriangleTopologyGetsTheTriangleShaders)
    {
        attach(physics_particles(16));
        render(RenderConfig { .topology = PrimitiveTopology::TRIANGLE_LIST });

        const auto config = buffer->get_render_config();
        EXPECT_EQ(config.vertex_shader, "triangle.vert.spv");
        EXPECT_EQ(config.fragment_shader, "triangle.frag.spv");
    }

    TEST_F(NetworkBufferDeviceTest, MilledPointModeStopsUsingTheVertexExtent)
    {
        attach(physics_particles(16));
        render(RenderConfig { .polygon_mode = PolygonMode::POINT, .triangulate = true });

        const auto processor = buffer->get_render_processor();
        ASSERT_NE(processor, nullptr);
        EXPECT_TRUE(processor->is_triangulate());
        EXPECT_FALSE(processor->get_mill_spec().use_vertex_extent);
    }

    TEST_F(NetworkBufferDeviceTest, SetupRenderingAddsTheRenderProcessorAfterTheNetworkProcessor)
    {
        attach(physics_particles(16));
        render();

        const auto& processors = buffer->get_processing_chain()->get_processors(buffer);
        const auto render_at = std::ranges::find(processors, buffer->get_render_processor());
        ASSERT_NE(render_at, processors.end());
        EXPECT_EQ(render_at, processors.end() - 1);
    }

    TEST_F(NetworkBufferDeviceTest, RenderRunsAreHandedToThePrimaryProcessor)
    {
        attach(physics_particles(16));
        render(RenderConfig { .polygon_mode = PolygonMode::POINT, .triangulate = true });

        buffer->update_render_runs({
            { .topology = PrimitiveTopology::LINE_LIST, .vertex_offset = 0, .vertex_count = 4 },
            { .topology = PrimitiveTopology::TRIANGLE_LIST, .vertex_offset = 4, .vertex_count = 3 },
        });

        const auto& runs = buffer->get_render_processor()->get_runs();
        ASSERT_EQ(runs.size(), 2U);
        EXPECT_EQ(runs[0].topology, PrimitiveTopology::POINT_LIST);
        EXPECT_EQ(runs[1].topology, PrimitiveTopology::TRIANGLE_LIST);
        EXPECT_EQ(runs[1].vertex_offset, 4U);
        EXPECT_EQ(runs[1].vertex_count, 3U);
    }

    TEST_F(NetworkBufferDeviceTest, UpdateRenderRunsBeforeRenderingIsInert)
    {
        attach(physics_particles(16));
        EXPECT_NO_THROW(buffer->update_render_runs({ { .vertex_count = 4 } }));
        EXPECT_EQ(buffer->get_render_processor(), nullptr);
    }

    // Chain render processors

    TEST_F(NetworkBufferDeviceTest, ChainRenderProcessorsAreAddressedByIndexAndBoundsChecked)
    {
        attach(physics_particles(16));
        render();

        buffer->add_chain_operator_rendering(RenderConfig { .target_window = window });

        EXPECT_NE(buffer->get_chain_render_processor(0), nullptr);
        EXPECT_NE(buffer->get_chain_render_processor(0), buffer->get_render_processor());
        EXPECT_EQ(buffer->get_chain_render_processor(1), nullptr);
    }

    TEST_F(NetworkBufferDeviceTest, ChainRangeIndexZeroTargetsThePrimaryProcessorAndTheRestTargetChainEntries)
    {
        attach(physics_particles(16));
        render(RenderConfig { .polygon_mode = PolygonMode::POINT, .triangulate = true });
        buffer->add_chain_operator_rendering(RenderConfig { .target_window = window, .triangulate = true });

        buffer->update_chain_render_range(
            0, 0, 8, std::nullopt,
            { { .topology = PrimitiveTopology::TRIANGLE_LIST, .vertex_offset = 0, .vertex_count = 8 } });
        buffer->update_chain_render_range(
            1, 8, 4, std::nullopt,
            { { .topology = PrimitiveTopology::LINE_LIST, .vertex_offset = 8, .vertex_count = 4 } });

        const auto& primary_runs = buffer->get_render_processor()->get_runs();
        ASSERT_EQ(primary_runs.size(), 1U);
        EXPECT_EQ(primary_runs[0].vertex_count, 8U);

        const auto& chain_runs = buffer->get_chain_render_processor(0)->get_runs();
        ASSERT_EQ(chain_runs.size(), 1U);
        EXPECT_EQ(chain_runs[0].vertex_offset, 8U);
        EXPECT_EQ(chain_runs[0].vertex_count, 4U);
    }

    TEST_F(NetworkBufferDeviceTest, ChainRangeOutOfBoundsAndBeforeRenderingAreInert)
    {
        attach(physics_particles(16));

        EXPECT_NO_THROW(buffer->update_chain_render_range(0, 0, 4, std::nullopt));
        EXPECT_NO_THROW(buffer->update_chain_render_range(3, 0, 4, std::nullopt));
    }

} // namespace
} // namespace MayaFlux::Test
