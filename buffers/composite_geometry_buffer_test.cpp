#include "gtest/gtest.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <memory>
#include <span>
#include <type_traits>
#include <vector>

#include "../support/native_window_test_support.hpp"
#include "../support/scripted_window.hpp"
#include "../support/vulkan_test_support.hpp"

#include "MayaFlux/Buffers/BufferProcessingChain.hpp"
#include "MayaFlux/Buffers/Geometry/CompositeGeometryBuffer.hpp"
#include "MayaFlux/Buffers/Root/RootGraphicsBuffer.hpp"
#include "MayaFlux/Buffers/Shaders/RenderProcessor.hpp"
#include "MayaFlux/Buffers/Staging/StagingUtils.hpp"
#include "MayaFlux/Nodes/Graphics/GeometryLeafNode.hpp"
#include "MayaFlux/Nodes/Graphics/GeometryWriterNode.hpp"
#include "MayaFlux/Kakshya/NDData/VertexLayout.hpp"
#include "MayaFlux/Portal/Graphics/ShaderFoundry.hpp"
#include "MayaFlux/Portal/Graphics/RenderFlow.hpp"
#include "MayaFlux/Registry/BackendRegistry.hpp"
#include "MayaFlux/Registry/Service/BufferService.hpp"
#include "MayaFlux/Registry/Service/DisplayService.hpp"

namespace MayaFlux::Test {
namespace {

using Buffers::CompositeGeometryBuffer;
using Buffers::ProcessingToken;
using Nodes::GpuSync::GeometryLeafNode;
using Portal::Graphics::PolygonMode;
using Portal::Graphics::PrimitiveTopology;

class RawGeometryNode final : public Nodes::GpuSync::GeometryWriterNode {
public:
    RawGeometryNode()
        : GeometryWriterNode(0)
    {
    }

    template<typename Vertex, size_t Extent>
    void set(std::span<Vertex, Extent> vertices, Kakshya::VertexLayout layout)
    {
        using Value = std::remove_cv_t<Vertex>;
        set_vertices<Value>(std::span<const Value>(vertices.data(), vertices.size()));
        layout.vertex_count = static_cast<uint32_t>(vertices.size());
        layout.stride_bytes = sizeof(Value);
        set_vertex_layout(layout);
    }

    void compute_frame() override { }
};

std::shared_ptr<GeometryLeafNode> geometry(
    std::initializer_list<glm::vec3> positions,
    PrimitiveTopology source_topology = PrimitiveTopology::POINT_LIST)
{
    auto node = std::make_shared<GeometryLeafNode>(std::vector<glm::vec3>(positions));
    node->set_primitive_topology(source_topology);
    node->compute_frame();
    return node;
}

std::vector<uint8_t> concatenate(
    std::initializer_list<std::shared_ptr<GeometryLeafNode>> nodes)
{
    std::vector<uint8_t> bytes;
    for (const auto& node : nodes) {
        const auto data = node->get_vertex_data();
        bytes.insert(bytes.end(), data.begin(), data.end());
    }
    return bytes;
}

class CompositeGeometryBufferDeviceTest : public VulkanDeviceTest {
protected:
    static void SetUpTestSuite()
    {
        VulkanDeviceTest::SetUpTestSuite();
        if (!vulkan_unavailable_reason().empty())
            return;

        auto borrowed = std::shared_ptr<Core::VulkanBackend>(&backend(), [](Core::VulkanBackend*) {});
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
        window = std::make_shared<ScriptedWindow>(320, 240);
        buffer = std::make_shared<CompositeGeometryBuffer>(256);
        service->initialize_buffer(buffer);
        ASSERT_TRUE(buffer->is_initialized());
        buffer->setup_processors(ProcessingToken::GRAPHICS_BACKEND);
    }

    void TearDown() override
    {
        if (buffer) {
            if (auto chain = buffer->get_processing_chain()) {
                for (const auto& render : buffer->get_render_processors())
                    chain->remove_processor_anywhere(render, buffer);
            }
            buffer->set_default_processor(nullptr);
            if (buffer->is_initialized() && service)
                service->destroy_buffer(buffer);
        }
        buffer.reset();
        window.reset();
    }

    void add(
        const std::string& name,
        const std::shared_ptr<Nodes::GpuSync::GeometryWriterNode>& node,
        PrimitiveTopology topology,
        PolygonMode mode = PolygonMode::FILL,
        bool triangulate = false)
    {
        buffer->add_geometry(name, node, Buffers::VKBuffer::RenderConfig {
            .target_window = window,
            .topology = topology,
            .polygon_mode = mode,
            .triangulate = triangulate
        });
    }

    void process()
    {
        auto processor = buffer->get_composite_processor();
        ASSERT_NE(processor, nullptr);
        processor->processing_function(buffer);
    }

    std::vector<uint8_t> read_bytes(size_t count)
    {
        std::vector<uint8_t> bytes(buffer->get_size_bytes());
        Buffers::download_from_gpu(buffer, bytes.data(), bytes.size());
        bytes.resize(count);
        return bytes;
    }

    static inline bool s_foundry_ready {};
    Registry::Service::BufferService* service {};
    std::shared_ptr<ScriptedWindow> window;
    std::shared_ptr<CompositeGeometryBuffer> buffer;
};

TEST_F(CompositeGeometryBufferDeviceTest, ProcessorUploadsAllCollectionsInRegistrationOrderAndPublishesRanges)
{
    auto lines = geometry({ { 1, 2, 3 }, { 4, 5, 6 } }, PrimitiveTopology::LINE_LIST);
    auto points = geometry({ { 7, 8, 9 }, { 10, 11, 12 }, { 13, 14, 15 } });
    add("lines", lines, PrimitiveTopology::LINE_LIST);
    add("points", points, PrimitiveTopology::POINT_LIST);

    process();

    const auto expected = concatenate({ lines, points });
    EXPECT_EQ(read_bytes(expected.size()), expected);

    const auto line_collection = buffer->get_collection("lines");
    const auto point_collection = buffer->get_collection("points");
    ASSERT_TRUE(line_collection.has_value());
    ASSERT_TRUE(point_collection.has_value());
    EXPECT_EQ(line_collection->vertex_offset, 0U);
    EXPECT_EQ(line_collection->vertex_count, 2U);
    EXPECT_EQ(point_collection->vertex_offset, 2U);
    EXPECT_EQ(point_collection->vertex_count, 3U);

    ASSERT_TRUE(line_collection->vertex_layout.has_value());
    ASSERT_TRUE(point_collection->vertex_layout.has_value());
    EXPECT_EQ(line_collection->vertex_layout->vertex_count, 2U);
    EXPECT_EQ(point_collection->vertex_layout->vertex_count, 3U);
    EXPECT_EQ(line_collection->vertex_layout->stride_bytes, lines->get_vertex_stride());
    EXPECT_EQ(point_collection->vertex_layout->stride_bytes, points->get_vertex_stride());
    ASSERT_GT(line_collection->vertex_layout->attributes.size(), 2U);
    ASSERT_GT(point_collection->vertex_layout->attributes.size(), 2U);
    EXPECT_EQ(line_collection->vertex_layout->attributes[2].name, "thickness");
    EXPECT_EQ(point_collection->vertex_layout->attributes[2].name, "size");
}

TEST_F(CompositeGeometryBufferDeviceTest, MixedVertexStridesKeepEachCollectionLayoutAndGpuRangeInRecordUnits)
{
    const std::array<glm::vec3, 2> positions {
        glm::vec3 { -0.5F, 0.0F, 0.0F },
        glm::vec3 { 0.0F, 0.5F, 0.0F }
    };
    struct PositionColor {
        glm::vec3 position;
        glm::vec3 color;
    };
    const std::array<PositionColor, 2> colored {
        PositionColor { { 0.5F, 0.0F, 0.0F }, { 1.0F, 0.0F, 0.0F } },
        PositionColor { { 0.0F, -0.5F, 0.0F }, { 0.0F, 1.0F, 0.0F } }
    };

    auto position_node = std::make_shared<RawGeometryNode>();
    auto position_layout = Kakshya::VertexLayout::for_raw(sizeof(glm::vec3));
    position_layout.attributes = {
        { .component_modality = Kakshya::DataModality::VERTEX_POSITIONS_3D,
            .offset_in_vertex = 0, .name = "position" }
    };
    position_node->set(std::span { positions }, std::move(position_layout));

    auto colored_node = std::make_shared<RawGeometryNode>();
    auto colored_layout = Kakshya::VertexLayout::for_raw(sizeof(PositionColor));
    colored_layout.attributes = {
        { .component_modality = Kakshya::DataModality::VERTEX_POSITIONS_3D,
            .offset_in_vertex = 0, .name = "position" },
        { .component_modality = Kakshya::DataModality::VERTEX_COLORS_RGB,
            .offset_in_vertex = sizeof(glm::vec3), .name = "color" }
    };
    colored_node->set(std::span { colored }, std::move(colored_layout));

    add("positions", position_node, PrimitiveTopology::TRIANGLE_LIST);
    add("colored", colored_node, PrimitiveTopology::TRIANGLE_LIST);
    process();

    std::vector<uint8_t> expected;
    const auto append = [&expected](const auto& vertices) {
        const auto* begin = reinterpret_cast<const uint8_t*>(vertices.data());
        expected.insert(expected.end(), begin, begin + sizeof(vertices));
    };
    append(positions);
    append(colored);
    EXPECT_EQ(read_bytes(expected.size()), expected);

    const auto positions_collection = buffer->get_collection("positions");
    const auto colored_collection = buffer->get_collection("colored");
    ASSERT_TRUE(positions_collection.has_value());
    ASSERT_TRUE(colored_collection.has_value());
    EXPECT_EQ(positions_collection->vertex_offset, 0U);
    EXPECT_EQ(positions_collection->vertex_count, positions.size());
    EXPECT_EQ(colored_collection->vertex_offset, 1U);
    EXPECT_EQ(colored_collection->vertex_count, colored.size());

    ASSERT_TRUE(positions_collection->vertex_layout.has_value());
    ASSERT_TRUE(colored_collection->vertex_layout.has_value());
    EXPECT_EQ(positions_collection->vertex_layout->stride_bytes, sizeof(glm::vec3));
    ASSERT_EQ(positions_collection->vertex_layout->attributes.size(), 1U);
    EXPECT_EQ(positions_collection->vertex_layout->attributes[0].component_modality,
        Kakshya::DataModality::VERTEX_POSITIONS_3D);
    EXPECT_EQ(colored_collection->vertex_layout->stride_bytes, sizeof(PositionColor));
    ASSERT_EQ(colored_collection->vertex_layout->attributes.size(), 2U);
    EXPECT_EQ(colored_collection->vertex_layout->attributes[0].offset_in_vertex, 0U);
    EXPECT_EQ(colored_collection->vertex_layout->attributes[1].component_modality,
        Kakshya::DataModality::VERTEX_COLORS_RGB);
    EXPECT_EQ(colored_collection->vertex_layout->attributes[1].offset_in_vertex, sizeof(glm::vec3));
}

TEST_F(CompositeGeometryBufferDeviceTest, DirtyCollectionChangesAreReaggregatedOnTheNextProcessorPass)
{
    auto first = geometry({ { 1, 0, 0 }, { 2, 0, 0 } }, PrimitiveTopology::LINE_LIST);
    auto second = geometry({ { 3, 0, 0 } });
    add("first", first, PrimitiveTopology::LINE_LIST);
    add("second", second, PrimitiveTopology::POINT_LIST);
    process();

    first->set_positions({ { 20, 0, 0 }, { 21, 0, 0 }, { 22, 0, 0 } });
    first->compute_frame();
    process();

    const auto expected = concatenate({ first, second });
    EXPECT_EQ(read_bytes(expected.size()), expected);
    const auto first_collection = buffer->get_collection("first");
    const auto second_collection = buffer->get_collection("second");
    ASSERT_TRUE(first_collection.has_value());
    ASSERT_TRUE(second_collection.has_value());
    EXPECT_EQ(first_collection->vertex_count, 3U);
    EXPECT_EQ(second_collection->vertex_offset, 3U);
    EXPECT_EQ(second_collection->vertex_count, 1U);
}

TEST_F(CompositeGeometryBufferDeviceTest, CollectionRemovalAndReplacementChangeTheGpuAggregate)
{
    auto retained = geometry({ { 1, 0, 0 } });
    auto removed = geometry({ { 2, 0, 0 }, { 3, 0, 0 } }, PrimitiveTopology::LINE_LIST);
    add("retained", retained, PrimitiveTopology::POINT_LIST);
    add("removed", removed, PrimitiveTopology::LINE_LIST);
    process();

    buffer->remove_geometry("removed");
    process();
    EXPECT_EQ(read_bytes(retained->get_vertex_data().size_bytes()),
        std::vector<uint8_t>(retained->get_vertex_data().begin(), retained->get_vertex_data().end()));
    EXPECT_EQ(buffer->get_collection_count(), 1U);
    EXPECT_FALSE(buffer->get_collection("removed").has_value());

    auto replacement = geometry({ { 8, 0, 0 }, { 9, 0, 0 }, { 10, 0, 0 } },
        PrimitiveTopology::TRIANGLE_LIST);
    add("retained", replacement, PrimitiveTopology::TRIANGLE_LIST);
    process();
    EXPECT_EQ(read_bytes(replacement->get_vertex_data().size_bytes()),
        std::vector<uint8_t>(replacement->get_vertex_data().begin(), replacement->get_vertex_data().end()));
    ASSERT_EQ(buffer->get_collection_count(), 1U);
    const auto collection = buffer->get_collection("retained");
    ASSERT_TRUE(collection.has_value());
    EXPECT_EQ(collection->node, replacement);
    EXPECT_EQ(collection->topology, PrimitiveTopology::TRIANGLE_LIST);
    EXPECT_EQ(collection->vertex_count, 3U);
}

TEST_F(CompositeGeometryBufferDeviceTest, AggregateGrowthPreservesEveryVertexAcrossGpuUpload)
{
    std::vector<glm::vec3> positions(128);
    for (size_t i = 0; i < positions.size(); ++i)
        positions[i] = { static_cast<float>(i), static_cast<float>(i * 2), -static_cast<float>(i) };
    auto large = geometry({});
    large->set_primitive_topology(PrimitiveTopology::TRIANGLE_LIST);
    large->set_positions(std::move(positions));
    large->compute_frame();
    const auto initial_capacity = buffer->get_size_bytes();
    add("large", large, PrimitiveTopology::TRIANGLE_LIST);

    process();

    const auto expected = concatenate({ large });
    EXPECT_GT(buffer->get_size_bytes(), initial_capacity);
    EXPECT_EQ(read_bytes(expected.size()), expected);
}

TEST_F(CompositeGeometryBufferDeviceTest, PerCollectionRenderConfigSelectsIndependentMillingAndShaders)
{
    add("points", geometry({ { 0, 0, 0 } }), PrimitiveTopology::POINT_LIST);
    add("wire",
        geometry({ { 0, 0, 0 }, { 1, 0, 0 } }, PrimitiveTopology::LINE_LIST),
        PrimitiveTopology::LINE_LIST, PolygonMode::LINE, true);
    add("milled", geometry({ { 0, 0, 0 }, { 1, 0, 0 }, { 0, 1, 0 } },
        PrimitiveTopology::TRIANGLE_LIST),
        PrimitiveTopology::TRIANGLE_LIST, PolygonMode::POINT, true);

    const auto renders = buffer->get_render_processors();
    ASSERT_EQ(renders.size(), 3U);
    size_t point_shaders = 0;
    size_t line_shaders = 0;
    size_t triangle_shaders = 0;
    for (const auto& render : renders) {
        ASSERT_NE(render, nullptr);
        EXPECT_EQ(render->get_target_window(), window);
        const auto& shader = render->get_config().shader_path;
        if (shader == "point.vert.spv") {
            ++point_shaders;
            EXPECT_FALSE(render->is_triangulate());
        } else if (shader == "line.vert.spv") {
            ++line_shaders;
            EXPECT_TRUE(render->is_triangulate());
        } else if (shader == "triangle.vert.spv") {
            ++triangle_shaders;
            EXPECT_TRUE(render->is_triangulate());
        }
    }
    EXPECT_EQ(point_shaders, 1U);
    EXPECT_EQ(line_shaders, 1U);
    EXPECT_EQ(triangle_shaders, 1U);
}

#if defined(MAYAFLUX_TEST_NATIVE_WINDOWS)

class CompositeGeometryBufferPresentationTest : public PresentingVulkanTest {
protected:
    void SetUp() override
    {
        PresentingVulkanTest::SetUp();
        if (IsSkipped())
            return;

        Portal::Graphics::get_render_flow().shutdown();
        Portal::Graphics::get_shader_foundry().shutdown();
        auto borrowed = std::shared_ptr<Core::VulkanBackend>(&backend(), [](Core::VulkanBackend*) {});
        ASSERT_TRUE(Portal::Graphics::get_shader_foundry().initialize(borrowed));
        foundry_ready = true;
        ASSERT_TRUE(Portal::Graphics::get_render_flow().initialize());
        flow_ready = true;

        window = make_window({ 0.0F, 0.0F, 0.0F, 1.0F });
        ASSERT_NE(window, nullptr);
        ASSERT_TRUE(backend().register_window(window));
        service = Registry::BackendRegistry::instance()
                      .get_service<Registry::Service::BufferService>();
        ASSERT_NE(service, nullptr);
        buffer = std::make_shared<CompositeGeometryBuffer>(256);
        service->initialize_buffer(buffer);
        ASSERT_TRUE(buffer->is_initialized());
        buffer->setup_processors(ProcessingToken::GRAPHICS_BACKEND);
        root = std::make_shared<Buffers::RootGraphicsBuffer>();
        root->initialize();
        root->add_child_buffer(buffer);
    }

    void TearDown() override
    {
        if (buffer) {
            if (auto chain = buffer->get_processing_chain()) {
                for (const auto& render : buffer->get_render_processors())
                    chain->remove_processor_anywhere(render, buffer);
            }
            buffer->set_default_processor(nullptr);
        }
        root.reset();
        if (buffer && buffer->is_initialized() && service)
            service->destroy_buffer(buffer);
        buffer.reset();
        window.reset();
        if (flow_ready)
            Portal::Graphics::get_render_flow().shutdown();
        if (foundry_ready)
            Portal::Graphics::get_shader_foundry().shutdown();
        for (const auto& path : temporary_shaders)
            std::filesystem::remove(path);
        temporary_shaders.clear();
        PresentingVulkanTest::TearDown();
    }

    std::string write_shader(const char* extension, const char* source)
    {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto path = std::filesystem::temp_directory_path()
            / ("mayaflux_composite_" + std::to_string(stamp) + extension);
        std::ofstream file(path);
        EXPECT_TRUE(file.good());
        file << source;
        file.close();
        EXPECT_TRUE(file.good());
        temporary_shaders.push_back(path);
        return path.string();
    }

    std::array<uint8_t, 4> pixel_at_ndc(float x, float y)
    {
        auto* display = Registry::BackendRegistry::instance()
                            .get_service<Registry::Service::DisplayService>();
        EXPECT_NE(display, nullptr);
        if (!display)
            return {};

        uint32_t width = 0;
        uint32_t height = 0;
        display->get_swapchain_extent(window, width, height);
        std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 4);
        if (pixels.empty())
            return {};
        EXPECT_TRUE(display->readback_swapchain_region(
            window, pixels.data(), 0, 0, width, height, pixels.size()));

        const uint32_t px = std::min(static_cast<uint32_t>((x + 1.0F) * 0.5F * width), width - 1);
        const uint32_t py = std::min(static_cast<uint32_t>((1.0F - y) * 0.5F * height), height - 1);
        const size_t offset = (static_cast<size_t>(py) * width + px) * 4;
        return { pixels[offset], pixels[offset + 1], pixels[offset + 2], pixels[offset + 3] };
    }

    std::shared_ptr<Core::Window> window;
    std::shared_ptr<CompositeGeometryBuffer> buffer;
    std::shared_ptr<Buffers::RootGraphicsBuffer> root;
    Registry::Service::BufferService* service {};
    std::vector<std::filesystem::path> temporary_shaders;
    bool foundry_ready {};
    bool flow_ready {};
};

TEST_F(CompositeGeometryBufferPresentationTest, DrawsEachCollectionFromItsOwnStrideLayoutAndVertexRange)
{
    const auto position_vertex = write_shader(".vert", R"(#version 460
layout(location = 0) in vec3 position;
void main() { gl_Position = vec4(position, 1.0); }
)");
    const auto red_fragment = write_shader(".frag", R"(#version 460
layout(location = 0) out vec4 color;
void main() { color = vec4(1.0, 0.0, 0.0, 1.0); }
)");
    const auto colored_vertex = write_shader(".vert", R"(#version 460
layout(location = 0) in vec3 position;
layout(location = 1) in vec3 vertex_color;
layout(location = 0) out vec3 color;
void main() { gl_Position = vec4(position, 1.0); color = vertex_color; }
)");
    const auto colored_fragment = write_shader(".frag", R"(#version 460
layout(location = 0) in vec3 color;
layout(location = 0) out vec4 output_color;
void main() { output_color = vec4(color, 1.0); }
)");

    const std::array<glm::vec3, 4> left_quad {
        glm::vec3 { -0.9F, -0.6F, 0.0F }, glm::vec3 { -0.1F, -0.6F, 0.0F },
        glm::vec3 { -0.9F, 0.6F, 0.0F }, glm::vec3 { -0.1F, 0.6F, 0.0F }
    };
    struct PositionColor {
        glm::vec3 position;
        glm::vec3 color;
    };
    const std::array<PositionColor, 3> right_triangle {
        PositionColor { { 0.1F, -0.6F, 0.0F }, { 0.0F, 1.0F, 0.0F } },
        PositionColor { { 0.9F, -0.6F, 0.0F }, { 0.0F, 1.0F, 0.0F } },
        PositionColor { { 0.5F, 0.6F, 0.0F }, { 0.0F, 1.0F, 0.0F } }
    };

    auto left_node = std::make_shared<RawGeometryNode>();
    auto left_layout = Kakshya::VertexLayout::for_raw(sizeof(glm::vec3));
    left_layout.attributes = {
        { .component_modality = Kakshya::DataModality::VERTEX_POSITIONS_3D,
            .offset_in_vertex = 0, .name = "position" }
    };
    left_node->set(std::span { left_quad }, std::move(left_layout));

    auto right_node = std::make_shared<RawGeometryNode>();
    auto right_layout = Kakshya::VertexLayout::for_raw(sizeof(PositionColor));
    right_layout.attributes = {
        { .component_modality = Kakshya::DataModality::VERTEX_POSITIONS_3D,
            .offset_in_vertex = 0, .name = "position" },
        { .component_modality = Kakshya::DataModality::VERTEX_COLORS_RGB,
            .offset_in_vertex = sizeof(glm::vec3), .name = "color" }
    };
    right_node->set(std::span { right_triangle }, std::move(right_layout));

    buffer->add_geometry("left", left_node, Buffers::VKBuffer::RenderConfig {
        .target_window = window,
        .vertex_shader = position_vertex,
        .fragment_shader = red_fragment,
        .topology = PrimitiveTopology::TRIANGLE_STRIP
    });
    buffer->add_geometry("right", right_node, Buffers::VKBuffer::RenderConfig {
        .target_window = window,
        .vertex_shader = colored_vertex,
        .fragment_shader = colored_fragment,
        .topology = PrimitiveTopology::TRIANGLE_LIST
    });

    root->process_default();
    Buffers::PresentProcessor presenter;
    presenter.processing_function(root);

    const auto left_pixel = pixel_at_ndc(-0.5F, 0.0F);
    EXPECT_GT(left_pixel[2], 200U);
    EXPECT_LT(left_pixel[1], 40U);
    EXPECT_LT(left_pixel[0], 40U);
    const auto right_pixel = pixel_at_ndc(0.5F, 0.0F);
    EXPECT_LT(right_pixel[0], 40U);
    EXPECT_GT(right_pixel[1], 200U);
    EXPECT_LT(right_pixel[2], 40U);

    const auto left_collection = buffer->get_collection("left");
    const auto right_collection = buffer->get_collection("right");
    ASSERT_TRUE(left_collection.has_value());
    ASSERT_TRUE(right_collection.has_value());
    EXPECT_EQ(left_collection->vertex_offset, 0U);
    EXPECT_EQ(right_collection->vertex_offset, 2U);
    EXPECT_EQ(left_collection->vertex_layout->stride_bytes, sizeof(glm::vec3));
    EXPECT_EQ(right_collection->vertex_layout->stride_bytes, sizeof(PositionColor));
}

#endif

} // namespace
} // namespace MayaFlux::Test
