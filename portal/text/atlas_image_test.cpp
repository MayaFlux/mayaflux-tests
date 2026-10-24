#include "gtest/gtest.h"

#include "../../support/text_test_support.hpp"
#include "../../support/vulkan_test_support.hpp"

#include "MayaFlux/Core/Backends/Graphics/Vulkan/VKImage.hpp"
#include "MayaFlux/Portal/Graphics/TextureLoom.hpp"
#include "MayaFlux/Portal/Text/AtlasImage.hpp"
#include "MayaFlux/Portal/Text/TypeSetter.hpp"

namespace MayaFlux::Test {
namespace {

    using Portal::Text::AtlasImage;
    using Portal::Text::GlyphAtlas;

    constexpr uint32_t k_pixel_size = 24;
    constexpr std::string_view k_long_text = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";

    class AtlasImageTest : public VulkanDeviceTest {
    protected:
        void SetUp() override
        {
            VulkanDeviceTest::SetUp();
            if (IsSkipped()) {
                return;
            }

            Portal::Graphics::get_texture_manager().shutdown();
            auto borrowed_backend = std::shared_ptr<Core::VulkanBackend>(&backend(), [](Core::VulkanBackend*) {});
            ASSERT_TRUE(Portal::Graphics::get_texture_manager().initialize(borrowed_backend));

            auto& foundry = Portal::Text::TypeFaceFoundry::instance();
            m_owns_library = !foundry.is_initialized();
            ASSERT_TRUE(foundry.initialize());

            const auto& path = system_font_path();
            ASSERT_TRUE(path.has_value()) << "no system font could be resolved on this host";
            ASSERT_TRUE(face.load(*path)) << *path;
        }

        void TearDown() override
        {
            face.unload();
            if (m_owns_library) {
                Portal::Text::TypeFaceFoundry::instance().shutdown();
            }
            Portal::Graphics::get_texture_manager().shutdown();
            VulkanDeviceTest::TearDown();
        }

        Portal::Text::FontFace face;

    private:
        bool m_owns_library { false };
    };

    TEST_F(AtlasImageTest, TheFirstSyncCreatesAnImageTheSizeOfTheAtlas)
    {
        GlyphAtlas atlas(face, k_pixel_size, 128);
        AtlasImage image;

        const auto synced = image.sync(atlas);

        ASSERT_NE(synced, nullptr);
        EXPECT_EQ(synced->get_width(), 128U);
        EXPECT_EQ(synced->get_height(), 128U);
        EXPECT_EQ(image.generation(), 1U);
        EXPECT_EQ(image.image(), synced);
    }

    TEST_F(AtlasImageTest, ASyncWithNothingNewKeepsTheImageAndTheGeneration)
    {
        GlyphAtlas atlas(face, k_pixel_size, 128);
        AtlasImage image;
        const auto first = image.sync(atlas);

        const auto second = image.sync(atlas);

        EXPECT_EQ(second, first);
        EXPECT_EQ(image.generation(), 1U);
    }

    TEST_F(AtlasImageTest, NewGlyphsAreUploadedIntoTheSameImageAndClearTheDirtyFlag)
    {
        GlyphAtlas atlas(face, k_pixel_size, 128);
        AtlasImage image;
        const auto first = image.sync(atlas);

        ASSERT_NE(atlas.get_or_rasterize(static_cast<FT_ULong>('A')), nullptr);
        ASSERT_TRUE(atlas.is_dirty());
        const auto second = image.sync(atlas);

        EXPECT_EQ(second, first);
        EXPECT_EQ(image.generation(), 1U);
        EXPECT_FALSE(atlas.is_dirty());
    }

    TEST_F(AtlasImageTest, AGrownAtlasReplacesTheImageAndRaisesTheGeneration)
    {
        GlyphAtlas atlas(face, k_pixel_size, 32);
        AtlasImage image;
        const auto first = image.sync(atlas);
        ASSERT_NE(first, nullptr);

        (void)Portal::Text::lay_out(k_long_text, atlas);
        ASSERT_GT(atlas.atlas_size(), 32U);
        const auto second = image.sync(atlas);

        ASSERT_NE(second, nullptr);
        EXPECT_NE(second, first);
        EXPECT_EQ(second->get_width(), atlas.atlas_size());
        EXPECT_EQ(image.generation(), 2U);
    }

    TEST_F(AtlasImageTest, ADifferentAtlasReplacesTheImage)
    {
        GlyphAtlas first_atlas(face, k_pixel_size, 128);
        GlyphAtlas second_atlas(face, k_pixel_size, 128);
        AtlasImage image;
        const auto first = image.sync(first_atlas);

        const auto second = image.sync(second_atlas);

        EXPECT_NE(second, first);
        EXPECT_EQ(image.generation(), 2U);
    }

} // namespace
} // namespace MayaFlux::Test
