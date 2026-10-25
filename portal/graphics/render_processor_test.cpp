#include "gtest/gtest.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "../../support/native_window_test_support.hpp"

#if defined(MAYAFLUX_TEST_NATIVE_WINDOWS)

#include "MayaFlux/Buffers/BufferProcessingChain.hpp"
#include "MayaFlux/Buffers/Root/RootGraphicsBuffer.hpp"
#include "MayaFlux/Buffers/Shaders/RenderProcessor.hpp"
#include "MayaFlux/Buffers/VKBuffer.hpp"
#include "MayaFlux/Core/Backends/Graphics/Vulkan/VKImage.hpp"
#include "MayaFlux/Kakshya/NDData/VertexLayout.hpp"
#include "MayaFlux/Kinesis/Viewport/ViewTransform.hpp"
#include "MayaFlux/Portal/Graphics/RenderFlow.hpp"
#include "MayaFlux/Portal/Graphics/TextureLoom.hpp"
#include "MayaFlux/Registry/BackendRegistry.hpp"
#include "MayaFlux/Registry/Service/BufferService.hpp"
#include "MayaFlux/Registry/Service/DisplayService.hpp"

namespace MayaFlux::Test {
namespace {

    using Buffers::BufferProcessingChain;
    using Buffers::ProcessingToken;
    using Buffers::RenderProcessor;
    using Buffers::RootGraphicsBuffer;
    using Buffers::ShaderBinding;
    using Buffers::ShaderConfig;
    using Buffers::VKBuffer;
    using Portal::Graphics::RenderPipelineID;

    constexpr const char* k_plain_vertex = R"(#version 460
void main() {
    vec2 p[3] = vec2[3](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));
    gl_Position = vec4(p[gl_VertexIndex], 0.0, 1.0);
}
)";

    constexpr const char* k_push_vertex = R"(#version 460
layout(push_constant) uniform Constants { float scale; float tail; } pc;
void main() {
    vec2 p[3] = vec2[3](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));
    gl_Position = vec4(p[gl_VertexIndex] * pc.scale, 0.0, 1.0);
}
)";

    constexpr const char* k_red_fragment = R"(#version 460
layout(location = 0) out vec4 color;
void main() { color = vec4(1.0, 0.0, 0.0, 1.0); }
)";

    constexpr const char* k_specialized_fragment = R"(#version 460
layout(constant_id = 0) const bool mill_active = true;
layout(location = 0) out vec4 color;
void main() { color = vec4(float(!mill_active), float(mill_active), 0.0, 1.0); }
)";

    constexpr const char* k_storage_fragment = R"(#version 460
layout(set = 1, binding = 0, std430) readonly buffer Data { float values[]; } data;
layout(location = 0) out vec4 color;
void main() { color = vec4(data.values[0], 0.0, 0.0, 1.0); }
)";

    class RenderProcessorPresentationTest : public PresentingVulkanTest {
    protected:
        void SetUp() override
        {
            PresentingVulkanTest::SetUp();
            if (IsSkipped())
                return;

            Portal::Graphics::get_render_flow().shutdown();
            Portal::Graphics::get_shader_foundry().shutdown();
            auto borrowed_backend = std::shared_ptr<Core::VulkanBackend>(&backend(), [](Core::VulkanBackend*) {});
            ASSERT_TRUE(Portal::Graphics::get_shader_foundry().initialize(borrowed_backend));
            foundry_ready = true;
            ASSERT_TRUE(Portal::Graphics::get_render_flow().initialize());
            flow_ready = true;
        }

        void TearDown() override
        {
            if (chain && render && child)
                chain->remove_processor_anywhere(render, child);
            render.reset();
            chain.reset();
            if (root)
                root->set_default_processor(nullptr);
            root.reset();

            for (const auto& buffer : extra_buffers) {
                if (buffer && buffer->is_initialized()) {
                    auto* service = Registry::BackendRegistry::instance()
                                        .get_service<Registry::Service::BufferService>();
                    if (service)
                        service->destroy_buffer(buffer);
                }
            }
            extra_buffers.clear();

            if (child && child->is_initialized()) {
                auto* service = Registry::BackendRegistry::instance()
                                    .get_service<Registry::Service::BufferService>();
                if (service)
                    service->destroy_buffer(child);
            }
            child.reset();

            Portal::Graphics::get_texture_manager().shutdown();
            if (flow_ready)
                Portal::Graphics::get_render_flow().shutdown();
            if (foundry_ready)
                Portal::Graphics::get_shader_foundry().shutdown();
            PresentingVulkanTest::TearDown();
        }

        std::shared_ptr<VKBuffer> storage_buffer()
        {
            auto buffer = std::make_shared<VKBuffer>(16, VKBuffer::Usage::HOST_STORAGE,
                Kakshya::DataModality::UNKNOWN);
            extra_buffers.push_back(buffer);
            return buffer;
        }

        void prepare_draw(const char* vertex_source, bool with_layout = true, size_t push_constant_size = 0,
            const char* fragment_source = k_red_fragment, size_t buffer_size = 64)
        {
            window = make_window();
            ASSERT_NE(window, nullptr);
            ASSERT_TRUE(backend().register_window(window));

            child = std::make_shared<VKBuffer>(buffer_size, VKBuffer::Usage::VERTEX,
                Kakshya::DataModality::VERTEX_POSITIONS_3D);
            auto* service = Registry::BackendRegistry::instance()
                                .get_service<Registry::Service::BufferService>();
            ASSERT_NE(service, nullptr);
            service->initialize_buffer(child);
            ASSERT_TRUE(child->is_initialized());

            if (with_layout)
                child->set_vertex_layout(Kakshya::VertexLayout {});

            ShaderConfig shader { vertex_source };
            shader.stage = Portal::Graphics::ShaderStage::VERTEX;
            shader.push_constant_size = push_constant_size;
            render = std::make_shared<RenderProcessor>(shader);
            render->set_fragment_shader(fragment_source);
            render->set_target_window(window, child);
            render->set_primitive_topology(Portal::Graphics::PrimitiveTopology::TRIANGLE_LIST);
            render->disable_depth_test();
            render->set_vertex_range(0, 3);

            chain = child->get_processing_chain();
            chain->set_preferred_token(ProcessingToken::GRAPHICS_BACKEND);
            chain->add_final_processor(render, child);

            root = std::make_shared<RootGraphicsBuffer>();
            root->initialize();
            root->add_child_buffer(child);
        }

        void tick()
        {
            child->mark_for_processing(true);
            root->process_default();
        }

        void present()
        {
            Buffers::PresentProcessor presenter;
            presenter.processing_function(root);
        }

        static bool pipeline_alive(RenderPipelineID id)
        {
            return static_cast<bool>(Portal::Graphics::get_render_flow().get_view_transform_layout(id));
        }

        bool retired_within(RenderPipelineID id, int max_ticks = 16)
        {
            for (int i = 0; i < max_ticks; ++i) {
                if (!pipeline_alive(id)) {
                    return true;
                }
                tick();
                present();
            }
            return !pipeline_alive(id);
        }

        std::shared_ptr<Core::Window> window;
        std::shared_ptr<RootGraphicsBuffer> root;
        std::shared_ptr<VKBuffer> child;
        std::shared_ptr<BufferProcessingChain> chain;
        std::shared_ptr<RenderProcessor> render;
        std::vector<std::shared_ptr<VKBuffer>> extra_buffers;
        bool foundry_ready {};
        bool flow_ready {};
    };

    TEST_F(RenderProcessorPresentationTest, ProcessingCreatesOnePipelineAssociatedWithTheBufferAndWindow)
    {
        prepare_draw(k_plain_vertex);

        tick();

        ASSERT_TRUE(render->is_pipeline_ready());
        const auto id = render->get_render_pipeline_id();
        const auto associations = child->get_render_pipelines();
        ASSERT_EQ(associations.size(), 1U);
        EXPECT_EQ(associations.at(id), window);
        EXPECT_NE(child->get_pipeline_command(id), Portal::Graphics::INVALID_COMMAND_BUFFER);
        EXPECT_EQ(root->get_renderable_buffers().size(), 1U);
        EXPECT_TRUE(pipeline_alive(id));
    }

    TEST_F(RenderProcessorPresentationTest, RepeatedProcessingKeepsTheSamePipeline)
    {
        prepare_draw(k_plain_vertex);
        tick();
        const auto id = render->get_render_pipeline_id();

        tick();
        tick();

        EXPECT_EQ(render->get_render_pipeline_id(), id);
        EXPECT_EQ(child->get_render_pipelines().size(), 1U);
        EXPECT_EQ(root->get_renderable_buffers().size(), 1U);
    }

    TEST_F(RenderProcessorPresentationTest, TriangulationStateSpecializesEachCreatedPipeline)
    {
        prepare_draw(k_plain_vertex, true, 0, k_specialized_fragment, 3 * 68);
        auto layout = Kakshya::VertexLayout::for_points();
        layout.vertex_count = 3;
        child->set_vertex_layout(layout);
        render->set_cull_mode(Portal::Graphics::CullMode::NONE);
        auto* display = Registry::BackendRegistry::instance()
                            .get_service<Registry::Service::DisplayService>();
        ASSERT_NE(display, nullptr);

        const auto color_format = static_cast<vk::Format>(display->get_swapchain_format(window));
        Kakshya::ImageFormat image_format = Kakshya::ImageFormat::RGBA8;
        bool bgra = false;
        switch (color_format) {
        case vk::Format::eR8G8B8A8Unorm:
            image_format = Kakshya::ImageFormat::RGBA8;
            break;
        case vk::Format::eR8G8B8A8Srgb:
            image_format = Kakshya::ImageFormat::RGBA8_SRGB;
            break;
        case vk::Format::eB8G8R8A8Unorm:
            image_format = Kakshya::ImageFormat::BGRA8;
            bgra = true;
            break;
        case vk::Format::eB8G8R8A8Srgb:
            image_format = Kakshya::ImageFormat::BGRA8_SRGB;
            bgra = true;
            break;
        default:
            GTEST_SKIP() << "Unsupported swapchain format for specialization readback";
        }

        auto borrowed_backend = std::shared_ptr<Core::VulkanBackend>(&backend(), [](Core::VulkanBackend*) {});
        auto& loom = Portal::Graphics::get_texture_manager();
        ASSERT_TRUE(loom.initialize(borrowed_backend));

        const auto draw_pixel = [&](RenderPipelineID pipeline) -> std::optional<std::array<uint8_t, 4>> {
            constexpr uint32_t extent = 8;
            const auto target = loom.create_render_target(extent, extent, image_format);
            if (!target)
                return std::nullopt;

            auto& foundry = Portal::Graphics::get_shader_foundry();
            auto& flow = Portal::Graphics::get_render_flow();
            const auto id = foundry.begin_commands(Portal::Graphics::ShaderFoundry::CommandBufferType::GRAPHICS);
            auto cmd = foundry.get_command_buffer(id);

            vk::RenderingAttachmentInfo attachment {};
            attachment.imageView = target->get_image_view();
            attachment.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
            attachment.loadOp = vk::AttachmentLoadOp::eClear;
            attachment.storeOp = vk::AttachmentStoreOp::eStore;
            attachment.clearValue.color = vk::ClearColorValue { std::array<float, 4> { 0.0F, 0.0F, 0.0F, 0.0F } };

            vk::RenderingInfo info {};
            info.renderArea = vk::Rect2D { { 0, 0 }, { extent, extent } };
            info.layerCount = 1;
            info.colorAttachmentCount = 1;
            info.pColorAttachments = &attachment;

            cmd.beginRendering(info);
            flow.bind_pipeline(id, pipeline);
            cmd.setViewport(0, vk::Viewport { 0.0F, 0.0F, static_cast<float>(extent), static_cast<float>(extent), 0.0F, 1.0F });
            cmd.setScissor(0, vk::Rect2D { { 0, 0 }, { extent, extent } });
            flow.draw(id, 3);
            cmd.endRendering();
            foundry.submit_and_wait(id);

            const auto downloaded = loom.download_image(target);
            if (!downloaded || downloaded->byte_size() < 4)
                return std::nullopt;
            std::array<uint8_t, 4> pixel {};
            std::memcpy(pixel.data(), downloaded->data(), pixel.size());
            return pixel;
        };

        tick();
        ASSERT_TRUE(render->is_pipeline_ready());
        const auto native = render->get_render_pipeline_id();
        const auto native_pixel = draw_pixel(native);
        ASSERT_TRUE(native_pixel.has_value());
        const std::array<uint8_t, 4> expected_native {
            bgra ? uint8_t { 0 } : uint8_t { 255 }, 0,
            bgra ? uint8_t { 255 } : uint8_t { 0 }, 255
        };
        EXPECT_EQ(*native_pixel, expected_native);

        render->set_vertex_range(0, 0);
        render->set_triangulate(true);
        tick();
        ASSERT_TRUE(render->is_pipeline_ready());
        const auto milled = render->get_render_pipeline_id();
        ASSERT_NE(milled, native);
        const auto milled_pixel = draw_pixel(milled);
        ASSERT_TRUE(milled_pixel.has_value());
        EXPECT_EQ(*milled_pixel, (std::array<uint8_t, 4> { 0, 255, 0, 255 }));
    }

    TEST_F(RenderProcessorPresentationTest, ReplacingThePipelineLeavesOneAssociationAndRetiresTheOldPipeline)
    {
        prepare_draw(k_plain_vertex);
        tick();
        present();
        const auto original = render->get_render_pipeline_id();
        ASSERT_TRUE(pipeline_alive(original));

        render->set_cull_mode(Portal::Graphics::CullMode::FRONT);
        tick();

        const auto replacement = render->get_render_pipeline_id();
        EXPECT_NE(replacement, original);
        const auto associations = child->get_render_pipelines();
        EXPECT_EQ(associations.size(), 1U);
        EXPECT_TRUE(associations.contains(replacement));
        EXPECT_FALSE(associations.contains(original));
        EXPECT_TRUE(pipeline_alive(replacement));
        EXPECT_EQ(root->get_renderable_buffers().size(), 1U);

        present();
        EXPECT_TRUE(retired_within(original));
        EXPECT_TRUE(pipeline_alive(replacement));
    }

    TEST_F(RenderProcessorPresentationTest, PipelineIsCreatedOnceTheBufferGainsItsVertexLayout)
    {
        prepare_draw(k_plain_vertex, false);

        tick();
        EXPECT_FALSE(render->is_pipeline_ready());
        EXPECT_TRUE(root->get_renderable_buffers().empty());

        child->set_vertex_layout(Kakshya::VertexLayout {});
        tick();

        EXPECT_TRUE(render->is_pipeline_ready());
        EXPECT_EQ(root->get_renderable_buffers().size(), 1U);
    }

    TEST_F(RenderProcessorPresentationTest, PushConstantFeedBeyondTheCreatedRangeCreatesAReplacementPipeline)
    {
        prepare_draw(k_push_vertex, true, sizeof(float));
        render->set_push_constant_data(1.0F);
        tick();
        present();
        const auto original = render->get_render_pipeline_id();
        ASSERT_TRUE(pipeline_alive(original));

        render->feed("tail", [] { return Buffers::ShaderProcessor::FeedValue { 2.0 }; }, sizeof(float), sizeof(float));
        tick();

        const auto replacement = render->get_render_pipeline_id();
        EXPECT_NE(replacement, original);
        EXPECT_EQ(child->get_render_pipelines().size(), 1U);
        EXPECT_EQ(root->get_renderable_buffers().size(), 1U);

        present();
        EXPECT_TRUE(retired_within(original));
    }

    TEST_F(RenderProcessorPresentationTest, PushConstantFeedWithinTheCreatedRangeKeepsThePipeline)
    {
        prepare_draw(k_push_vertex, true, 2 * sizeof(float));
        render->set_push_constant_data(std::array<float, 2> { 1.0F, 0.0F });
        tick();
        const auto original = render->get_render_pipeline_id();

        render->feed("tail", [] { return Buffers::ShaderProcessor::FeedValue { 2.0 }; }, sizeof(float), sizeof(float));
        tick();

        EXPECT_EQ(render->get_render_pipeline_id(), original);
    }

    TEST_F(RenderProcessorPresentationTest, BindingAddedAfterTheFirstDrawCreatesAReplacementPipeline)
    {
        prepare_draw(k_plain_vertex);
        tick();
        present();
        const auto original = render->get_render_pipeline_id();

        render->add_binding("extra", ShaderBinding { 1, 0, vk::DescriptorType::eStorageBuffer });
        tick();

        const auto replacement = render->get_render_pipeline_id();
        EXPECT_NE(replacement, original);
        EXPECT_EQ(child->get_render_pipelines().size(), 1U);

        present();
        EXPECT_TRUE(retired_within(original));
    }

    TEST_F(RenderProcessorPresentationTest, PipelineWaitsForABindingTheShaderUsesAndIsCreatedOnceItIsBound)
    {
        prepare_draw(k_plain_vertex, true, 0, k_storage_fragment);

        tick();
        tick();
        EXPECT_FALSE(render->is_pipeline_ready());
        EXPECT_TRUE(child->get_render_pipelines().empty());
        EXPECT_TRUE(root->get_renderable_buffers().empty());

        render->bind_buffer("data", storage_buffer());
        tick();

        EXPECT_TRUE(render->is_pipeline_ready());
        EXPECT_EQ(child->get_render_pipelines().size(), 1U);
        EXPECT_EQ(root->get_renderable_buffers().size(), 1U);
    }

    TEST_F(RenderProcessorPresentationTest, PipelineIsCreatedOnceTheMissingBindingIsStagedOnTheBuffer)
    {
        prepare_draw(k_plain_vertex, true, 0, k_storage_fragment);
        tick();
        ASSERT_FALSE(render->is_pipeline_ready());

        auto data = storage_buffer();
        auto* service = Registry::BackendRegistry::instance()
                            .get_service<Registry::Service::BufferService>();
        ASSERT_NE(service, nullptr);
        service->initialize_buffer(data);
        Portal::Graphics::DescriptorBindingInfo descriptor;
        descriptor.set = 1;
        descriptor.binding = 0;
        descriptor.type = vk::DescriptorType::eStorageBuffer;
        descriptor.buffer_info = vk::DescriptorBufferInfo(data->get_buffer(), 0, data->get_size_bytes());
        child->get_pipeline_context().descriptor_buffer_bindings.push_back(descriptor);
        tick();

        EXPECT_TRUE(render->is_pipeline_ready());
        EXPECT_EQ(root->get_renderable_buffers().size(), 1U);
    }

    TEST_F(RenderProcessorPresentationTest, PipelineIsCreatedWithAPlaceholderWhenTheBindingNeverArrivesAndYieldsWhenItDoes)
    {
        prepare_draw(k_plain_vertex, true, 0, k_storage_fragment);

        int ticks = 0;
        while (!render->is_pipeline_ready() && ticks < 200) {
            tick();
            ++ticks;
        }

        ASSERT_TRUE(render->is_pipeline_ready());
        EXPECT_GT(ticks, 1);
        EXPECT_EQ(child->get_render_pipelines().size(), 1U);
        const auto placeholder = render->get_render_pipeline_id();

        render->bind_buffer("data", storage_buffer());
        tick();

        EXPECT_NE(render->get_render_pipeline_id(), placeholder);
        EXPECT_EQ(child->get_render_pipelines().size(), 1U);
        EXPECT_FALSE(child->get_render_pipelines().contains(placeholder));
    }

    struct UnchangedSetterCase {
        const char* name;
        std::function<void(RenderProcessor&, const std::shared_ptr<VKBuffer>&)> apply;
    };

    std::vector<UnchangedSetterCase> unchanged_setter_cases()
    {
        using Portal::Graphics::CullMode;
        const auto view = Kinesis::look_at_perspective(
            { 0.0F, 1.0F, 3.0F }, { 0.0F, 0.0F, 0.0F }, glm::radians(45.0F), 1.0F, 0.1F, 100.0F);
        return {
            { .name = "CullMode", .apply = [](RenderProcessor& render, const auto&) { render.set_cull_mode(CullMode::NONE); } },
            { .name = "PrimitiveTopology", .apply = [](RenderProcessor& render, const auto&) { render.set_primitive_topology(Portal::Graphics::PrimitiveTopology::TRIANGLE_LIST); } },
            { .name = "PolygonMode", .apply = [](RenderProcessor& render, const auto&) { render.set_polygon_mode(Portal::Graphics::PolygonMode::FILL); } },
            { .name = "AlphaBlendingOn", .apply = [](RenderProcessor& render, const auto&) { render.enable_alpha_blending(); } },
            { .name = "AlphaBlendingOff", .apply = [](RenderProcessor& render, const auto&) { render.disable_alpha_blending(); } },
            { .name = "DepthTestOff", .apply = [](RenderProcessor& render, const auto&) { render.disable_depth_test(); } },
            { .name = "ViewTransform", .apply = [view](RenderProcessor& render, const auto&) { render.set_view_transform(view, false, CullMode::NONE); } },
            { .name = "PushConstantSize", .apply = [](RenderProcessor& render, const auto&) { render.set_push_constant_size(sizeof(float)); } },
            { .name = "VertexLayoutCountOnly", .apply = [count = 0U](RenderProcessor& render, const std::shared_ptr<VKBuffer>& buffer) mutable {
                 Kakshya::VertexLayout layout;
                 layout.vertex_count = ++count;
                 render.set_buffer_vertex_layout(buffer, layout);
             } },
        };
    }

    class RenderProcessorUnchangedSetterTest : public RenderProcessorPresentationTest,
                                               public ::testing::WithParamInterface<UnchangedSetterCase> { };

    TEST_P(RenderProcessorUnchangedSetterTest, RepeatingTheSameValueKeepsThePipeline)
    {
        prepare_draw(k_push_vertex, true, sizeof(float));
        render->set_push_constant_data(1.0F);
        auto apply = GetParam().apply;
        apply(*render, child);
        tick();
        const auto original = render->get_render_pipeline_id();
        ASSERT_TRUE(pipeline_alive(original));

        apply(*render, child);
        tick();
        apply(*render, child);
        tick();

        EXPECT_EQ(render->get_render_pipeline_id(), original);
        EXPECT_EQ(child->get_render_pipelines().size(), 1U);
    }

    INSTANTIATE_TEST_SUITE_P(Setters, RenderProcessorUnchangedSetterTest,
        ::testing::ValuesIn(unchanged_setter_cases()),
        [](const ::testing::TestParamInfo<UnchangedSetterCase>& info) { return std::string(info.param.name); });

    TEST_F(RenderProcessorPresentationTest, DetachingTheProcessorRemovesItsAssociationAndDestroyingItRetiresThePipeline)
    {
        prepare_draw(k_plain_vertex);
        tick();
        const auto id = render->get_render_pipeline_id();
        ASSERT_TRUE(pipeline_alive(id));

        chain->remove_processor_anywhere(render, child);
        EXPECT_TRUE(child->get_render_pipelines().empty());
        EXPECT_EQ(child->get_pipeline_command(id), Portal::Graphics::INVALID_COMMAND_BUFFER);

        render.reset();
        EXPECT_FALSE(pipeline_alive(id));
    }

}
}

#endif
