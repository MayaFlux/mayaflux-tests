#include "gtest/gtest.h"
#include "../support/radiometry_support.hpp"
#include "../support/test_metadata.hpp"

#include "MayaFlux/Kinesis/Radiometry/Accumulate.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {

namespace {

    const glm::vec3 k_accumulate_up(0.0F, 0.0F, 1.0F);

    Kinesis::RadiantSource overhead(Kinesis::RadiantShape shape)
    {
        return { .shape = shape, .position = { 0.0F, 0.0F, 2.0F }, .range = 1000.0F, .extent = glm::vec2(0.01F) };
    }

}

TEST(RadiometryAccumulateTest, APointDeliversInverseSquareAndASunDoesNot)
{
    const auto point = Kinesis::source_irradiance(overhead(Kinesis::RadiantShape::Point), glm::vec3(0.0F), k_accumulate_up);
    EXPECT_NEAR(point.x, 0.25F, 1.0e-5F);

    auto sun = overhead(Kinesis::RadiantShape::Directional);
    sun.intensity = 3.0F;
    EXPECT_FLOAT_EQ(Kinesis::source_irradiance(sun, glm::vec3(0.0F), k_accumulate_up).x, 3.0F);
    EXPECT_FLOAT_EQ(Kinesis::source_irradiance(sun, glm::vec3(1.0e4F), k_accumulate_up).x, 3.0F);
    EXPECT_EQ(Kinesis::source_irradiance(sun, glm::vec3(0.0F), -k_accumulate_up).x, 0.0F);
}

TEST(RadiometryAccumulateTest, ASmallSphereLooksLikeAPointOfTheSamePower)
{
    auto sphere = overhead(Kinesis::RadiantShape::Sphere);
    sphere.intensity = 1.0F / (std::numbers::pi_v<float> * 0.01F * 0.01F);
    const auto point = overhead(Kinesis::RadiantShape::Point);
    const Kinesis::SurfaceResponse surface { .roughness = 0.6F };
    const glm::vec3 view = Radiometry::direction_at(0.7, 1.0);

    EXPECT_NEAR(Kinesis::source_irradiance(sphere, glm::vec3(0.0F), k_accumulate_up).x, 0.25F, 1.0e-4F);
    EXPECT_NEAR(Kinesis::source_radiance(sphere, surface, glm::vec3(0.0F), k_accumulate_up, view).x,
        Kinesis::source_radiance(point, surface, glm::vec3(0.0F), k_accumulate_up, view).x, 2.0e-3F);
}

TEST(RadiometryAccumulateTest, WhatShouldNotArriveDoesNot)
{
    const Kinesis::SurfaceResponse surface;
    auto disk = overhead(Kinesis::RadiantShape::Disk);
    disk.axis = k_accumulate_up;
    EXPECT_EQ(Kinesis::source_irradiance(disk, glm::vec3(0.0F), k_accumulate_up), glm::vec3(0.0F));

    auto spot = overhead(Kinesis::RadiantShape::Spot);
    spot.axis = { 1.0F, 0.0F, 0.0F };
    spot.cos_inner = 0.9F;
    spot.cos_outer = 0.8F;
    EXPECT_EQ(Kinesis::source_irradiance(spot, glm::vec3(0.0F), k_accumulate_up), glm::vec3(0.0F));

    auto near = overhead(Kinesis::RadiantShape::Point);
    near.range = 1.0F;
    EXPECT_EQ(Kinesis::source_irradiance(near, glm::vec3(0.0F), k_accumulate_up), glm::vec3(0.0F));
    EXPECT_EQ(Kinesis::source_radiance(near, surface, near.position, k_accumulate_up, k_accumulate_up), glm::vec3(0.0F));
}

TEST(RadiometryAccumulateTest, BulkSumsTheSourcesAndAnOccluderCastsShadows)
{
    const std::array<Kinesis::RadiantSource, 2> sources {
        overhead(Kinesis::RadiantShape::Point), overhead(Kinesis::RadiantShape::Directional)
    };
    std::vector<glm::vec3> positions;
    for (int i = 0; i < 600; ++i) {
        positions.emplace_back(-3.0F + 0.01F * i, 0.0F, 0.0F);
    }
    const std::vector<glm::vec3> normals(600, k_accumulate_up);

    const auto open = Kinesis::accumulate_irradiance(sources, positions, normals);
    ASSERT_EQ(open.size(), 600U);
    for (size_t i = 0; i < open.size(); i += 37) {
        const float expected = Kinesis::source_irradiance(sources[0], positions[i], k_accumulate_up).x
            + Kinesis::source_irradiance(sources[1], positions[i], k_accumulate_up).x;
        ASSERT_FLOAT_EQ(open[i].x, expected) << i;
    }

    const std::array<glm::vec3, 4> corners { glm::vec3(-0.5F, -1.0F, 1.0F), glm::vec3(0.5F, -1.0F, 1.0F),
        glm::vec3(0.5F, 1.0F, 1.0F), glm::vec3(-0.5F, 1.0F, 1.0F) };
    const std::array<uint32_t, 6> indices { 0, 1, 2, 0, 2, 3 };
    const auto lid = std::make_shared<const Kinesis::TriangleOccluder>(corners, indices);
    const auto shaded = Kinesis::accumulate_irradiance(sources, positions, normals, lid);

    EXPECT_EQ(shaded[300].x, 0.0F);
    EXPECT_FLOAT_EQ(shaded[0].x, open[0].x);

    const auto seen = Kinesis::accumulate_radiance(
        sources, Kinesis::SurfaceResponse {}, positions, normals, { 0.0F, 0.0F, 5.0F }, lid);
    EXPECT_EQ(seen[300], glm::vec3(0.0F));
    EXPECT_GT(seen[0].x, 0.0F);
    EXPECT_TRUE(Kinesis::accumulate_irradiance({}, positions, normals)[0] == glm::vec3(0.0F));
}

} // namespace MayaFlux::Test
