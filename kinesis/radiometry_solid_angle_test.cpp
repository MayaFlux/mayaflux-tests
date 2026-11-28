#include "gtest/gtest.h"
#include "../support/radiometry_support.hpp"
#include "../support/test_metadata.hpp"

#include "MayaFlux/Kinesis/Radiometry/SolidAngle.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {

namespace {

    const glm::vec3 k_polygon_normal(0.0F, 0.0F, 1.0F);

    /** Irradiance of a sphere by quadrature over the hemisphere. */
    double sphere_by_quadrature(const glm::vec3& center, float radius)
    {
        const glm::vec3 axis = glm::normalize(center);
        const float cos_edge = std::sqrt(std::max(1.0F - radius * radius / glm::dot(center, center), 0.0F));
        return Radiometry::integrate_hemisphere(
            [&](const glm::vec3& d) { return glm::dot(d, axis) >= cos_edge ? d.z : 0.0F; }, 1024, 1024);
    }

    /** Irradiance of a one metre square facing the origin, by quadrature over its area. */
    double square_by_area(const glm::vec3& center, const glm::vec3& right, const glm::vec3& up)
    {
        constexpr int steps = 600;
        const glm::dvec3 face = glm::normalize(glm::cross(glm::dvec3(right), glm::dvec3(up)));
        double sum = 0.0;
        for (int i = 0; i < steps; ++i) {
            for (int j = 0; j < steps; ++j) {
                const glm::dvec3 p = glm::dvec3(center) + glm::dvec3(right) * ((i + 0.5) / steps - 0.5)
                    + glm::dvec3(up) * ((j + 0.5) / steps - 0.5);
                const double distance_sq = glm::dot(p, p);
                const glm::dvec3 d = p / std::sqrt(distance_sq);
                sum += std::max(d.z, 0.0) * std::abs(glm::dot(face, d)) / distance_sq;
            }
        }
        return sum / (steps * steps);
    }

}

TEST(RadiometrySolidAngleTest, SphereSubtendsFromNothingToEverything)
{
    EXPECT_FLOAT_EQ(Kinesis::sphere_solid_angle(0.5F, 1.0F), 4.0F * std::numbers::pi_v<float>);
    EXPECT_NEAR(Kinesis::sphere_solid_angle(1000.0F, 1.0F), std::numbers::pi_v<float> * 1.0e-6F, 1.0e-9F);
    EXPECT_EQ(Kinesis::sphere_solid_angle(2.0F, 0.0F), 0.0F);
}

TEST(RadiometrySolidAngleTest, SphereIrradianceHoldsAcrossTheHorizon)
{
    for (const double cos_theta : { -0.4, -0.1, 0.0, 0.1, 0.4, 0.9 }) {
        for (const float radius : { 0.5F, 0.9F }) {
            const glm::vec3 center = Radiometry::direction_at(cos_theta, 0.7);
            EXPECT_NEAR(Kinesis::sphere_irradiance(center.z, radius * radius),
                sphere_by_quadrature(center, radius), 5.0e-3)
                << cos_theta << ' ' << radius;
        }
    }
}

TEST(RadiometrySolidAngleTest, SphereIrradianceSurvivesItsLimits)
{
    EXPECT_EQ(Kinesis::sphere_irradiance(1.0F, 0.0F), 0.0F);
    EXPECT_EQ(Kinesis::sphere_irradiance(-1.0F, 0.5F), 0.0F);
    EXPECT_FLOAT_EQ(Kinesis::sphere_irradiance(-1.0F, 2.0F), std::numbers::pi_v<float>);
    EXPECT_FLOAT_EQ(Kinesis::sphere_irradiance(1.0F, 0.25F), std::numbers::pi_v<float> * 0.25F);
    for (const float c : { -1.0F, 0.0F, 1.0F, 1.5F }) {
        for (const float s : { 1.0e-8F, 0.5F, 0.999999F }) {
            EXPECT_TRUE(std::isfinite(Kinesis::sphere_irradiance(c, s))) << c << ' ' << s;
        }
    }
}

TEST(RadiometrySolidAngleTest, PolygonMatchesIntegrationOverItsArea)
{
    const glm::vec3 right = glm::normalize(glm::vec3(1.0F, 0.2F, 0.1F));
    const glm::vec3 up = glm::normalize(glm::cross(glm::vec3(0.3F, 0.1F, 1.0F), right));
    for (const glm::vec3 center : { glm::vec3(0.0F, 0.0F, 2.0F), glm::vec3(1.5F, -0.5F, 1.0F),
             glm::vec3(1.0F, 0.0F, 0.1F), glm::vec3(0.6F, 0.4F, -0.1F) }) {
        const auto corners = Kinesis::rectangle_corners(center, right, up, glm::vec2(1.0F));
        EXPECT_NEAR(Kinesis::polygon_irradiance(corners, k_polygon_normal), square_by_area(center, right, up), 2.0e-3)
            << center.x << ' ' << center.z;
    }
}

TEST(RadiometrySolidAngleTest, PolygonIgnoresWindingAndRejectsWhatIsBelow)
{
    auto corners = Kinesis::rectangle_corners({ 0.3F, 0.2F, 1.0F }, { 1.0F, 0.0F, 0.0F }, { 0.0F, 1.0F, 0.0F },
        glm::vec2(1.0F));
    const float forward = Kinesis::polygon_irradiance(corners, k_polygon_normal);
    std::ranges::reverse(corners);
    EXPECT_FLOAT_EQ(Kinesis::polygon_irradiance(corners, k_polygon_normal), forward);
    EXPECT_EQ(Kinesis::polygon_irradiance(corners, -k_polygon_normal), 0.0F);
    EXPECT_EQ(Kinesis::polygon_irradiance(std::span<const glm::vec3>(corners).first(2), k_polygon_normal), 0.0F);

    const std::array<glm::vec3, 4> collapsed { glm::vec3(0.0F, 0.0F, 1.0F), glm::vec3(0.0F, 0.0F, 1.0F),
        glm::vec3(0.0F, 0.0F, 1.0F), glm::vec3(0.0F, 0.0F, 1.0F) };
    EXPECT_EQ(Kinesis::polygon_irradiance(collapsed, k_polygon_normal), 0.0F);
}

TEST(RadiometrySolidAngleTest, DiskOutlineMatchesTheDiskOnItsAxis)
{
    for (const float distance : { 0.5F, 1.0F, 4.0F }) {
        const auto corners = Kinesis::disk_corners(
            { 0.0F, 0.0F, distance }, { 1.0F, 0.0F, 0.0F }, { 0.0F, 1.0F, 0.0F }, 1.0F);
        const float exact = std::numbers::pi_v<float> / (1.0F + distance * distance);
        EXPECT_NEAR(Kinesis::polygon_irradiance(corners, k_polygon_normal), exact, 0.02F * exact) << distance;
    }
}

TEST(RadiometrySolidAngleTest, NearestPointsStayOnTheirShapes)
{
    const glm::vec3 right(1.0F, 0.0F, 0.0F);
    const glm::vec3 up(0.0F, 1.0F, 0.0F);
    const glm::vec3 center(0.5F, -0.25F, 3.0F);
    for (uint32_t i = 0; i < 200; ++i) {
        const double c = -1.0 + 2.0 * (i + 0.5) / 200.0;
        const glm::vec3 ray = Radiometry::direction_at(c, 2.4 * i);

        EXPECT_LE(glm::distance(Kinesis::sphere_toward(center, 0.5F, ray), center), 0.5F + 1.0e-5F);

        const glm::vec3 on_rect = Kinesis::rectangle_toward(center, right, up, { 2.0F, 1.0F }, ray);
        EXPECT_LE(std::abs(on_rect.x - center.x), 1.0F + 1.0e-5F);
        EXPECT_LE(std::abs(on_rect.y - center.y), 0.5F + 1.0e-5F);
        EXPECT_NEAR(on_rect.z, center.z, 1.0e-4F);

        EXPECT_LE(glm::distance(Kinesis::disk_toward(center, { 0.0F, 0.0F, 1.0F }, 0.7F, ray), center), 0.7F + 1.0e-4F);

        const glm::vec3 on_segment = Kinesis::segment_toward(center - right, center + right, ray);
        EXPECT_LE(std::abs(on_segment.x - center.x), 1.0F + 1.0e-5F);
        EXPECT_EQ(on_segment.y, center.y);
    }
    EXPECT_TRUE(Radiometry::finite(Kinesis::segment_toward(center, center, { 0.0F, 0.0F, 1.0F })));
    EXPECT_TRUE(Radiometry::finite(Kinesis::segment_toward(-right, right, right)));
    EXPECT_TRUE(Radiometry::finite(Kinesis::tube_corners(center, glm::normalize(center), 0.1F, 1.0F)[0]));
}

TEST(RadiometrySolidAngleTest, WidenedLobeNeverNarrows)
{
    for (const float alpha : { 0.001F, 0.5F, 1.0F }) {
        EXPECT_FLOAT_EQ(Kinesis::widened_alpha(alpha, 0.0F, 1.0F), alpha);
        EXPECT_GE(Kinesis::widened_alpha(alpha, 1.0F, 5.0F), alpha);
        EXPECT_FLOAT_EQ(Kinesis::widened_alpha(alpha, 10.0F, 0.0F), 1.0F);
    }
}

} // namespace MayaFlux::Test
