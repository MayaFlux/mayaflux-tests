#include "gtest/gtest.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "../support/scripted_window.hpp"
#include "../support/native_window_test_support.hpp"
#include "../support/vulkan_test_support.hpp"

#include "MayaFlux/Buffers/BufferProcessingChain.hpp"
#include "MayaFlux/Buffers/Forma/FormaBindingsProcessor.hpp"
#include "MayaFlux/Buffers/Forma/FormaBuffer.hpp"
#include "MayaFlux/Buffers/Forma/FormaProcessor.hpp"
#include "MayaFlux/Buffers/Root/RootGraphicsBuffer.hpp"
#include "MayaFlux/Buffers/Shaders/RenderProcessor.hpp"
#include "MayaFlux/Buffers/Staging/StagingUtils.hpp"
#include "MayaFlux/Kakshya/NDData/VertexFormats.hpp"
#include "MayaFlux/Kakshya/NDData/VertexLayout.hpp"
#include "MayaFlux/Portal/Graphics/ShaderFoundry.hpp"
#include "MayaFlux/Portal/Graphics/ComputePress.hpp"
#include "MayaFlux/Portal/Graphics/RenderFlow.hpp"
#include "MayaFlux/Portal/Graphics/TextureLoom.hpp"
#include "MayaFlux/Portal/Forma/Element.hpp"
#include "MayaFlux/Portal/Forma/Layer.hpp"
#include "MayaFlux/Registry/BackendRegistry.hpp"
#include "MayaFlux/Registry/Service/BufferService.hpp"
#include "MayaFlux/Registry/Service/DisplayService.hpp"

namespace MayaFlux::Test {
namespace {

using Buffers::FormaBuffer;
using Buffers::FormaProcessor;
using Buffers::ProcessingToken;
using Portal::Graphics::PrimitiveTopology;

std::vector<uint8_t> geometry_bytes(size_t count, uint8_t seed)
{
    std::vector<uint8_t> bytes(count);
    for (size_t i = 0; i < count; ++i)
        bytes[i] = static_cast<uint8_t>(seed + i * 37);
    return bytes;
}

TEST(FormaBufferConstructionTest, KeepsTheRequestedCapacityAndTopology)
{
    const FormaBuffer buffer(240, PrimitiveTopology::LINE_STRIP);

    EXPECT_EQ(buffer.get_size_bytes(), 240U);
    EXPECT_EQ(buffer.topology(), PrimitiveTopology::LINE_STRIP);
    EXPECT_EQ(buffer.get_usage(), Buffers::VKBuffer::Usage::VERTEX);
    EXPECT_EQ(buffer.get_modality(), Kakshya::DataModality::VERTEX_POSITIONS_3D);
}

class FormaBufferDeviceTest : public VulkanDeviceTest {
protected:
    static void SetUpTestSuite()
    {
        VulkanDeviceTest::SetUpTestSuite();
        if (!vulkan_unavailable_reason().empty())
            return;

        auto borrowed = std::shared_ptr<Core::VulkanBackend>(&backend(), [](Core::VulkanBackend*) {});
        Portal::Graphics::get_shader_foundry().shutdown();
        s_foundry_ready = Portal::Graphics::get_shader_foundry().initialize(borrowed);
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
    }

    void TearDown() override
    {
        if (buffer) {
            if (auto render = buffer->get_render_processor())
                buffer->get_processing_chain()->remove_processor_anywhere(render, buffer);
            buffer->set_default_processor(nullptr);
            if (buffer->is_initialized()) {
                auto* service = Registry::BackendRegistry::instance()
                                    .get_service<Registry::Service::BufferService>();
                if (service)
                    service->destroy_buffer(buffer);
            }
        }
        buffer.reset();
        window.reset();
    }

    bool make_buffer(size_t capacity, PrimitiveTopology topology)
    {
        buffer = std::make_shared<FormaBuffer>(capacity, topology);
        auto* service = Registry::BackendRegistry::instance()
                            .get_service<Registry::Service::BufferService>();
        if (!service)
            return false;
        service->initialize_buffer(buffer);
        return buffer->is_initialized();
    }

    bool attach()
    {
        if (!buffer)
            return false;
        buffer->setup_processors(ProcessingToken::GRAPHICS_BACKEND);
        return processor() != nullptr;
    }

    [[nodiscard]] std::shared_ptr<FormaProcessor> processor() const
    {
        return std::dynamic_pointer_cast<FormaProcessor>(buffer->get_default_processor());
    }

    [[nodiscard]] uint32_t vertex_count() const
    {
        const auto layout = buffer->get_vertex_layout();
        return layout ? layout->vertex_count : 0;
    }

    [[nodiscard]] std::vector<uint8_t> read_bytes(size_t count) const
    {
        std::vector<uint8_t> bytes(buffer->get_size_bytes());
        Buffers::download_from_gpu(buffer, bytes.data(), bytes.size());
        bytes.resize(count);
        return bytes;
    }

    std::shared_ptr<FormaBuffer> buffer;
    std::shared_ptr<ScriptedWindow> window;

private:
    static inline bool s_foundry_ready {};
};

struct FormaGeometryCase {
    const char* name;
    PrimitiveTopology topology;
    uint32_t stride;
};

class FormaProcessorTopologyTest : public FormaBufferDeviceTest,
                                   public ::testing::WithParamInterface<FormaGeometryCase> { };

TEST_P(FormaProcessorTopologyTest, SubmissionUploadsEveryByteAndUsesTheTopologyStride)
{
    const auto& c = GetParam();
    ASSERT_TRUE(make_buffer(c.stride * 4, c.topology));
    ASSERT_TRUE(attach());
    ASSERT_TRUE(buffer->get_vertex_layout().has_value());
    EXPECT_EQ(buffer->get_vertex_layout()->stride_bytes, c.stride);

    const auto submitted = geometry_bytes(c.stride * 3, 0x5A);
    buffer->submit(submitted);
    ASSERT_TRUE(processor()->has_pending());
    buffer->process_default();

    EXPECT_FALSE(processor()->has_pending());
    EXPECT_EQ(vertex_count(), 3U);
    EXPECT_EQ(read_bytes(submitted.size()), submitted);
}

INSTANTIATE_TEST_SUITE_P(
    Topologies, FormaProcessorTopologyTest,
    ::testing::Values(
        FormaGeometryCase { "Points", PrimitiveTopology::POINT_LIST,
            Kakshya::VertexLayout::for_points().stride_bytes },
        FormaGeometryCase { "LineList", PrimitiveTopology::LINE_LIST,
            Kakshya::VertexLayout::for_lines().stride_bytes },
        FormaGeometryCase { "LineStrip", PrimitiveTopology::LINE_STRIP,
            Kakshya::VertexLayout::for_lines().stride_bytes },
        FormaGeometryCase { "TriangleList", PrimitiveTopology::TRIANGLE_LIST,
            Kakshya::VertexLayout::for_meshes().stride_bytes },
        FormaGeometryCase { "TriangleStrip", PrimitiveTopology::TRIANGLE_STRIP,
            Kakshya::VertexLayout::for_meshes().stride_bytes }),
    [](const ::testing::TestParamInfo<FormaGeometryCase>& info) {
        return info.param.name;
    });

TEST_F(FormaBufferDeviceTest, SubmissionBeforeProcessorSetupIsUploadedOnTheFirstTick)
{
    const auto stride = Kakshya::VertexLayout::for_points().stride_bytes;
    ASSERT_TRUE(make_buffer(stride * 4, PrimitiveTopology::POINT_LIST));
    buffer->submit(geometry_bytes(stride, 0x11));
    const auto latest = geometry_bytes(stride * 2, 0x3C);
    buffer->submit(latest);

    ASSERT_TRUE(attach());
    ASSERT_TRUE(processor()->has_pending());
    buffer->process_default();

    EXPECT_EQ(vertex_count(), 2U);
    EXPECT_FALSE(processor()->has_pending());
    EXPECT_EQ(read_bytes(latest.size()), latest);
}

TEST_F(FormaBufferDeviceTest, LatestSubmissionBeforeATickReplacesEarlierGeometry)
{
    const auto stride = Kakshya::VertexLayout::for_meshes().stride_bytes;
    ASSERT_TRUE(make_buffer(stride * 4, PrimitiveTopology::TRIANGLE_LIST));
    ASSERT_TRUE(attach());

    buffer->submit(geometry_bytes(stride, 0x11));
    const auto latest = geometry_bytes(stride * 3, 0x22);
    buffer->submit(latest);
    buffer->process_default();

    EXPECT_EQ(vertex_count(), 3U);
    EXPECT_EQ(read_bytes(latest.size()), latest);
}

TEST_F(FormaBufferDeviceTest, SubmissionBeyondCapacityGrowsTheBuffer)
{
    const auto stride = Kakshya::VertexLayout::for_points().stride_bytes;
    ASSERT_TRUE(make_buffer(stride, PrimitiveTopology::POINT_LIST));
    ASSERT_TRUE(attach());

    const auto first = geometry_bytes(stride * 4, 0x5A);
    buffer->submit(first);
    buffer->process_default();

    EXPECT_EQ(buffer->get_size_bytes(), static_cast<size_t>(stride * 4 * 1.5F));
    EXPECT_EQ(vertex_count(), 4U);
    EXPECT_EQ(read_bytes(first.size()), first);

    const auto second = geometry_bytes(stride * 5, 0x3C);
    buffer->submit(second);
    buffer->process_default();

    EXPECT_EQ(vertex_count(), 5U);
    EXPECT_EQ(read_bytes(second.size()), second);
}

TEST_F(FormaBufferDeviceTest, TypedSingleVertexAndContiguousRangePreserveAllFields)
{
    ASSERT_TRUE(make_buffer(sizeof(Kakshya::PointVertex) * 3, PrimitiveTopology::POINT_LIST));
    ASSERT_TRUE(attach());

    const Kakshya::PointVertex point {
        .position = { -0.75F, 0.25F, 0.5F },
        .color = { 0.1F, 0.4F, 0.9F },
        .size = 17.0F,
        .uv = { 0.2F, 0.8F }
    };
    buffer->submit(point);
    buffer->process_default();

    std::array<uint8_t, sizeof(point)> expected_single {};
    std::memcpy(expected_single.data(), &point, expected_single.size());
    EXPECT_EQ(vertex_count(), 1U);
    EXPECT_EQ(read_bytes(expected_single.size()),
        std::vector<uint8_t>(expected_single.begin(), expected_single.end()));

    const std::array<Kakshya::PointVertex, 2> points {
        point,
        Kakshya::PointVertex {
            .position = { 0.5F, -0.5F, 0.0F },
            .color = { 0.9F, 0.2F, 0.3F },
            .size = 4.0F,
            .uv = { 0.75F, 0.25F }
        }
    };
    buffer->submit(points);
    buffer->process_default();

    std::vector<uint8_t> expected_range(sizeof(points));
    std::memcpy(expected_range.data(), points.data(), expected_range.size());
    EXPECT_EQ(vertex_count(), 2U);
    EXPECT_EQ(read_bytes(expected_range.size()), expected_range);
}

TEST_F(FormaBufferDeviceTest, ATrailingPartialVertexIsDroppedAndCannotAddADrawableVertex)
{
    const auto stride = Kakshya::VertexLayout::for_lines().stride_bytes;
    ASSERT_TRUE(make_buffer(stride * 2, PrimitiveTopology::LINE_STRIP));
    ASSERT_TRUE(attach());

    const auto partial = geometry_bytes(stride + 7, 0x91);
    buffer->submit(partial);
    buffer->process_default();

    EXPECT_EQ(vertex_count(), 1U);
    EXPECT_EQ(read_bytes(stride), std::vector<uint8_t>(partial.begin(), partial.begin() + stride));
}

TEST_F(FormaBufferDeviceTest, IdleTickKeepsTheLastGeometryAndShorterSubmissionReplacesIt)
{
    const auto stride = Kakshya::VertexLayout::for_meshes().stride_bytes;
    ASSERT_TRUE(make_buffer(stride * 3, PrimitiveTopology::TRIANGLE_LIST));
    ASSERT_TRUE(attach());

    const auto first = geometry_bytes(stride * 3, 0x17);
    buffer->submit(first);
    buffer->process_default();
    buffer->process_default();

    EXPECT_EQ(vertex_count(), 3U);
    EXPECT_EQ(read_bytes(first.size()), first);

    const auto shorter = geometry_bytes(stride, 0xB6);
    buffer->submit(shorter);
    buffer->process_default();

    EXPECT_EQ(vertex_count(), 1U);
    EXPECT_EQ(read_bytes(shorter.size()), shorter);
}

TEST_F(FormaBufferDeviceTest, EmptySubmissionClearsThePreviousDrawCount)
{
    const auto stride = Kakshya::VertexLayout::for_points().stride_bytes;
    ASSERT_TRUE(make_buffer(stride * 4, PrimitiveTopology::POINT_LIST));
    ASSERT_TRUE(attach());

    buffer->submit(std::vector<uint8_t>(stride * 2, 0x7E));
    buffer->process_default();
    ASSERT_EQ(vertex_count(), 2U);

    buffer->submit(std::vector<uint8_t> {});
    buffer->process_default();

    EXPECT_EQ(vertex_count(), 0U);
    EXPECT_FALSE(processor()->has_pending());

    const auto replacement = geometry_bytes(stride * 3, 0x23);
    buffer->submit(replacement);
    buffer->process_default();

    EXPECT_EQ(vertex_count(), 3U);
    EXPECT_EQ(read_bytes(replacement.size()), replacement);
}

TEST_F(FormaBufferDeviceTest, HidingAndShowingAnElementRetainsItsGeometry)
{
    const auto stride = Kakshya::VertexLayout::for_points().stride_bytes;
    ASSERT_TRUE(make_buffer(stride * 4, PrimitiveTopology::POINT_LIST));
    ASSERT_TRUE(attach());
    window = std::make_shared<ScriptedWindow>(128, 128);
    buffer->setup_rendering({ .target_window = window });

    buffer->submit(std::vector<uint8_t>(stride * 2, 0x5A));
    buffer->process_default();
    ASSERT_EQ(vertex_count(), 2U);

    Portal::Forma::Layer layer;
    Portal::Forma::Element element;
    element.with_buffer(buffer);
    const uint32_t id = layer.add(std::move(element));
    const auto render = buffer->get_render_processor();
    ASSERT_NE(render, nullptr);

    ASSERT_TRUE(layer.set_visible(id, false));
    EXPECT_FALSE(render->is_visible(buffer));
    EXPECT_EQ(vertex_count(), 2U);

    ASSERT_TRUE(layer.set_visible(id, true));
    EXPECT_TRUE(render->is_visible(buffer));
    EXPECT_EQ(vertex_count(), 2U);
}

TEST_F(FormaBufferDeviceTest, EmptyGeometryWhileHiddenDoesNotChangeVisibility)
{
    const auto stride = Kakshya::VertexLayout::for_points().stride_bytes;
    ASSERT_TRUE(make_buffer(stride * 4, PrimitiveTopology::POINT_LIST));
    ASSERT_TRUE(attach());
    window = std::make_shared<ScriptedWindow>(128, 128);
    buffer->setup_rendering({ .target_window = window });

    buffer->submit(std::vector<uint8_t>(stride * 2, 0x5A));
    buffer->process_default();
    ASSERT_EQ(vertex_count(), 2U);

    Portal::Forma::Layer layer;
    Portal::Forma::Element element;
    element.with_buffer(buffer);
    const uint32_t id = layer.add(std::move(element));
    const auto render = buffer->get_render_processor();
    ASSERT_NE(render, nullptr);

    ASSERT_TRUE(layer.set_visible(id, false));
    buffer->submit(std::vector<uint8_t> {});
    buffer->process_default();

    EXPECT_FALSE(render->is_visible(buffer));
    EXPECT_EQ(vertex_count(), 0U);

    ASSERT_TRUE(layer.set_visible(id, true));
    EXPECT_TRUE(render->is_visible(buffer));
    EXPECT_EQ(vertex_count(), 0U);
}

TEST_F(FormaBufferDeviceTest, BindingATextureWithoutRenderingIsIgnored)
{
    ASSERT_TRUE(make_buffer(1024, PrimitiveTopology::TRIANGLE_STRIP));
    ASSERT_TRUE(attach());

    EXPECT_NO_THROW(buffer->bind_texture(0, nullptr));
    EXPECT_EQ(buffer->get_render_processor(), nullptr);
}

TEST_F(FormaBufferDeviceTest, RenderingSetupBeforeProcessorSetupKeepsTheRenderProcessor)
{
    ASSERT_TRUE(make_buffer(1024, PrimitiveTopology::TRIANGLE_STRIP));
    window = std::make_shared<ScriptedWindow>(128, 128);

    buffer->setup_rendering({ .target_window = window });
    const auto render = buffer->get_render_processor();
    ASSERT_NE(render, nullptr);

    ASSERT_TRUE(attach());

    EXPECT_EQ(buffer->get_render_processor(), render);
    EXPECT_EQ(buffer->get_processing_chain()->get_final_processor(buffer), render);
}

TEST_F(FormaBufferDeviceTest, RenderingPreservesExplicitShaderOverrides)
{
    ASSERT_TRUE(make_buffer(1024, PrimitiveTopology::TRIANGLE_LIST));
    ASSERT_TRUE(attach());
    window = std::make_shared<ScriptedWindow>(128, 128);

    buffer->setup_rendering({
        .target_window = window,
        .vertex_shader = "point.vert.spv",
        .fragment_shader = "point.frag.spv"
    });

    const auto config = buffer->get_render_config();
    EXPECT_EQ(config.topology, PrimitiveTopology::TRIANGLE_LIST);
    EXPECT_EQ(config.vertex_shader, "point.vert.spv");
    EXPECT_EQ(config.fragment_shader, "point.frag.spv");
}

struct FormaShaderCase {
    const char* name;
    PrimitiveTopology topology;
    bool triangulate;
    bool single_texture;
    bool multi_texture;
    const char* vertex_shader;
    const char* fragment_shader;
    const char* geometry_shader;
};

class FormaBufferShaderTest : public FormaBufferDeviceTest,
                              public ::testing::WithParamInterface<FormaShaderCase> { };

TEST_P(FormaBufferShaderTest, RenderingSelectsShadersForTopologyAndTextureMode)
{
    const auto& c = GetParam();
    ASSERT_TRUE(make_buffer(1024, c.topology));
    ASSERT_TRUE(attach());
    window = std::make_shared<ScriptedWindow>(128, 128);

    FormaBuffer::RenderConfig config;
    config.target_window = window;
    config.triangulate = c.triangulate;
    if (c.single_texture)
        config.default_texture_binding = "texSampler";
    if (c.multi_texture)
        config.additional_textures.emplace_back("otherTexture", nullptr);

    buffer->setup_rendering(config);

    const auto chosen = buffer->get_render_config();
    EXPECT_EQ(chosen.topology, c.topology);
    EXPECT_EQ(chosen.vertex_shader, c.vertex_shader);
    EXPECT_EQ(chosen.fragment_shader, c.fragment_shader);
    EXPECT_EQ(chosen.geometry_shader, c.geometry_shader);
    ASSERT_NE(buffer->get_render_processor(), nullptr);
    const auto render = buffer->get_render_processor();
    EXPECT_TRUE(render->is_alpha_blending_enabled());
    EXPECT_FALSE(buffer->needs_depth_attachment());

    const auto& bindings = render->get_config().bindings;
    if (c.multi_texture) {
        ASSERT_TRUE(bindings.contains("textures"));
        EXPECT_EQ(bindings.at("textures").count, c.single_texture ? 2U : 1U);
    } else if (c.single_texture) {
        ASSERT_TRUE(bindings.contains("texSampler"));
        EXPECT_EQ(bindings.at("texSampler").count, 1U);
    } else {
        EXPECT_TRUE(bindings.empty());
    }
}

INSTANTIATE_TEST_SUITE_P(
    Variants, FormaBufferShaderTest,
    ::testing::Values(
        FormaShaderCase { "Points", PrimitiveTopology::POINT_LIST, false, false, false,
            "point.vert.spv", "point.frag.spv", "" },
        FormaShaderCase { "PointsTextured", PrimitiveTopology::POINT_LIST, false, true, false,
            "point.vert.spv", "point.frag.spv", "" },
        FormaShaderCase { "PointsMilled", PrimitiveTopology::POINT_LIST, true, false, false,
            "milled_shape.vert.spv", "milled_shape.frag.spv", "" },
        FormaShaderCase { "PointsMilledTextured", PrimitiveTopology::POINT_LIST, true, true, false,
            "milled_shape.vert.spv", "milled_shape_textured.frag.spv", "" },
        FormaShaderCase { "PointsMilledMulti", PrimitiveTopology::POINT_LIST, true, false, true,
            "milled_shape.vert.spv", "milled_shape_multi.frag.spv", "" },
        FormaShaderCase { "LineList", PrimitiveTopology::LINE_LIST, false, false, false,
            "line.vert.spv", "line.frag.spv",
#ifdef MAYAFLUX_PLATFORM_MACOS
            "" },
#else
            "line.geom.spv" },
#endif
        FormaShaderCase { "LineListTriangulated", PrimitiveTopology::LINE_LIST, true, false, false,
            "line.vert.spv", "line.frag.spv", "" },
        FormaShaderCase { "LineStripMilled", PrimitiveTopology::LINE_STRIP, true, false, false,
            "line.vert.spv", "line.frag.spv", "" },
        FormaShaderCase { "TriangleList", PrimitiveTopology::TRIANGLE_LIST, false, false, false,
            "triangle.vert.spv", "triangle.frag.spv", "" },
        FormaShaderCase { "TriangleListTextured", PrimitiveTopology::TRIANGLE_LIST, false, true, false,
            "triangle.vert.spv", "forma_textured.frag.spv", "" },
        FormaShaderCase { "TriangleListMulti", PrimitiveTopology::TRIANGLE_LIST, false, false, true,
            "triangle_multi.vert.spv", "forma_multi.frag.spv", "" },
        FormaShaderCase { "TriangleListSingleAndMulti", PrimitiveTopology::TRIANGLE_LIST, true, true, true,
            "triangle_multi.vert.spv", "forma_multi.frag.spv", "" },
        FormaShaderCase { "TriangleStripMilled", PrimitiveTopology::TRIANGLE_STRIP, true, false, false,
            "triangle.vert.spv", "triangle.frag.spv", "" },
        FormaShaderCase { "TriangleStripMilledTextured", PrimitiveTopology::TRIANGLE_STRIP, true, true, false,
            "triangle.vert.spv", "forma_textured.frag.spv", "" }),
    [](const ::testing::TestParamInfo<FormaShaderCase>& info) {
        return info.param.name;
    });

#if defined(MAYAFLUX_TEST_NATIVE_WINDOWS)

class FormaBufferPresentationTest : public PresentingVulkanTest {
protected:
    void SetUp() override
    {
        PresentingVulkanTest::SetUp();
        if (IsSkipped())
            return;

        auto borrowed = std::shared_ptr<Core::VulkanBackend>(&backend(), [](Core::VulkanBackend*) {});
        Portal::Graphics::get_render_flow().shutdown();
        Portal::Graphics::get_texture_manager().shutdown();
        Portal::Graphics::get_shader_foundry().shutdown();
        ASSERT_TRUE(Portal::Graphics::get_shader_foundry().initialize(borrowed));
        foundry_ready = true;
        ASSERT_TRUE(Portal::Graphics::get_render_flow().initialize());
        flow_ready = true;
        ASSERT_TRUE(Portal::Graphics::get_texture_manager().initialize(borrowed));
        loom_ready = true;

        window = make_window();
        ASSERT_NE(window, nullptr);
        ASSERT_TRUE(backend().register_window(window));
    }

    void TearDown() override
    {
        if (buffer) {
            if (bindings)
                buffer->get_processing_chain()->remove_processor(bindings, buffer);
            if (auto render = buffer->get_render_processor())
                buffer->get_processing_chain()->remove_processor_anywhere(render, buffer);
            buffer->set_default_processor(nullptr);
        }
        bindings.reset();
        if (root)
            root->set_default_processor(nullptr);
        root.reset();
        if (buffer && buffer->is_initialized()) {
            auto* service = Registry::BackendRegistry::instance()
                                .get_service<Registry::Service::BufferService>();
            if (service)
                service->destroy_buffer(buffer);
        }
        buffer.reset();
        if (loom_ready)
            Portal::Graphics::get_texture_manager().shutdown();
        if (flow_ready)
            Portal::Graphics::get_render_flow().shutdown();
        if (foundry_ready)
            Portal::Graphics::get_shader_foundry().shutdown();
        PresentingVulkanTest::TearDown();
        window.reset();
    }

    void prepare(bool textured,
        const std::vector<std::shared_ptr<Core::VKImage>>& pre_setup_textures = {},
        const std::vector<std::pair<std::string, std::shared_ptr<Core::VKImage>>>& additional_textures = {})
    {
        buffer = std::make_shared<FormaBuffer>(sizeof(Kakshya::MeshVertex) * 4,
            PrimitiveTopology::TRIANGLE_STRIP);
        auto* service = Registry::BackendRegistry::instance()
                            .get_service<Registry::Service::BufferService>();
        ASSERT_NE(service, nullptr);
        service->initialize_buffer(buffer);
        ASSERT_TRUE(buffer->is_initialized());
        for (const auto& image : pre_setup_textures)
            buffer->set_texture(image, "texSampler");
        buffer->setup_processors(ProcessingToken::GRAPHICS_BACKEND);

        FormaBuffer::RenderConfig config;
        config.target_window = window;
        if (textured)
            config.default_texture_binding = "texSampler";
        config.additional_textures = additional_textures;
        buffer->setup_rendering(config);

        root = std::make_shared<Buffers::RootGraphicsBuffer>();
        root->initialize();
        root->add_child_buffer(buffer);
    }

    void present()
    {
        root->process_default();
        Buffers::PresentProcessor presenter;
        presenter.processing_function(root);
    }

    [[nodiscard]] std::array<uint8_t, 4> center_pixel() const
    {
        auto* display = Registry::BackendRegistry::instance()
                            .get_service<Registry::Service::DisplayService>();
        EXPECT_NE(display, nullptr);
        if (!display)
            return {};
        uint32_t width = 0;
        uint32_t height = 0;
        display->get_swapchain_extent(window, width, height);
        EXPECT_GT(width, 0U);
        EXPECT_GT(height, 0U);
        std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 4);
        if (pixels.empty())
            return {};
        const bool read = display->readback_swapchain_region(
            window, pixels.data(), 0, 0, width, height, pixels.size());
        EXPECT_TRUE(read);
        if (!read)
            return {};
        const size_t center = (static_cast<size_t>(height / 2) * width + width / 2) * 4;
        return { pixels[center], pixels[center + 1], pixels[center + 2], pixels[center + 3] };
    }

    static std::array<Kakshya::MeshVertex, 4> fullscreen_quad()
    {
        return {
            Kakshya::MeshVertex { .position = { -1.0F, -1.0F, 0.0F }, .uv = { 0.0F, 1.0F } },
            Kakshya::MeshVertex { .position = { -1.0F, 1.0F, 0.0F }, .uv = { 0.0F, 0.0F } },
            Kakshya::MeshVertex { .position = { 1.0F, -1.0F, 0.0F }, .uv = { 1.0F, 1.0F } },
            Kakshya::MeshVertex { .position = { 1.0F, 1.0F, 0.0F }, .uv = { 1.0F, 0.0F } }
        };
    }

    std::shared_ptr<Core::Window> window;
    std::shared_ptr<FormaBuffer> buffer;
    std::shared_ptr<Buffers::RootGraphicsBuffer> root;
    std::shared_ptr<Buffers::FormaBindingsProcessor> bindings;
    bool foundry_ready {};
    bool flow_ready {};
    bool loom_ready {};
};

TEST_F(FormaBufferPresentationTest, EmptyGeometryRemovesTheDrawAndLaterSubmissionRestoresIt)
{
    prepare(false);
    ASSERT_NE(root, nullptr);
    buffer->submit(fullscreen_quad());
    present();
    const auto drawn = center_pixel();
    EXPECT_GT(drawn[0], 200U);
    EXPECT_GT(drawn[1], 200U);
    EXPECT_GT(drawn[2], 200U);

    buffer->submit(std::vector<uint8_t> {});
    present();
    const auto cleared = center_pixel();
    EXPECT_LT(cleared[0], 32U);
    EXPECT_LT(cleared[1], 32U);
    EXPECT_LT(cleared[2], 32U);

    buffer->submit(fullscreen_quad());
    present();
    const auto restored = center_pixel();
    EXPECT_GT(restored[0], 200U);
    EXPECT_GT(restored[1], 200U);
    EXPECT_GT(restored[2], 200U);
}

TEST_F(FormaBufferPresentationTest, LayerVisibilityHidesAndRestoresExistingGeometry)
{
    prepare(false);
    ASSERT_NE(root, nullptr);
    buffer->submit(fullscreen_quad());
    present();
    EXPECT_GT(center_pixel()[0], 200U);

    Portal::Forma::Layer layer;
    Portal::Forma::Element element;
    element.with_buffer(buffer);
    const uint32_t id = layer.add(std::move(element));

    ASSERT_TRUE(layer.set_visible(id, false));
    present();
    EXPECT_LT(center_pixel()[0], 32U);

    ASSERT_TRUE(layer.set_visible(id, true));
    present();
    EXPECT_GT(center_pixel()[0], 200U);
}

TEST_F(FormaBufferPresentationTest, LastPendingTextureAndLaterReplacementChangeThePixels)
{
    prepare(true);
    ASSERT_NE(root, nullptr);
    const std::array<uint8_t, 16> white {
        255, 255, 255, 255, 255, 255, 255, 255,
        255, 255, 255, 255, 255, 255, 255, 255
    };
    const std::array<uint8_t, 16> black {
        0, 0, 0, 255, 0, 0, 0, 255,
        0, 0, 0, 255, 0, 0, 0, 255
    };
    auto& loom = Portal::Graphics::get_texture_manager();
    auto white_image = loom.create_2d(2, 2, Portal::Graphics::ImageFormat::RGBA8, white.data());
    auto black_image = loom.create_2d(2, 2, Portal::Graphics::ImageFormat::RGBA8, black.data());
    ASSERT_NE(white_image, nullptr);
    ASSERT_NE(black_image, nullptr);

    buffer->submit(fullscreen_quad());
    buffer->set_texture(black_image, "texSampler");
    buffer->set_texture(white_image, "texSampler");
    present();
    const auto first = center_pixel();
    EXPECT_GT(first[0], 200U);
    EXPECT_GT(first[1], 200U);
    EXPECT_GT(first[2], 200U);

    buffer->set_texture(black_image, "texSampler");
    present();
    const auto replaced = center_pixel();
    EXPECT_LT(replaced[0], 32U);
    EXPECT_LT(replaced[1], 32U);
    EXPECT_LT(replaced[2], 32U);
}

TEST_F(FormaBufferPresentationTest, TexturesQueuedBeforeProcessorSetupReplayInOrder)
{
    const std::array<uint8_t, 16> white {
        255, 255, 255, 255, 255, 255, 255, 255,
        255, 255, 255, 255, 255, 255, 255, 255
    };
    const std::array<uint8_t, 16> black {
        0, 0, 0, 255, 0, 0, 0, 255,
        0, 0, 0, 255, 0, 0, 0, 255
    };
    auto& loom = Portal::Graphics::get_texture_manager();
    auto white_image = loom.create_2d(2, 2, Portal::Graphics::ImageFormat::RGBA8, white.data());
    auto black_image = loom.create_2d(2, 2, Portal::Graphics::ImageFormat::RGBA8, black.data());
    ASSERT_NE(white_image, nullptr);
    ASSERT_NE(black_image, nullptr);

    prepare(true, { black_image, white_image });
    ASSERT_NE(root, nullptr);
    buffer->submit(fullscreen_quad());
    present();

    const auto pixel = center_pixel();
    EXPECT_GT(pixel[0], 200U);
    EXPECT_GT(pixel[1], 200U);
    EXPECT_GT(pixel[2], 200U);
}

TEST_F(FormaBufferPresentationTest, TextureTickedBeforeRenderingSetupIsNotLost)
{
    const std::array<uint8_t, 16> white {
        255, 255, 255, 255, 255, 255, 255, 255,
        255, 255, 255, 255, 255, 255, 255, 255
    };
    auto image = Portal::Graphics::get_texture_manager().create_2d(
        2, 2, Portal::Graphics::ImageFormat::RGBA8, white.data());
    ASSERT_NE(image, nullptr);

    buffer = std::make_shared<FormaBuffer>(sizeof(Kakshya::MeshVertex) * 4,
        PrimitiveTopology::TRIANGLE_STRIP);
    auto* service = Registry::BackendRegistry::instance()
                        .get_service<Registry::Service::BufferService>();
    ASSERT_NE(service, nullptr);
    service->initialize_buffer(buffer);
    ASSERT_TRUE(buffer->is_initialized());
    buffer->setup_processors(ProcessingToken::GRAPHICS_BACKEND);
    buffer->set_texture(image, "texSampler");
    buffer->submit(fullscreen_quad());
    buffer->process_default();
    buffer->setup_rendering({ .target_window = window, .default_texture_binding = "texSampler" });
    root = std::make_shared<Buffers::RootGraphicsBuffer>();
    root->initialize();
    root->add_child_buffer(buffer);

    present();
    EXPECT_GT(center_pixel()[0], 200U);
}

TEST_F(FormaBufferPresentationTest, RuntimeMultiTextureSlotReplacementChangesTheDraw)
{
    const std::array<uint8_t, 16> white {
        255, 255, 255, 255, 255, 255, 255, 255,
        255, 255, 255, 255, 255, 255, 255, 255
    };
    const std::array<uint8_t, 16> black {
        0, 0, 0, 255, 0, 0, 0, 255,
        0, 0, 0, 255, 0, 0, 0, 255
    };
    auto& loom = Portal::Graphics::get_texture_manager();
    auto white_image = loom.create_2d(2, 2, Portal::Graphics::ImageFormat::RGBA8, white.data());
    auto black_image = loom.create_2d(2, 2, Portal::Graphics::ImageFormat::RGBA8, black.data());
    ASSERT_NE(white_image, nullptr);
    ASSERT_NE(black_image, nullptr);

    prepare(false, {}, {
        { "slot0", white_image }, { "slot1", black_image },
        { "slot2", white_image }, { "slot3", white_image },
        { "slot4", white_image }, { "slot5", white_image },
        { "slot6", white_image }, { "slot7", white_image }
    });
    ASSERT_NE(root, nullptr);
    auto quad = fullscreen_quad();
    for (auto& vertex : quad)
        vertex.weight = 2.0F;
    buffer->submit(quad);
    present();
    EXPECT_LT(center_pixel()[0], 32U);

    buffer->bind_texture(1, white_image);
    present();
    EXPECT_GT(center_pixel()[0], 200U);
}

TEST_F(FormaBufferPresentationTest, BoundPushConstantDrivesThePixels)
{
    prepare(false);
    ASSERT_NE(root, nullptr);
    buffer->get_render_processor()->set_fragment_shader(R"(#version 460
layout(location = 0) in vec3 in_color;
layout(location = 0) out vec4 out_color;
layout(push_constant) uniform Constants { float level; } pc;
void main()
{
    out_color = vec4(vec3(pc.level), 1.0);
}
)");

    float level = 1.0F;
    Buffers::ShaderConfig config { "triangle.vert.spv" };
    config.stage = Portal::Graphics::ShaderStage::VERTEX;
    bindings = std::make_shared<Buffers::FormaBindingsProcessor>(std::move(config));
    std::string rejection;
    ASSERT_TRUE(buffer->get_processing_chain()->add_processor(bindings, buffer, &rejection))
        << rejection;
    bindings->bind_push_constant("level", [&] { return level; }, 0);

    buffer->submit(fullscreen_quad());
    buffer->get_processing_chain()->process(buffer);
    present();
    const auto bright = center_pixel();
    EXPECT_GT(bright[0], 200U);
    EXPECT_GT(bright[1], 200U);
    EXPECT_GT(bright[2], 200U);

    level = 0.0F;
    buffer->get_processing_chain()->process(buffer);
    present();
    const auto dark = center_pixel();
    EXPECT_LT(dark[0], 32U);
    EXPECT_LT(dark[1], 32U);
    EXPECT_LT(dark[2], 32U);
}

class FormaTriangulatedBatchPresentationTest : public FormaBufferPresentationTest {
protected:
    void SetUp() override
    {
        FormaBufferPresentationTest::SetUp();
        if (IsSkipped())
            return;
        Portal::Graphics::get_compute_press().shutdown();
        ASSERT_TRUE(Portal::Graphics::get_compute_press().initialize());
        compute_ready = true;
    }

    void TearDown() override
    {
        if (buffer) {
            if (auto render = buffer->get_render_processor())
                buffer->get_processing_chain()->remove_processor_anywhere(render, buffer);
            buffer->set_default_processor(nullptr);
        }
        if (compute_ready)
            Portal::Graphics::get_compute_press().shutdown();
        compute_ready = false;
        FormaBufferPresentationTest::TearDown();
    }

    bool compute_ready {};

    void prepare_batch()
    {
        buffer = std::make_shared<FormaBuffer>(sizeof(Kakshya::MeshVertex) * 10,
            PrimitiveTopology::TRIANGLE_LIST);
        auto* service = Registry::BackendRegistry::instance()
                            .get_service<Registry::Service::BufferService>();
        ASSERT_NE(service, nullptr);
        service->initialize_buffer(buffer);
        ASSERT_TRUE(buffer->is_initialized());
        buffer->setup_processors(ProcessingToken::GRAPHICS_BACKEND);

        FormaBuffer::RenderConfig config;
        config.target_window = window;
        config.triangulate = true;
        buffer->setup_rendering(config);

        root = std::make_shared<Buffers::RootGraphicsBuffer>();
        root->initialize();
        root->add_child_buffer(buffer);
    }

    static std::vector<uint8_t> bytes_of(const std::vector<Kakshya::MeshVertex>& vertices)
    {
        std::vector<uint8_t> bytes(vertices.size() * sizeof(Kakshya::MeshVertex));
        std::memcpy(bytes.data(), vertices.data(), bytes.size());
        return bytes;
    }

    static std::vector<Kakshya::MeshVertex> left_half_as_list(glm::vec3 color)
    {
        auto vertex = [&](float x, float y) {
            return Kakshya::MeshVertex { .position = { x, y, 0.0F }, .color = color };
        };
        return { vertex(-1.0F, -1.0F), vertex(-1.0F, 1.0F), vertex(0.0F, -1.0F),
            vertex(0.0F, -1.0F), vertex(-1.0F, 1.0F), vertex(0.0F, 1.0F) };
    }

    static std::vector<Kakshya::MeshVertex> right_half_as_strip(glm::vec3 color)
    {
        auto vertex = [&](float x, float y) {
            return Kakshya::MeshVertex { .position = { x, y, 0.0F }, .color = color };
        };
        return { vertex(0.0F, -1.0F), vertex(0.0F, 1.0F), vertex(1.0F, -1.0F), vertex(1.0F, 1.0F) };
    }

    [[nodiscard]] std::array<uint8_t, 4> pixel_at(float fraction_x) const
    {
        auto* display = Registry::BackendRegistry::instance()
                            .get_service<Registry::Service::DisplayService>();
        EXPECT_NE(display, nullptr);
        if (!display)
            return {};
        uint32_t width = 0;
        uint32_t height = 0;
        display->get_swapchain_extent(window, width, height);
        EXPECT_GT(width, 0U);
        EXPECT_GT(height, 0U);
        std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 4);
        if (pixels.empty())
            return {};
        const bool read = display->readback_swapchain_region(
            window, pixels.data(), 0, 0, width, height, pixels.size());
        EXPECT_TRUE(read);
        if (!read)
            return {};
        const auto column = static_cast<size_t>(fraction_x * static_cast<float>(width));
        const size_t index = (static_cast<size_t>(height / 2) * width + column) * 4;
        return { pixels[index], pixels[index + 1], pixels[index + 2], pixels[index + 3] };
    }
};

TEST_F(FormaTriangulatedBatchPresentationTest, MixedTopologySlicesDrawThroughOneMilledBatch)
{
    prepare_batch();
    ASSERT_NE(root, nullptr);
    buffer->add_slice(bytes_of(left_half_as_list({ 1.0F, 0.0F, 0.0F })));
    buffer->add_slice(bytes_of(right_half_as_strip({ 0.0F, 1.0F, 0.0F })),
        PrimitiveTopology::TRIANGLE_STRIP);
    present();

    const auto left = pixel_at(0.25F);
    EXPECT_GT(left[2], 200U);
    EXPECT_LT(left[1], 32U);
    EXPECT_LT(left[0], 32U);

    const auto right = pixel_at(0.75F);
    EXPECT_LT(right[2], 32U);
    EXPECT_GT(right[1], 200U);
    EXPECT_LT(right[0], 32U);
}

TEST_F(FormaTriangulatedBatchPresentationTest, HidingASliceRemovesOnlyItsPixelsAndShowingRestoresThem)
{
    prepare_batch();
    ASSERT_NE(root, nullptr);
    const auto list_slice = buffer->add_slice(bytes_of(left_half_as_list({ 1.0F, 0.0F, 0.0F })));
    const auto strip_slice = buffer->add_slice(bytes_of(right_half_as_strip({ 0.0F, 1.0F, 0.0F })),
        PrimitiveTopology::TRIANGLE_STRIP);
    ASSERT_NE(list_slice, strip_slice);
    present();

    buffer->set_slice_visible(strip_slice, false);
    present();
    EXPECT_GT(pixel_at(0.25F)[2], 200U);
    EXPECT_LT(pixel_at(0.75F)[1], 32U);

    buffer->set_slice_visible(strip_slice, true);
    buffer->set_slice_visible(list_slice, false);
    present();
    EXPECT_LT(pixel_at(0.25F)[2], 32U);
    EXPECT_GT(pixel_at(0.75F)[1], 200U);

    buffer->set_slice_visible(list_slice, true);
    present();
    EXPECT_GT(pixel_at(0.25F)[2], 200U);
    EXPECT_GT(pixel_at(0.75F)[1], 200U);
}

#endif

}
}
