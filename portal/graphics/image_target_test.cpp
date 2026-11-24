#include "gtest/gtest.h"
#include "../../support/test_metadata.hpp"

#include "../../support/vulkan_test_support.hpp"

#include "MayaFlux/Core/Backends/Graphics/Vulkan/VKImage.hpp"
#include "MayaFlux/Portal/Graphics/ImageTarget.hpp"
#include "MayaFlux/Portal/Graphics/TextureLoom.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::vulkan>() }))

namespace MayaFlux::Test {
namespace {

    using Portal::Graphics::ImageTarget;

    class ImageTargetDeviceTest : public ::testing::Test {
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

        static void TearDownTestSuite() { s_backend.reset(); }

        void SetUp() override
        {
            if (!s_backend) {
                GTEST_SKIP() << "No usable Vulkan device: " << vulkan_unavailable_reason();
            }
            loom().shutdown();
            ASSERT_TRUE(loom().initialize(s_backend));
        }

        void TearDown() override
        {
            if (s_backend) {
                loom().shutdown();
            }
        }

        static Portal::Graphics::TextureLoom& loom() { return Portal::Graphics::get_texture_manager(); }

        static inline std::shared_ptr<Core::VulkanBackend> s_backend;
    };

}

TEST_F(ImageTargetDeviceTest, ATargetReportsItsSizeAndTheFormatOfItsImage)
{
    const auto target = loom().create_image_target({ .width = 64, .height = 32 });

    ASSERT_NE(target, nullptr);
    EXPECT_EQ(target->width(), 64U);
    EXPECT_EQ(target->height(), 32U);
    ASSERT_NE(target->image(), nullptr);
    EXPECT_EQ(target->color_format(), target->image()->get_format());
}

TEST_F(ImageTargetDeviceTest, ATargetRestsInShaderReadLayoutSoItCanBeSampled)
{
    const auto target = loom().create_image_target({ .width = 16, .height = 16 });

    ASSERT_NE(target, nullptr);
    EXPECT_EQ(target->image()->get_current_layout(), vk::ImageLayout::eShaderReadOnlyOptimal);
}

TEST_F(ImageTargetDeviceTest, ADepthImageExistsOnlyWhenAsked)
{
    const auto without = loom().create_image_target({ .width = 8, .height = 8 });
    const auto with = loom().create_image_target({ .width = 8, .height = 8, .depth = true });

    ASSERT_NE(without, nullptr);
    ASSERT_NE(with, nullptr);
    EXPECT_EQ(without->depth_image(), nullptr);
    EXPECT_EQ(without->depth_format(), vk::Format::eUndefined);
    ASSERT_NE(with->depth_image(), nullptr);
    EXPECT_EQ(with->depth_format(), with->depth_image()->get_format());
}

TEST_F(ImageTargetDeviceTest, ASizeWithNoPixelsMakesNoTarget)
{
    EXPECT_EQ(loom().create_image_target({ .width = 0, .height = 8 }), nullptr);
    EXPECT_EQ(loom().create_image_target({ .width = 8, .height = 0 }), nullptr);
}

TEST_F(ImageTargetDeviceTest, ANewTargetDrawsOnceThenWaitsForARequest)
{
    const auto target = loom().create_image_target({ .width = 8, .height = 8 });
    ASSERT_NE(target, nullptr);

    EXPECT_TRUE(target->take_redraw());
    EXPECT_FALSE(target->take_redraw());

    target->request_redraw();
    EXPECT_TRUE(target->take_redraw());
    EXPECT_FALSE(target->take_redraw());
}

TEST_F(ImageTargetDeviceTest, AContinuousTargetDrawsEveryCycle)
{
    const auto target = loom().create_image_target({ .width = 8, .height = 8, .continuous = true });
    ASSERT_NE(target, nullptr);

    EXPECT_TRUE(target->take_redraw());
    EXPECT_TRUE(target->take_redraw());

    target->set_continuous(false);
    EXPECT_FALSE(target->take_redraw());
}

TEST_F(ImageTargetDeviceTest, ChangingTheClearColorAsksForARedraw)
{
    const auto target = loom().create_image_target({ .width = 8, .height = 8, .clear_color = { 0.1F, 0.2F, 0.3F, 0.4F } });
    ASSERT_NE(target, nullptr);
    EXPECT_TRUE(target->take_redraw());

    EXPECT_EQ(target->clear_color(), (std::array<float, 4> { 0.1F, 0.2F, 0.3F, 0.4F }));

    target->set_clear_color({ 1.0F, 0.0F, 0.0F, 1.0F });
    EXPECT_EQ(target->clear_color(), (std::array<float, 4> { 1.0F, 0.0F, 0.0F, 1.0F }));
    EXPECT_TRUE(target->take_redraw());
}

TEST_F(ImageTargetDeviceTest, ATargetIsActiveUntilToldOtherwise)
{
    const auto target = loom().create_image_target({ .width = 8, .height = 8 });
    ASSERT_NE(target, nullptr);

    EXPECT_TRUE(target->active());
    target->set_active(false);
    EXPECT_FALSE(target->active());
}

TEST_F(ImageTargetDeviceTest, TargetsAreIndependentOfEachOther)
{
    const auto a = loom().create_image_target({ .width = 8, .height = 8 });
    const auto b = loom().create_image_target({ .width = 8, .height = 8 });
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);

    EXPECT_NE(a->image(), b->image());
    EXPECT_TRUE(a->take_redraw());
    EXPECT_TRUE(b->take_redraw());
}

}
