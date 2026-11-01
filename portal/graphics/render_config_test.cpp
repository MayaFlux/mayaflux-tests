#include "gtest/gtest.h"

#include "MayaFlux/Portal/Graphics/GraphicsUtils.hpp"

namespace MayaFlux::Test {
namespace {

    using Portal::Graphics::AddressMode;
    using Portal::Graphics::FilterMode;
    using Portal::Graphics::RenderConfig;
    using Portal::Graphics::SampledTexture;
    using Portal::Graphics::SamplerConfig;

    SamplerConfig nearest_clamped()
    {
        return { .mag_filter = FilterMode::NEAREST, .min_filter = FilterMode::NEAREST, .address_mode_u = AddressMode::CLAMP_TO_EDGE };
    }

    TEST(SampledTextureTest, ANameAndAnImageLeaveTheSamplerUnset)
    {
        const SampledTexture entry { .name = "text", .image = nullptr };

        EXPECT_EQ(entry.name, "text");
        EXPECT_EQ(entry.image, nullptr);
        EXPECT_FALSE(entry.sampler.has_value());
    }

    TEST(SampledTextureTest, ANameAloneIsEnough)
    {
        const SampledTexture entry { .name = "text" };

        EXPECT_EQ(entry.name, "text");
        EXPECT_EQ(entry.image, nullptr);
        EXPECT_FALSE(entry.sampler.has_value());
    }

    TEST(SampledTextureTest, ADesignatedInitializerCanSetTheSamplerAndSkipTheImage)
    {
        const SampledTexture entry { .name = "map", .sampler = nearest_clamped() };

        EXPECT_EQ(entry.name, "map");
        EXPECT_EQ(entry.image, nullptr);
        ASSERT_TRUE(entry.sampler.has_value());
        EXPECT_TRUE(*entry.sampler == nearest_clamped());
    }

    TEST(RenderConfigSampledTextureTest, ABracedListDeclaresEachTextureWithoutASampler)
    {
        const RenderConfig config { .additional_textures = { { .name = "first" }, { .name = "second" } } };

        ASSERT_EQ(config.additional_textures.size(), 2U);
        EXPECT_EQ(config.additional_textures[0].name, "first");
        EXPECT_EQ(config.additional_textures[1].name, "second");
        EXPECT_FALSE(config.additional_textures[0].sampler.has_value());
        EXPECT_FALSE(config.default_sampler.has_value());
    }

    TEST(RenderConfigSampledTextureTest, AnEntryCanCarryItsOwnSamplerInsideTheList)
    {
        const RenderConfig config { .additional_textures = { { .name = "text" }, { .name = "map", .sampler = nearest_clamped() } } };

        ASSERT_EQ(config.additional_textures.size(), 2U);
        EXPECT_FALSE(config.additional_textures[0].sampler.has_value());
        ASSERT_TRUE(config.additional_textures[1].sampler.has_value());
        EXPECT_EQ(config.additional_textures[1].sampler->mag_filter, FilterMode::NEAREST);
    }

    TEST(RenderConfigEqualityTest, TwoDefaultConfigsAreEqual)
    {
        EXPECT_TRUE(RenderConfig {} == RenderConfig {});
    }

    TEST(RenderConfigEqualityTest, ConfigsWithTheSameTexturesAndSamplersAreEqual)
    {
        const RenderConfig a { .additional_textures = { { .name = "map", .sampler = nearest_clamped() } }, .default_sampler = nearest_clamped() };
        const RenderConfig b { .additional_textures = { { .name = "map", .sampler = nearest_clamped() } }, .default_sampler = nearest_clamped() };

        EXPECT_TRUE(a == b);
    }

    TEST(RenderConfigEqualityTest, ATextureNamedDifferentlyMakesConfigsUnequal)
    {
        const RenderConfig a { .additional_textures = { { .name = "one" } } };
        const RenderConfig b { .additional_textures = { { .name = "two" } } };

        EXPECT_FALSE(a == b);
    }

    TEST(RenderConfigEqualityTest, ASamplerOnATextureMakesConfigsUnequal)
    {
        const RenderConfig bare { .additional_textures = { { .name = "map" } } };
        const RenderConfig sampled { .additional_textures = { { .name = "map", .sampler = nearest_clamped() } } };

        EXPECT_FALSE(bare == sampled);
    }

    TEST(RenderConfigEqualityTest, ADifferentSamplerOnATextureMakesConfigsUnequal)
    {
        SamplerConfig other = nearest_clamped();
        other.address_mode_v = AddressMode::MIRRORED_REPEAT;
        const RenderConfig a { .additional_textures = { { .name = "map", .sampler = nearest_clamped() } } };
        const RenderConfig b { .additional_textures = { { .name = "map", .sampler = other } } };

        EXPECT_FALSE(a == b);
    }

    TEST(RenderConfigEqualityTest, ADefaultSamplerMakesConfigsUnequal)
    {
        const RenderConfig bare;
        const RenderConfig sampled { .default_sampler = nearest_clamped() };

        EXPECT_FALSE(bare == sampled);
    }

} // namespace
} // namespace MayaFlux::Test
