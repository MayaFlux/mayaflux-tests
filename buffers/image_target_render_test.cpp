#include "gtest/gtest.h"
#include "../support/test_metadata.hpp"

#include "../support/vulkan_test_support.hpp"

#include "MayaFlux/Buffers/BufferProcessingChain.hpp"
#include "MayaFlux/Buffers/Geometry/MeshBuffer.hpp"
#include "MayaFlux/Buffers/Root/RootGraphicsBuffer.hpp"
#include "MayaFlux/Buffers/Shaders/RenderProcessor.hpp"
#include "MayaFlux/Buffers/VKBuffer.hpp"
#include "MayaFlux/Core/Backends/Graphics/Vulkan/VKImage.hpp"
#include "MayaFlux/Kakshya/NDData/VertexLayout.hpp"
#include "MayaFlux/Portal/Graphics/ImageTarget.hpp"
#include "MayaFlux/Portal/Graphics/RenderFlow.hpp"
#include "MayaFlux/Portal/Graphics/SamplerForge.hpp"
#include "MayaFlux/Portal/Graphics/ShaderFoundry.hpp"
#include "MayaFlux/Portal/Graphics/TextureLoom.hpp"
#include "MayaFlux/Registry/BackendRegistry.hpp"
#include "MayaFlux/Registry/Service/BufferService.hpp"
#include "MayaFlux/Registry/Service/DisplayService.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::vulkan>() }))

namespace MayaFlux::Test {
namespace {

    using Buffers::ProcessingToken;
    using Buffers::RenderProcessor;
    using Buffers::RootGraphicsBuffer;
    using Buffers::ShaderConfig;
    using Buffers::VKBuffer;
    using Portal::Graphics::ImageTarget;

    constexpr const char* k_full_vertex = R"(#version 460
void main() {
    vec2 p[3] = vec2[3](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));
    gl_Position = vec4(p[gl_VertexIndex], 0.0, 1.0);
}
)";

    constexpr const char* k_red_fragment = R"(#version 460
layout(location = 0) out vec4 color;
void main() { color = vec4(1.0, 0.0, 0.0, 1.0); }
)";

    constexpr const char* k_green_fragment = R"(#version 460
layout(location = 0) out vec4 color;
void main() { color = vec4(0.0, 1.0, 0.0, 1.0); }
)";

    /** Red or green on the left half of a 32 pixel wide target, discarded on the right. */
    constexpr const char* k_left_red_fragment = R"(#version 460
layout(location = 0) out vec4 color;
void main() { if (gl_FragCoord.x > 16.0) discard; color = vec4(1.0, 0.0, 0.0, 1.0); }
)";

    constexpr const char* k_left_green_fragment = R"(#version 460
layout(location = 0) out vec4 color;
void main() { if (gl_FragCoord.x > 16.0) discard; color = vec4(0.0, 1.0, 0.0, 1.0); }
)";

    constexpr const char* k_right_green_fragment = R"(#version 460
layout(location = 0) out vec4 color;
void main() { if (gl_FragCoord.x < 16.0) discard; color = vec4(0.0, 1.0, 0.0, 1.0); }
)";

    constexpr const char* k_two_outputs_fragment = R"(#version 460
layout(location = 0) out vec4 first;
layout(location = 1) out vec4 second;
void main() { first = vec4(1.0, 0.0, 0.0, 1.0); second = vec4(0.0, 1.0, 0.0, 1.0); }
)";

    constexpr const char* k_half_depth_vertex = R"(#version 460
void main() {
    vec2 p[3] = vec2[3](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));
    gl_Position = vec4(p[gl_VertexIndex], 0.5, 1.0);
}
)";

    constexpr const char* k_sample_red_channel_fragment = R"(#version 460
layout(set = 1, binding = 0) uniform sampler2D tex;
layout(location = 0) out vec4 color;
void main() { color = vec4(texture(tex, vec2(0.5)).r, 0.0, 0.0, 1.0); }
)";

    /** Covers the target and hands the view matrix's x and y translation on as color. */
    constexpr const char* k_view_probe_vertex = R"(#version 460
layout(set = 0, binding = 0) uniform RenderTransformBlock { mat4 view; mat4 projection; mat4 geometry; } vt;
layout(location = 0) out vec3 probe;
void main() {
    vec2 p[3] = vec2[3](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));
    probe = vec3(vt.view[3][0], vt.view[3][1], 0.0);
    gl_Position = vec4(p[gl_VertexIndex], 0.0, 1.0);
}
)";

    constexpr const char* k_view_probe_fragment = R"(#version 460
layout(location = 0) in vec3 probe;
layout(location = 0) out vec4 color;
void main() { color = vec4(probe, 1.0); }
)";

    Kinesis::ViewTransform view_showing(float value)
    {
        Kinesis::ViewTransform transform;
        transform.view[3][0] = value;
        return transform;
    }

    constexpr const char* k_sample_lod_fragment = R"(#version 460
layout(set = 1, binding = 0) uniform sampler2D tex;
layout(location = 0) out vec4 color;
void main() { color = textureLod(tex, vec2(0.5), 2.0); }
)";

    constexpr const char* k_sample_fragment = R"(#version 460
layout(set = 1, binding = 0) uniform sampler2D tex;
layout(location = 0) out vec4 color;
void main() { color = texture(tex, vec2(0.5)); }
)";

    struct Pixel {
        uint8_t r {}, g {}, b {}, a {};
    };

    class ConfigProbeBuffer : public VKBuffer {
    public:
        ConfigProbeBuffer()
            : VKBuffer(64, VKBuffer::Usage::VERTEX, Kakshya::DataModality::VERTEX_POSITIONS_3D)
        {
        }

        std::shared_ptr<RenderProcessor> apply(const Portal::Graphics::RenderConfig& config)
        {
            ShaderConfig shader { k_full_vertex };
            shader.stage = Portal::Graphics::ShaderStage::VERTEX;
            std::shared_ptr<RenderProcessor> processor;
            apply_render_config(processor, config, shader);
            return processor;
        }
    };

    /** One drawable: a buffer and the processor that draws it. */
    struct Drawable {
        std::shared_ptr<VKBuffer> buffer;
        std::shared_ptr<RenderProcessor> render;
    };

    class ImageTargetRenderTest : public VulkanDeviceTest {
    protected:
        void SetUp() override
        {
            VulkanDeviceTest::SetUp();
            if (IsSkipped())
                return;

            auto borrowed = std::shared_ptr<Core::VulkanBackend>(&backend(), [](Core::VulkanBackend*) { });
            Portal::Graphics::get_render_flow().shutdown();
            Portal::Graphics::get_shader_foundry().shutdown();
            Portal::Graphics::get_texture_manager().shutdown();
            Portal::Graphics::SamplerForge::instance().shutdown();
            ASSERT_TRUE(Portal::Graphics::get_shader_foundry().initialize(borrowed));
            ASSERT_TRUE(Portal::Graphics::get_render_flow().initialize());
            ASSERT_TRUE(Portal::Graphics::get_texture_manager().initialize(borrowed));
            ASSERT_TRUE(Portal::Graphics::SamplerForge::instance().initialize(borrowed));
            ready = true;

            root = std::make_shared<RootGraphicsBuffer>();
            root->initialize();
        }

        void TearDown() override
        {
            auto* service = Registry::BackendRegistry::instance()
                                .get_service<Registry::Service::BufferService>();
            for (auto& drawable : drawables) {
                if (drawable.buffer && drawable.render) {
                    drawable.buffer->get_processing_chain()->remove_processor_anywhere(drawable.render, drawable.buffer);
                }
                if (service && drawable.buffer && drawable.buffer->is_initialized()) {
                    service->destroy_buffer(drawable.buffer);
                }
            }
            drawables.clear();
            root.reset();
            targets.clear();

            if (ready) {
                Portal::Graphics::SamplerForge::instance().shutdown();
                Portal::Graphics::get_texture_manager().shutdown();
                Portal::Graphics::get_render_flow().shutdown();
                Portal::Graphics::get_shader_foundry().shutdown();
            }
            VulkanDeviceTest::TearDown();
        }

        static Portal::Graphics::TextureLoom& loom() { return Portal::Graphics::get_texture_manager(); }

        static bool pipeline_alive(Portal::Graphics::RenderPipelineID id)
        {
            return static_cast<bool>(Portal::Graphics::get_render_flow().get_view_transform_layout(id));
        }

        /** The pipeline a buffer draws into one target with. */
        static Portal::Graphics::RenderPipelineID pipeline_for(const Drawable& drawable, const std::shared_ptr<ImageTarget>& target)
        {
            for (const auto& [id, held] : drawable.buffer->get_image_pipelines()) {
                if (held == target)
                    return id;
            }
            return Portal::Graphics::INVALID_RENDER_PIPELINE;
        }

        std::shared_ptr<ImageTarget> make_target(ImageTarget::Config config = { .width = 32, .height = 32 })
        {
            auto target = loom().create_image_target(config);
            targets.push_back(target);
            return target;
        }

        /** A drawable of one triangle into @p target, with its own fragment shader. */
        Drawable& add_drawable(
            const std::shared_ptr<ImageTarget>& target,
            const char* vertex = k_full_vertex,
            const char* fragment = k_red_fragment,
            bool depth = false)
        {
            auto buffer = std::make_shared<VKBuffer>(64, VKBuffer::Usage::VERTEX,
                Kakshya::DataModality::VERTEX_POSITIONS_3D);
            auto* service = Registry::BackendRegistry::instance()
                                .get_service<Registry::Service::BufferService>();
            service->initialize_buffer(buffer);
            buffer->set_vertex_layout(Kakshya::VertexLayout {});

            ShaderConfig shader { vertex };
            shader.stage = Portal::Graphics::ShaderStage::VERTEX;
            auto render = std::make_shared<RenderProcessor>(shader);
            render->set_fragment_shader(fragment);
            render->set_target_image(target, buffer);
            render->set_primitive_topology(Portal::Graphics::PrimitiveTopology::TRIANGLE_LIST);
            if (!depth)
                render->disable_depth_test();
            render->set_vertex_range(0, 3);

            auto chain = buffer->get_processing_chain();
            chain->set_preferred_token(ProcessingToken::GRAPHICS_BACKEND);
            chain->add_final_processor(render, buffer);
            root->add_child_buffer(buffer);

            drawables.push_back({ buffer, render });
            return drawables.back();
        }

        void tick()
        {
            root->process_default();
        }

        void present()
        {
            Buffers::PresentProcessor presenter;
            presenter.processing_function(root);
        }

        void cycle()
        {
            tick();
            present();
        }

        Pixel pixel(const std::shared_ptr<ImageTarget>& target, uint32_t x, uint32_t y)
        {
            std::vector<uint8_t> bytes(static_cast<size_t>(target->width()) * target->height() * 4);
            loom().download_data(target->image(), bytes.data(), bytes.size());
            const size_t at = (static_cast<size_t>(y) * target->width() + x) * 4;
            return { bytes[at], bytes[at + 1], bytes[at + 2], bytes[at + 3] };
        }

        /** A pixel of one layer of any 8 bit, 4 channel image, read back whole. */
        Pixel texel(const std::shared_ptr<Core::VKImage>& image, uint32_t x, uint32_t y, uint32_t layer = 0)
        {
            const size_t layer_bytes = static_cast<size_t>(image->get_width()) * image->get_height() * 4;
            std::vector<uint8_t> bytes(layer_bytes * image->get_array_layers());
            loom().download_data(image, bytes.data(), bytes.size());
            const size_t at = layer * layer_bytes + (static_cast<size_t>(y) * image->get_width() + x) * 4;
            return { bytes[at], bytes[at + 1], bytes[at + 2], bytes[at + 3] };
        }

        std::shared_ptr<RootGraphicsBuffer> root;
        std::vector<Drawable> drawables;
        std::vector<std::shared_ptr<ImageTarget>> targets;
        bool ready {};
    };

    // =========================================================================
    // Pixels
    // =========================================================================

    TEST_F(ImageTargetRenderTest, ADrawLandsInTheImageAndTheClearColorSurvivesWhereNothingCovers)
    {
        auto target = make_target({ .width = 32, .height = 32, .clear_color = { 0.0F, 0.0F, 1.0F, 1.0F } });
        ASSERT_NE(target, nullptr);
        add_drawable(target, k_full_vertex, k_left_red_fragment);

        cycle();

        const auto covered = pixel(target, 4, 16);
        EXPECT_EQ(covered.r, 255);
        EXPECT_EQ(covered.b, 0);

        const auto clear = pixel(target, 28, 16);
        EXPECT_EQ(clear.r, 0);
        EXPECT_EQ(clear.b, 255);
    }

    TEST_F(ImageTargetRenderTest, TheImageIsSampleableAfterTheDraw)
    {
        auto source = make_target();
        add_drawable(source);
        cycle();
        ASSERT_EQ(pixel(source, 16, 16).r, 255);

        auto sink = make_target();
        auto& consumer = add_drawable(sink, k_full_vertex, k_sample_fragment);
        consumer.render->add_binding("tex", Buffers::ShaderBinding { 1, 0, vk::DescriptorType::eCombinedImageSampler });
        consumer.render->bind_texture({ .name = "tex", .image = source->image() });

        source->request_redraw();
        sink->request_redraw();
        cycle();

        EXPECT_EQ(pixel(sink, 16, 16).r, 255);
    }

    TEST_F(ImageTargetRenderTest, ATargetThatSamplesAnotherDrawsAfterItWhicheverOrderTheyWereAdded)
    {
        auto source = make_target();
        auto sink = make_target();

        auto& consumer = add_drawable(sink, k_full_vertex, k_sample_fragment);
        consumer.render->add_binding("tex", Buffers::ShaderBinding { 1, 0, vk::DescriptorType::eCombinedImageSampler });
        consumer.render->bind_texture({ .name = "tex", .image = source->image() });
        add_drawable(source);

        cycle();

        EXPECT_EQ(pixel(sink, 16, 16).r, 255);
    }

    TEST_F(ImageTargetRenderTest, TwoBuffersInOneTargetShareOneScope)
    {
        auto target = make_target();
        add_drawable(target, k_full_vertex, k_left_red_fragment);
        add_drawable(target, k_full_vertex, k_left_green_fragment);

        tick();

        ASSERT_EQ(root->get_renderable_buffers().size(), 2U);
        for (const auto& info : root->get_renderable_buffers()) {
            EXPECT_EQ(info.target_image, target);
        }
        present();

        const auto left = pixel(target, 4, 16);
        EXPECT_EQ(left.g, 255);
        EXPECT_EQ(left.r, 0);
    }

    TEST_F(ImageTargetRenderTest, ATargetWithDepthDrawsLikeOneWithout)
    {
        auto target = make_target({ .width = 32, .height = 32, .depth = true });
        ASSERT_NE(target, nullptr);
        auto& drawable = add_drawable(target, k_full_vertex, k_red_fragment, true);
        drawable.render->enable_depth_test();
        drawable.buffer->set_needs_depth_attachment(true);

        cycle();

        EXPECT_EQ(pixel(target, 16, 16).r, 255);
    }

    // =========================================================================
    // When it redraws
    // =========================================================================

    TEST_F(ImageTargetRenderTest, ATargetDrawsOnceAndThenOnlyWhenAsked)
    {
        auto target = make_target();
        add_drawable(target);

        tick();
        EXPECT_EQ(root->get_renderable_buffers().size(), 1U);
        present();

        tick();
        EXPECT_TRUE(root->get_renderable_buffers().empty());
        present();

        target->request_redraw();
        tick();
        EXPECT_EQ(root->get_renderable_buffers().size(), 1U);
        present();
    }

    TEST_F(ImageTargetRenderTest, ASkippedCycleLeavesThePixelsAlone)
    {
        auto target = make_target({ .width = 32, .height = 32, .clear_color = { 0.0F, 0.0F, 1.0F, 1.0F } });
        add_drawable(target);
        cycle();
        ASSERT_EQ(pixel(target, 16, 16).r, 255);

        target->set_clear_color({ 0.0F, 1.0F, 0.0F, 1.0F });
        tick();
        present();
        EXPECT_EQ(pixel(target, 16, 16).r, 255);
    }

    TEST_F(ImageTargetRenderTest, AContinuousTargetDrawsEveryCycle)
    {
        auto target = make_target({ .width = 32, .height = 32, .continuous = true });
        add_drawable(target);

        for (int i = 0; i < 4; ++i) {
            tick();
            EXPECT_EQ(root->get_renderable_buffers().size(), 1U) << "cycle " << i;
            present();
        }
    }

    TEST_F(ImageTargetRenderTest, AnInactiveTargetDrawsNothingAndKeepsItsImage)
    {
        auto target = make_target();
        add_drawable(target);
        cycle();
        ASSERT_EQ(pixel(target, 16, 16).r, 255);

        target->set_active(false);
        target->request_redraw();
        target->set_clear_color({ 0.0F, 0.0F, 1.0F, 1.0F });
        tick();

        EXPECT_TRUE(root->get_renderable_buffers().empty());
        present();
        EXPECT_EQ(pixel(target, 16, 16).r, 255);

        target->set_active(true);
        target->request_redraw();
        cycle();
        EXPECT_EQ(pixel(target, 16, 16).r, 255);
    }

    TEST_F(ImageTargetRenderTest, WritingABuffersContentAsksItsTargetToRedraw)
    {
        auto target = make_target();
        auto& drawable = add_drawable(target);
        cycle();
        tick();
        ASSERT_TRUE(root->get_renderable_buffers().empty());
        present();

        drawable.buffer->mark_dirty_range(0, 16);
        tick();
        EXPECT_EQ(root->get_renderable_buffers().size(), 1U);
        present();
    }

    TEST_F(ImageTargetRenderTest, TakingTheDirtyFlagClearsIt)
    {
        auto buffer = std::make_shared<VKBuffer>(64, VKBuffer::Usage::VERTEX,
            Kakshya::DataModality::VERTEX_POSITIONS_3D);

        EXPECT_TRUE(buffer->take_content_dirty());
        EXPECT_FALSE(buffer->take_content_dirty());
        buffer->mark_dirty_range(0, 8);
        EXPECT_TRUE(buffer->take_content_dirty());
        EXPECT_FALSE(buffer->take_content_dirty());
    }

    // =========================================================================
    // Processor and buffer state
    // =========================================================================

    TEST_F(ImageTargetRenderTest, AProcessorWithAnImageHasNoWindowAndTheOtherWayRound)
    {
        auto target = make_target();
        auto& drawable = add_drawable(target);

        EXPECT_EQ(drawable.render->get_target_image(), target);
        EXPECT_EQ(drawable.render->get_target_window(), nullptr);
    }

    TEST_F(ImageTargetRenderTest, ProcessingAssociatesThePipelineWithTheImageOnly)
    {
        auto target = make_target();
        auto& drawable = add_drawable(target);

        tick();

        ASSERT_TRUE(drawable.render->is_pipeline_ready());
        const auto id = drawable.render->get_render_pipeline_id();
        EXPECT_EQ(drawable.buffer->get_pipeline_image(id), target);
        EXPECT_EQ(drawable.buffer->get_image_pipelines().size(), 1U);
        EXPECT_TRUE(drawable.buffer->get_render_pipelines().empty());
        EXPECT_TRUE(drawable.buffer->has_render_pipeline());
    }

    TEST_F(ImageTargetRenderTest, RetargetingToAnotherImageRebuildsAndLeavesOneAssociation)
    {
        auto first = make_target();
        auto second = make_target();
        auto& drawable = add_drawable(first);
        cycle();
        const auto old_id = drawable.render->get_render_pipeline_id();
        ASSERT_TRUE(first->take_redraw() == false);

        drawable.render->set_target_image(second, drawable.buffer);
        EXPECT_EQ(drawable.render->get_target_image(), second);
        cycle();

        const auto associations = drawable.buffer->get_image_pipelines();
        ASSERT_EQ(associations.size(), 1U);
        EXPECT_EQ(associations.begin()->second, second);
        EXPECT_EQ(drawable.buffer->get_pipeline_image(old_id), nullptr);
        EXPECT_EQ(pixel(second, 16, 16).r, 255);
    }

    TEST_F(ImageTargetRenderTest, LeavingATargetAsksItToRedrawSoItCanBeClearedOfTheOldContent)
    {
        auto first = make_target();
        auto second = make_target();
        auto& drawable = add_drawable(first);
        cycle();
        ASSERT_FALSE(first->take_redraw());

        drawable.render->set_target_image(second, drawable.buffer);

        EXPECT_TRUE(first->take_redraw());
    }

    TEST_F(ImageTargetRenderTest, SettingTheSameTargetAgainKeepsThePipeline)
    {
        auto target = make_target();
        auto& drawable = add_drawable(target);
        cycle();
        const auto id = drawable.render->get_render_pipeline_id();

        drawable.render->set_target_image(target, drawable.buffer);
        cycle();

        EXPECT_EQ(drawable.render->get_render_pipeline_id(), id);
    }

    TEST_F(ImageTargetRenderTest, ANullTargetIsRefusedAndKeepsTheCurrentOne)
    {
        auto target = make_target();
        auto& drawable = add_drawable(target);

        drawable.render->set_target_image(nullptr, drawable.buffer);

        EXPECT_EQ(drawable.render->get_target_image(), target);
    }

    TEST_F(ImageTargetRenderTest, RemovingAPipelineDropsItsImageAssociationAndAsksTheTargetToRedraw)
    {
        auto target = make_target();
        auto& drawable = add_drawable(target);
        cycle();
        const auto id = drawable.render->get_render_pipeline_id();
        ASSERT_FALSE(target->take_redraw());

        drawable.buffer->remove_pipeline_window(id);

        EXPECT_EQ(drawable.buffer->get_pipeline_image(id), nullptr);
        EXPECT_TRUE(drawable.buffer->get_image_pipelines().empty());
        EXPECT_TRUE(target->take_redraw());
    }

    TEST_F(ImageTargetRenderTest, RemovingAnAbsentImageAssociationIsHarmless)
    {
        auto buffer = std::make_shared<VKBuffer>(64, VKBuffer::Usage::VERTEX,
            Kakshya::DataModality::VERTEX_POSITIONS_3D);

        buffer->remove_pipeline_image(99);

        EXPECT_FALSE(buffer->has_render_pipeline());
    }

    TEST_F(ImageTargetRenderTest, ABufferWithOnlyAnImagePipelineHasARenderPipeline)
    {
        auto target = make_target();
        auto buffer = std::make_shared<VKBuffer>(64, VKBuffer::Usage::VERTEX,
            Kakshya::DataModality::VERTEX_POSITIONS_3D);
        ASSERT_FALSE(buffer->has_render_pipeline());

        buffer->set_pipeline_image(5, target);

        EXPECT_TRUE(buffer->has_render_pipeline());
        EXPECT_EQ(buffer->get_pipeline_image(5), target);
        EXPECT_EQ(buffer->get_pipeline_image(6), nullptr);
    }

    // =========================================================================
    // setup_rendering
    // =========================================================================

    TEST_F(ImageTargetRenderTest, ARenderConfigWithTwoImagesDrawsIntoBoth)
    {
        const auto buffer = std::make_shared<ConfigProbeBuffer>();
        const auto first = make_target();
        const auto second = make_target();
        Portal::Graphics::RenderConfig config;
        config.fragment_shader = k_red_fragment;
        config.target_images = { first, second };

        const auto processor = buffer->apply(config);

        ASSERT_NE(processor, nullptr);
        EXPECT_EQ(processor->destination_count(), 2U);
        ASSERT_EQ(processor->get_target_images().size(), 2U);
        EXPECT_EQ(processor->get_target_images()[0], first);
        EXPECT_EQ(processor->get_target_images()[1], second);
        EXPECT_EQ(processor->get_target_window(), nullptr);
    }

    TEST_F(ImageTargetRenderTest, ARenderConfigWithTheSameImageTwiceHasOneDestination)
    {
        const auto buffer = std::make_shared<ConfigProbeBuffer>();
        const auto target = make_target();
        Portal::Graphics::RenderConfig config;
        config.fragment_shader = k_red_fragment;
        config.target_images = { target, target };

        const auto processor = buffer->apply(config);

        ASSERT_NE(processor, nullptr);
        EXPECT_EQ(processor->destination_count(), 1U);
        EXPECT_EQ(processor->get_target_image(), target);
    }

    TEST_F(ImageTargetRenderTest, ARenderConfigWithOneImageMakesItTheOnlyDestination)
    {
        const auto buffer = std::make_shared<ConfigProbeBuffer>();
        const auto target = make_target();
        Portal::Graphics::RenderConfig config;
        config.fragment_shader = k_red_fragment;
        config.target_images = { target };

        const auto processor = buffer->apply(config);

        ASSERT_NE(processor, nullptr);
        EXPECT_EQ(processor->destination_count(), 1U);
        EXPECT_EQ(processor->get_target_image(), target);
        EXPECT_EQ(processor->get_target_window(), nullptr);
    }

    TEST_F(ImageTargetRenderTest, ARenderConfigWithNoDestinationLeavesTheProcessorWithoutOne)
    {
        const auto buffer = std::make_shared<ConfigProbeBuffer>();
        Portal::Graphics::RenderConfig config;
        config.fragment_shader = k_red_fragment;

        const auto processor = buffer->apply(config);

        ASSERT_NE(processor, nullptr);
        EXPECT_EQ(processor->destination_count(), 0U);
        EXPECT_EQ(processor->get_target_image(), nullptr);
        EXPECT_EQ(processor->get_target_window(), nullptr);
    }

    // =========================================================================
    // Root edge cases
    // =========================================================================

    TEST_F(ImageTargetRenderTest, ADestroyedTargetLeavesTheBufferHarmlessToProcess)
    {
        auto target = make_target();
        add_drawable(target);
        cycle();

        targets.clear();
        target.reset();

        EXPECT_NO_THROW(cycle());
    }

    TEST_F(ImageTargetRenderTest, ARemovedChildStopsDrawingAndAsksItsTargetToRedraw)
    {
        auto target = make_target();
        auto& drawable = add_drawable(target);
        cycle();
        ASSERT_FALSE(target->take_redraw());

        drawable.buffer->mark_for_removal();
        tick();

        EXPECT_TRUE(root->get_renderable_buffers().empty());
        EXPECT_TRUE(target->take_redraw());
        present();
    }

    TEST_F(ImageTargetRenderTest, ManyCyclesWithAndWithoutRedrawsStayStable)
    {
        auto target = make_target();
        add_drawable(target);

        for (int i = 0; i < 50; ++i) {
            if (i % 3 == 0)
                target->request_redraw();
            ASSERT_NO_THROW(cycle()) << "cycle " << i;
        }

        EXPECT_EQ(pixel(target, 16, 16).r, 255);
    }

    TEST_F(ImageTargetRenderTest, TargetsDrawIndependentlyOfEachOther)
    {
        auto a = make_target({ .width = 16, .height = 16 });
        auto b = make_target({ .width = 64, .height = 8 });
        add_drawable(a, k_full_vertex, k_red_fragment);
        add_drawable(b, k_full_vertex, k_green_fragment);

        cycle();

        EXPECT_EQ(pixel(a, 8, 8).r, 255);
        EXPECT_EQ(pixel(a, 8, 8).g, 0);
        EXPECT_EQ(pixel(b, 32, 4).g, 255);
        EXPECT_EQ(pixel(b, 32, 4).r, 0);
    }

    // =========================================================================
    // One processor, several destinations
    // =========================================================================

    TEST_F(ImageTargetRenderTest, OneProcessorDrawsIntoTwoTargetsEachWithItsOwnClearColor)
    {
        const auto first = make_target({ .width = 32, .height = 32, .clear_color = { 0.0F, 0.0F, 1.0F, 1.0F } });
        const auto second = make_target({ .width = 32, .height = 32, .clear_color = { 0.0F, 1.0F, 0.0F, 1.0F } });
        const Drawable drawable = add_drawable(first, k_full_vertex, k_left_red_fragment);
        drawable.render->add_target_image(second, drawable.buffer);

        cycle();

        EXPECT_EQ(pixel(first, 4, 16).r, 255);
        EXPECT_EQ(pixel(second, 4, 16).r, 255);
        EXPECT_EQ(pixel(first, 28, 16).b, 255);
        EXPECT_EQ(pixel(first, 28, 16).g, 0);
        EXPECT_EQ(pixel(second, 28, 16).g, 255);
        EXPECT_EQ(pixel(second, 28, 16).b, 0);
    }

    TEST_F(ImageTargetRenderTest, EveryDestinationHasItsOwnPipelineAndItsOwnScope)
    {
        const auto first = make_target();
        const auto second = make_target();
        const Drawable drawable = add_drawable(first);
        drawable.render->add_target_image(second, drawable.buffer);

        tick();

        const auto associations = drawable.buffer->get_image_pipelines();
        ASSERT_EQ(associations.size(), 2U);
        const auto first_id = pipeline_for(drawable, first);
        const auto second_id = pipeline_for(drawable, second);
        EXPECT_NE(first_id, Portal::Graphics::INVALID_RENDER_PIPELINE);
        EXPECT_NE(second_id, Portal::Graphics::INVALID_RENDER_PIPELINE);
        EXPECT_NE(first_id, second_id);
        ASSERT_EQ(root->get_renderable_buffers().size(), 2U);
        EXPECT_NE(root->get_renderable_buffers()[0].target_image, root->get_renderable_buffers()[1].target_image);
        present();
    }

    TEST_F(ImageTargetRenderTest, EachDestinationDrawsAtItsOwnSize)
    {
        const auto small = make_target({ .width = 32, .height = 32 });
        const auto large = make_target({ .width = 64, .height = 64 });
        const Drawable drawable = add_drawable(small);
        drawable.render->add_target_image(large, drawable.buffer);

        cycle();

        EXPECT_EQ(pixel(small, 28, 28).r, 255);
        EXPECT_EQ(pixel(large, 60, 60).r, 255);
        EXPECT_EQ(pixel(large, 60, 4).r, 255);
    }

    TEST_F(ImageTargetRenderTest, DestinationsOfDifferentFormatsEachGetAPipeline)
    {
        const auto bytes = make_target({ .width = 16, .height = 16 });
        const auto floats = make_target({ .width = 16, .height = 16, .format = Portal::Graphics::ImageFormat::RGBA16F });
        ASSERT_NE(floats, nullptr);
        const Drawable drawable = add_drawable(bytes);
        drawable.render->add_target_image(floats, drawable.buffer);

        tick();

        EXPECT_EQ(root->get_renderable_buffers().size(), 2U);
        EXPECT_NE(pipeline_for(drawable, bytes), pipeline_for(drawable, floats));
        EXPECT_NO_THROW(present());
        EXPECT_EQ(pixel(bytes, 8, 8).r, 255);
    }

    TEST_F(ImageTargetRenderTest, TheProcessorOwnsEveryPipelineItDrawsWithAndNoOther)
    {
        const auto first = make_target();
        const auto second = make_target();
        const Drawable drawable = add_drawable(first);
        drawable.render->add_target_image(second, drawable.buffer);
        tick();
        const auto first_id = pipeline_for(drawable, first);
        const auto second_id = pipeline_for(drawable, second);

        EXPECT_TRUE(drawable.render->owns_pipeline(first_id));
        EXPECT_TRUE(drawable.render->owns_pipeline(second_id));
        EXPECT_FALSE(drawable.render->owns_pipeline(Portal::Graphics::INVALID_RENDER_PIPELINE));
        EXPECT_EQ(drawable.buffer->get_render_processor(first_id), drawable.render);
        EXPECT_EQ(drawable.buffer->get_render_processor(second_id), drawable.render);
        present();
    }

    TEST_F(ImageTargetRenderTest, AddingADestinationAlreadyThereChangesNothing)
    {
        const auto first = make_target();
        const auto second = make_target();
        const Drawable drawable = add_drawable(first);
        drawable.render->add_target_image(second, drawable.buffer);
        cycle();
        const auto first_id = pipeline_for(drawable, first);
        const auto second_id = pipeline_for(drawable, second);

        drawable.render->add_target_image(first, drawable.buffer);
        drawable.render->add_target_image(second, drawable.buffer);
        cycle();

        EXPECT_EQ(drawable.render->destination_count(), 2U);
        EXPECT_EQ(pipeline_for(drawable, first), first_id);
        EXPECT_EQ(pipeline_for(drawable, second), second_id);
    }

    TEST_F(ImageTargetRenderTest, AddingANullDestinationIsRefused)
    {
        const auto target = make_target();
        const Drawable drawable = add_drawable(target);

        drawable.render->add_target_image(nullptr, drawable.buffer);
        drawable.render->add_target_window(nullptr, drawable.buffer);

        EXPECT_EQ(drawable.render->destination_count(), 1U);
        EXPECT_EQ(drawable.render->get_target_image(), target);
    }

    TEST_F(ImageTargetRenderTest, RemovingTheFirstDestinationLetsTheNextOneKeepDrawing)
    {
        const auto first = make_target({ .width = 32, .height = 32 });
        const auto second = make_target({ .width = 32, .height = 32 });
        const Drawable drawable = add_drawable(first, k_full_vertex, k_left_red_fragment);
        drawable.render->add_target_image(second, drawable.buffer);
        cycle();
        ASSERT_FALSE(first->take_redraw());

        drawable.render->remove_target_image(first, drawable.buffer);

        EXPECT_EQ(drawable.render->get_target_image(), second);
        EXPECT_EQ(drawable.render->destination_count(), 1U);
        EXPECT_EQ(drawable.buffer->get_image_pipelines().size(), 1U);
        EXPECT_EQ(pipeline_for(drawable, first), Portal::Graphics::INVALID_RENDER_PIPELINE);
        EXPECT_TRUE(first->take_redraw());

        second->set_clear_color({ 0.0F, 0.0F, 1.0F, 1.0F });
        cycle();

        EXPECT_EQ(pixel(second, 4, 16).r, 255);
        EXPECT_EQ(pixel(second, 28, 16).b, 255);
    }

    TEST_F(ImageTargetRenderTest, RemovingALaterDestinationLeavesTheFirstDrawing)
    {
        const auto first = make_target({ .width = 32, .height = 32 });
        const auto second = make_target({ .width = 32, .height = 32 });
        const Drawable drawable = add_drawable(first, k_full_vertex, k_left_red_fragment);
        drawable.render->add_target_image(second, drawable.buffer);
        cycle();
        ASSERT_FALSE(second->take_redraw());

        drawable.render->remove_target_image(second, drawable.buffer);

        EXPECT_EQ(drawable.render->get_target_image(), first);
        EXPECT_EQ(drawable.buffer->get_image_pipelines().size(), 1U);
        EXPECT_TRUE(second->take_redraw());

        first->set_clear_color({ 0.0F, 0.0F, 1.0F, 1.0F });
        cycle();

        EXPECT_EQ(pixel(first, 28, 16).b, 255);
    }

    TEST_F(ImageTargetRenderTest, RemovingEveryDestinationLeavesNothingToDrawAndProcessingHarmless)
    {
        const auto first = make_target();
        const auto second = make_target();
        const Drawable drawable = add_drawable(first);
        drawable.render->add_target_image(second, drawable.buffer);
        cycle();

        drawable.render->remove_target_image(first, drawable.buffer);
        drawable.render->remove_target_image(second, drawable.buffer);

        EXPECT_EQ(drawable.render->destination_count(), 0U);
        EXPECT_EQ(drawable.render->get_target_image(), nullptr);
        EXPECT_TRUE(drawable.render->get_target_images().empty());
        EXPECT_TRUE(drawable.buffer->get_image_pipelines().empty());
        EXPECT_NO_THROW(cycle());
        EXPECT_TRUE(root->get_renderable_buffers().empty());
    }

    TEST_F(ImageTargetRenderTest, ADestinationAddedAfterEveryOneWasRemovedDrawsAgain)
    {
        const auto first = make_target();
        const auto second = make_target();
        const Drawable drawable = add_drawable(first);
        cycle();
        drawable.render->remove_target_image(first, drawable.buffer);
        cycle();

        drawable.render->add_target_image(second, drawable.buffer);
        cycle();

        EXPECT_EQ(drawable.render->get_target_image(), second);
        EXPECT_EQ(pixel(second, 16, 16).r, 255);
    }

    TEST_F(ImageTargetRenderTest, SettingASingleTargetReplacesEveryDestination)
    {
        const auto first = make_target();
        const auto second = make_target();
        const auto third = make_target();
        const Drawable drawable = add_drawable(first);
        drawable.render->add_target_image(second, drawable.buffer);
        cycle();
        ASSERT_FALSE(first->take_redraw());
        ASSERT_FALSE(second->take_redraw());

        drawable.render->set_target_image(third, drawable.buffer);
        cycle();

        EXPECT_EQ(drawable.render->destination_count(), 1U);
        ASSERT_EQ(drawable.render->get_target_images().size(), 1U);
        EXPECT_EQ(drawable.render->get_target_images().front(), third);
        EXPECT_EQ(drawable.buffer->get_image_pipelines().size(), 1U);
        EXPECT_EQ(pipeline_for(drawable, first), Portal::Graphics::INVALID_RENDER_PIPELINE);
        EXPECT_EQ(pipeline_for(drawable, second), Portal::Graphics::INVALID_RENDER_PIPELINE);
        EXPECT_TRUE(first->take_redraw());
        EXPECT_TRUE(second->take_redraw());
        EXPECT_EQ(pixel(third, 16, 16).r, 255);
    }

    TEST_F(ImageTargetRenderTest, ADestinationCanBeSkippedAndDrawnAgainWithoutTouchingTheOthers)
    {
        const auto first = make_target({ .width = 32, .height = 32, .clear_color = { 0.0F, 0.0F, 1.0F, 1.0F } });
        const auto second = make_target({ .width = 32, .height = 32, .clear_color = { 0.0F, 0.0F, 1.0F, 1.0F } });
        const Drawable drawable = add_drawable(first, k_full_vertex, k_left_red_fragment);
        drawable.render->add_target_image(second, drawable.buffer);
        cycle();
        ASSERT_EQ(pixel(second, 28, 16).b, 255);
        const auto second_id = pipeline_for(drawable, second);

        drawable.render->set_target_enabled(second, false);
        EXPECT_TRUE(second->take_redraw());
        first->set_clear_color({ 0.0F, 1.0F, 0.0F, 1.0F });
        second->set_clear_color({ 0.0F, 1.0F, 0.0F, 1.0F });
        cycle();

        EXPECT_EQ(pixel(first, 28, 16).g, 255);
        EXPECT_EQ(pixel(second, 28, 16).g, 0);
        EXPECT_EQ(pixel(second, 28, 16).b, 255);
        EXPECT_EQ(pipeline_for(drawable, second), second_id);
        EXPECT_TRUE(pipeline_alive(second_id));

        drawable.render->set_target_enabled(second, true);
        cycle();

        EXPECT_EQ(pixel(second, 28, 16).g, 255);
        EXPECT_EQ(pixel(second, 4, 16).r, 255);
    }

    TEST_F(ImageTargetRenderTest, EveryDestinationSkippedMeansNothingIsDrawn)
    {
        const auto first = make_target();
        const auto second = make_target();
        const Drawable drawable = add_drawable(first);
        drawable.render->add_target_image(second, drawable.buffer);
        cycle();

        drawable.render->set_target_enabled(first, false);
        drawable.render->set_target_enabled(second, false);
        tick();

        EXPECT_TRUE(root->get_renderable_buffers().empty());
        present();

        drawable.render->set_target_enabled(first, true);
        tick();
        EXPECT_EQ(root->get_renderable_buffers().size(), 1U);
        present();
    }

    TEST_F(ImageTargetRenderTest, EnablingAnUnknownTargetChangesNothing)
    {
        const auto target = make_target();
        const auto stranger = make_target();
        const Drawable drawable = add_drawable(target);
        cycle();
        ASSERT_FALSE(target->take_redraw());
        ASSERT_TRUE(stranger->take_redraw());

        drawable.render->set_target_enabled(stranger, false);

        EXPECT_FALSE(target->take_redraw());
        EXPECT_FALSE(stranger->take_redraw());
        EXPECT_EQ(drawable.render->destination_count(), 1U);
    }

    TEST_F(ImageTargetRenderTest, ABindingChangedAfterTheFirstDrawReachesEveryDestination)
    {
        const auto red = make_target();
        const auto green = make_target();
        add_drawable(red);
        add_drawable(green, k_full_vertex, k_green_fragment);
        const auto sink_a = make_target();
        const auto sink_b = make_target();
        const Drawable consumer = add_drawable(sink_a, k_full_vertex, k_sample_fragment);
        consumer.render->add_target_image(sink_b, consumer.buffer);
        consumer.render->add_binding("tex", Buffers::ShaderBinding { 1, 0, vk::DescriptorType::eCombinedImageSampler });
        consumer.render->bind_texture({ .name = "tex", .image = red->image() });
        cycle();
        ASSERT_EQ(pixel(sink_a, 16, 16).r, 255);
        ASSERT_EQ(pixel(sink_b, 16, 16).r, 255);

        consumer.render->bind_texture({ .name = "tex", .image = green->image() });
        sink_a->request_redraw();
        sink_b->request_redraw();
        cycle();

        EXPECT_EQ(pixel(sink_a, 16, 16).g, 255);
        EXPECT_EQ(pixel(sink_a, 16, 16).r, 0);
        EXPECT_EQ(pixel(sink_b, 16, 16).g, 255);
        EXPECT_EQ(pixel(sink_b, 16, 16).r, 0);
    }

    TEST_F(ImageTargetRenderTest, AShaderChangedAfterTheFirstDrawReachesEveryDestination)
    {
        const auto first = make_target();
        const auto second = make_target();
        const Drawable drawable = add_drawable(first);
        drawable.render->add_target_image(second, drawable.buffer);
        cycle();
        ASSERT_EQ(pixel(first, 16, 16).r, 255);
        ASSERT_EQ(pixel(second, 16, 16).r, 255);

        drawable.render->set_fragment_shader(k_green_fragment);
        first->request_redraw();
        second->request_redraw();
        cycle();

        EXPECT_EQ(pixel(first, 16, 16).g, 255);
        EXPECT_EQ(pixel(first, 16, 16).r, 0);
        EXPECT_EQ(pixel(second, 16, 16).g, 255);
        EXPECT_EQ(pixel(second, 16, 16).r, 0);
    }

    TEST_F(ImageTargetRenderTest, WritingTheBuffersContentAsksEveryDestinationToRedraw)
    {
        const auto first = make_target();
        const auto second = make_target();
        const Drawable drawable = add_drawable(first);
        drawable.render->add_target_image(second, drawable.buffer);
        cycle();
        tick();
        ASSERT_TRUE(root->get_renderable_buffers().empty());
        present();

        drawable.buffer->mark_dirty_range(0, 16);
        tick();

        EXPECT_EQ(root->get_renderable_buffers().size(), 2U);
        present();
    }

    TEST_F(ImageTargetRenderTest, ARemovedChildAsksEveryDestinationToRedraw)
    {
        const auto first = make_target();
        const auto second = make_target();
        const Drawable drawable = add_drawable(first);
        drawable.render->add_target_image(second, drawable.buffer);
        cycle();
        ASSERT_FALSE(first->take_redraw());
        ASSERT_FALSE(second->take_redraw());

        drawable.buffer->mark_for_removal();
        tick();

        EXPECT_TRUE(first->take_redraw());
        EXPECT_TRUE(second->take_redraw());
        present();
    }

    TEST_F(ImageTargetRenderTest, DetachingTheProcessorReleasesEveryDestination)
    {
        const auto first = make_target();
        const auto second = make_target();
        const Drawable drawable = add_drawable(first);
        drawable.render->add_target_image(second, drawable.buffer);
        cycle();
        ASSERT_FALSE(first->take_redraw());
        ASSERT_FALSE(second->take_redraw());

        drawable.buffer->get_processing_chain()->remove_processor_anywhere(drawable.render, drawable.buffer);

        EXPECT_TRUE(drawable.buffer->get_image_pipelines().empty());
        EXPECT_TRUE(first->take_redraw());
        EXPECT_TRUE(second->take_redraw());
    }

    TEST_F(ImageTargetRenderTest, DestroyingTheProcessorRetiresEveryPipeline)
    {
        const auto first = make_target();
        const auto second = make_target();
        Drawable drawable = add_drawable(first);
        drawable.render->add_target_image(second, drawable.buffer);
        cycle();
        const auto first_id = pipeline_for(drawable, first);
        const auto second_id = pipeline_for(drawable, second);
        ASSERT_TRUE(pipeline_alive(first_id));
        ASSERT_TRUE(pipeline_alive(second_id));

        drawable.buffer->get_processing_chain()->remove_processor_anywhere(drawable.render, drawable.buffer);
        drawables.back().render.reset();
        drawable.render.reset();

        EXPECT_FALSE(pipeline_alive(first_id));
        EXPECT_FALSE(pipeline_alive(second_id));
    }

    TEST_F(ImageTargetRenderTest, TheRefreshPathRecordsOnlyTheDestinationItIsGiven)
    {
        const auto first = make_target();
        const auto second = make_target();
        const Drawable drawable = add_drawable(first);
        drawable.render->add_target_image(second, drawable.buffer);
        cycle();
        const auto first_id = pipeline_for(drawable, first);
        const auto second_id = pipeline_for(drawable, second);
        drawable.buffer->clear_pipeline_commands();

        drawable.render->record_draw(drawable.buffer, second_id);

        EXPECT_NE(drawable.buffer->get_pipeline_command(second_id), Portal::Graphics::INVALID_COMMAND_BUFFER);
        EXPECT_EQ(drawable.buffer->get_pipeline_command(first_id), Portal::Graphics::INVALID_COMMAND_BUFFER);

        EXPECT_NO_THROW(drawable.render->record_draw(drawable.buffer, 987654U));
        drawable.buffer->clear_pipeline_commands();
    }

    // =========================================================================
    // Keeping what was drawn
    // =========================================================================

    TEST_F(ImageTargetRenderTest, AFirstScopeOfAPreservingTargetClearsToItsColor)
    {
        const auto target = make_target({ .width = 32, .height = 32, .clear_color = { 0.0F, 0.0F, 1.0F, 1.0F }, .preserve = true });
        add_drawable(target, k_full_vertex, k_left_red_fragment);

        cycle();

        EXPECT_EQ(pixel(target, 4, 16).r, 255);
        EXPECT_EQ(pixel(target, 28, 16).b, 255);
        EXPECT_EQ(pixel(target, 28, 16).r, 0);
    }

    TEST_F(ImageTargetRenderTest, APreservingTargetKeepsWhatWasDrawnAndANormalOneStartsOver)
    {
        const auto keep = make_target({ .width = 32, .height = 32, .clear_color = { 0.0F, 0.0F, 1.0F, 1.0F }, .preserve = true });
        const auto wipe = make_target({ .width = 32, .height = 32, .clear_color = { 0.0F, 0.0F, 1.0F, 1.0F } });
        const Drawable drawable = add_drawable(keep, k_full_vertex, k_left_red_fragment);
        drawable.render->add_target_image(wipe, drawable.buffer);
        cycle();
        ASSERT_EQ(pixel(keep, 4, 16).r, 255);
        ASSERT_EQ(pixel(wipe, 4, 16).r, 255);

        drawable.render->set_fragment_shader(k_right_green_fragment);
        keep->request_redraw();
        wipe->request_redraw();
        cycle();

        EXPECT_EQ(pixel(keep, 4, 16).r, 255);
        EXPECT_EQ(pixel(keep, 28, 16).g, 255);
        EXPECT_EQ(pixel(wipe, 4, 16).r, 0);
        EXPECT_EQ(pixel(wipe, 4, 16).b, 255);
        EXPECT_EQ(pixel(wipe, 28, 16).g, 255);
    }

    TEST_F(ImageTargetRenderTest, AClearRequestWipesAPreservingTargetBeforeTheNextDraw)
    {
        const auto keep = make_target({ .width = 32, .height = 32, .clear_color = { 0.0F, 0.0F, 1.0F, 1.0F }, .preserve = true });
        const Drawable drawable = add_drawable(keep, k_full_vertex, k_left_red_fragment);
        cycle();
        ASSERT_EQ(pixel(keep, 4, 16).r, 255);

        drawable.render->set_fragment_shader(k_right_green_fragment);
        keep->request_clear();
        cycle();

        EXPECT_EQ(pixel(keep, 4, 16).r, 0);
        EXPECT_EQ(pixel(keep, 4, 16).b, 255);
        EXPECT_EQ(pixel(keep, 28, 16).g, 255);
    }

    TEST_F(ImageTargetRenderTest, ANewClearColorShowsAtTheNextDrawOfAPreservingTarget)
    {
        const auto keep = make_target({ .width = 32, .height = 32, .clear_color = { 0.0F, 0.0F, 1.0F, 1.0F }, .preserve = true });
        add_drawable(keep, k_full_vertex, k_left_red_fragment);
        cycle();
        ASSERT_EQ(pixel(keep, 28, 16).b, 255);

        keep->set_clear_color({ 0.0F, 1.0F, 0.0F, 1.0F });
        cycle();

        EXPECT_EQ(pixel(keep, 28, 16).g, 255);
        EXPECT_EQ(pixel(keep, 28, 16).b, 0);
        EXPECT_EQ(pixel(keep, 4, 16).r, 255);
    }

    TEST_F(ImageTargetRenderTest, APreservingTargetThatLosesItsBufferIsClearedWhenAnotherTakesOver)
    {
        const auto keep = make_target({ .width = 32, .height = 32, .clear_color = { 0.0F, 0.0F, 1.0F, 1.0F }, .preserve = true });
        const Drawable first = add_drawable(keep, k_full_vertex, k_left_red_fragment);
        cycle();
        ASSERT_EQ(pixel(keep, 4, 16).r, 255);

        first.buffer->mark_for_removal();
        add_drawable(keep, k_full_vertex, k_right_green_fragment);
        cycle();

        EXPECT_EQ(pixel(keep, 4, 16).r, 0);
        EXPECT_EQ(pixel(keep, 4, 16).b, 255);
        EXPECT_EQ(pixel(keep, 28, 16).g, 255);
    }

    // =========================================================================
    // Resizing
    // =========================================================================

    TEST_F(ImageTargetRenderTest, AResizedTargetDrawsAtItsNewSize)
    {
        const auto target = make_target({ .width = 32, .height = 32 });
        add_drawable(target);
        cycle();
        ASSERT_EQ(pixel(target, 16, 16).r, 255);

        ASSERT_TRUE(target->resize(64, 16));
        cycle();

        EXPECT_EQ(target->width(), 64U);
        EXPECT_EQ(target->height(), 16U);
        EXPECT_EQ(pixel(target, 60, 12).r, 255);
        EXPECT_EQ(pixel(target, 2, 2).r, 255);
    }

    TEST_F(ImageTargetRenderTest, AResizedTargetWithDepthStillDraws)
    {
        const auto target = make_target({ .width = 32, .height = 32, .depth = true });
        const Drawable drawable = add_drawable(target, k_full_vertex, k_red_fragment, true);
        drawable.render->enable_depth_test();
        drawable.buffer->set_needs_depth_attachment(true);
        cycle();
        ASSERT_EQ(pixel(target, 16, 16).r, 255);

        ASSERT_TRUE(target->resize(48, 48));
        cycle();

        EXPECT_EQ(pixel(target, 40, 40).r, 255);
    }

    TEST_F(ImageTargetRenderTest, AResizedPreservingTargetStartsFromItsClearColor)
    {
        const auto target = make_target({ .width = 32, .height = 32, .clear_color = { 0.0F, 0.0F, 1.0F, 1.0F }, .preserve = true });
        add_drawable(target, k_full_vertex, k_left_red_fragment);
        cycle();
        ASSERT_EQ(pixel(target, 4, 16).r, 255);

        ASSERT_TRUE(target->resize(48, 24));
        cycle();

        EXPECT_EQ(pixel(target, 4, 12).r, 255);
        EXPECT_EQ(pixel(target, 40, 12).b, 255);
        EXPECT_EQ(pixel(target, 40, 12).r, 0);
    }

    TEST_F(ImageTargetRenderTest, ASamplerShowsAResizedSourceOnceItBindsTheNewImage)
    {
        const auto source = make_target({ .width = 16, .height = 16 });
        add_drawable(source);
        const auto sink = make_target();
        const Drawable consumer = add_drawable(sink, k_full_vertex, k_sample_fragment);
        consumer.render->add_binding("tex", Buffers::ShaderBinding { 1, 0, vk::DescriptorType::eCombinedImageSampler });
        consumer.render->bind_texture({ .name = "tex", .image = source->image() });
        cycle();
        ASSERT_EQ(pixel(sink, 16, 16).r, 255);
        const auto old_image = source->image();

        ASSERT_TRUE(source->resize(32, 32));
        EXPECT_FALSE(old_image->is_initialized());
        consumer.render->bind_texture({ .name = "tex", .image = source->image() });
        sink->set_clear_color({ 0.0F, 0.0F, 1.0F, 1.0F });
        cycle();

        EXPECT_EQ(pixel(sink, 16, 16).r, 255);
        EXPECT_EQ(pixel(sink, 16, 16).b, 0);
    }

    // =========================================================================
    // Bindings made with a target
    // =========================================================================

    TEST_F(ImageTargetRenderTest, ASamplerBoundToATargetFollowsItsResizeWithoutBindingAgain)
    {
        const auto source = make_target({ .width = 16, .height = 16 });
        const Drawable producer = add_drawable(source);
        const auto sink = make_target();
        const Drawable consumer = add_drawable(sink, k_full_vertex, k_sample_fragment);
        consumer.render->add_binding("tex", Buffers::ShaderBinding { 1, 0, vk::DescriptorType::eCombinedImageSampler });
        consumer.render->bind_texture({ .name = "tex", .target = source });
        cycle();
        ASSERT_EQ(pixel(sink, 16, 16).r, 255);
        const auto old_image = source->image();

        producer.render->set_fragment_shader(k_green_fragment);
        ASSERT_TRUE(source->resize(32, 32));
        EXPECT_FALSE(old_image->is_initialized());
        cycle();

        EXPECT_EQ(pixel(sink, 16, 16).g, 255);
        EXPECT_EQ(pixel(sink, 16, 16).r, 0);
    }

    TEST_F(ImageTargetRenderTest, ATargetBoundByTargetDrawsAfterItsSourceWhicheverOrderTheyWereAdded)
    {
        auto source = make_target();
        auto sink = make_target();

        auto& consumer = add_drawable(sink, k_full_vertex, k_sample_fragment);
        consumer.render->add_binding("tex", Buffers::ShaderBinding { 1, 0, vk::DescriptorType::eCombinedImageSampler });
        consumer.render->bind_texture({ .name = "tex", .target = source });
        add_drawable(source);

        cycle();

        EXPECT_EQ(pixel(sink, 16, 16).r, 255);
    }

    TEST_F(ImageTargetRenderTest, ASourceThatRedrawsAsksATargetBoundByTargetToRedraw)
    {
        const auto source = make_target();
        const Drawable producer = add_drawable(source);
        const auto sink = make_target();
        const Drawable consumer = add_drawable(sink, k_full_vertex, k_sample_fragment);
        consumer.render->add_binding("tex", Buffers::ShaderBinding { 1, 0, vk::DescriptorType::eCombinedImageSampler });
        consumer.render->bind_texture({ .name = "tex", .target = source });
        cycle();
        ASSERT_EQ(pixel(sink, 16, 16).r, 255);

        producer.render->set_fragment_shader(k_green_fragment);
        source->request_redraw();
        cycle();

        EXPECT_EQ(pixel(sink, 16, 16).g, 255);
        EXPECT_EQ(pixel(sink, 16, 16).r, 0);
    }

    TEST_F(ImageTargetRenderTest, AProcessorSamplesATargetBoundByTargetOrByItsImageAndNoOther)
    {
        const auto source = make_target();
        const auto other = make_target();
        const Drawable consumer = add_drawable(make_target(), k_full_vertex, k_sample_fragment);
        consumer.render->add_binding("tex", Buffers::ShaderBinding { 1, 0, vk::DescriptorType::eCombinedImageSampler });
        EXPECT_FALSE(consumer.render->samples(source));

        consumer.render->bind_texture({ .name = "tex", .target = source });
        EXPECT_TRUE(consumer.render->samples(source));
        EXPECT_FALSE(consumer.render->samples(other));

        consumer.render->bind_texture({ .name = "tex", .image = other->image() });
        EXPECT_FALSE(consumer.render->samples(source));
        EXPECT_TRUE(consumer.render->samples(other));
        EXPECT_FALSE(consumer.render->samples(nullptr));
    }

    TEST_F(ImageTargetRenderTest, ABindingMadeWithAnImageStopsFollowingTheTargetItReplaced)
    {
        const auto source = make_target({ .width = 16, .height = 16 });
        add_drawable(source);
        const auto other = make_target({ .width = 16, .height = 16 });
        const Drawable consumer = add_drawable(make_target(), k_full_vertex, k_sample_fragment);
        consumer.render->add_binding("tex", Buffers::ShaderBinding { 1, 0, vk::DescriptorType::eCombinedImageSampler });
        consumer.render->bind_texture({ .name = "tex", .target = source });
        consumer.render->bind_texture({ .name = "tex", .image = other->image() });

        ASSERT_TRUE(source->resize(32, 32));
        cycle();

        EXPECT_FALSE(consumer.render->samples(source));
        EXPECT_TRUE(consumer.render->samples(other));
    }

    TEST_F(ImageTargetRenderTest, ABindingGivenAnIndexNeedsNoName)
    {
        const auto source = make_target();
        const Drawable consumer = add_drawable(make_target(), k_full_vertex, k_sample_fragment);

        consumer.render->bind_texture({ .binding = 0, .target = source });

        EXPECT_TRUE(consumer.render->samples(source));
    }

    TEST_F(ImageTargetRenderTest, AnImageAndATargetTogetherBindNothing)
    {
        const auto source = make_target();
        const auto other = make_target();
        const Drawable consumer = add_drawable(make_target(), k_full_vertex, k_sample_fragment);
        consumer.render->add_binding("tex", Buffers::ShaderBinding { 1, 0, vk::DescriptorType::eCombinedImageSampler });

        consumer.render->bind_texture({ .name = "tex", .image = other->image(), .target = source });

        EXPECT_FALSE(consumer.render->samples(source));
        EXPECT_FALSE(consumer.render->samples(other));
    }

    TEST_F(ImageTargetRenderTest, ANameTheProcessorDoesNotListBindsNothing)
    {
        const auto source = make_target();
        const Drawable consumer = add_drawable(make_target(), k_full_vertex, k_sample_fragment);

        consumer.render->bind_texture({ .name = "missing", .target = source });

        EXPECT_FALSE(consumer.render->samples(source));
    }

    TEST_F(ImageTargetRenderTest, ANullTargetAndNullImageBindNothing)
    {
        const Drawable consumer = add_drawable(make_target(), k_full_vertex, k_sample_fragment);
        consumer.render->add_binding("tex", Buffers::ShaderBinding { 1, 0, vk::DescriptorType::eCombinedImageSampler });

        EXPECT_NO_THROW(consumer.render->bind_texture({ .name = "tex" }));
        EXPECT_NO_THROW(consumer.render->bind_texture({ .binding = 0 }));
    }

    TEST_F(ImageTargetRenderTest, TheGenerationCountsOnlyResizesThatMadeNewImages)
    {
        const auto target = make_target({ .width = 16, .height = 16 });
        EXPECT_EQ(target->generation(), 0U);

        EXPECT_TRUE(target->resize(16, 16));
        EXPECT_EQ(target->generation(), 0U);

        EXPECT_FALSE(target->resize(0, 16));
        EXPECT_EQ(target->generation(), 0U);

        EXPECT_TRUE(target->resize(32, 16));
        EXPECT_EQ(target->generation(), 1U);

        EXPECT_TRUE(target->resize(32, 32));
        EXPECT_EQ(target->generation(), 2U);
    }

    // =========================================================================
    // Layers, cube faces and areas
    // =========================================================================

    TEST_F(ImageTargetRenderTest, DestinationsDrawIntoTheLayersTheyNameOfOneTarget)
    {
        const auto target = make_target({ .width = 16, .height = 16, .layers = 3 });
        ASSERT_NE(target, nullptr);
        EXPECT_EQ(target->layers(), 3U);
        EXPECT_EQ(target->image()->get_array_layers(), 3U);
        const Drawable first = add_drawable(target, k_full_vertex, k_red_fragment);
        const Drawable last = add_drawable(target, k_full_vertex, k_green_fragment);
        first.render->set_target_area(target, { .layer = 0 });
        last.render->set_target_area(target, { .layer = 2 });

        cycle();

        EXPECT_EQ(texel(target->image(), 8, 8, 0).r, 255);
        EXPECT_EQ(texel(target->image(), 8, 8, 0).g, 0);
        EXPECT_EQ(texel(target->image(), 8, 8, 2).g, 255);
        EXPECT_EQ(texel(target->image(), 8, 8, 2).r, 0);
    }

    TEST_F(ImageTargetRenderTest, ACubeTargetDrawsIntoItsFaces)
    {
        const auto target = make_target({ .width = 16, .height = 16, .cube = true });
        ASSERT_NE(target, nullptr);
        EXPECT_TRUE(target->cube());
        EXPECT_EQ(target->layers(), 6U);
        const Drawable face = add_drawable(target, k_full_vertex, k_green_fragment);
        face.render->set_target_area(target, { .layer = 4 });

        cycle();

        EXPECT_EQ(texel(target->image(), 8, 8, 4).g, 255);
        EXPECT_EQ(texel(target->image(), 8, 8, 4).r, 0);
    }

    TEST_F(ImageTargetRenderTest, ALayerBeyondTheTargetIsRefused)
    {
        const auto target = make_target({ .width = 16, .height = 16, .layers = 2 });
        const Drawable drawable = add_drawable(target);

        drawable.render->set_target_area(target, { .layer = 5 });

        EXPECT_EQ(drawable.render->get_target_area(target).layer, 0U);
    }

    TEST_F(ImageTargetRenderTest, ALayeredTargetResizesEveryLayer)
    {
        const auto target = make_target({ .width = 16, .height = 16, .layers = 2 });
        ASSERT_NE(target, nullptr);

        ASSERT_TRUE(target->resize(32, 16));

        EXPECT_EQ(target->image()->get_width(), 32U);
        EXPECT_EQ(target->image()->get_array_layers(), 2U);
        EXPECT_EQ(target->generation(), 1U);
    }

    TEST_F(ImageTargetRenderTest, LayersAndCubesRefuseMipLevelsAndCubesRefuseNonSquareFaces)
    {
        EXPECT_EQ(loom().create_image_target({ .width = 16, .height = 16, .mip_levels = 3, .layers = 2 }), nullptr);
        EXPECT_EQ(loom().create_image_target({ .width = 16, .height = 16, .mip_levels = 3, .cube = true }), nullptr);
        EXPECT_EQ(loom().create_image_target({ .width = 16, .height = 8, .cube = true }), nullptr);
    }

    TEST_F(ImageTargetRenderTest, ADestinationDrawsOnlyIntoTheAreaItNames)
    {
        const auto target = make_target({ .width = 32, .height = 32 });
        const Drawable left = add_drawable(target, k_full_vertex, k_red_fragment);
        const Drawable right = add_drawable(target, k_full_vertex, k_green_fragment);
        left.render->set_target_area(target,
            { .area = Kinesis::Scissor::from(Kinesis::AABB2D { .min = { -1.F, -1.F }, .max = { 0.F, 1.F } }) });
        right.render->set_target_area(target,
            { .area = Kinesis::Scissor::from(Kinesis::AABB2D { .min = { 0.F, -1.F }, .max = { 1.F, 1.F } }) });

        cycle();

        EXPECT_EQ(pixel(target, 8, 16).r, 255);
        EXPECT_EQ(pixel(target, 8, 16).g, 0);
        EXPECT_EQ(pixel(target, 24, 16).g, 255);
        EXPECT_EQ(pixel(target, 24, 16).r, 0);
    }

    TEST_F(ImageTargetRenderTest, AnAreaFollowsTheTargetsSizeAfterAResize)
    {
        const auto target = make_target({ .width = 32, .height = 32 });
        const Drawable left = add_drawable(target, k_full_vertex, k_red_fragment);
        const Drawable right = add_drawable(target, k_full_vertex, k_green_fragment);
        left.render->set_target_area(target,
            { .area = Kinesis::Scissor::from(Kinesis::AABB2D { .min = { -1.F, -1.F }, .max = { 0.F, 1.F } }) });
        right.render->set_target_area(target,
            { .area = Kinesis::Scissor::from(Kinesis::AABB2D { .min = { 0.F, -1.F }, .max = { 1.F, 1.F } }) });
        cycle();

        ASSERT_TRUE(target->resize(64, 32));
        cycle();

        EXPECT_EQ(pixel(target, 16, 16).r, 255);
        EXPECT_EQ(pixel(target, 48, 16).g, 255);
    }

    TEST(TargetAreaDescriptionTest, TwoAreasAreEqualWhenLayerAndRegionAre)
    {
        const Portal::Graphics::TargetArea whole {};
        const Portal::Graphics::TargetArea half { .area = Kinesis::Scissor::from(Kinesis::AABB2D { .min = { -1.F, -1.F }, .max = { 0.F, 1.F } }) };

        EXPECT_TRUE(whole == Portal::Graphics::TargetArea {});
        EXPECT_FALSE(whole == half);
        EXPECT_TRUE(half == half);
        EXPECT_FALSE(whole == (Portal::Graphics::TargetArea { .layer = 1 }));
    }

    // =========================================================================
    // Several color attachments
    // =========================================================================

    TEST_F(ImageTargetRenderTest, OnePassDrawsEveryColorImageOfATarget)
    {
        const auto target = make_target({ .width = 16, .height = 16, .extra_formats = { Portal::Graphics::ImageFormat::RGBA8 } });
        if (!target) {
            GTEST_SKIP() << "no independent blend on this device";
        }
        EXPECT_EQ(target->attachment_count(), 2U);
        EXPECT_EQ(target->color_formats().size(), 2U);
        add_drawable(target, k_full_vertex, k_two_outputs_fragment);

        cycle();

        EXPECT_EQ(texel(target->image(0), 8, 8).r, 255);
        EXPECT_EQ(texel(target->image(0), 8, 8).g, 0);
        EXPECT_EQ(texel(target->image(1), 8, 8).g, 255);
        EXPECT_EQ(texel(target->image(1), 8, 8).r, 0);
    }

    TEST_F(ImageTargetRenderTest, ATargetWithExtraColorImagesResizesThemAll)
    {
        const auto target = make_target({ .width = 16, .height = 16, .extra_formats = { Portal::Graphics::ImageFormat::RGBA8 } });
        if (!target) {
            GTEST_SKIP() << "no independent blend on this device";
        }

        ASSERT_TRUE(target->resize(32, 8));

        EXPECT_EQ(target->image(0)->get_width(), 32U);
        EXPECT_EQ(target->image(1)->get_width(), 32U);
        EXPECT_EQ(target->image(1)->get_height(), 8U);
    }

    TEST_F(ImageTargetRenderTest, AProcessorSamplesAnExtraColorImageOfATarget)
    {
        const auto source = make_target({ .width = 16, .height = 16, .extra_formats = { Portal::Graphics::ImageFormat::RGBA8 } });
        if (!source) {
            GTEST_SKIP() << "no independent blend on this device";
        }
        const Drawable consumer = add_drawable(make_target(), k_full_vertex, k_sample_fragment);
        consumer.render->add_binding("tex", Buffers::ShaderBinding { 1, 0, vk::DescriptorType::eCombinedImageSampler });

        consumer.render->bind_texture({ .name = "tex", .image = source->image(1) });

        EXPECT_TRUE(consumer.render->samples(source));
    }

    TEST_F(ImageTargetRenderTest, ExtraColorImagesRefuseMipLevels)
    {
        EXPECT_EQ(loom().create_image_target({ .width = 16, .height = 16, .mip_levels = 3, .extra_formats = { Portal::Graphics::ImageFormat::RGBA8 } }), nullptr);
    }

    // =========================================================================
    // Compute writes and sampled depth
    // =========================================================================

    TEST_F(ImageTargetRenderTest, ATargetMadeForStorageAllowsComputeWritesAndOthersDoNot)
    {
        const auto plain = make_target({ .width = 16, .height = 16 });
        const auto writable = make_target({ .width = 16, .height = 16, .storage = true });
        ASSERT_NE(plain, nullptr);
        if (!writable) {
            GTEST_SKIP() << "the format has no storage image support on this device";
        }

        EXPECT_FALSE(plain->storage());
        EXPECT_FALSE(static_cast<bool>(plain->image()->get_usage_flags() & vk::ImageUsageFlagBits::eStorage));
        EXPECT_TRUE(writable->storage());
        EXPECT_TRUE(static_cast<bool>(writable->image()->get_usage_flags() & vk::ImageUsageFlagBits::eStorage));
    }

    TEST_F(ImageTargetRenderTest, ATargetDrawsAgainAfterAComputePassLeftItInTheGeneralLayout)
    {
        const auto target = make_target({ .width = 16, .height = 16, .storage = true });
        if (!target) {
            GTEST_SKIP() << "the format has no storage image support on this device";
        }
        const Drawable drawable = add_drawable(target);
        cycle();
        ASSERT_EQ(pixel(target, 8, 8).r, 255);

        loom().transition_layout(target->image(), vk::ImageLayout::eGeneral, Portal::Graphics::SubmitMode::IMMEDIATE);
        ASSERT_EQ(target->image()->get_current_layout(), vk::ImageLayout::eGeneral);
        drawable.render->set_fragment_shader(k_green_fragment);
        target->request_redraw();
        cycle();

        EXPECT_EQ(pixel(target, 8, 8).g, 255);
        EXPECT_EQ(pixel(target, 8, 8).r, 0);
        EXPECT_EQ(target->image()->get_current_layout(), vk::ImageLayout::eShaderReadOnlyOptimal);
    }

    TEST_F(ImageTargetRenderTest, ADepthImageKeptForSamplingHoldsTheDepthThatWasDrawn)
    {
        const auto source = make_target({ .width = 16, .height = 16, .depth = true, .sample_depth = true });
        ASSERT_NE(source, nullptr);
        const Drawable producer = add_drawable(source, k_half_depth_vertex, k_red_fragment, true);
        producer.render->enable_depth_test();
        producer.buffer->set_needs_depth_attachment(true);
        const auto sink = make_target();
        const Drawable consumer = add_drawable(sink, k_full_vertex, k_sample_red_channel_fragment);
        consumer.render->add_binding("tex", Buffers::ShaderBinding { 1, 0, vk::DescriptorType::eCombinedImageSampler });
        consumer.render->bind_texture({ .name = "tex",
            .image = source->depth_image(),
            .sampler = Portal::Graphics::SamplerConfig {
                .mag_filter = Portal::Graphics::FilterMode::NEAREST,
                .min_filter = Portal::Graphics::FilterMode::NEAREST } });
        EXPECT_TRUE(consumer.render->samples(source));

        cycle();

        EXPECT_GE(pixel(sink, 8, 8).r, 120);
        EXPECT_LE(pixel(sink, 8, 8).r, 135);
        EXPECT_EQ(source->depth_image()->get_current_layout(), vk::ImageLayout::eShaderReadOnlyOptimal);
    }

    TEST_F(ImageTargetRenderTest, ADepthImageNotKeptStaysAnAttachmentAfterTheDraw)
    {
        const auto target = make_target({ .width = 16, .height = 16, .depth = true });
        ASSERT_NE(target, nullptr);
        EXPECT_FALSE(target->sample_depth());
        const Drawable drawable = add_drawable(target, k_half_depth_vertex, k_red_fragment, true);
        drawable.render->enable_depth_test();
        drawable.buffer->set_needs_depth_attachment(true);

        cycle();

        EXPECT_NE(target->depth_image()->get_current_layout(), vk::ImageLayout::eShaderReadOnlyOptimal);
    }

    TEST_F(ImageTargetRenderTest, SampledDepthNeedsDepth)
    {
        EXPECT_EQ(loom().create_image_target({ .width = 16, .height = 16, .sample_depth = true }), nullptr);
    }

    // =========================================================================
    // A view for each destination
    // =========================================================================

    TEST_F(ImageTargetRenderTest, TwoDestinationsOfOneProcessorDrawWithTheirOwnViews)
    {
        const auto first = make_target();
        const auto second = make_target();
        const Drawable drawable = add_drawable(first, k_view_probe_vertex, k_view_probe_fragment);
        drawable.render->add_target_image(second, drawable.buffer);
        ASSERT_TRUE(drawable.render->set_destination_view(
            { .target = first, .transform = view_showing(0.25F), .enable_depth = false, .cull = Portal::Graphics::CullMode::NONE }));
        ASSERT_TRUE(drawable.render->set_destination_view(
            { .target = second, .transform = view_showing(0.75F), .enable_depth = false, .cull = Portal::Graphics::CullMode::NONE }));

        cycle();

        EXPECT_NEAR(static_cast<int>(pixel(first, 16, 16).r), 64, 2);
        EXPECT_NEAR(static_cast<int>(pixel(second, 16, 16).r), 191, 2);
    }

    TEST_F(ImageTargetRenderTest, ADestinationWithoutItsOwnViewKeepsTheViewOfTheProcessor)
    {
        const auto shared = make_target();
        const auto own = make_target();
        const Drawable drawable = add_drawable(shared, k_view_probe_vertex, k_view_probe_fragment);
        drawable.render->add_target_image(own, drawable.buffer);
        drawable.render->set_view_transform(view_showing(0.5F), false, Portal::Graphics::CullMode::NONE);
        ASSERT_TRUE(drawable.render->set_destination_view(
            { .target = own, .transform = view_showing(0.75F), .enable_depth = false, .cull = Portal::Graphics::CullMode::NONE }));

        cycle();

        EXPECT_NEAR(static_cast<int>(pixel(shared, 16, 16).r), 128, 2);
        EXPECT_NEAR(static_cast<int>(pixel(own, 16, 16).r), 191, 2);
        EXPECT_FALSE(drawable.render->has_destination_view({ .target = shared }));
        EXPECT_TRUE(drawable.render->has_destination_view({ .target = own }));
    }

    TEST_F(ImageTargetRenderTest, ClearingADestinationViewReturnsItToTheViewOfTheProcessor)
    {
        const auto target = make_target();
        const Drawable drawable = add_drawable(target, k_view_probe_vertex, k_view_probe_fragment);
        drawable.render->set_view_transform(view_showing(0.5F), false, Portal::Graphics::CullMode::NONE);
        ASSERT_TRUE(drawable.render->set_destination_view(
            { .target = target, .transform = view_showing(0.25F), .enable_depth = false, .cull = Portal::Graphics::CullMode::NONE }));
        cycle();
        ASSERT_NEAR(static_cast<int>(pixel(target, 16, 16).r), 64, 2);

        drawable.render->clear_destination_view({ .target = target });
        cycle();

        EXPECT_NEAR(static_cast<int>(pixel(target, 16, 16).r), 128, 2);
        EXPECT_FALSE(drawable.render->has_destination_view({ .target = target }));
    }

    TEST_F(ImageTargetRenderTest, AViewSourceIsGivenThePixelSizeOfWhatItsDestinationFills)
    {
        const auto target = make_target({ .width = 32, .height = 16 });
        const Drawable drawable = add_drawable(target, k_view_probe_vertex, k_view_probe_fragment);
        drawable.render->set_target_area(target,
            { .area = Kinesis::Scissor::from(Kinesis::AABB2D { .min = { -1.F, -1.F }, .max = { 0.F, 1.F } }) });
        auto seen = std::make_shared<glm::uvec2>(0U, 0U);
        ASSERT_TRUE(drawable.render->set_destination_view(
            { .target = target,
                .source = [seen](glm::uvec2 size) {
                    *seen = size;
                    return view_showing(0.25F);
                },
                .enable_depth = false,
                .cull = Portal::Graphics::CullMode::NONE }));

        cycle();

        EXPECT_EQ(*seen, glm::uvec2(16U, 16U));
        EXPECT_NEAR(static_cast<int>(pixel(target, 8, 8).r), 64, 2);
    }

    TEST_F(ImageTargetRenderTest, ADestinationViewIsRefusedUnlessItNamesOneDestinationAndOneView)
    {
        const auto target = make_target();
        const auto stranger = make_target();
        const Drawable drawable = add_drawable(target);

        EXPECT_FALSE(drawable.render->set_destination_view({ .target = target }));
        EXPECT_FALSE(drawable.render->set_destination_view(
            { .target = target, .transform = view_showing(0.5F), .source = [](glm::uvec2) { return view_showing(0.5F); } }));
        EXPECT_FALSE(drawable.render->set_destination_view({ .target = stranger, .transform = view_showing(0.5F) }));
        EXPECT_FALSE(drawable.render->set_destination_view({ .transform = view_showing(0.5F) }));
        EXPECT_FALSE(drawable.render->has_destination_view({ .target = target }));
    }

    TEST_F(ImageTargetRenderTest, ARemovedDestinationDoesNotKeepItsView)
    {
        const auto first = make_target();
        const auto second = make_target();
        const Drawable drawable = add_drawable(first);
        drawable.render->add_target_image(second, drawable.buffer);
        ASSERT_TRUE(drawable.render->set_destination_view(
            { .target = first, .transform = view_showing(0.5F), .enable_depth = false }));

        drawable.render->remove_target_image(first, drawable.buffer);
        drawable.render->add_target_image(first, drawable.buffer);

        EXPECT_FALSE(drawable.render->has_destination_view({ .target = first }));
    }

    // =========================================================================
    // No display service
    // =========================================================================

    TEST_F(ImageTargetRenderTest, TheFlowInitializesAndDrawsIntoATargetWithoutADisplayService)
    {
        auto& registry = Registry::BackendRegistry::instance();
        auto* display = registry.get_service<Registry::Service::DisplayService>();
        registry.unregister_service<Registry::Service::DisplayService>();
        Portal::Graphics::get_render_flow().shutdown();

        const bool initialized = Portal::Graphics::get_render_flow().initialize();
        const auto target = make_target();
        add_drawable(target);
        if (initialized)
            cycle();
        const auto drawn = initialized ? pixel(target, 16, 16) : Pixel {};

        if (display)
            registry.register_service<Registry::Service::DisplayService>([display]() -> void* { return display; });

        ASSERT_TRUE(initialized);
        EXPECT_EQ(drawn.r, 255);
        EXPECT_EQ(drawn.g, 0);
    }

    // =========================================================================
    // Mip chains
    // =========================================================================

    TEST_F(ImageTargetRenderTest, AMippedTargetStillReadsBackItsFirstLevel)
    {
        const auto target = make_target({ .width = 32, .height = 32, .mip_levels = 3 });
        ASSERT_NE(target, nullptr);
        add_drawable(target);

        cycle();

        EXPECT_EQ(pixel(target, 16, 16).r, 255);
        EXPECT_EQ(pixel(target, 16, 16).g, 0);
    }

    TEST_F(ImageTargetRenderTest, ALowerLevelOfAMippedTargetHoldsTheDrawAfterTheScope)
    {
        const auto source = make_target({ .width = 32, .height = 32, .mip_levels = 3 });
        ASSERT_NE(source, nullptr);
        add_drawable(source);
        const auto sink = make_target();
        const Drawable consumer = add_drawable(sink, k_full_vertex, k_sample_lod_fragment);
        consumer.render->add_binding("tex", Buffers::ShaderBinding { 1, 0, vk::DescriptorType::eCombinedImageSampler });
        consumer.render->bind_texture({ .name = "tex", .image = source->image() });

        cycle();

        EXPECT_EQ(pixel(sink, 16, 16).r, 255);
        EXPECT_EQ(pixel(sink, 16, 16).g, 0);
        EXPECT_EQ(pixel(sink, 16, 16).b, 0);
    }

    TEST_F(ImageTargetRenderTest, AMippedTargetMakesItsChainAgainAtEveryRedraw)
    {
        const auto source = make_target({ .width = 32, .height = 32, .mip_levels = 3 });
        ASSERT_NE(source, nullptr);
        const Drawable producer = add_drawable(source);
        const auto sink = make_target();
        const Drawable consumer = add_drawable(sink, k_full_vertex, k_sample_lod_fragment);
        consumer.render->add_binding("tex", Buffers::ShaderBinding { 1, 0, vk::DescriptorType::eCombinedImageSampler });
        consumer.render->bind_texture({ .name = "tex", .image = source->image() });
        cycle();
        ASSERT_EQ(pixel(sink, 16, 16).r, 255);

        producer.render->set_fragment_shader(k_green_fragment);
        source->request_redraw();
        sink->request_redraw();
        cycle();

        EXPECT_EQ(pixel(sink, 16, 16).g, 255);
        EXPECT_EQ(pixel(sink, 16, 16).r, 0);
    }

    TEST_F(ImageTargetRenderTest, AResizedMippedTargetMakesItsChainAtTheNewSize)
    {
        const auto source = make_target({ .width = 16, .height = 16, .mip_levels = 0 });
        ASSERT_NE(source, nullptr);
        add_drawable(source);
        const auto sink = make_target();
        const Drawable consumer = add_drawable(sink, k_full_vertex, k_sample_lod_fragment);
        consumer.render->add_binding("tex", Buffers::ShaderBinding { 1, 0, vk::DescriptorType::eCombinedImageSampler });
        consumer.render->bind_texture({ .name = "tex", .image = source->image() });
        cycle();
        ASSERT_EQ(pixel(sink, 16, 16).r, 255);

        ASSERT_TRUE(source->resize(64, 64));
        ASSERT_EQ(source->image()->get_mip_levels(), 7U);
        consumer.render->bind_texture({ .name = "tex", .image = source->image() });
        sink->request_redraw();
        cycle();

        EXPECT_EQ(pixel(sink, 16, 16).r, 255);
        EXPECT_EQ(pixel(sink, 16, 16).g, 0);
    }


    // =========================================================================
    // Targets that sample other targets
    // =========================================================================

    TEST_F(ImageTargetRenderTest, ASourceThatRedrawsAsksTheTargetThatSamplesItToRedraw)
    {
        const auto source = make_target();
        const Drawable producer = add_drawable(source);
        const auto sink = make_target();
        const Drawable consumer = add_drawable(sink, k_full_vertex, k_sample_fragment);
        consumer.render->add_binding("tex", Buffers::ShaderBinding { 1, 0, vk::DescriptorType::eCombinedImageSampler });
        consumer.render->bind_texture({ .name = "tex", .image = source->image() });
        cycle();
        ASSERT_EQ(pixel(sink, 16, 16).r, 255);

        producer.render->set_fragment_shader(k_green_fragment);
        source->request_redraw();
        cycle();

        EXPECT_EQ(pixel(source, 16, 16).g, 255);
        EXPECT_EQ(pixel(sink, 16, 16).g, 255);
        EXPECT_EQ(pixel(sink, 16, 16).r, 0);
    }

    TEST_F(ImageTargetRenderTest, ARedrawReachesATargetThroughAChainOfSamplers)
    {
        const auto first = make_target();
        const Drawable producer = add_drawable(first);
        const auto second = make_target();
        const Drawable middle = add_drawable(second, k_full_vertex, k_sample_fragment);
        middle.render->add_binding("tex", Buffers::ShaderBinding { 1, 0, vk::DescriptorType::eCombinedImageSampler });
        middle.render->bind_texture({ .name = "tex", .image = first->image() });
        const auto third = make_target();
        const Drawable last = add_drawable(third, k_full_vertex, k_sample_fragment);
        last.render->add_binding("tex", Buffers::ShaderBinding { 1, 0, vk::DescriptorType::eCombinedImageSampler });
        last.render->bind_texture({ .name = "tex", .image = second->image() });
        cycle();
        ASSERT_EQ(pixel(third, 16, 16).r, 255);

        producer.render->set_fragment_shader(k_green_fragment);
        first->request_redraw();
        cycle();

        EXPECT_EQ(pixel(second, 16, 16).g, 255);
        EXPECT_EQ(pixel(third, 16, 16).g, 255);
        EXPECT_EQ(pixel(third, 16, 16).r, 0);
    }

    TEST_F(ImageTargetRenderTest, ATargetThatDoesNotSampleTheSourceStaysUntouched)
    {
        const auto source = make_target();
        add_drawable(source);
        const auto sink = make_target();
        const Drawable consumer = add_drawable(sink, k_full_vertex, k_sample_fragment);
        consumer.render->add_binding("tex", Buffers::ShaderBinding { 1, 0, vk::DescriptorType::eCombinedImageSampler });
        consumer.render->bind_texture({ .name = "tex", .image = source->image() });
        const auto stranger = make_target();
        add_drawable(stranger);
        cycle();

        source->request_redraw();
        tick();

        ASSERT_EQ(root->get_renderable_buffers().size(), 2U);
        for (const auto& info : root->get_renderable_buffers()) {
            EXPECT_NE(info.target_image, stranger);
        }
        present();
    }

    TEST_F(ImageTargetRenderTest, ATargetThatSamplesAnUnchangedSourceIsNotRedrawn)
    {
        const auto source = make_target();
        add_drawable(source);
        const auto sink = make_target();
        const Drawable consumer = add_drawable(sink, k_full_vertex, k_sample_fragment);
        consumer.render->add_binding("tex", Buffers::ShaderBinding { 1, 0, vk::DescriptorType::eCombinedImageSampler });
        consumer.render->bind_texture({ .name = "tex", .image = source->image() });
        cycle();

        tick();

        EXPECT_TRUE(root->get_renderable_buffers().empty());
        present();
    }

    TEST_F(ImageTargetRenderTest, AnInactiveTargetIsNotDrawnWhenItsSourceRedraws)
    {
        const auto source = make_target();
        add_drawable(source);
        const auto sink = make_target();
        const Drawable consumer = add_drawable(sink, k_full_vertex, k_sample_fragment);
        consumer.render->add_binding("tex", Buffers::ShaderBinding { 1, 0, vk::DescriptorType::eCombinedImageSampler });
        consumer.render->bind_texture({ .name = "tex", .image = source->image() });
        cycle();
        sink->set_active(false);

        source->request_redraw();
        tick();

        ASSERT_EQ(root->get_renderable_buffers().size(), 1U);
        EXPECT_EQ(root->get_renderable_buffers().front().target_image, source);
        present();
    }

    TEST_F(ImageTargetRenderTest, SettingDestinationsFromAWindowlessConfigUsesTheImages)
    {
        const auto first = make_target();
        const auto second = make_target();
        const Drawable drawable = add_drawable(first);

        drawable.render->set_destinations(nullptr, { second }, drawable.buffer);

        EXPECT_EQ(drawable.render->destination_count(), 1U);
        EXPECT_EQ(drawable.render->get_target_image(), second);
        EXPECT_TRUE(first->take_redraw());
    }

    TEST_F(ImageTargetRenderTest, SettingNoDestinationKeepsTheCurrentOnes)
    {
        const auto target = make_target();
        const Drawable drawable = add_drawable(target);

        drawable.render->set_destinations(nullptr, {}, drawable.buffer);

        EXPECT_EQ(drawable.render->destination_count(), 1U);
        EXPECT_EQ(drawable.render->get_target_image(), target);
    }


    // =========================================================================
    // Drawing a scope through the render flow
    // =========================================================================

    TEST_F(ImageTargetRenderTest, TheFlowDrawsARecordedSecondaryIntoATarget)
    {
        const auto target = make_target({ .width = 32, .height = 32, .clear_color = { 0.0F, 0.0F, 1.0F, 1.0F } });
        const Drawable drawable = add_drawable(target, k_full_vertex, k_left_red_fragment);
        tick();
        const auto command = drawable.buffer->get_pipeline_command(pipeline_for(drawable, target));
        ASSERT_NE(command, Portal::Graphics::INVALID_COMMAND_BUFFER);

        const auto point = Portal::Graphics::get_render_flow().draw_image_target(*target, { command }, false);

        EXPECT_TRUE(static_cast<bool>(point));
        EXPECT_EQ(pixel(target, 4, 16).r, 255);
        EXPECT_EQ(pixel(target, 28, 16).b, 255);
        EXPECT_EQ(pixel(target, 28, 16).r, 0);
        drawable.buffer->clear_pipeline_commands();
        const std::array<Portal::Graphics::CommandBufferID, 1> done { command };
        Portal::Graphics::get_shader_foundry().retire_after_submit(done);
    }

    TEST_F(ImageTargetRenderTest, ASecondScopeCompletesAfterTheFirst)
    {
        const auto target = make_target();
        auto& flow = Portal::Graphics::get_render_flow();

        const auto first = flow.draw_image_target(*target, {}, false);
        const auto second = flow.draw_image_target(*target, {}, false);

        ASSERT_TRUE(static_cast<bool>(first));
        ASSERT_TRUE(static_cast<bool>(second));
        EXPECT_EQ(second.timeline, first.timeline);
        EXPECT_GT(second.value, first.value);
    }

    TEST_F(ImageTargetRenderTest, AScopeWithNothingRecordedStillClearsTheTarget)
    {
        const auto target = make_target({ .width = 16, .height = 16, .clear_color = { 0.0F, 1.0F, 0.0F, 1.0F } });

        const auto point = Portal::Graphics::get_render_flow().draw_image_target(*target, {}, false);

        EXPECT_TRUE(static_cast<bool>(point));
        EXPECT_EQ(pixel(target, 8, 8).g, 255);
        EXPECT_EQ(pixel(target, 8, 8).r, 0);
        EXPECT_EQ(pixel(target, 8, 8).b, 0);
    }

    TEST_F(ImageTargetRenderTest, SecondaryCommandsTheFoundryDoesNotKnowAreSkipped)
    {
        const auto target = make_target({ .width = 16, .height = 16, .clear_color = { 0.0F, 1.0F, 0.0F, 1.0F } });

        const auto point = Portal::Graphics::get_render_flow().draw_image_target(
            *target, { Portal::Graphics::INVALID_COMMAND_BUFFER, 987654U }, false);

        EXPECT_TRUE(static_cast<bool>(point));
        EXPECT_EQ(pixel(target, 8, 8).g, 255);
    }

    TEST_F(ImageTargetRenderTest, APreservingTargetKeepsItsContentWhenTheFlowDrawsAgain)
    {
        const auto target = make_target({ .width = 32, .height = 32, .clear_color = { 0.0F, 0.0F, 1.0F, 1.0F }, .preserve = true });
        const Drawable drawable = add_drawable(target, k_full_vertex, k_left_red_fragment);
        tick();
        const auto command = drawable.buffer->get_pipeline_command(pipeline_for(drawable, target));
        ASSERT_NE(command, Portal::Graphics::INVALID_COMMAND_BUFFER);
        auto& flow = Portal::Graphics::get_render_flow();
        ASSERT_TRUE(static_cast<bool>(flow.draw_image_target(*target, { command }, false)));
        ASSERT_EQ(pixel(target, 4, 16).r, 255);

        ASSERT_TRUE(static_cast<bool>(flow.draw_image_target(*target, {}, false)));

        EXPECT_EQ(pixel(target, 4, 16).r, 255);
        EXPECT_EQ(pixel(target, 28, 16).b, 255);
        drawable.buffer->clear_pipeline_commands();
        const std::array<Portal::Graphics::CommandBufferID, 1> done { command };
        Portal::Graphics::get_shader_foundry().retire_after_submit(done);
    }

    TEST_F(ImageTargetRenderTest, ATargetWithDepthTakesAScopeWithOrWithoutItsDepthAttachment)
    {
        const auto target = make_target({ .width = 16, .height = 16, .depth = true, .clear_color = { 1.0F, 0.0F, 0.0F, 1.0F } });
        ASSERT_NE(target, nullptr);
        auto& flow = Portal::Graphics::get_render_flow();

        EXPECT_TRUE(static_cast<bool>(flow.draw_image_target(*target, {}, true)));
        EXPECT_EQ(pixel(target, 8, 8).r, 255);
        EXPECT_TRUE(static_cast<bool>(flow.draw_image_target(*target, {}, false)));
        EXPECT_EQ(pixel(target, 8, 8).r, 255);
    }


}
}
