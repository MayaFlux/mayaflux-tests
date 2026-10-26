#include "gtest/gtest.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "../../support/vulkan_test_support.hpp"

#include "MayaFlux/Core/Backends/Graphics/Vulkan/VKGraphicsPipeline.hpp"
#include "MayaFlux/Core/Backends/Graphics/Vulkan/VKImage.hpp"
#include "MayaFlux/Portal/Graphics/RenderFlow.hpp"
#include "MayaFlux/Portal/Graphics/TextureLoom.hpp"

namespace MayaFlux::Test {
namespace {

    using Kakshya::ImageFormat;
    using Portal::Graphics::CullMode;
    using Portal::Graphics::DescriptorBindingInfo;
    using Portal::Graphics::INVALID_RENDER_PIPELINE;
    using Portal::Graphics::INVALID_SHADER;
    using Portal::Graphics::RenderFlow;
    using Portal::Graphics::RenderPipelineConfig;
    using Portal::Graphics::RenderPipelineID;
    using Portal::Graphics::ShaderID;
    using Portal::Graphics::ShaderStage;
    using Portal::Graphics::TextureLoom;

    constexpr vk::Format k_color_format = vk::Format::eR8G8B8A8Unorm;
    constexpr uint32_t k_extent = 8;

    constexpr std::string_view k_vertex_source = R"(#version 450
void main()
{
    vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";

    constexpr std::string_view k_position_vertex_source = R"(#version 450
layout(location = 0) in vec3 position;
void main()
{
    gl_Position = vec4(position, 1.0);
}
)";

    constexpr std::string_view k_transform_vertex_source = R"(#version 450
layout(set = 0, binding = 0) uniform Transform { vec4 offset; } transform;
void main()
{
    vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0) + transform.offset;
}
)";

    constexpr std::string_view k_fragment_source = R"(#version 450
layout(location = 0) out vec4 color;
layout(push_constant) uniform Params { vec4 value; } params;
void main() { color = params.value; }
)";

    constexpr std::string_view k_specialized_fragment_source = R"(#version 450
layout(constant_id = 3) const uint selected = 2u;
layout(location = 0) out vec4 color;
void main() { color = vec4(float(selected == 0u), float(selected == 1u), 0.0, 1.0); }
)";

    RenderFlow& flow()
    {
        return Portal::Graphics::get_render_flow();
    }

    Portal::Graphics::ShaderFoundry& foundry()
    {
        return Portal::Graphics::get_shader_foundry();
    }

    TextureLoom& loom()
    {
        return Portal::Graphics::get_texture_manager();
    }

    bool share_state(RenderPipelineID a, RenderPipelineID b)
    {
        const auto layout = flow().get_view_transform_layout(a);
        return layout && layout == flow().get_view_transform_layout(b);
    }

    class RenderFlowTest : public ::testing::Test {
    protected:
        static void SetUpTestSuite()
        {
            if (!vulkan_unavailable_reason().empty()) {
                return;
            }
            auto backend = std::make_shared<Core::VulkanBackend>();
            if (backend->initialize(headless_graphics_config())) {
                s_backend = std::move(backend);
            }
        }

        static void TearDownTestSuite()
        {
            s_backend.reset();
        }

        void SetUp() override
        {
            if (!s_backend) {
                GTEST_SKIP() << "No usable Vulkan device: " << vulkan_unavailable_reason();
            }
            teardown_portal();
            ASSERT_TRUE(foundry().initialize(s_backend));
            ASSERT_TRUE(flow().initialize());
        }

        void TearDown() override
        {
            teardown_portal();
        }

        static const std::shared_ptr<Core::VulkanBackend>& backend() { return s_backend; }

        static void teardown_portal()
        {
            flow().shutdown();
            loom().shutdown();
            foundry().shutdown();
        }

        static ShaderID vertex_shader() { return foundry().load_shader(std::string(k_vertex_source), ShaderStage::VERTEX); }
        static ShaderID fragment_shader() { return foundry().load_shader(std::string(k_fragment_source), ShaderStage::FRAGMENT); }

        static RenderPipelineConfig basic_config()
        {
            RenderPipelineConfig config;
            config.vertex_shader = vertex_shader();
            config.fragment_shader = fragment_shader();
            config.rasterization.cull_mode = CullMode::NONE;
            config.depth_stencil.depth_test_enable = false;
            config.depth_stencil.depth_write_enable = false;
            config.blend_attachments.emplace_back();
            config.push_constant_size = sizeof(float) * 4;
            return config;
        }

    private:
        static inline std::shared_ptr<Core::VulkanBackend> s_backend;
    };

    // ------------------------------------------------------------------
    // Lifecycle
    // ------------------------------------------------------------------

    TEST_F(RenderFlowTest, InitializeNeedsAnInitializedShaderFoundry)
    {
        flow().shutdown();
        foundry().shutdown();

        EXPECT_FALSE(flow().initialize());
        EXPECT_FALSE(flow().is_initialized());
    }

    TEST_F(RenderFlowTest, InitializingAgainReportsSuccess)
    {
        EXPECT_TRUE(flow().initialize());
        EXPECT_TRUE(flow().is_initialized());
    }

    TEST_F(RenderFlowTest, ShutdownIsRepeatableAndTheFlowInitializesAgain)
    {
        flow().shutdown();

        EXPECT_NO_THROW(flow().shutdown());
        EXPECT_FALSE(flow().is_initialized());
        EXPECT_TRUE(flow().initialize());
        EXPECT_NE(flow().create_pipeline(basic_config(), { k_color_format }), INVALID_RENDER_PIPELINE);
    }

    TEST_F(RenderFlowTest, UninitializedFlowCreatesNoPipelines)
    {
        const auto config = basic_config();
        flow().shutdown();

        EXPECT_EQ(flow().create_pipeline(config, { k_color_format }), INVALID_RENDER_PIPELINE);
    }

    // ------------------------------------------------------------------
    // Pipelines
    // ------------------------------------------------------------------

    TEST_F(RenderFlowTest, PipelineNeedsAVertexOrMeshShader)
    {
        RenderPipelineConfig config = basic_config();
        config.vertex_shader = INVALID_SHADER;

        EXPECT_EQ(flow().create_pipeline(config, { k_color_format }), INVALID_RENDER_PIPELINE);
    }

    TEST_F(RenderFlowTest, PipelineNeedsAtLeastOneColorFormat)
    {
        EXPECT_EQ(flow().create_pipeline(basic_config(), {}), INVALID_RENDER_PIPELINE);
    }

    TEST_F(RenderFlowTest, ValidConfigCreatesPipelinesWithDistinctIds)
    {
        const auto first = flow().create_pipeline(basic_config(), { k_color_format });
        const auto second = flow().create_pipeline(basic_config(), { k_color_format });

        EXPECT_NE(first, INVALID_RENDER_PIPELINE);
        EXPECT_NE(second, INVALID_RENDER_PIPELINE);
        EXPECT_NE(first, second);
    }

    // ------------------------------------------------------------------
    // State sharing
    // ------------------------------------------------------------------

    TEST_F(RenderFlowTest, IdenticalRequestsShareOneStateUnderDistinctIds)
    {
        const auto config = basic_config();
        const auto first = flow().create_pipeline(config, { k_color_format });
        const auto second = flow().create_pipeline(config, { k_color_format });
        const auto third = flow().create_pipeline(config, { k_color_format });
        ASSERT_NE(first, INVALID_RENDER_PIPELINE);
        ASSERT_NE(second, INVALID_RENDER_PIPELINE);
        ASSERT_NE(third, INVALID_RENDER_PIPELINE);

        EXPECT_NE(first, second);
        EXPECT_NE(second, third);
        EXPECT_TRUE(share_state(first, second));
        EXPECT_TRUE(share_state(second, third));
    }

    TEST_F(RenderFlowTest, DifferentAttachmentFormatsDoNotShare)
    {
        const auto config = basic_config();
        const auto base = flow().create_pipeline(config, { k_color_format });
        const auto other_color = flow().create_pipeline(config, { vk::Format::eB8G8R8A8Unorm });
        const auto with_depth = flow().create_pipeline(config, { k_color_format }, vk::Format::eD32Sfloat);
        ASSERT_NE(base, INVALID_RENDER_PIPELINE);
        ASSERT_NE(other_color, INVALID_RENDER_PIPELINE);
        ASSERT_NE(with_depth, INVALID_RENDER_PIPELINE);

        EXPECT_FALSE(share_state(base, other_color));
        EXPECT_FALSE(share_state(base, with_depth));
        EXPECT_FALSE(share_state(other_color, with_depth));
    }

    TEST_F(RenderFlowTest, FixedFunctionStateSplitsSharing)
    {
        const auto config = basic_config();
        const auto base = flow().create_pipeline(config, { k_color_format });

        RenderPipelineConfig culled = config;
        culled.rasterization.cull_mode = CullMode::BACK;
        const auto culled_id = flow().create_pipeline(culled, { k_color_format });

        RenderPipelineConfig blended = config;
        blended.blend_attachments[0].blend_enable = true;
        const auto blended_id = flow().create_pipeline(blended, { k_color_format });

        RenderPipelineConfig depth_tested = config;
        depth_tested.depth_stencil.depth_test_enable = true;
        const auto depth_tested_id = flow().create_pipeline(depth_tested, { k_color_format });

        for (const auto id : { base, culled_id, blended_id, depth_tested_id }) {
            ASSERT_NE(id, INVALID_RENDER_PIPELINE);
        }
        EXPECT_FALSE(share_state(base, culled_id));
        EXPECT_FALSE(share_state(base, blended_id));
        EXPECT_FALSE(share_state(base, depth_tested_id));
        EXPECT_FALSE(share_state(culled_id, blended_id));
    }

    TEST_F(RenderFlowTest, PushSetAndSpecializationSplitSharing)
    {
        const auto config = basic_config();
        const auto pooled = flow().create_pipeline(config, { k_color_format });
        const auto pushed = flow().create_pipeline(config, { k_color_format }, vk::Format::eUndefined, 0U);
        ASSERT_NE(pooled, INVALID_RENDER_PIPELINE);
        ASSERT_NE(pushed, INVALID_RENDER_PIPELINE);
        EXPECT_FALSE(share_state(pooled, pushed));

        RenderPipelineConfig specialized = config;
        specialized.fragment_shader = foundry().load_shader(
            std::string(k_specialized_fragment_source), ShaderStage::FRAGMENT);
        specialized.push_constant_size = 0;
        ASSERT_NE(specialized.fragment_shader, INVALID_SHADER);

        const auto red = flow().create_pipeline(specialized, { k_color_format },
            vk::Format::eUndefined, std::nullopt, { { 3U, 0U } });
        const auto green = flow().create_pipeline(specialized, { k_color_format },
            vk::Format::eUndefined, std::nullopt, { { 3U, 1U } });
        const auto green_again = flow().create_pipeline(specialized, { k_color_format },
            vk::Format::eUndefined, std::nullopt, { { 3U, 1U } });
        ASSERT_NE(red, INVALID_RENDER_PIPELINE);
        ASSERT_NE(green, INVALID_RENDER_PIPELINE);
        ASSERT_NE(green_again, INVALID_RENDER_PIPELINE);

        EXPECT_FALSE(share_state(red, green));
        EXPECT_TRUE(share_state(green, green_again));
    }

    TEST_F(RenderFlowTest, DescriptorLayoutDecidesSharingNotBindingNames)
    {
        const auto config = basic_config();

        RenderPipelineConfig with_set = config;
        with_set.descriptor_sets = { { DescriptorBindingInfo { .set = 1, .binding = 0, .type = vk::DescriptorType::eUniformBuffer, .name = "first" } } };

        RenderPipelineConfig renamed = config;
        renamed.descriptor_sets = { { DescriptorBindingInfo { .set = 1, .binding = 0, .type = vk::DescriptorType::eUniformBuffer, .name = "second" } } };

        RenderPipelineConfig storage = config;
        storage.descriptor_sets = { { DescriptorBindingInfo { .set = 1, .binding = 0, .type = vk::DescriptorType::eStorageBuffer, .name = "first" } } };

        const auto none = flow().create_pipeline(config, { k_color_format });
        const auto uniform = flow().create_pipeline(with_set, { k_color_format });
        const auto uniform_renamed = flow().create_pipeline(renamed, { k_color_format });
        const auto storage_id = flow().create_pipeline(storage, { k_color_format });
        for (const auto id : { none, uniform, uniform_renamed, storage_id }) {
            ASSERT_NE(id, INVALID_RENDER_PIPELINE);
        }

        EXPECT_TRUE(share_state(uniform, uniform_renamed));
        EXPECT_FALSE(share_state(none, uniform));
        EXPECT_FALSE(share_state(uniform, storage_id));
    }

    TEST_F(RenderFlowTest, VertexInputShapeDecidesSharingNotVertexCount)
    {
        RenderPipelineConfig config = basic_config();
        config.vertex_shader = foundry().load_shader(
            std::string(k_position_vertex_source), ShaderStage::VERTEX);
        ASSERT_NE(config.vertex_shader, INVALID_SHADER);

        const auto position_layout = [](uint32_t stride, uint32_t vertex_count) {
            Kakshya::VertexLayout layout;
            layout.stride_bytes = stride;
            layout.vertex_count = vertex_count;
            layout.attributes.push_back({ .component_modality = Kakshya::DataModality::VERTEX_POSITIONS_3D,
                .offset_in_vertex = 0,
                .name = "position" });
            return layout;
        };

        const auto few = position_layout(12, 10);
        const auto many = position_layout(12, 5000);
        const auto wider = position_layout(16, 10);

        RenderPipelineConfig few_config = config;
        few_config.semantic_vertex_layout = few;
        RenderPipelineConfig many_config = config;
        many_config.semantic_vertex_layout = many;
        RenderPipelineConfig wider_config = config;
        wider_config.semantic_vertex_layout = wider;

        const auto few_id = flow().create_pipeline(few_config, { k_color_format });
        const auto many_id = flow().create_pipeline(many_config, { k_color_format });
        const auto wider_id = flow().create_pipeline(wider_config, { k_color_format });
        for (const auto id : { few_id, many_id, wider_id }) {
            ASSERT_NE(id, INVALID_RENDER_PIPELINE);
        }

        EXPECT_TRUE(share_state(few_id, many_id));
        EXPECT_FALSE(share_state(few_id, wider_id));
    }

    TEST_F(RenderFlowTest, ASharedStateOutlivesTheIdThatCreatedIt)
    {
        const auto config = basic_config();
        const auto creator = flow().create_pipeline(config, { k_color_format });
        const auto sharer = flow().create_pipeline(config, { k_color_format });
        ASSERT_NE(creator, INVALID_RENDER_PIPELINE);
        ASSERT_TRUE(share_state(creator, sharer));
        const auto layout = flow().get_view_transform_layout(sharer);

        flow().destroy_pipeline(creator);

        EXPECT_FALSE(flow().get_view_transform_layout(creator));
        EXPECT_EQ(flow().get_view_transform_layout(sharer), layout);

        const auto latecomer = flow().create_pipeline(config, { k_color_format });
        ASSERT_NE(latecomer, INVALID_RENDER_PIPELINE);
        EXPECT_TRUE(share_state(sharer, latecomer));
    }

    TEST_F(RenderFlowTest, TheLastIdReleasesTheStateAndALaterRequestBuildsAnother)
    {
        const auto config = basic_config();
        const auto first = flow().create_pipeline(config, { k_color_format });
        const auto second = flow().create_pipeline(config, { k_color_format });
        ASSERT_TRUE(share_state(first, second));

        flow().destroy_pipeline(first);
        flow().destroy_pipeline(second);
        EXPECT_FALSE(flow().get_view_transform_layout(first));
        EXPECT_FALSE(flow().get_view_transform_layout(second));

        const auto rebuilt = flow().create_pipeline(config, { k_color_format });
        ASSERT_NE(rebuilt, INVALID_RENDER_PIPELINE);
        EXPECT_TRUE(flow().get_view_transform_layout(rebuilt));
        EXPECT_FALSE(flow().allocate_pipeline_descriptors(rebuilt).empty());
    }

    TEST_F(RenderFlowTest, ShutdownReleasesSharedStatesAndTheFlowStartsClean)
    {
        const auto config = basic_config();
        const auto first = flow().create_pipeline(config, { k_color_format });
        const auto second = flow().create_pipeline(config, { k_color_format });
        ASSERT_TRUE(share_state(first, second));

        EXPECT_NO_THROW(flow().shutdown());
        ASSERT_TRUE(flow().initialize());

        EXPECT_FALSE(flow().get_view_transform_layout(first));
        EXPECT_FALSE(flow().get_view_transform_layout(second));
        const auto fresh = flow().create_pipeline(basic_config(), { k_color_format });
        EXPECT_NE(fresh, INVALID_RENDER_PIPELINE);
        EXPECT_TRUE(flow().get_view_transform_layout(fresh));
    }

    TEST_F(RenderFlowTest, SpecializationValuesProduceDistinctResultsFromOneShaderModule)
    {
        ASSERT_TRUE(loom().initialize(backend()));
        RenderPipelineConfig config = basic_config();
        config.fragment_shader = foundry().load_shader(
            std::string(k_specialized_fragment_source), ShaderStage::FRAGMENT);
        config.push_constant_size = 0;
        ASSERT_NE(config.fragment_shader, INVALID_SHADER);

        const auto red = flow().create_pipeline(config, { k_color_format },
            vk::Format::eUndefined, std::nullopt, { { 3U, 0U } });
        const auto green = flow().create_pipeline(config, { k_color_format },
            vk::Format::eUndefined, std::nullopt, { { 3U, 1U } });
        ASSERT_NE(red, INVALID_RENDER_PIPELINE);
        ASSERT_NE(green, INVALID_RENDER_PIPELINE);

        const auto draw_and_check = [&](auto pipeline, const std::array<uint8_t, 4>& expected) {
            const auto target = loom().create_render_target(k_extent, k_extent, ImageFormat::RGBA8);
            ASSERT_NE(target, nullptr);
            const auto id = foundry().begin_commands(Portal::Graphics::ShaderFoundry::CommandBufferType::GRAPHICS);
            auto cmd = foundry().get_command_buffer(id);

            vk::RenderingAttachmentInfo attachment {};
            attachment.imageView = target->get_image_view();
            attachment.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
            attachment.loadOp = vk::AttachmentLoadOp::eClear;
            attachment.storeOp = vk::AttachmentStoreOp::eStore;
            attachment.clearValue.color = vk::ClearColorValue { std::array<float, 4> { 0.0F, 0.0F, 0.0F, 0.0F } };

            vk::RenderingInfo info {};
            info.renderArea = vk::Rect2D { { 0, 0 }, { k_extent, k_extent } };
            info.layerCount = 1;
            info.colorAttachmentCount = 1;
            info.pColorAttachments = &attachment;

            cmd.beginRendering(info);
            flow().bind_pipeline(id, pipeline);
            cmd.setViewport(0, vk::Viewport { 0.0F, 0.0F, static_cast<float>(k_extent), static_cast<float>(k_extent), 0.0F, 1.0F });
            cmd.setScissor(0, vk::Rect2D { { 0, 0 }, { k_extent, k_extent } });
            flow().draw(id, 3);
            cmd.endRendering();
            foundry().submit_and_wait(id);

            const auto downloaded = loom().download_image(target);
            ASSERT_TRUE(downloaded.has_value());
            ASSERT_EQ(downloaded->byte_size(), static_cast<size_t>(k_extent) * k_extent * sizeof(uint32_t));
            const auto* pixels = static_cast<const uint8_t*>(downloaded->data());
            for (size_t i = 0; i < static_cast<size_t>(k_extent) * k_extent; ++i) {
                const std::array<uint8_t, 4> actual {
                    pixels[i * 4], pixels[i * 4 + 1], pixels[i * 4 + 2], pixels[i * 4 + 3]
                };
                ASSERT_EQ(actual, expected) << "pixel " << i;
            }
        };

        draw_and_check(red, { 255, 0, 0, 255 });
        draw_and_check(green, { 0, 255, 0, 255 });
    }

    TEST_F(RenderFlowTest, ViewTransformLayoutExistsForAPipelineAndNotForAnUnknownId)
    {
        const auto id = flow().create_pipeline(basic_config(), { k_color_format });
        ASSERT_NE(id, INVALID_RENDER_PIPELINE);

        EXPECT_TRUE(flow().get_view_transform_layout(id));
        EXPECT_FALSE(flow().get_view_transform_layout(9999));
    }

    TEST_F(RenderFlowTest, DestroyPipelineForgetsItAndIgnoresUnknownIds)
    {
        const auto id = flow().create_pipeline(basic_config(), { k_color_format });
        ASSERT_NE(id, INVALID_RENDER_PIPELINE);

        flow().destroy_pipeline(id);

        EXPECT_FALSE(flow().get_view_transform_layout(id));
        EXPECT_NO_THROW(flow().destroy_pipeline(id));
        EXPECT_NO_THROW(flow().destroy_pipeline(9999));
    }

    // ------------------------------------------------------------------
    // Descriptors
    // ------------------------------------------------------------------

    TEST_F(RenderFlowTest, ADescriptorSetIsAllocatedForTheReservedViewTransformLayout)
    {
        const auto id = flow().create_pipeline(basic_config(), { k_color_format });
        ASSERT_NE(id, INVALID_RENDER_PIPELINE);

        const auto sets = flow().allocate_pipeline_descriptors(id);

        ASSERT_EQ(sets.size(), 1U);
        EXPECT_TRUE(foundry().get_descriptor_set(sets[0]));
    }

    TEST_F(RenderFlowTest, EachExtraDescriptorSetAddsALayoutAndFirstIndexSkipsLayouts)
    {
        RenderPipelineConfig config = basic_config();
        config.descriptor_sets = { { DescriptorBindingInfo { .set = 1, .binding = 0, .type = vk::DescriptorType::eUniformBuffer } } };
        const auto id = flow().create_pipeline(config, { k_color_format });
        ASSERT_NE(id, INVALID_RENDER_PIPELINE);

        EXPECT_EQ(flow().allocate_pipeline_descriptors(id).size(), 2U);
        EXPECT_EQ(flow().allocate_pipeline_descriptors(id, 1).size(), 1U);
        EXPECT_TRUE(flow().allocate_pipeline_descriptors(id, 2).empty());
    }

    TEST_F(RenderFlowTest, AllocatingForAnUnknownPipelineGivesNothing)
    {
        EXPECT_TRUE(flow().allocate_pipeline_descriptors(9999).empty());
    }

    TEST_F(RenderFlowTest, APushDescriptorSetGetsNoPooledSetAndTheOthersStillAllocate)
    {
        const auto only_push = flow().create_pipeline(basic_config(), { k_color_format }, vk::Format::eUndefined, 0U);
        ASSERT_NE(only_push, INVALID_RENDER_PIPELINE);
        EXPECT_TRUE(flow().allocate_pipeline_descriptors(only_push).empty());

        RenderPipelineConfig config = basic_config();
        config.descriptor_sets = { { DescriptorBindingInfo { .set = 1, .binding = 0, .type = vk::DescriptorType::eUniformBuffer } } };
        const auto pooled = flow().create_pipeline(config, { k_color_format });
        const auto with_push = flow().create_pipeline(config, { k_color_format }, vk::Format::eUndefined, 0U);
        ASSERT_NE(pooled, INVALID_RENDER_PIPELINE);
        ASSERT_NE(with_push, INVALID_RENDER_PIPELINE);

        EXPECT_EQ(flow().allocate_pipeline_descriptors(pooled).size(), 2U);
        EXPECT_EQ(flow().allocate_pipeline_descriptors(with_push).size(), 1U);
    }

    TEST_F(RenderFlowTest, APushDescriptorSetBeyondTheDescriptorSetsIsRefused)
    {
        EXPECT_EQ(flow().create_pipeline(basic_config(), { k_color_format }, vk::Format::eUndefined, 5U),
            INVALID_RENDER_PIPELINE);
        EXPECT_NE(flow().create_pipeline(basic_config(), { k_color_format }, vk::Format::eUndefined, 0U),
            INVALID_RENDER_PIPELINE);
    }

    // ------------------------------------------------------------------
    // Recording
    // ------------------------------------------------------------------

    TEST_F(RenderFlowTest, RecordingWithUnknownIdsIsIgnored)
    {
        const auto pipeline = flow().create_pipeline(basic_config(), { k_color_format });
        ASSERT_NE(pipeline, INVALID_RENDER_PIPELINE);
        const auto cmd = foundry().begin_commands(Portal::Graphics::ShaderFoundry::CommandBufferType::GRAPHICS);
        const std::array<float, 4> color { 1.0F, 0.0F, 0.0F, 1.0F };

        EXPECT_NO_THROW(flow().bind_pipeline(cmd, 9999));
        EXPECT_NO_THROW(flow().bind_pipeline(9999, pipeline));
        EXPECT_NO_THROW(flow().bind_descriptor_sets(cmd, 9999, {}));
        EXPECT_NO_THROW(flow().push_constants(cmd, 9999, color.data(), sizeof(color)));
        EXPECT_NO_THROW(flow().push_constants(9999, pipeline, color.data(), sizeof(color)));
        EXPECT_NO_THROW(flow().bind_vertex_buffers(9999, {}));
        EXPECT_NO_THROW(flow().bind_index_buffer(9999, nullptr));
        EXPECT_NO_THROW(flow().draw(9999, 3));
        EXPECT_NO_THROW(flow().draw_indexed(9999, 3));
        EXPECT_NO_THROW(flow().draw_mesh_tasks(9999, 1, 1, 1));
        EXPECT_NO_THROW(flow().draw_mesh_tasks_indirect(9999, nullptr, 0, 1));

        foundry().submit_and_wait(cmd);
    }

    TEST_F(RenderFlowTest, PushingDescriptorsIsIgnoredWithoutAPushSetOrWithUnknownIds)
    {
        const auto plain = flow().create_pipeline(basic_config(), { k_color_format });
        const auto pushable = flow().create_pipeline(basic_config(), { k_color_format }, vk::Format::eUndefined, 0U);
        ASSERT_NE(plain, INVALID_RENDER_PIPELINE);
        ASSERT_NE(pushable, INVALID_RENDER_PIPELINE);
        const auto cmd = foundry().begin_commands(Portal::Graphics::ShaderFoundry::CommandBufferType::GRAPHICS);
        const std::vector<vk::WriteDescriptorSet> writes(1);

        EXPECT_NO_THROW(flow().push_descriptors(cmd, plain, writes));
        EXPECT_NO_THROW(flow().push_descriptors(cmd, 9999, writes));
        EXPECT_NO_THROW(flow().push_descriptors(9999, pushable, writes));

        foundry().submit_and_wait(cmd);
    }

    TEST_F(RenderFlowTest, TheFirstUnwrittenPushBindingNamesAUsedBindingNobodyWrites)
    {
        RenderPipelineConfig config = basic_config();
        config.vertex_shader = foundry().load_shader(std::string(k_transform_vertex_source), ShaderStage::VERTEX);
        const auto pushable = flow().create_pipeline(config, { k_color_format }, vk::Format::eUndefined, 0U);
        const auto plain = flow().create_pipeline(config, { k_color_format });
        ASSERT_NE(pushable, INVALID_RENDER_PIPELINE);
        ASSERT_NE(plain, INVALID_RENDER_PIPELINE);

        std::vector<vk::WriteDescriptorSet> writes;
        EXPECT_EQ(flow().first_unwritten_push_binding(pushable, writes), std::optional<uint32_t> { 0U });

        vk::WriteDescriptorSet transform {};
        transform.dstBinding = 0;
        writes.push_back(transform);
        EXPECT_FALSE(flow().first_unwritten_push_binding(pushable, writes).has_value());

        EXPECT_FALSE(flow().first_unwritten_push_binding(plain, {}).has_value());
        EXPECT_FALSE(flow().first_unwritten_push_binding(9999, {}).has_value());
    }

    TEST_F(RenderFlowTest, APushPipelineWhoseShadersUseNoPushBindingsHasNothingUnwritten)
    {
        const auto pipeline = flow().create_pipeline(basic_config(), { k_color_format }, vk::Format::eUndefined, 0U);
        ASSERT_NE(pipeline, INVALID_RENDER_PIPELINE);

        EXPECT_FALSE(flow().first_unwritten_push_binding(pipeline, {}).has_value());
    }

    TEST_F(RenderFlowTest, BindPushAndDrawFillATargetWithThePushedColor)
    {
        ASSERT_TRUE(loom().initialize(backend()));
        const auto target = loom().create_render_target(k_extent, k_extent, ImageFormat::RGBA8);
        ASSERT_NE(target, nullptr);
        const auto pipeline = flow().create_pipeline(basic_config(), { k_color_format });
        ASSERT_NE(pipeline, INVALID_RENDER_PIPELINE);
        const std::array<float, 4> green { 0.0F, 1.0F, 0.0F, 1.0F };

        const auto id = foundry().begin_commands(Portal::Graphics::ShaderFoundry::CommandBufferType::GRAPHICS);
        auto cmd = foundry().get_command_buffer(id);

        vk::RenderingAttachmentInfo attachment {};
        attachment.imageView = target->get_image_view();
        attachment.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
        attachment.loadOp = vk::AttachmentLoadOp::eClear;
        attachment.storeOp = vk::AttachmentStoreOp::eStore;
        attachment.clearValue.color = vk::ClearColorValue { std::array<float, 4> { 0.0F, 0.0F, 0.0F, 0.0F } };

        vk::RenderingInfo info {};
        info.renderArea = vk::Rect2D { { 0, 0 }, { k_extent, k_extent } };
        info.layerCount = 1;
        info.colorAttachmentCount = 1;
        info.pColorAttachments = &attachment;

        cmd.beginRendering(info);
        flow().bind_pipeline(id, pipeline);
        cmd.setViewport(0, vk::Viewport { 0.0F, 0.0F, static_cast<float>(k_extent), static_cast<float>(k_extent), 0.0F, 1.0F });
        cmd.setScissor(0, vk::Rect2D { { 0, 0 }, { k_extent, k_extent } });
        flow().push_constants(id, pipeline, green.data(), sizeof(green));
        flow().draw(id, 3);
        cmd.endRendering();
        foundry().submit_and_wait(id);

        const auto downloaded = loom().download_image(target);
        ASSERT_TRUE(downloaded.has_value());
        ASSERT_EQ(downloaded->byte_size(), static_cast<size_t>(k_extent) * k_extent * sizeof(uint32_t));
        std::vector<uint32_t> pixels(static_cast<size_t>(k_extent) * k_extent);
        std::memcpy(pixels.data(), downloaded->data(), downloaded->byte_size());
        EXPECT_EQ(pixels, std::vector<uint32_t>(pixels.size(), 0xFF00FF00U));
    }

    // ------------------------------------------------------------------
    // Window registration without a window
    // ------------------------------------------------------------------

    TEST_F(RenderFlowTest, NullWindowsAreNeverRegistered)
    {
        EXPECT_NO_THROW(flow().register_window_for_rendering(nullptr));
        EXPECT_NO_THROW(flow().unregister_window(nullptr));
        EXPECT_FALSE(flow().is_window_registered(nullptr));
        EXPECT_TRUE(flow().get_registered_windows().empty());
    }

} // namespace
} // namespace MayaFlux::Test
