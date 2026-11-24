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

        void apply(const Portal::Graphics::RenderConfig& config)
        {
            std::shared_ptr<RenderProcessor> processor;
            apply_render_config(processor, config, ShaderConfig { "unused.vert.spv" });
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
        consumer.render->bind_texture("tex", source->image());

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
        consumer.render->bind_texture("tex", source->image());
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

    TEST_F(ImageTargetRenderTest, ARenderConfigWithTwoImagesIsRefused)
    {
        const auto buffer = std::make_shared<ConfigProbeBuffer>();
        Portal::Graphics::RenderConfig config;
        config.target_images = { make_target(), make_target() };

        EXPECT_THROW(buffer->apply(config), std::invalid_argument);
    }

    TEST_F(ImageTargetRenderTest, ARenderConfigWithTheSameImageTwiceIsRefused)
    {
        const auto buffer = std::make_shared<ConfigProbeBuffer>();
        const auto target = make_target();
        Portal::Graphics::RenderConfig config;
        config.target_images = { target, target };

        EXPECT_THROW(buffer->apply(config), std::invalid_argument);
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

}
}
