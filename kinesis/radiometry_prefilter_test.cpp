#include "gtest/gtest.h"
#include "../support/radiometry_support.hpp"
#include "../support/test_metadata.hpp"

#include "MayaFlux/Kinesis/Radiometry/Prefilter.hpp"
#include "MayaFlux/Kinesis/Radiometry/Reflectance.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {

namespace {

    /** An equirectangular grid filled from a function of direction. */
    template <typename Fn>
    std::vector<float> grid_of(uint32_t width, uint32_t height, const Fn& fn, uint32_t channels = 3)
    {
        std::vector<float> pixels(static_cast<size_t>(width) * height * channels, -7.0F);
        for (uint32_t y = 0; y < height; ++y) {
            for (uint32_t x = 0; x < width; ++x) {
                const glm::vec3 value = fn(Kinesis::grid_direction(
                    { (x + 0.5F) / width, (y + 0.5F) / height }));
                for (int c = 0; c < 3; ++c) {
                    pixels[(static_cast<size_t>(y) * width + x) * channels + c] = value[c];
                }
            }
        }
        return pixels;
    }

}

TEST(RadiometryGridTest, DirectionsAndCoordinatesAreInverses)
{
    for (float u = 0.05F; u < 1.0F; u += 0.1F) {
        for (float v = 0.05F; v < 1.0F; v += 0.1F) {
            const glm::vec2 back = Kinesis::grid_coordinate(Kinesis::grid_direction({ u, v }));
            EXPECT_NEAR(back.x, u, 1.0e-4F);
            EXPECT_NEAR(back.y, v, 1.0e-4F);
        }
    }
    EXPECT_GT(Kinesis::grid_direction({ 0.5F, 0.0F }).y, 0.999F);
    EXPECT_NEAR(Kinesis::grid_direction({ 0.5F, 0.5F }).x, 1.0F, 1.0e-6F);
}

TEST(RadiometryGridTest, LookupWrapsAcrossTheSeamAndHoldsAtThePoles)
{
    const auto pixels = grid_of(8, 4, [](const glm::vec3& d) { return glm::vec3(d.z, d.y, 1.0F); }, 4);
    const Kinesis::RadianceGrid grid { pixels, 8, 4, 4 };
    const glm::vec3 left = Kinesis::radiance_at(grid, glm::normalize(glm::vec3(-1.0F, 0.0F, -1.0e-4F)));
    const glm::vec3 right = Kinesis::radiance_at(grid, glm::normalize(glm::vec3(-1.0F, 0.0F, 1.0e-4F)));
    EXPECT_NEAR(left.x, right.x, 1.0e-3F);
    for (const glm::vec3 d : { glm::vec3(0.0F, 1.0F, 0.0F), glm::vec3(0.0F, -1.0F, 0.0F) }) {
        const glm::vec3 value = Kinesis::radiance_at(grid, d);
        EXPECT_TRUE(Radiometry::finite(value));
        EXPECT_NEAR(value.z, 1.0F, 1.0e-5F);
    }
}

TEST(RadiometryGridTest, UnusableGridsGiveNothing)
{
    const std::vector<float> pixels(12, 1.0F);
    for (const Kinesis::RadianceGrid& grid : { Kinesis::RadianceGrid {}, Kinesis::RadianceGrid { pixels, 4, 4, 3 },
             Kinesis::RadianceGrid { pixels, 2, 2, 2 } }) {
        EXPECT_FALSE(Kinesis::usable_grid(grid));
        EXPECT_EQ(Kinesis::project_harmonics(grid)[0], glm::vec3(0.0F));
        EXPECT_TRUE(Kinesis::prefilter_radiance(grid, 0.5F, 4, 2).empty());
        EXPECT_TRUE(Kinesis::prefilter_surround(grid).levels[0].empty());
    }
    EXPECT_TRUE(Kinesis::specular_albedo_table(0).empty());
}

TEST(RadiometryHarmonicsTest, UniformRadianceGivesPiTimesItEverywhere)
{
    const auto pixels = grid_of(64, 32, [](const glm::vec3&) { return glm::vec3(2.0F, 0.5F, 0.0F); });
    const auto harmonics = Kinesis::project_harmonics({ pixels, 64, 32, 3 });
    for (size_t i = 1; i < harmonics.size(); ++i) {
        EXPECT_NEAR(harmonics[i].x, 0.0F, 1.0e-2F) << i;
    }
    for (int i = 0; i < 20; ++i) {
        const glm::vec3 normal = Radiometry::direction_at(-0.95 + 0.1 * i, 1.3 * i);
        const glm::vec3 irradiance = Kinesis::harmonic_irradiance(harmonics, normal);
        EXPECT_NEAR(irradiance.x, 2.0F * std::numbers::pi_v<float>, 1.0e-2F);
        EXPECT_NEAR(irradiance.y, 0.5F * std::numbers::pi_v<float>, 3.0e-3F);
    }
}

TEST(RadiometryHarmonicsTest, ALinearSkyIsReproducedExactly)
{
    const auto pixels = grid_of(128, 64, [](const glm::vec3& d) { return glm::vec3(1.0F + d.y, 1.0F + d.x, 1.0F + d.z); });
    const auto harmonics = Kinesis::project_harmonics({ pixels, 128, 64, 3 });
    constexpr float pi = std::numbers::pi_v<float>;
    for (int i = 0; i < 20; ++i) {
        const glm::vec3 n = Radiometry::direction_at(-0.95 + 0.1 * i, 2.1 * i);
        const glm::vec3 irradiance = Kinesis::harmonic_irradiance(harmonics, n);
        EXPECT_NEAR(irradiance.x, pi + 2.0F * pi / 3.0F * n.y, 1.0e-2F);
        EXPECT_NEAR(irradiance.y, pi + 2.0F * pi / 3.0F * n.x, 1.0e-2F);
        EXPECT_NEAR(irradiance.z, pi + 2.0F * pi / 3.0F * n.z, 1.0e-2F);
    }
}

TEST(RadiometryPrefilterTest, UniformRadianceStaysUniformAtEveryRoughness)
{
    const auto pixels = grid_of(64, 32, [](const glm::vec3&) { return glm::vec3(3.0F); });
    for (const float roughness : { 0.0F, 0.2F, 0.6F, 1.0F }) {
        const auto out = Kinesis::prefilter_radiance({ pixels, 64, 32, 3 }, roughness, 16, 8, 64);
        ASSERT_EQ(out.size(), 128U);
        for (const auto& texel : out) {
            ASSERT_NEAR(texel.x, 3.0F, 1.0e-4F) << roughness;
        }
    }
}

TEST(RadiometryPrefilterTest, ABrightSpotSpreadsWithoutNegativeOrRunawayValues)
{
    const auto pixels = grid_of(64, 32, [](const glm::vec3& d) { return glm::vec3(d.y > 0.97F ? 1000.0F : 0.0F); });
    float previous = std::numeric_limits<float>::max();
    for (const float roughness : { 0.2F, 0.5F, 1.0F }) {
        const auto out = Kinesis::prefilter_radiance({ pixels, 64, 32, 3 }, roughness, 32, 16);
        float highest = 0.0F;
        for (const auto& texel : out) {
            ASSERT_TRUE(Radiometry::finite(texel));
            ASSERT_GE(texel.x, 0.0F);
            highest = std::max(highest, texel.x);
        }
        EXPECT_LE(highest, 1000.0F);
        EXPECT_LT(highest, previous) << roughness;
        previous = highest;
    }
}

TEST(RadiometryPrefilterTest, AlbedoTableMatchesQuadratureOfTheSpecularLobe)
{
    constexpr uint32_t size = 16;
    const auto table = Kinesis::specular_albedo_table(size, 4096);
    ASSERT_EQ(table.size(), size * size);
    const glm::vec3 up(0.0F, 0.0F, 1.0F);
    for (const uint32_t column : { 2U, 8U, 15U }) {
        for (const uint32_t row : { 7U, 11U, 15U }) {
            const float cos_view = (column + 0.5F) / size;
            const float roughness = (row + 0.5F) / size;
            const Kinesis::SurfaceResponse metal { .base_color = glm::vec3(0.3F), .roughness = roughness, .metallic = 1.0F };
            const glm::vec3 view = Radiometry::direction_at(cos_view, 0.0);
            const double albedo = Radiometry::integrate_hemisphere(
                [&](const glm::vec3& source) {
                    return Kinesis::surface_specular(metal, up, view, source,
                               Kinesis::roughness_to_alpha(roughness))
                               .x
                        * source.z;
                },
                1024, 1024);
            const glm::vec2 entry = table[row * size + column];
            EXPECT_NEAR(0.3F * entry.x + entry.y, albedo, 5.0e-3) << cos_view << ' ' << roughness;
        }
    }
}

TEST(RadiometryPrefilterTest, AlbedoNeverExceedsOneAndNearsItWhenSmooth)
{
    const auto table = Kinesis::specular_albedo_table(32);
    for (const auto& entry : table) {
        ASSERT_GE(entry.x, 0.0F);
        ASSERT_GE(entry.y, 0.0F);
        ASSERT_LE(entry.x + entry.y, 1.0F + 1.0e-3F);
    }
    EXPECT_NEAR(table[31].x + table[31].y, 1.0F, 1.0e-2F);
}

TEST(RadiometryPrefilterTest, SurroundHasTheSizesItPromises)
{
    const auto pixels = grid_of(32, 16, [](const glm::vec3&) { return glm::vec3(1.0F); });
    const auto surround = Kinesis::prefilter_surround({ pixels, 32, 16, 3 }, 16);
    for (uint32_t level = 0; level < Kinesis::k_surround_levels; ++level) {
        EXPECT_EQ(surround.levels[level].size(),
            static_cast<size_t>(Kinesis::k_surround_width >> level) * (Kinesis::k_surround_height >> level));
    }
    EXPECT_EQ(surround.albedo.size(), size_t { Kinesis::k_albedo_table_size } * Kinesis::k_albedo_table_size);
}

} // namespace MayaFlux::Test
