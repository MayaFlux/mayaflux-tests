#include "gtest/gtest.h"
#include "../support/test_metadata.hpp"

#include "../support/vulkan_test_support.hpp"

#include "MayaFlux/Buffers/Shaders/RenderProcessor.hpp"
#include "MayaFlux/Portal/Graphics/SamplerForge.hpp"
#include "MayaFlux/Portal/Graphics/ShaderFoundry.hpp"
#include "MayaFlux/Portal/Graphics/TextureLoom.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::vulkan>() }))

namespace MayaFlux::Test {
namespace {

    using Buffers::RenderProcessor;
    using Buffers::ShaderBinding;
    using Buffers::ShaderConfig;
    using Portal::Graphics::AddressMode;
    using Portal::Graphics::FilterMode;
    using Portal::Graphics::SampledTexture;
    using Portal::Graphics::SamplerConfig;
    using Portal::Graphics::SamplerForge;

    constexpr const char* k_vertex = R"(#version 460
void main() {
    vec2 p[3] = vec2[3](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));
    gl_Position = vec4(p[gl_VertexIndex], 0.0, 1.0);
}
)";

    SamplerConfig nearest_clamped()
    {
        return { .mag_filter = FilterMode::NEAREST, .min_filter = FilterMode::NEAREST, .address_mode_u = AddressMode::CLAMP_TO_EDGE };
    }

    class RenderProcessorSamplerTest : public VulkanDeviceTest {
    protected:
        static void SetUpTestSuite()
        {
            VulkanDeviceTest::SetUpTestSuite();
            if (!vulkan_unavailable_reason().empty())
                return;

            s_backend_ref = std::shared_ptr<Core::VulkanBackend>(&backend(), [](Core::VulkanBackend*) { });
            auto& foundry = Portal::Graphics::get_shader_foundry();
            foundry.shutdown();
            s_foundry_ready = foundry.initialize(s_backend_ref);
            if (!s_foundry_ready)
                return;

            auto& loom = Portal::Graphics::get_texture_manager();
            loom.shutdown();
            s_texture_ready = loom.initialize(s_backend_ref);
        }

        static void TearDownTestSuite()
        {
            SamplerForge::instance().shutdown();
            if (s_texture_ready)
                Portal::Graphics::get_texture_manager().shutdown();
            if (s_foundry_ready)
                Portal::Graphics::get_shader_foundry().shutdown();
            s_texture_ready = false;
            s_foundry_ready = false;
            s_backend_ref.reset();
            VulkanDeviceTest::TearDownTestSuite();
        }

        void SetUp() override
        {
            VulkanDeviceTest::SetUp();
            if (IsSkipped())
                return;
            ASSERT_TRUE(s_foundry_ready);
            ASSERT_TRUE(s_texture_ready);

            SamplerForge::instance().shutdown();
            ASSERT_TRUE(SamplerForge::instance().initialize(s_backend_ref));

            ShaderConfig shader { k_vertex };
            shader.stage = Portal::Graphics::ShaderStage::VERTEX;
            shader.bindings["map"] = ShaderBinding(0, 2, vk::DescriptorType::eCombinedImageSampler);
            render = std::make_shared<RenderProcessor>(shader);

            image = Portal::Graphics::get_texture_manager().create_2d(4, 4, Portal::Graphics::ImageFormat::RGBA8);
            ASSERT_NE(image, nullptr);
        }

        void TearDown() override
        {
            render.reset();
            image.reset();
            SamplerForge::instance().shutdown();
        }

        static size_t cached() { return SamplerForge::instance().get_sampler_count(); }

        std::shared_ptr<RenderProcessor> render;
        std::shared_ptr<Core::VKImage> image;

    private:
        static inline std::shared_ptr<Core::VulkanBackend> s_backend_ref;
        static inline bool s_foundry_ready {};
        static inline bool s_texture_ready {};
    };

    TEST_F(RenderProcessorSamplerTest, ABindWithoutASamplerCreatesTheDefault)
    {
        render->bind_texture(2, image);

        EXPECT_EQ(cached(), 1U);
    }

    TEST_F(RenderProcessorSamplerTest, ABindWithAConfigCreatesThatSampler)
    {
        render->bind_texture(2, image, nearest_clamped());

        EXPECT_EQ(cached(), 1U);
        EXPECT_EQ(SamplerForge::instance().get_or_create(nearest_clamped()), SamplerForge::instance().get_or_create(nearest_clamped()));
        EXPECT_EQ(cached(), 1U);
    }

    TEST_F(RenderProcessorSamplerTest, ABindingKeepsItsChosenSamplerWhenBoundAgainWithoutOne)
    {
        render->bind_texture(2, image, nearest_clamped());
        ASSERT_EQ(cached(), 1U);

        render->bind_texture(2, image);

        EXPECT_EQ(cached(), 1U);
    }

    TEST_F(RenderProcessorSamplerTest, ABindingWithoutAChoiceGetsTheDefaultWhileAnotherKeepsItsOwn)
    {
        render->bind_texture(2, image, nearest_clamped());
        render->bind_texture(3, image);

        EXPECT_EQ(cached(), 2U);
    }

    TEST_F(RenderProcessorSamplerTest, ASamplerChosenBeforeTheTextureExistsAppliesToTheFirstBind)
    {
        render->set_binding_sampler(2, nearest_clamped());
        ASSERT_EQ(cached(), 1U);

        render->bind_texture(2, image);

        EXPECT_EQ(cached(), 1U);
    }

    TEST_F(RenderProcessorSamplerTest, TheNamedFormsReachTheBindingTheNameIsDeclaredAt)
    {
        render->bind_texture("map", image, nearest_clamped());
        ASSERT_EQ(cached(), 1U);

        render->bind_texture(2, image);

        EXPECT_EQ(cached(), 1U);
    }

    TEST_F(RenderProcessorSamplerTest, ANamedBindToAnUnknownDescriptorChoosesNothing)
    {
        render->set_binding_sampler("missing", nearest_clamped());

        EXPECT_EQ(cached(), 0U);
    }

    TEST_F(RenderProcessorSamplerTest, ASampledTextureWithASamplerBindsItAndKeepsItForTheBinding)
    {
        render->bind_texture(SampledTexture { .name = "map", .image = image, .sampler = nearest_clamped() });
        ASSERT_EQ(cached(), 1U);

        render->bind_texture(2, image);

        EXPECT_EQ(cached(), 1U);
    }

    TEST_F(RenderProcessorSamplerTest, ASampledTextureWithoutASamplerGetsTheDefault)
    {
        render->bind_texture(SampledTexture { .name = "map", .image = image });

        EXPECT_EQ(cached(), 1U);
        EXPECT_EQ(SamplerForge::instance().get_or_create({ .enable_mipmaps = true, .border_color = Portal::Graphics::BorderColor::OPAQUE_BLACK }),
            SamplerForge::instance().get_or_create({ .enable_mipmaps = true, .border_color = Portal::Graphics::BorderColor::OPAQUE_BLACK }));
        EXPECT_EQ(cached(), 1U);
    }

    TEST_F(RenderProcessorSamplerTest, ChangingTheSamplerOfABoundBindingCreatesItAndKeepsTheTexture)
    {
        render->bind_texture(2, image);
        ASSERT_EQ(cached(), 1U);

        render->set_binding_sampler(2, nearest_clamped());

        EXPECT_EQ(cached(), 2U);

        render->bind_texture(2, image);
        EXPECT_EQ(cached(), 2U);
    }

} // namespace
} // namespace MayaFlux::Test
