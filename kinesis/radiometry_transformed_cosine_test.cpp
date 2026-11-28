#include "gtest/gtest.h"
#include "../support/radiometry_support.hpp"
#include "../support/test_metadata.hpp"

#include "MayaFlux/Kinesis/Radiometry/Microfacet.hpp"
#include "MayaFlux/Kinesis/Radiometry/TransformedCosine.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {

namespace {

    constexpr uint32_t k_fit_size = 8;

    const std::vector<glm::vec4>& coarse_fit()
    {
        static const std::vector<glm::vec4> table = Kinesis::fit_transformed_cosines(k_fit_size);
        return table;
    }

    struct FitEntry {
        glm::vec3 view;
        float alpha;
        glm::mat3 inverse;
    };

    FitEntry fit_entry_at(uint32_t column, uint32_t row)
    {
        const float x = static_cast<float>(column) / (k_fit_size - 1);
        const float theta = std::min(std::acos(1.0F - x * x), 1.57F);
        return {
            .view = { std::sin(theta), 0.0F, std::cos(theta) },
            .alpha = Kinesis::roughness_to_alpha(static_cast<float>(row) / (k_fit_size - 1)),
            .inverse = Kinesis::lobe_matrix(coarse_fit()[row * k_fit_size + column])
        };
    }

    float specular_value(const FitEntry& entry, const glm::vec3& source)
    {
        const glm::vec3 half = glm::normalize(entry.view + source);
        return Kinesis::ggx_density(half.z, entry.alpha)
            * Kinesis::ggx_visibility(entry.view.z, source.z, entry.alpha) * source.z;
    }

}

TEST(RadiometryTransformedCosineTest, TheIdentityIsThePlainCosine)
{
    const glm::mat3 identity(1.0F);
    const glm::vec3 up(0.0F, 0.0F, 1.0F);
    EXPECT_NEAR(Kinesis::transformed_cosine_density(identity, up), std::numbers::inv_pi_v<float>, 1.0e-6F);
    EXPECT_EQ(Kinesis::transformed_cosine_density(identity, -up), 0.0F);

    const auto corners = Kinesis::rectangle_corners(
        { 0.3F, -0.2F, 1.0F }, { 1.0F, 0.0F, 0.0F }, { 0.0F, 1.0F, 0.0F }, { 1.0F, 2.0F });
    const float plain = Kinesis::polygon_irradiance(corners, up) * std::numbers::inv_pi_v<float>;
    for (const glm::vec3 view : { up, glm::normalize(glm::vec3(0.8F, 0.1F, 0.3F)) }) {
        EXPECT_NEAR(Kinesis::transformed_coverage(identity, up, view, corners), plain, 1.0e-5F);
    }
}

TEST(RadiometryTransformedCosineTest, ASizeBelowTwoGivesNothing)
{
    EXPECT_TRUE(Kinesis::fit_transformed_cosines(0).empty());
    EXPECT_TRUE(Kinesis::fit_transformed_cosines(1).empty());
    EXPECT_TRUE(Kinesis::fit_transformed_cosines(4, 0).empty());
}

TEST(RadiometryTransformedCosineTest, EveryFittedEntryIsFiniteAndInvertible)
{
    ASSERT_EQ(coarse_fit().size(), k_fit_size * k_fit_size);
    for (size_t i = 0; i < coarse_fit().size(); ++i) {
        const glm::vec4& packed = coarse_fit()[i];
        ASSERT_TRUE(std::isfinite(packed.x) && std::isfinite(packed.y) && std::isfinite(packed.z) && std::isfinite(packed.w)) << i;
        ASSERT_NE(glm::determinant(Kinesis::lobe_matrix(packed)), 0.0F) << i;
    }
}

TEST(RadiometryTransformedCosineTest, FittedLobeFollowsTheSpecularLobe)
{
    for (const uint32_t column : { 0U, 3U, 5U }) {
        for (const uint32_t row : { 4U, 5U, 7U }) {
            const FitEntry entry = fit_entry_at(column, row);
            const double albedo = Radiometry::integrate_hemisphere(
                [&](const glm::vec3& s) { return specular_value(entry, s); });
            const double apart = Radiometry::integrate_hemisphere([&](const glm::vec3& s) {
                return std::abs(specular_value(entry, s)
                    - static_cast<float>(albedo) * Kinesis::transformed_cosine_density(entry.inverse, s));
            });
            EXPECT_LT(apart / albedo, 0.35) << column << ' ' << row;
        }
    }
}

TEST(RadiometryTransformedCosineTest, CoverageOfARectangleFollowsIntegrationOverIt)
{
    const glm::vec3 up(0.0F, 0.0F, 1.0F);
    for (const uint32_t column : { 0U, 3U, 5U }) {
        for (const uint32_t row : { 4U, 7U }) {
            const FitEntry entry = fit_entry_at(column, row);
            const glm::vec3 center(-1.0F, 0.0F, 1.0F);
            const glm::vec3 right = glm::normalize(glm::vec3(1.0F, 0.0F, 1.0F));
            const glm::vec3 across(0.0F, 1.0F, 0.0F);
            const auto corners = Kinesis::rectangle_corners(center, right, across, glm::vec2(1.0F));

            constexpr int steps = 300;
            const glm::vec3 face = glm::normalize(glm::cross(right, across));
            double inside = 0.0;
            for (int i = 0; i < steps; ++i) {
                for (int j = 0; j < steps; ++j) {
                    const glm::vec3 p = center + right * ((i + 0.5F) / steps - 0.5F) + across * ((j + 0.5F) / steps - 0.5F);
                    const float distance_sq = glm::dot(p, p);
                    const glm::vec3 s = p / std::sqrt(distance_sq);
                    if (s.z > 0.0F) {
                        inside += specular_value(entry, s) * std::abs(glm::dot(face, s)) / distance_sq;
                    }
                }
            }
            inside /= steps * steps;
            const double albedo = Radiometry::integrate_hemisphere(
                [&](const glm::vec3& s) { return specular_value(entry, s); });

            EXPECT_NEAR(Kinesis::transformed_coverage(entry.inverse, up, entry.view, corners),
                inside / albedo, 0.08)
                << column << ' ' << row;
        }
    }
}

} // namespace MayaFlux::Test
