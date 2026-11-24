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

    constexpr const char* k_right_green_fragment = R"(#version 460
layout(location = 0) out vec4 color;
void main() { if (gl_FragCoord.x < 16.0) discard; color = vec4(0.0, 1.0, 0.0, 1.0); }
)";

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
        consumer.render->bind_texture("tex", red->image());
        cycle();
        ASSERT_EQ(pixel(sink_a, 16, 16).r, 255);
        ASSERT_EQ(pixel(sink_b, 16, 16).r, 255);

        consumer.render->bind_texture("tex", green->image());
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
        consumer.render->bind_texture("tex", source->image());
        cycle();
        ASSERT_EQ(pixel(sink, 16, 16).r, 255);
        const auto old_image = source->image();

        ASSERT_TRUE(source->resize(32, 32));
        EXPECT_FALSE(old_image->is_initialized());
        consumer.render->bind_texture("tex", source->image());
        sink->set_clear_color({ 0.0F, 0.0F, 1.0F, 1.0F });
        cycle();

        EXPECT_EQ(pixel(sink, 16, 16).r, 255);
        EXPECT_EQ(pixel(sink, 16, 16).b, 0);
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
        consumer.render->bind_texture("tex", source->image());

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
        consumer.render->bind_texture("tex", source->image());
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
        consumer.render->bind_texture("tex", source->image());
        cycle();
        ASSERT_EQ(pixel(sink, 16, 16).r, 255);

        ASSERT_TRUE(source->resize(64, 64));
        ASSERT_EQ(source->image()->get_mip_levels(), 7U);
        consumer.render->bind_texture("tex", source->image());
        sink->request_redraw();
        cycle();

        EXPECT_EQ(pixel(sink, 16, 16).r, 255);
        EXPECT_EQ(pixel(sink, 16, 16).g, 0);
    }


}
}
