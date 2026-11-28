#include "gtest/gtest.h"
#include "../support/radiometry_support.hpp"
#include "../support/test_metadata.hpp"

#include "MayaFlux/Kinesis/Radiometry/Reflectance.hpp"
#include "MayaFlux/Kinesis/Stochastic/Sequences.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {

namespace {

    using Radiometry::direction_at;
    using Radiometry::integrate_hemisphere;

    const glm::vec3 k_up(0.0F, 0.0F, 1.0F);

    double albedo_of(const Kinesis::SurfaceResponse& surface, const glm::vec3& view)
    {
        return integrate_hemisphere([&](const glm::vec3& source) {
            return Kinesis::surface_reflectance(surface, k_up, view, source).x * source.z;
        });
    }

}

TEST(RadiometryMicrofacetTest, DistributionProjectsToUnitArea)
{
    for (const float alpha : { 0.1F, 0.3F, 0.7F, 1.0F }) {
        const double area = integrate_hemisphere(
            [alpha](const glm::vec3& h) { return Kinesis::ggx_density(h.z, alpha) * h.z; }, 4096, 16);
        EXPECT_NEAR(area, 1.0, 2.0e-3) << alpha;
    }
}

TEST(RadiometryMicrofacetTest, VisibleNormalDensityIntegratesToOneEvenAtGrazing)
{
    for (const float alpha : { 0.3F, 0.8F }) {
        for (const double cos_view : { 0.05, 0.5, 1.0 }) {
            const glm::vec3 view = direction_at(cos_view, 0.0);
            const double total = integrate_hemisphere(
                [&](const glm::vec3& h) {
                    return Kinesis::ggx_masking(view.z, alpha) * std::max(glm::dot(view, h), 0.0F)
                        * Kinesis::ggx_density(h.z, alpha) / view.z;
                },
                2048, 512);
            EXPECT_NEAR(total, 1.0, 5.0e-3) << alpha << ' ' << cos_view;
        }
    }
}

TEST(RadiometryMicrofacetTest, VisibleNormalSamplesFollowThatDensity)
{
    const float alpha = 0.5F;
    const glm::vec3 view = direction_at(0.3, 0.4);

    glm::dvec3 expected(0.0);
    for (int axis = 0; axis < 3; ++axis) {
        expected[axis] = integrate_hemisphere([&](const glm::vec3& h) {
            return h[axis] * Kinesis::ggx_masking(view.z, alpha) * std::max(glm::dot(view, h), 0.0F)
                * Kinesis::ggx_density(h.z, alpha) / view.z;
        });
    }

    constexpr uint32_t count = 1U << 16U;
    glm::dvec3 mean(0.0);
    for (uint32_t i = 0; i < count; ++i) {
        const glm::vec3 h = Kinesis::ggx_visible_normal(view, alpha, Kinesis::Stochastic::hammersley_point(i, count));
        ASSERT_NEAR(glm::length(h), 1.0F, 1.0e-4F);
        ASSERT_GE(h.z, 0.0F);
        mean += glm::dvec3(h);
    }
    mean /= static_cast<double>(count);

    EXPECT_NEAR(mean.x, expected.x, 3.0e-3);
    EXPECT_NEAR(mean.y, expected.y, 3.0e-3);
    EXPECT_NEAR(mean.z, expected.z, 3.0e-3);
}

TEST(RadiometryMicrofacetTest, ExtremesStayFinite)
{
    EXPECT_FLOAT_EQ(Kinesis::roughness_to_alpha(0.0F), 1.0e-3F);
    EXPECT_TRUE(std::isfinite(Kinesis::ggx_density(1.0F, 1.0e-3F)));
    EXPECT_TRUE(std::isfinite(Kinesis::ggx_density(1.0000002F, 1.0e-3F)));
    EXPECT_EQ(Kinesis::ggx_density(0.0F, 0.5F), 0.0F);
    EXPECT_EQ(Kinesis::ggx_masking(-0.5F, 0.5F), 0.0F);
    EXPECT_FLOAT_EQ(Kinesis::ggx_masking(1.0F, 0.5F), 1.0F);
    EXPECT_EQ(Kinesis::ggx_visibility(0.0F, 0.0F, 0.5F), 0.0F);
    EXPECT_EQ(Kinesis::ggx_reflected_density(0.0F, 1.0F, 0.5F), 0.0F);
    EXPECT_TRUE(Radiometry::finite(Kinesis::multiple_scattering_compensation(glm::vec3(1.0F), 0.0F)));
    EXPECT_EQ(Kinesis::multiple_scattering_compensation(glm::vec3(0.7F), 1.0F), glm::vec3(1.0F));
    EXPECT_TRUE(Radiometry::finite(
        Kinesis::ggx_visible_normal(direction_at(1.0e-4, 1.0), 1.0e-3F, { 0.999F, 0.999F })));
}

TEST(RadiometryReflectanceTest, UnroughDiffuseIsLambert)
{
    const glm::vec3 albedo(0.2F, 0.5F, 1.0F);
    const glm::vec3 value = Kinesis::oren_nayar_energy_preserving(
        albedo, 0.0F, k_up, direction_at(0.7, 0.0), direction_at(0.2, 2.0));
    EXPECT_NEAR(value.x, albedo.x * std::numbers::inv_pi_v<float>, 1.0e-6F);
    EXPECT_NEAR(value.y, albedo.y * std::numbers::inv_pi_v<float>, 1.0e-6F);
    EXPECT_NEAR(value.z, albedo.z * std::numbers::inv_pi_v<float>, 1.0e-6F);
}

TEST(RadiometryReflectanceTest, WhiteRoughDiffuseReturnsAllItReceives)
{
    for (const float roughness : { 0.3F, 1.0F }) {
        for (const double cos_view : { 0.05, 0.4, 1.0 }) {
            const glm::vec3 view = direction_at(cos_view, 0.3);
            const double total = integrate_hemisphere([&](const glm::vec3& source) {
                return Kinesis::oren_nayar_energy_preserving(glm::vec3(1.0F), roughness, k_up, view, source).x
                    * source.z;
            });
            EXPECT_NEAR(total, 1.0, 5.0e-3) << roughness << ' ' << cos_view;
        }
    }
}

TEST(RadiometryReflectanceTest, SwappingViewAndSourceChangesNothing)
{
    const Kinesis::SurfaceResponse surface {
        .base_color = { 0.9F, 0.4F, 0.1F }, .roughness = 0.35F, .metallic = 0.4F, .diffuse_roughness = 0.8F
    };
    for (uint32_t i = 0; i < 256; ++i) {
        const glm::vec3 a = direction_at(0.02 + 0.98 * Kinesis::Stochastic::hashed_unit(i, 1),
            6.2 * Kinesis::Stochastic::hashed_unit(i, 2));
        const glm::vec3 b = direction_at(0.02 + 0.98 * Kinesis::Stochastic::hashed_unit(i, 3),
            6.2 * Kinesis::Stochastic::hashed_unit(i, 4));
        const glm::vec3 forward = Kinesis::surface_reflectance(surface, k_up, a, b);
        const glm::vec3 backward = Kinesis::surface_reflectance(surface, k_up, b, a);
        ASSERT_NEAR(forward.x, backward.x, 1.0e-4F * std::max(1.0F, forward.x));
        ASSERT_NEAR(forward.z, backward.z, 1.0e-4F * std::max(1.0F, forward.z));
        ASSERT_TRUE(Radiometry::finite(forward));
        ASSERT_GE(std::min({ forward.x, forward.y, forward.z }), 0.0F);
    }
}

TEST(RadiometryReflectanceTest, NothingLeavesBelowTheHorizon)
{
    const Kinesis::SurfaceResponse surface;
    const glm::vec3 above = direction_at(0.5, 0.0);
    const glm::vec3 below = direction_at(-0.5, 0.0);
    EXPECT_EQ(Kinesis::surface_reflectance(surface, k_up, above, below), glm::vec3(0.0F));
    EXPECT_EQ(Kinesis::surface_reflectance(surface, k_up, below, above), glm::vec3(0.0F));
    EXPECT_EQ(Kinesis::surface_reflectance(surface, k_up, below, -below), glm::vec3(0.0F));
    EXPECT_EQ(Kinesis::surface_density(surface, above, below), 0.0F);
    EXPECT_EQ(Kinesis::surface_density(surface, below, above), 0.0F);
}

TEST(RadiometryReflectanceTest, AWhiteSurfaceDoesNotCreateEnergy)
{
    for (const float metallic : { 0.0F, 1.0F }) {
        for (const float roughness : { 0.3F, 0.6F, 1.0F }) {
            for (const double cos_view : { 0.1, 0.5, 1.0 }) {
                const Kinesis::SurfaceResponse surface {
                    .base_color = glm::vec3(1.0F), .roughness = roughness, .metallic = metallic
                };
                EXPECT_LE(albedo_of(surface, direction_at(cos_view, 0.0)), 1.02)
                    << metallic << ' ' << roughness << ' ' << cos_view;
            }
        }
    }
}

TEST(RadiometryReflectanceTest, SampleDensityMatchesTheSamplesThatStayAbove)
{
    const Kinesis::SurfaceResponse surface { .roughness = 0.6F, .metallic = 0.3F };
    for (const double cos_view : { 0.2, 0.9 }) {
        const glm::vec3 view = direction_at(cos_view, 0.0);
        const double mass = integrate_hemisphere(
            [&](const glm::vec3& source) { return Kinesis::surface_density(surface, view, source); });

        constexpr uint32_t count = 1U << 17U;
        uint32_t above = 0;
        for (uint32_t i = 0; i < count; ++i) {
            const glm::vec3 source = Kinesis::surface_sample(surface, view,
                { Kinesis::Stochastic::hashed_unit(i, 10), Kinesis::Stochastic::hashed_unit(i, 11) },
                Kinesis::Stochastic::hashed_unit(i, 12));
            ASSERT_NEAR(glm::length(source), 1.0F, 1.0e-4F);
            above += source.z > 0.0F ? 1U : 0U;
        }
        EXPECT_NEAR(static_cast<double>(above) / count, mass, 5.0e-3) << cos_view;
    }
}

TEST(RadiometryReflectanceTest, ImportanceSamplingAgreesWithQuadrature)
{
    const Kinesis::SurfaceResponse surface {
        .base_color = glm::vec3(0.7F), .roughness = 0.5F, .metallic = 0.5F, .diffuse_roughness = 0.5F
    };
    const glm::vec3 view = direction_at(0.4, 0.0);

    constexpr uint32_t count = 1U << 17U;
    double sum = 0.0;
    for (uint32_t i = 0; i < count; ++i) {
        const glm::vec3 source = Kinesis::surface_sample(surface, view,
            { Kinesis::Stochastic::hashed_unit(i, 20), Kinesis::Stochastic::hashed_unit(i, 21) },
            Kinesis::Stochastic::hashed_unit(i, 22));
        const float density = Kinesis::surface_density(surface, view, source);
        if (density > 0.0F) {
            sum += Kinesis::surface_reflectance(surface, k_up, view, source).x * source.z / density;
        }
    }
    EXPECT_NEAR(sum / count, albedo_of(surface, view), 1.0e-2);
}

} // namespace MayaFlux::Test
