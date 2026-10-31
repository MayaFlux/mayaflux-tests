#include "gtest/gtest.h"


#include "../../support/native_window_test_support.hpp"
#include "../../support/scripted_window.hpp"
#include "../../support/vulkan_test_support.hpp"

#include "MayaFlux/Buffers/BufferProcessingChain.hpp"
#include "MayaFlux/Buffers/Network/InstanceNetworkBuffer.hpp"
#include "MayaFlux/Buffers/Root/RootGraphicsBuffer.hpp"
#include "MayaFlux/Buffers/Shaders/RenderProcessor.hpp"
#include "MayaFlux/Nodes/Graphics/GeometryLeafNode.hpp"
#include "MayaFlux/Nodes/Graphics/MeshWriterNode.hpp"
#include "MayaFlux/Nodes/Network/InstanceNetwork.hpp"
#include "MayaFlux/Portal/Graphics/ComputePress.hpp"
#include "MayaFlux/Portal/Graphics/RenderFlow.hpp"
#include "MayaFlux/Portal/Graphics/ShaderFoundry.hpp"
#include "MayaFlux/Registry/BackendRegistry.hpp"
#include "MayaFlux/Registry/Service/BufferService.hpp"
#include "MayaFlux/Registry/Service/DisplayService.hpp"

namespace MayaFlux::Test {
namespace {

using Buffers::InstanceNetworkBuffer;
using Buffers::ProcessingToken;
using Nodes::GpuSync::GeometryLeafNode;
using Nodes::GpuSync::MeshWriterNode;
using Nodes::Network::InstanceNetwork;

std::shared_ptr<GeometryLeafNode> point_template()
{
    Nodes::Vertex point {};
    point.position = { 0.0F, 0.0F, 0.0F };
    point.color = { 1.0F, 0.0F, 0.0F, 1.0F };
    auto node = std::make_shared<GeometryLeafNode>(std::vector<Nodes::Vertex> { point });
    node->set_primitive_topology(Portal::Graphics::PrimitiveTopology::POINT_LIST);
    node->compute_frame();
    return node;
}

std::shared_ptr<MeshWriterNode> indexed_quad_template()
{
    std::vector<Nodes::MeshVertex> vertices(4);
    vertices[0].position = { -0.2F, -0.2F, 0.0F };
    vertices[1].position = { 0.2F, -0.2F, 0.0F };
    vertices[2].position = { 0.2F, 0.2F, 0.0F };
    vertices[3].position = { -0.2F, 0.2F, 0.0F };
    const std::vector<uint32_t> indices { 0, 1, 2, 0, 2, 3 };
    auto node = std::make_shared<MeshWriterNode>(vertices.size());
    node->set_mesh(vertices, indices);
    node->compute_frame();
    return node;
}

void* vulkan_handle(vk::Buffer buffer)
{
    return static_cast<void*>(static_cast<VkBuffer>(buffer));
}

class InstanceNetworkBufferDeviceTest : public VulkanDeviceTest {
protected:
    static void SetUpTestSuite()
    {
        VulkanDeviceTest::SetUpTestSuite();
        if (!vulkan_unavailable_reason().empty())
            return;
        auto borrowed = std::shared_ptr<Core::VulkanBackend>(&backend(), [](Core::VulkanBackend*) { });
        auto& foundry = Portal::Graphics::get_shader_foundry();
        foundry.shutdown();
        s_foundry_ready = foundry.initialize(borrowed);
    }

    static void TearDownTestSuite()
    {
        if (s_foundry_ready)
            Portal::Graphics::get_shader_foundry().shutdown();
        s_foundry_ready = false;
        VulkanDeviceTest::TearDownTestSuite();
    }

    void SetUp() override
    {
        VulkanDeviceTest::SetUp();
        if (IsSkipped())
            return;
        ASSERT_TRUE(s_foundry_ready);
        service = Registry::BackendRegistry::instance()
                      .get_service<Registry::Service::BufferService>();
        ASSERT_NE(service, nullptr);
    }

    void TearDown() override
    {
        if (buffer) {
            if (auto render = buffer->get_render_processor()) {
                if (auto chain = buffer->get_processing_chain())
                    chain->remove_processor_anywhere(render, buffer);
            }
            buffer->set_default_processor(nullptr);
            if (buffer->is_initialized() && service)
                service->destroy_buffer(buffer);
        }
        buffer.reset();
        network.reset();
        window.reset();
    }

    void attach(std::shared_ptr<InstanceNetwork> value)
    {
        network = std::move(value);
        buffer = std::make_shared<InstanceNetworkBuffer>(network);
        service->initialize_buffer(buffer);
        ASSERT_TRUE(buffer->is_initialized());
        buffer->setup_processors(ProcessingToken::GRAPHICS_BACKEND);
        ASSERT_NE(buffer->get_ssbo_processor(), nullptr);
        window = std::make_shared<ScriptedWindow>(320, 240);
        buffer->setup_rendering(InstanceNetworkBuffer::RenderConfig {
            .target_window = window, .triangulate = true });
        buffer->process_default();
    }

    std::vector<uint8_t> read_raw_buffer(vk::Buffer source, size_t byte_count)
    {
        RawVulkanBuffer destination(
            backend(), byte_count, vk::BufferUsageFlagBits::eTransferDst, true);
        service->copy_buffer(
            vulkan_handle(source), vulkan_handle(destination.buffer()), byte_count, 0, 0);
        const auto* data = static_cast<const uint8_t*>(destination.mapped());
        return { data, data + byte_count };
    }

    static inline bool s_foundry_ready {};
    Registry::Service::BufferService* service {};
    std::shared_ptr<InstanceNetwork> network;
    std::shared_ptr<InstanceNetworkBuffer> buffer;
    std::shared_ptr<ScriptedWindow> window;
};

TEST_F(InstanceNetworkBufferDeviceTest, ProcessorUploadsEachTransformAndSetsTheInstanceCount)
{
    auto net = std::make_shared<InstanceNetwork>();
    auto shape = point_template();
    net->add_slot("left", shape);
    net->add_slot("right", shape);
    net->get_slot(0).transform[3] = glm::vec4(-0.5F, 0.0F, 0.0F, 1.0F);
    net->get_slot(1).transform[3] = glm::vec4(0.5F, 0.0F, 0.0F, 1.0F);
    attach(net);

    const auto render = buffer->get_render_processor();
    ASSERT_NE(render, nullptr);
    EXPECT_EQ(render->get_instance_count(), 2U);
    EXPECT_EQ(render->get_config().shader_path, "instance_point.vert.spv");

    const auto transform_ssbo = buffer->get_ssbo_processor()->get_transform_ssbo();
    ASSERT_NE(transform_ssbo, nullptr);
    const auto actual = read_raw_buffer(transform_ssbo->get_buffer(), 2 * sizeof(glm::mat4));
    const std::array<glm::mat4, 2> expected {
        net->get_slot(0).transform, net->get_slot(1).transform
    };
    EXPECT_EQ(std::memcmp(actual.data(), expected.data(), actual.size()), 0);

    net->get_slot(1).transform[3] = glm::vec4(0.25F, 0.5F, 0.0F, 1.0F);
    net->get_slot(1).dirty = true;
    buffer->process_default();

    const auto updated = read_raw_buffer(transform_ssbo->get_buffer(), 2 * sizeof(glm::mat4));
    const std::array<glm::mat4, 2> expected_updated {
        net->get_slot(0).transform, net->get_slot(1).transform
    };
    EXPECT_EQ(std::memcmp(updated.data(), expected_updated.data(), updated.size()), 0);
    EXPECT_EQ(render->get_instance_count(), 2U);
}

TEST_F(InstanceNetworkBufferDeviceTest, IndexedTemplateKeepsItsIndexStreamForInstancedMilling)
{
    auto net = std::make_shared<InstanceNetwork>();
    auto shape = indexed_quad_template();
    net->add_slot("left", shape);
    net->add_slot("right", shape);
    attach(net);

    const auto render = buffer->get_render_processor();
    ASSERT_NE(render, nullptr);
    EXPECT_TRUE(render->is_triangulate());
    EXPECT_EQ(render->get_config().shader_path, "instance.vert.spv");
    ASSERT_TRUE(buffer->has_index_buffer());
    EXPECT_EQ(buffer->get_index_buffer_size(), 6U * sizeof(uint32_t));

    const auto actual = read_raw_buffer(buffer->get_index_buffer(), 6U * sizeof(uint32_t));
    const std::array<uint32_t, 6> expected { 0, 1, 2, 0, 2, 3 };
    EXPECT_EQ(std::memcmp(actual.data(), expected.data(), actual.size()), 0);
}

TEST_F(InstanceNetworkBufferDeviceTest, TriangulatedRenderingEnablesBillboardInstances)
{
    auto net = std::make_shared<InstanceNetwork>();
    net->add_slot("only", point_template());
    attach(net);

    const auto render = buffer->get_render_processor();
    ASSERT_NE(render, nullptr);
    EXPECT_TRUE(render->is_triangulate());
    EXPECT_TRUE(render->get_mill_spec().billboard_instances);
}

TEST_F(InstanceNetworkBufferDeviceTest, NativeRenderingLeavesBillboardInstancesOff)
{
    auto net = std::make_shared<InstanceNetwork>();
    net->add_slot("only", point_template());
    network = net;
    buffer = std::make_shared<InstanceNetworkBuffer>(network);
    service->initialize_buffer(buffer);
    ASSERT_TRUE(buffer->is_initialized());
    buffer->setup_processors(ProcessingToken::GRAPHICS_BACKEND);
    window = std::make_shared<ScriptedWindow>(320, 240);
    buffer->setup_rendering(InstanceNetworkBuffer::RenderConfig { .target_window = window });

    const auto render = buffer->get_render_processor();
    ASSERT_NE(render, nullptr);
    if (render->is_triangulate())
        GTEST_SKIP() << "device cannot draw this topology natively";
    EXPECT_FALSE(render->get_mill_spec().billboard_instances);
}

TEST_F(InstanceNetworkBufferDeviceTest, NonIndexedTemplateUploadsNoIndexStream)
{
    auto net = std::make_shared<InstanceNetwork>();
    net->add_slot("only", point_template());
    attach(net);

    EXPECT_FALSE(buffer->has_index_buffer());
    EXPECT_EQ(buffer->get_index_buffer_size(), 0U);
}

#if defined(MAYAFLUX_TEST_NATIVE_WINDOWS)

class InstanceNetworkBufferPresentationTest : public PresentingVulkanTest {
protected:
    void SetUp() override
    {
        PresentingVulkanTest::SetUp();
        if (IsSkipped())
            return;

        Portal::Graphics::get_render_flow().shutdown();
        Portal::Graphics::get_compute_press().shutdown();
        Portal::Graphics::get_shader_foundry().shutdown();
        auto borrowed = std::shared_ptr<Core::VulkanBackend>(&backend(), [](Core::VulkanBackend*) { });
        ASSERT_TRUE(Portal::Graphics::get_shader_foundry().initialize(borrowed));
        foundry_ready = true;
        ASSERT_TRUE(Portal::Graphics::get_compute_press().initialize());
        compute_ready = true;
        ASSERT_TRUE(Portal::Graphics::get_render_flow().initialize());
        flow_ready = true;

        window = make_window();
        ASSERT_NE(window, nullptr);
        ASSERT_TRUE(backend().register_window(window));
        service = Registry::BackendRegistry::instance()
                      .get_service<Registry::Service::BufferService>();
        ASSERT_NE(service, nullptr);

        network = std::make_shared<InstanceNetwork>();
        auto shape = point_template();
        network->add_slot("front", shape);
        network->add_slot("rotated", shape);
        network->get_slot(0).transform[3] = glm::vec4(-0.5F, 0.0F, 0.0F, 1.0F);
        network->get_slot(1).transform = glm::mat4(1.0F);
        network->get_slot(1).transform[1] = glm::vec4(0.0F, 0.0F, 1.0F, 0.0F);
        network->get_slot(1).transform[2] = glm::vec4(0.0F, -1.0F, 0.0F, 0.0F);
        network->get_slot(1).transform[3] = glm::vec4(0.5F, 0.0F, 0.0F, 1.0F);

        buffer = std::make_shared<InstanceNetworkBuffer>(network);
        service->initialize_buffer(buffer);
        ASSERT_TRUE(buffer->is_initialized());
        buffer->setup_processors(ProcessingToken::GRAPHICS_BACKEND);
        buffer->setup_rendering(InstanceNetworkBuffer::RenderConfig {
            .target_window = window, .triangulate = true });
        auto render = buffer->get_render_processor();
        ASSERT_NE(render, nullptr);
        auto mill_spec = render->get_mill_spec();
        mill_spec.point_scale = 0.4F;
        mill_spec.use_vertex_extent = false;
        render->set_mill_spec(mill_spec);

        root = std::make_shared<Buffers::RootGraphicsBuffer>();
        root->initialize();
        root->add_child_buffer(buffer);
    }

    void TearDown() override
    {
        if (buffer) {
            if (auto render = buffer->get_render_processor()) {
                if (auto chain = buffer->get_processing_chain())
                    chain->remove_processor_anywhere(render, buffer);
            }
            buffer->set_default_processor(nullptr);
        }
        root.reset();
        if (buffer && buffer->is_initialized() && service)
            service->destroy_buffer(buffer);
        buffer.reset();
        network.reset();
        if (flow_ready)
            Portal::Graphics::get_render_flow().shutdown();
        if (compute_ready)
            Portal::Graphics::get_compute_press().shutdown();
        if (foundry_ready)
            Portal::Graphics::get_shader_foundry().shutdown();
        PresentingVulkanTest::TearDown();
    }

    uint8_t red_at(float x, float y)
    {
        auto* display = Registry::BackendRegistry::instance()
                            .get_service<Registry::Service::DisplayService>();
        EXPECT_NE(display, nullptr);
        if (!display)
            return 0;
        uint32_t width = 0;
        uint32_t height = 0;
        display->get_swapchain_extent(window, width, height);
        std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 4);
        EXPECT_TRUE(display->readback_swapchain_region(
            window, pixels.data(), 0, 0, width, height, pixels.size()));
        const auto px = std::min(static_cast<uint32_t>((x + 1.0F) * 0.5F * width), width - 1);
        const auto py = std::min(static_cast<uint32_t>((1.0F - y) * 0.5F * height), height - 1);
        return pixels[(static_cast<size_t>(py) * width + px) * 4 + 2];
    }

    std::shared_ptr<Core::Window> window;
    std::shared_ptr<InstanceNetwork> network;
    std::shared_ptr<InstanceNetworkBuffer> buffer;
    std::shared_ptr<Buffers::RootGraphicsBuffer> root;
    Registry::Service::BufferService* service {};
    bool foundry_ready {};
    bool compute_ready {};
    bool flow_ready {};
};

TEST_F(InstanceNetworkBufferPresentationTest, RotatedInstanceKeepsItsMilledPointFacingTheView)
{
    root->process_default();
    Buffers::PresentProcessor presenter;
    presenter.processing_function(root);

    const auto render = buffer->get_render_processor();
    ASSERT_NE(render, nullptr);
    EXPECT_EQ(render->get_instance_count(), 2U);
    EXPECT_EQ(render->milled_vertex_count(), 6U);
    EXPECT_GT(red_at(-0.5F, 0.08F), 200U);
    EXPECT_GT(red_at(0.5F, 0.08F), 200U);
}

TEST_F(InstanceNetworkBufferPresentationTest, WorldPlaneHoldsARotatedInstanceEdgeOn)
{
    const auto render = buffer->get_render_processor();
    ASSERT_NE(render, nullptr);
    auto mill_spec = render->get_mill_spec();
    mill_spec.ribbon = Portal::Graphics::MillSpec::Ribbon::WorldPlane;
    render->set_mill_spec(mill_spec);

    root->process_default();
    Buffers::PresentProcessor presenter;
    presenter.processing_function(root);

    EXPECT_GT(red_at(-0.5F, 0.08F), 200U);
    EXPECT_EQ(red_at(0.5F, 0.08F), 0U);
}

#endif

} // namespace
} // namespace MayaFlux::Test
