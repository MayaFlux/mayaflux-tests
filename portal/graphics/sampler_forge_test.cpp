#include "gtest/gtest.h"
#include "../../support/test_metadata.hpp"

#include <string_view>

#include "../../support/vulkan_test_support.hpp"

#include "MayaFlux/Portal/Graphics/SamplerForge.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::vulkan>() }))

namespace MayaFlux::Test {
namespace {

    using Portal::Graphics::AddressMode;
    using Portal::Graphics::BorderColor;
    using Portal::Graphics::FilterMode;
    using Portal::Graphics::SamplerConfig;
    using Portal::Graphics::SamplerForge;

    SamplerForge& forge()
    {
        return Portal::Graphics::get_sampler_factory();
    }

    class SamplerForgeTest : public ::testing::Test {
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
            forge().shutdown();
        }

        void TearDown() override
        {
            forge().shutdown();
        }

        static const std::shared_ptr<Core::VulkanBackend>& backend() { return s_backend; }

    private:
        static inline std::shared_ptr<Core::VulkanBackend> s_backend;
    };

    // ------------------------------------------------------------------
    // Lifecycle
    // ------------------------------------------------------------------

    TEST_F(SamplerForgeTest, UninitializedForgeHandsOutNoSamplers)
    {
        EXPECT_FALSE(forge().is_initialized());
        EXPECT_FALSE(forge().get_or_create(SamplerConfig {}));
        EXPECT_FALSE(forge().get_default_linear());
        EXPECT_FALSE(forge().get_default_nearest());
        EXPECT_FALSE(forge().get_anisotropic());
        EXPECT_EQ(forge().get_sampler_count(), 0U);
    }

    TEST_F(SamplerForgeTest, DestroyingBeforeInitializationIsIgnored)
    {
        EXPECT_NO_THROW(forge().destroy_sampler(vk::Sampler {}));
        EXPECT_EQ(forge().get_sampler_count(), 0U);
    }

    TEST_F(SamplerForgeTest, InitializeRejectsANullBackend)
    {
        EXPECT_FALSE(forge().initialize(nullptr));
        EXPECT_FALSE(forge().is_initialized());
    }

    TEST_F(SamplerForgeTest, InitializeMakesTheForgeReadyWithAnEmptyCache)
    {
        ASSERT_TRUE(forge().initialize(backend()));

        EXPECT_TRUE(forge().is_initialized());
        EXPECT_EQ(forge().get_sampler_count(), 0U);
    }

    TEST_F(SamplerForgeTest, InitializingAgainKeepsTheCacheAndReportsSuccess)
    {
        ASSERT_TRUE(forge().initialize(backend()));
        const vk::Sampler sampler = forge().get_default_linear();
        ASSERT_TRUE(sampler);

        EXPECT_TRUE(forge().initialize(backend()));

        EXPECT_TRUE(forge().is_initialized());
        EXPECT_EQ(forge().get_sampler_count(), 1U);
        EXPECT_EQ(forge().get_default_linear(), sampler);
    }

    TEST_F(SamplerForgeTest, ShutdownEmptiesTheCacheAndReleasesTheBackend)
    {
        ASSERT_TRUE(forge().initialize(backend()));
        ASSERT_TRUE(forge().get_default_linear());
        ASSERT_TRUE(forge().get_default_nearest());

        forge().shutdown();

        EXPECT_FALSE(forge().is_initialized());
        EXPECT_EQ(forge().get_sampler_count(), 0U);
        EXPECT_FALSE(forge().get_default_linear());
    }

    TEST_F(SamplerForgeTest, ShutdownIsRepeatable)
    {
        ASSERT_TRUE(forge().initialize(backend()));

        forge().shutdown();

        EXPECT_NO_THROW(forge().shutdown());
        EXPECT_FALSE(forge().is_initialized());
    }

    TEST_F(SamplerForgeTest, ForgeServesSamplersAgainAfterAShutdownAndReinitialize)
    {
        ASSERT_TRUE(forge().initialize(backend()));
        ASSERT_TRUE(forge().get_default_linear());
        forge().shutdown();

        ASSERT_TRUE(forge().initialize(backend()));

        EXPECT_TRUE(forge().get_default_linear());
        EXPECT_EQ(forge().get_sampler_count(), 1U);
    }

    // ------------------------------------------------------------------
    // Caching
    // ------------------------------------------------------------------

    TEST_F(SamplerForgeTest, EqualConfigsShareOneCachedSampler)
    {
        ASSERT_TRUE(forge().initialize(backend()));
        SamplerConfig config;
        config.address_mode_u = AddressMode::MIRRORED_REPEAT;

        const vk::Sampler first = forge().get_or_create(config);
        const vk::Sampler second = forge().get_or_create(config);

        ASSERT_TRUE(first);
        EXPECT_EQ(first, second);
        EXPECT_EQ(forge().get_sampler_count(), 1U);
    }

    struct FieldCase {
        std::string_view name;
        std::function<void(SamplerConfig&)> change;
    };

    std::vector<FieldCase> field_cases()
    {
        return {
            { "MagFilter", [](SamplerConfig& c) { c.mag_filter = FilterMode::NEAREST; } },
            { "MinFilter", [](SamplerConfig& c) { c.min_filter = FilterMode::NEAREST; } },
            { "AddressU", [](SamplerConfig& c) { c.address_mode_u = AddressMode::CLAMP_TO_EDGE; } },
            { "AddressV", [](SamplerConfig& c) { c.address_mode_v = AddressMode::MIRRORED_REPEAT; } },
            { "AddressW", [](SamplerConfig& c) { c.address_mode_w = AddressMode::CLAMP_TO_EDGE; } },
            { "Anisotropy", [](SamplerConfig& c) { c.max_anisotropy = 4.0F; } },
            { "Mipmaps", [](SamplerConfig& c) { c.enable_mipmaps = true; } },
            { "BorderColor", [](SamplerConfig& c) { c.border_color = BorderColor::OPAQUE_WHITE; } }
        };
    }

    class SamplerForgeFieldTest : public SamplerForgeTest, public ::testing::WithParamInterface<FieldCase> { };

    TEST_P(SamplerForgeFieldTest, ChangingOneFieldYieldsAnotherSampler)
    {
        ASSERT_TRUE(forge().initialize(backend()));
        const SamplerConfig baseline;
        SamplerConfig changed;
        GetParam().change(changed);

        const vk::Sampler first = forge().get_or_create(baseline);
        const vk::Sampler second = forge().get_or_create(changed);

        ASSERT_TRUE(first);
        ASSERT_TRUE(second);
        EXPECT_NE(first, second);
        EXPECT_EQ(forge().get_sampler_count(), 2U);
    }

    INSTANTIATE_TEST_SUITE_P(Fields, SamplerForgeFieldTest,
        ::testing::ValuesIn(field_cases()),
        [](const ::testing::TestParamInfo<FieldCase>& info) { return std::string(info.param.name); });

    class SamplerConfigEqualityTest : public ::testing::TestWithParam<FieldCase> { };

    MAYAFLUX_TEST_WITH(TEST_P, SamplerConfigEqualityTest, ChangingOneFieldMakesTheConfigsUnequal, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))
    {
        const SamplerConfig baseline;
        SamplerConfig changed;
        GetParam().change(changed);

        EXPECT_FALSE(baseline == changed);
    }

    INSTANTIATE_TEST_SUITE_P(Fields, SamplerConfigEqualityTest,
        ::testing::ValuesIn(field_cases()),
        [](const ::testing::TestParamInfo<FieldCase>& info) { return std::string(info.param.name); });

    MAYAFLUX_TEST_WITH(TEST, SamplerPresetsTest, FilterPresetsSetTheirFiltersAndClampEveryAxis, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))
    {
        namespace P = Portal::Graphics::SamplerPresets;

        for (const auto& config : { P::nearest_clamped(), P::linear_clamped(), P::magnify_nearest(), P::minify_nearest() }) {
            EXPECT_EQ(config.address_mode_u, AddressMode::CLAMP_TO_EDGE);
            EXPECT_EQ(config.address_mode_v, AddressMode::CLAMP_TO_EDGE);
            EXPECT_EQ(config.address_mode_w, AddressMode::CLAMP_TO_EDGE);
        }
        EXPECT_EQ(P::nearest_clamped().mag_filter, FilterMode::NEAREST);
        EXPECT_EQ(P::nearest_clamped().min_filter, FilterMode::NEAREST);
        EXPECT_EQ(P::linear_clamped().mag_filter, FilterMode::LINEAR);
        EXPECT_EQ(P::linear_clamped().min_filter, FilterMode::LINEAR);
        EXPECT_EQ(P::magnify_nearest().mag_filter, FilterMode::NEAREST);
        EXPECT_EQ(P::magnify_nearest().min_filter, FilterMode::LINEAR);
        EXPECT_EQ(P::minify_nearest().mag_filter, FilterMode::LINEAR);
        EXPECT_EQ(P::minify_nearest().min_filter, FilterMode::NEAREST);
    }

    MAYAFLUX_TEST_WITH(TEST, SamplerPresetsTest, AddressingPresetsSetEveryAxisAndTakeTheFilter, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))
    {
        namespace P = Portal::Graphics::SamplerPresets;

        EXPECT_EQ(P::repeating().address_mode_u, AddressMode::REPEAT);
        EXPECT_EQ(P::repeating(FilterMode::NEAREST).mag_filter, FilterMode::NEAREST);
        EXPECT_EQ(P::repeating(FilterMode::NEAREST).min_filter, FilterMode::NEAREST);
        EXPECT_EQ(P::mirrored().address_mode_u, AddressMode::MIRRORED_REPEAT);
        EXPECT_EQ(P::mirrored().address_mode_v, AddressMode::MIRRORED_REPEAT);
        EXPECT_EQ(P::mirrored().address_mode_w, AddressMode::MIRRORED_REPEAT);
        EXPECT_EQ(P::mirrored(FilterMode::NEAREST).mag_filter, FilterMode::NEAREST);
    }

    MAYAFLUX_TEST_WITH(TEST, SamplerPresetsTest, ABorderedPresetClampsToTheBorderWithItsColour, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))
    {
        namespace P = Portal::Graphics::SamplerPresets;

        for (const auto color : { BorderColor::TRANSPARENT_BLACK, BorderColor::OPAQUE_BLACK, BorderColor::OPAQUE_WHITE }) {
            const auto config = P::bordered(color, FilterMode::NEAREST);
            EXPECT_EQ(config.address_mode_u, AddressMode::CLAMP_TO_BORDER);
            EXPECT_EQ(config.address_mode_v, AddressMode::CLAMP_TO_BORDER);
            EXPECT_EQ(config.address_mode_w, AddressMode::CLAMP_TO_BORDER);
            EXPECT_EQ(config.border_color, color);
            EXPECT_EQ(config.mag_filter, FilterMode::NEAREST);
        }
    }

    MAYAFLUX_TEST_WITH(TEST, SamplerPresetsTest, AxesSetsEachAxisOnItsOwn, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))
    {
        namespace P = Portal::Graphics::SamplerPresets;

        const auto config = P::axes(AddressMode::REPEAT, AddressMode::CLAMP_TO_EDGE, FilterMode::NEAREST, AddressMode::MIRRORED_REPEAT);

        EXPECT_EQ(config.address_mode_u, AddressMode::REPEAT);
        EXPECT_EQ(config.address_mode_v, AddressMode::CLAMP_TO_EDGE);
        EXPECT_EQ(config.address_mode_w, AddressMode::MIRRORED_REPEAT);
        EXPECT_EQ(config.mag_filter, FilterMode::NEAREST);
        EXPECT_EQ(P::axes(AddressMode::REPEAT, AddressMode::REPEAT).address_mode_w, AddressMode::CLAMP_TO_EDGE);
    }

    MAYAFLUX_TEST_WITH(TEST, SamplerPresetsTest, TheDepthPresetRepeatsOnlyTheDepthAxis, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))
    {
        namespace P = Portal::Graphics::SamplerPresets;

        const auto config = P::repeating_depth();

        EXPECT_EQ(config.address_mode_u, AddressMode::CLAMP_TO_EDGE);
        EXPECT_EQ(config.address_mode_v, AddressMode::CLAMP_TO_EDGE);
        EXPECT_EQ(config.address_mode_w, AddressMode::REPEAT);
    }

    MAYAFLUX_TEST_WITH(TEST, SamplerPresetsTest, ObliqueClampsAnisotropyIntoOneToSixteen, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))
    {
        namespace P = Portal::Graphics::SamplerPresets;

        EXPECT_EQ(P::oblique().max_anisotropy, 16.0F);
        EXPECT_EQ(P::oblique(4.0F).max_anisotropy, 4.0F);
        EXPECT_EQ(P::oblique(0.25F).max_anisotropy, 1.0F);
        EXPECT_EQ(P::oblique(64.0F).max_anisotropy, 16.0F);
    }

    MAYAFLUX_TEST_WITH(TEST, SamplerPresetsTest, EveryNamedPresetIsADifferentConfig, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))
    {
        namespace P = Portal::Graphics::SamplerPresets;
        const std::vector<SamplerConfig> all {
            P::nearest_clamped(), P::linear_clamped(), P::repeating(), P::repeating(FilterMode::NEAREST),
            P::mirrored(), P::mirrored(FilterMode::NEAREST), P::bordered(BorderColor::TRANSPARENT_BLACK),
            P::bordered(BorderColor::OPAQUE_WHITE), P::magnify_nearest(), P::minify_nearest(),
            P::repeating_depth(), P::oblique()
        };

        for (size_t i = 0; i < all.size(); ++i) {
            for (size_t j = i + 1; j < all.size(); ++j) {
                EXPECT_FALSE(all[i] == all[j]) << i << " " << j;
            }
        }
    }

    TEST_F(SamplerForgeTest, EveryNamedPresetCreatesItsOwnSampler)
    {
        namespace P = Portal::Graphics::SamplerPresets;
        ASSERT_TRUE(forge().initialize(backend()));
        const std::vector<SamplerConfig> all {
            P::nearest_clamped(), P::linear_clamped(), P::repeating(), P::repeating(FilterMode::NEAREST),
            P::mirrored(), P::mirrored(FilterMode::NEAREST), P::bordered(BorderColor::TRANSPARENT_BLACK),
            P::bordered(BorderColor::OPAQUE_WHITE), P::magnify_nearest(), P::minify_nearest(),
            P::repeating_depth(), P::oblique()
        };

        std::set<VkSampler> distinct;
        for (const auto& config : all) {
            const vk::Sampler sampler = forge().get_or_create(config);
            ASSERT_TRUE(sampler);
            distinct.insert(static_cast<VkSampler>(sampler));
        }

        EXPECT_EQ(distinct.size(), all.size());
        EXPECT_EQ(forge().get_sampler_count(), all.size());
    }

    MAYAFLUX_TEST_WITH(TEST, SamplerConfigValueTest, SeparatelyBuiltConfigsWithTheSameValuesAreEqual, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))
    {
        const auto build = [] {
            SamplerConfig config;
            config.mag_filter = FilterMode::NEAREST;
            config.address_mode_w = AddressMode::MIRRORED_REPEAT;
            config.max_anisotropy = 4.0F;
            config.enable_mipmaps = true;
            config.border_color = BorderColor::OPAQUE_WHITE;
            return config;
        };

        EXPECT_TRUE(SamplerConfig {} == SamplerConfig {});
        EXPECT_TRUE(build() == build());
    }

    TEST_F(SamplerForgeTest, EveryCombinationOfFieldsGetsItsOwnSamplerAndIsServedAgain)
    {
        ASSERT_TRUE(forge().initialize(backend()));
        const std::array filters { FilterMode::NEAREST, FilterMode::LINEAR };
        const std::array modes { AddressMode::REPEAT, AddressMode::MIRRORED_REPEAT, AddressMode::CLAMP_TO_EDGE, AddressMode::CLAMP_TO_BORDER };
        const std::array depth_modes { AddressMode::REPEAT, AddressMode::CLAMP_TO_EDGE };
        std::vector<SamplerConfig> configs;
        for (const auto mag : filters) {
            for (const auto min : filters) {
                for (const auto u : modes) {
                    for (const auto v : modes) {
                        for (const auto w : depth_modes) {
                            for (const bool mipmaps : { false, true }) {
                                SamplerConfig config;
                                config.mag_filter = mag;
                                config.min_filter = min;
                                config.address_mode_u = u;
                                config.address_mode_v = v;
                                config.address_mode_w = w;
                                config.enable_mipmaps = mipmaps;
                                configs.push_back(config);
                            }
                        }
                    }
                }
            }
        }

        std::vector<vk::Sampler> first;
        std::set<VkSampler> distinct;
        for (const auto& config : configs) {
            first.push_back(forge().get_or_create(config));
            ASSERT_TRUE(first.back());
            distinct.insert(static_cast<VkSampler>(first.back()));
        }

        EXPECT_EQ(distinct.size(), configs.size());
        EXPECT_EQ(forge().get_sampler_count(), configs.size());
        for (size_t i = 0; i < configs.size(); ++i) {
            EXPECT_EQ(forge().get_or_create(configs[i]), first[i]) << i;
        }
        EXPECT_EQ(forge().get_sampler_count(), configs.size());
    }

    TEST_F(SamplerForgeTest, SwappingValuesBetweenFieldsYieldsAnotherSampler)
    {
        ASSERT_TRUE(forge().initialize(backend()));
        SamplerConfig clamp_u;
        clamp_u.address_mode_u = AddressMode::CLAMP_TO_EDGE;
        SamplerConfig clamp_v;
        clamp_v.address_mode_v = AddressMode::CLAMP_TO_EDGE;
        SamplerConfig clamp_w;
        clamp_w.address_mode_w = AddressMode::CLAMP_TO_EDGE;
        SamplerConfig nearest_mag;
        nearest_mag.mag_filter = FilterMode::NEAREST;
        SamplerConfig nearest_min;
        nearest_min.min_filter = FilterMode::NEAREST;

        const std::set<VkSampler> distinct {
            static_cast<VkSampler>(forge().get_or_create(clamp_u)),
            static_cast<VkSampler>(forge().get_or_create(clamp_v)),
            static_cast<VkSampler>(forge().get_or_create(clamp_w)),
            static_cast<VkSampler>(forge().get_or_create(nearest_mag)),
            static_cast<VkSampler>(forge().get_or_create(nearest_min))
        };

        EXPECT_EQ(distinct.size(), 5U);
        EXPECT_EQ(forge().get_sampler_count(), 5U);
    }

    TEST_F(SamplerForgeTest, EveryBorderColorOfAClampToBorderConfigIsItsOwnSampler)
    {
        ASSERT_TRUE(forge().initialize(backend()));
        std::set<VkSampler> distinct;
        for (const auto color : { BorderColor::DEFAULT, BorderColor::TRANSPARENT_BLACK, BorderColor::OPAQUE_BLACK, BorderColor::OPAQUE_WHITE }) {
            SamplerConfig config;
            config.address_mode_u = AddressMode::CLAMP_TO_BORDER;
            config.address_mode_v = AddressMode::CLAMP_TO_BORDER;
            config.address_mode_w = AddressMode::CLAMP_TO_BORDER;
            config.border_color = color;
            distinct.insert(static_cast<VkSampler>(forge().get_or_create(config)));
        }

        EXPECT_EQ(distinct.size(), 4U);
        EXPECT_EQ(forge().get_sampler_count(), 4U);
    }

    TEST_F(SamplerForgeTest, SeparatelyBuiltEqualConfigsShareOneSampler)
    {
        ASSERT_TRUE(forge().initialize(backend()));

        const auto build = [] {
            SamplerConfig config;
            config.mag_filter = FilterMode::NEAREST;
            config.address_mode_v = AddressMode::MIRRORED_REPEAT;
            config.max_anisotropy = 8.0F;
            config.enable_mipmaps = true;
            config.border_color = BorderColor::OPAQUE_WHITE;
            return config;
        };

        const vk::Sampler first = forge().get_or_create(build());
        const vk::Sampler second = forge().get_or_create(build());

        ASSERT_TRUE(first);
        EXPECT_EQ(first, second);
        EXPECT_EQ(forge().get_sampler_count(), 1U);
    }

    // ------------------------------------------------------------------
    // Creation over the enumerations
    // ------------------------------------------------------------------

    struct AddressCase {
        std::string_view name;
        AddressMode mode;
    };

    class SamplerForgeAddressTest : public SamplerForgeTest, public ::testing::WithParamInterface<AddressCase> { };

    MAYAFLUX_TEST_WITH(TEST_P, SamplerForgeAddressTest, EveryAddressModeCreatesASampler, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))
    {
        ASSERT_TRUE(forge().initialize(backend()));
        SamplerConfig config;
        config.address_mode_u = GetParam().mode;
        config.address_mode_v = GetParam().mode;
        config.address_mode_w = GetParam().mode;

        EXPECT_TRUE(forge().get_or_create(config));
        EXPECT_EQ(forge().get_sampler_count(), 1U);
    }

    INSTANTIATE_TEST_SUITE_P(Modes, SamplerForgeAddressTest,
        ::testing::Values(
            AddressCase { "Repeat", AddressMode::REPEAT },
            AddressCase { "MirroredRepeat", AddressMode::MIRRORED_REPEAT },
            AddressCase { "ClampToEdge", AddressMode::CLAMP_TO_EDGE },
            AddressCase { "ClampToBorder", AddressMode::CLAMP_TO_BORDER }),
        [](const ::testing::TestParamInfo<AddressCase>& info) { return std::string(info.param.name); });

    struct BorderCase {
        std::string_view name;
        BorderColor color;
    };

    class SamplerForgeBorderTest : public SamplerForgeTest, public ::testing::WithParamInterface<BorderCase> { };

    MAYAFLUX_TEST_WITH(TEST_P, SamplerForgeBorderTest, EveryBorderColorCreatesAClampToBorderSampler, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))
    {
        ASSERT_TRUE(forge().initialize(backend()));
        SamplerConfig config;
        config.address_mode_u = AddressMode::CLAMP_TO_BORDER;
        config.address_mode_v = AddressMode::CLAMP_TO_BORDER;
        config.address_mode_w = AddressMode::CLAMP_TO_BORDER;
        config.border_color = GetParam().color;

        EXPECT_TRUE(forge().get_or_create(config));
    }

    INSTANTIATE_TEST_SUITE_P(Colors, SamplerForgeBorderTest,
        ::testing::Values(
            BorderCase { "Default", BorderColor::DEFAULT },
            BorderCase { "TransparentBlack", BorderColor::TRANSPARENT_BLACK },
            BorderCase { "OpaqueBlack", BorderColor::OPAQUE_BLACK },
            BorderCase { "OpaqueWhite", BorderColor::OPAQUE_WHITE }),
        [](const ::testing::TestParamInfo<BorderCase>& info) { return std::string(info.param.name); });

    struct FilterCase {
        std::string_view name;
        FilterMode mode;
        bool mipmaps;
    };

    class SamplerForgeFilterTest : public SamplerForgeTest, public ::testing::WithParamInterface<FilterCase> { };

    MAYAFLUX_TEST_WITH(TEST_P, SamplerForgeFilterTest, EachFilterCreatesASamplerWithAndWithoutMipmaps, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))
    {
        ASSERT_TRUE(forge().initialize(backend()));
        SamplerConfig config;
        config.mag_filter = GetParam().mode;
        config.min_filter = GetParam().mode;
        config.enable_mipmaps = GetParam().mipmaps;

        EXPECT_TRUE(forge().get_or_create(config));
    }

    INSTANTIATE_TEST_SUITE_P(Filters, SamplerForgeFilterTest,
        ::testing::Values(
            FilterCase { "NearestNoMips", FilterMode::NEAREST, false },
            FilterCase { "NearestMips", FilterMode::NEAREST, true },
            FilterCase { "LinearNoMips", FilterMode::LINEAR, false },
            FilterCase { "LinearMips", FilterMode::LINEAR, true }),
        [](const ::testing::TestParamInfo<FilterCase>& info) { return std::string(info.param.name); });

    TEST_F(SamplerForgeTest, CubicFilterEitherCreatesASamplerOrLeavesTheCacheEmpty)
    {
        ASSERT_TRUE(forge().initialize(backend()));
        SamplerConfig config;
        config.mag_filter = FilterMode::CUBIC;
        config.min_filter = FilterMode::CUBIC;

        const vk::Sampler sampler = forge().get_or_create(config);

        EXPECT_EQ(forge().get_sampler_count(), sampler ? 1U : 0U);
    }

    TEST_F(SamplerForgeTest, AnisotropyBeyondTheDeviceLimitStillCreatesASampler)
    {
        ASSERT_TRUE(forge().initialize(backend()));
        SamplerConfig config;
        config.max_anisotropy = 1024.0F;

        EXPECT_TRUE(forge().get_or_create(config));
    }

    // ------------------------------------------------------------------
    // Defaults
    // ------------------------------------------------------------------

    TEST_F(SamplerForgeTest, LinearAndNearestDefaultsAreDistinctAndStable)
    {
        ASSERT_TRUE(forge().initialize(backend()));

        const vk::Sampler linear = forge().get_default_linear();
        const vk::Sampler nearest = forge().get_default_nearest();

        ASSERT_TRUE(linear);
        ASSERT_TRUE(nearest);
        EXPECT_NE(linear, nearest);
        EXPECT_EQ(forge().get_default_linear(), linear);
        EXPECT_EQ(forge().get_default_nearest(), nearest);
        EXPECT_EQ(forge().get_sampler_count(), 2U);
    }

    TEST_F(SamplerForgeTest, LinearDefaultIsTheSamplerOfADefaultConfig)
    {
        ASSERT_TRUE(forge().initialize(backend()));

        EXPECT_EQ(forge().get_default_linear(), forge().get_or_create(SamplerConfig {}));
        EXPECT_EQ(forge().get_sampler_count(), 1U);
    }

    TEST_F(SamplerForgeTest, AnisotropicRequestsAreClampedIntoOneToSixteen)
    {
        ASSERT_TRUE(forge().initialize(backend()));

        const vk::Sampler sixteen = forge().get_anisotropic();

        ASSERT_TRUE(sixteen);
        EXPECT_EQ(forge().get_anisotropic(16.0F), sixteen);
        EXPECT_EQ(forge().get_anisotropic(100.0F), sixteen);
        EXPECT_EQ(forge().get_sampler_count(), 1U);
    }

    TEST_F(SamplerForgeTest, AnisotropicRequestBelowOneIsTheLinearDefault)
    {
        ASSERT_TRUE(forge().initialize(backend()));

        EXPECT_EQ(forge().get_anisotropic(0.25F), forge().get_default_linear());
        EXPECT_EQ(forge().get_sampler_count(), 1U);
    }

    TEST_F(SamplerForgeTest, DifferentAnisotropyLevelsAreDifferentSamplers)
    {
        ASSERT_TRUE(forge().initialize(backend()));

        const vk::Sampler four = forge().get_anisotropic(4.0F);
        const vk::Sampler eight = forge().get_anisotropic(8.0F);

        ASSERT_TRUE(four);
        ASSERT_TRUE(eight);
        EXPECT_NE(four, eight);
    }

    // ------------------------------------------------------------------
    // Destruction
    // ------------------------------------------------------------------

    TEST_F(SamplerForgeTest, DestroySamplerRemovesItFromTheCacheAndTheConfigCanBeRecreated)
    {
        ASSERT_TRUE(forge().initialize(backend()));
        const vk::Sampler sampler = forge().get_default_linear();
        ASSERT_TRUE(sampler);

        forge().destroy_sampler(sampler);

        EXPECT_EQ(forge().get_sampler_count(), 0U);
        EXPECT_TRUE(forge().get_default_linear());
        EXPECT_EQ(forge().get_sampler_count(), 1U);
    }

    TEST_F(SamplerForgeTest, DestroySamplerLeavesTheOtherCachedSamplersUntouched)
    {
        ASSERT_TRUE(forge().initialize(backend()));
        const vk::Sampler linear = forge().get_default_linear();
        const vk::Sampler nearest = forge().get_default_nearest();
        ASSERT_TRUE(linear);
        ASSERT_TRUE(nearest);

        forge().destroy_sampler(linear);

        EXPECT_EQ(forge().get_sampler_count(), 1U);
        EXPECT_EQ(forge().get_default_nearest(), nearest);
        EXPECT_EQ(forge().get_sampler_count(), 1U);
    }

    TEST_F(SamplerForgeTest, DestroySamplerIgnoresANullHandle)
    {
        ASSERT_TRUE(forge().initialize(backend()));
        ASSERT_TRUE(forge().get_default_linear());

        forge().destroy_sampler(vk::Sampler {});

        EXPECT_EQ(forge().get_sampler_count(), 1U);
    }

} // namespace
} // namespace MayaFlux::Test
