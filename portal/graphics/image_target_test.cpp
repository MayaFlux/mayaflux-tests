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

TEST_F(ImageTargetDeviceTest, ATargetClearsAtEveryScopeUnlessItPreserves)
{
    const auto clearing = loom().create_image_target({ .width = 8, .height = 8 });
    const auto preserving = loom().create_image_target({ .width = 8, .height = 8, .preserve = true });
    ASSERT_NE(clearing, nullptr);
    ASSERT_NE(preserving, nullptr);

    EXPECT_FALSE(clearing->preserve());
    EXPECT_TRUE(preserving->preserve());
    for (int i = 0; i < 3; ++i) {
        EXPECT_TRUE(clearing->take_clear()) << "scope " << i;
    }

    clearing->set_preserve(true);
    preserving->set_preserve(false);
    EXPECT_TRUE(clearing->preserve());
    EXPECT_FALSE(preserving->preserve());
    EXPECT_TRUE(preserving->take_clear());
}

TEST_F(ImageTargetDeviceTest, APreservingTargetClearsOnceAtFirstThenOnlyWhenAsked)
{
    const auto target = loom().create_image_target({ .width = 8, .height = 8, .preserve = true });
    ASSERT_NE(target, nullptr);

    EXPECT_TRUE(target->take_clear());
    EXPECT_FALSE(target->take_clear());

    target->request_clear();
    EXPECT_TRUE(target->take_clear());
    EXPECT_FALSE(target->take_clear());
}

TEST_F(ImageTargetDeviceTest, ARedrawRequestDoesNotAskAPreservingTargetToClear)
{
    const auto target = loom().create_image_target({ .width = 8, .height = 8, .preserve = true });
    ASSERT_NE(target, nullptr);
    ASSERT_TRUE(target->take_clear());
    ASSERT_TRUE(target->take_redraw());

    target->request_redraw();

    EXPECT_TRUE(target->take_redraw());
    EXPECT_FALSE(target->take_clear());
}

TEST_F(ImageTargetDeviceTest, AClearRequestAlsoAsksForARedrawAndKeepsItUntilTaken)
{
    const auto target = loom().create_image_target({ .width = 8, .height = 8, .preserve = true });
    ASSERT_NE(target, nullptr);
    ASSERT_TRUE(target->take_redraw());
    ASSERT_TRUE(target->take_clear());

    target->request_clear();

    EXPECT_TRUE(target->take_redraw());
    EXPECT_FALSE(target->take_redraw());
    EXPECT_TRUE(target->take_clear());
    EXPECT_FALSE(target->take_clear());
}

TEST_F(ImageTargetDeviceTest, ChangingTheClearColorAsksAPreservingTargetToClear)
{
    const auto target = loom().create_image_target({ .width = 8, .height = 8, .preserve = true });
    ASSERT_NE(target, nullptr);
    ASSERT_TRUE(target->take_clear());
    ASSERT_FALSE(target->take_clear());

    target->set_clear_color({ 0.5F, 0.5F, 0.5F, 1.0F });

    EXPECT_TRUE(target->take_clear());
}

TEST_F(ImageTargetDeviceTest, ATargetMakesAMipChainOnlyWhenAsked)
{
    const auto flat = loom().create_image_target({ .width = 16, .height = 16 });
    const auto three = loom().create_image_target({ .width = 16, .height = 16, .mip_levels = 3 });
    const auto full = loom().create_image_target({ .width = 16, .height = 16, .mip_levels = 0 });
    ASSERT_NE(flat, nullptr);
    ASSERT_NE(three, nullptr);
    ASSERT_NE(full, nullptr);

    EXPECT_EQ(flat->image()->get_mip_levels(), 1U);
    EXPECT_EQ(three->image()->get_mip_levels(), 3U);
    EXPECT_EQ(full->image()->get_mip_levels(), 5U);
    EXPECT_EQ(three->image()->get_current_layout(), vk::ImageLayout::eShaderReadOnlyOptimal);
}

TEST_F(ImageTargetDeviceTest, ATargetKeepsTheMipFilterItWasMadeWith)
{
    const auto linear = loom().create_image_target({ .width = 8, .height = 8, .mip_levels = 2 });
    const auto nearest = loom().create_image_target({ .width = 8, .height = 8, .mip_levels = 2, .mip_filter = Portal::Graphics::FilterMode::NEAREST });
    ASSERT_NE(linear, nullptr);
    ASSERT_NE(nearest, nullptr);

    EXPECT_EQ(linear->mip_filter(), Portal::Graphics::FilterMode::LINEAR);
    EXPECT_EQ(nearest->mip_filter(), Portal::Graphics::FilterMode::NEAREST);
}

TEST_F(ImageTargetDeviceTest, ResizingChangesTheSizeOfTheTargetAndItsImages)
{
    const auto target = loom().create_image_target({ .width = 16, .height = 16, .depth = true });
    ASSERT_NE(target, nullptr);

    ASSERT_TRUE(target->resize(32, 8));

    EXPECT_EQ(target->width(), 32U);
    EXPECT_EQ(target->height(), 8U);
    EXPECT_EQ(target->image()->get_width(), 32U);
    EXPECT_EQ(target->image()->get_height(), 8U);
    EXPECT_EQ(target->depth_image()->get_width(), 32U);
    EXPECT_EQ(target->depth_image()->get_height(), 8U);
}

TEST_F(ImageTargetDeviceTest, ResizingMakesNewImagesAndReleasesTheOldOnes)
{
    const auto target = loom().create_image_target({ .width = 16, .height = 16, .depth = true });
    ASSERT_NE(target, nullptr);
    const auto old_color = target->image();
    const auto old_depth = target->depth_image();

    ASSERT_TRUE(target->resize(32, 32));

    EXPECT_NE(target->image(), old_color);
    EXPECT_NE(target->depth_image(), old_depth);
    EXPECT_FALSE(old_color->is_initialized());
    EXPECT_FALSE(old_depth->is_initialized());
    EXPECT_TRUE(target->image()->is_initialized());
    EXPECT_TRUE(target->depth_image()->is_initialized());
}

TEST_F(ImageTargetDeviceTest, AResizedTargetKeepsItsFormatsAndRestsInShaderReadLayout)
{
    const auto target = loom().create_image_target({ .width = 16, .height = 16, .format = Portal::Graphics::ImageFormat::RGBA16F, .depth = true });
    ASSERT_NE(target, nullptr);
    const auto color_format = target->color_format();
    const auto depth_format = target->depth_format();

    ASSERT_TRUE(target->resize(24, 24));

    EXPECT_EQ(target->color_format(), color_format);
    EXPECT_EQ(target->image()->get_format(), color_format);
    EXPECT_EQ(target->depth_format(), depth_format);
    EXPECT_EQ(target->depth_image()->get_format(), depth_format);
    EXPECT_EQ(target->image()->get_current_layout(), vk::ImageLayout::eShaderReadOnlyOptimal);
}

TEST_F(ImageTargetDeviceTest, ResizingAsksForAClearAndARedraw)
{
    const auto target = loom().create_image_target({ .width = 16, .height = 16, .preserve = true });
    ASSERT_NE(target, nullptr);
    ASSERT_TRUE(target->take_redraw());
    ASSERT_TRUE(target->take_clear());
    ASSERT_FALSE(target->take_redraw());

    ASSERT_TRUE(target->resize(8, 8));

    EXPECT_TRUE(target->take_redraw());
    EXPECT_TRUE(target->take_clear());
}

TEST_F(ImageTargetDeviceTest, ResizingToTheSameSizeChangesNothing)
{
    const auto target = loom().create_image_target({ .width = 16, .height = 16, .preserve = true });
    ASSERT_NE(target, nullptr);
    ASSERT_TRUE(target->take_redraw());
    ASSERT_TRUE(target->take_clear());
    const auto image = target->image();

    EXPECT_TRUE(target->resize(16, 16));

    EXPECT_EQ(target->image(), image);
    EXPECT_FALSE(target->take_redraw());
    EXPECT_FALSE(target->take_clear());
}

TEST_F(ImageTargetDeviceTest, ResizingToNoPixelsIsRefusedAndLeavesTheTargetAlone)
{
    const auto target = loom().create_image_target({ .width = 16, .height = 16, .depth = true });
    ASSERT_NE(target, nullptr);
    const auto color = target->image();
    const auto depth = target->depth_image();

    EXPECT_FALSE(target->resize(0, 8));
    EXPECT_FALSE(target->resize(8, 0));

    EXPECT_EQ(target->width(), 16U);
    EXPECT_EQ(target->height(), 16U);
    EXPECT_EQ(target->image(), color);
    EXPECT_EQ(target->depth_image(), depth);
    EXPECT_TRUE(color->is_initialized());
}

TEST_F(ImageTargetDeviceTest, AResizedTargetWithoutDepthStaysWithoutDepth)
{
    const auto target = loom().create_image_target({ .width = 16, .height = 16 });
    ASSERT_NE(target, nullptr);

    ASSERT_TRUE(target->resize(8, 8));

    EXPECT_EQ(target->depth_image(), nullptr);
    EXPECT_EQ(target->depth_format(), vk::Format::eUndefined);
}

TEST_F(ImageTargetDeviceTest, ResizingKeepsAFixedLevelCountAndRecomputesAFullChain)
{
    const auto fixed = loom().create_image_target({ .width = 16, .height = 16, .mip_levels = 3 });
    const auto full = loom().create_image_target({ .width = 16, .height = 16, .mip_levels = 0 });
    ASSERT_NE(fixed, nullptr);
    ASSERT_NE(full, nullptr);
    ASSERT_EQ(full->image()->get_mip_levels(), 5U);

    ASSERT_TRUE(fixed->resize(64, 64));
    ASSERT_TRUE(full->resize(64, 64));

    EXPECT_EQ(fixed->image()->get_mip_levels(), 3U);
    EXPECT_EQ(full->image()->get_mip_levels(), 7U);
}

TEST_F(ImageTargetDeviceTest, ResizingBackAndForthKeepsWorking)
{
    const auto target = loom().create_image_target({ .width = 16, .height = 16, .depth = true });
    ASSERT_NE(target, nullptr);

    for (uint32_t size : { 32U, 8U, 16U, 64U, 16U }) {
        ASSERT_TRUE(target->resize(size, size)) << size;
        EXPECT_EQ(target->image()->get_width(), size);
        EXPECT_EQ(target->depth_image()->get_height(), size);
        EXPECT_TRUE(target->image()->is_initialized());
    }
}

TEST_F(ImageTargetDeviceTest, ATargetThatIsDestroyedReleasesItsImages)
{
    std::shared_ptr<Core::VKImage> color;
    std::shared_ptr<Core::VKImage> depth;
    {
        const auto target = loom().create_image_target({ .width = 8, .height = 8, .depth = true });
        ASSERT_NE(target, nullptr);
        color = target->image();
        depth = target->depth_image();
        ASSERT_TRUE(color->is_initialized());
    }

    EXPECT_FALSE(color->is_initialized());
    EXPECT_FALSE(depth->is_initialized());
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
