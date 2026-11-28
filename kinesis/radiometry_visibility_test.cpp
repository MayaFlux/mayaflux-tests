#include "gtest/gtest.h"
#include "../support/radiometry_support.hpp"
#include "../support/test_metadata.hpp"

#include "MayaFlux/Kinesis/Radiometry/Visibility.hpp"
#include "MayaFlux/Kinesis/Stochastic/Sequences.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {

namespace {

    /** A rectangle in the plane at height z. */
    Kinesis::TriangleOccluder occluder_plate(float z, glm::vec2 low, glm::vec2 high)
    {
        const std::array<glm::vec3, 4> positions {
            glm::vec3(low.x, low.y, z), glm::vec3(high.x, low.y, z),
            glm::vec3(high.x, high.y, z), glm::vec3(low.x, high.y, z)
        };
        const std::array<uint32_t, 6> indices { 0, 1, 2, 0, 2, 3 };
        return { positions, indices };
    }

    const glm::vec3 k_occluder_up(0.0F, 0.0F, 1.0F);

}

TEST(RadiometryOccluderTest, OnlyWhatLiesBetweenHides)
{
    const auto cover = occluder_plate(1.0F, { -1.0F, -1.0F }, { 1.0F, 1.0F });
    EXPECT_TRUE(cover.occluded({ 0.0F, 0.0F, 0.0F }, { 0.0F, 0.0F, 2.0F }));
    EXPECT_TRUE(cover.occluded({ 0.0F, 0.0F, 2.0F }, { 0.0F, 0.0F, 0.0F }));
    EXPECT_FALSE(cover.occluded({ 0.0F, 0.0F, 0.0F }, { 0.0F, 0.0F, 0.9F }));
    EXPECT_FALSE(cover.occluded({ 0.0F, 0.0F, 0.0F }, { 0.0F, 0.0F, 1.0F }));
    EXPECT_FALSE(cover.occluded({ 3.0F, 0.0F, 0.0F }, { 3.0F, 0.0F, 2.0F }));
    EXPECT_FALSE(cover.occluded({ 0.0F, 0.0F, 0.0F }, { 0.0F, 0.0F, 0.0F }));
}

TEST(RadiometryOccluderTest, NothingAndBrokenTrianglesHideNothing)
{
    const Kinesis::TriangleOccluder none({}, {});
    EXPECT_FALSE(none.occluded(glm::vec3(0.0F), k_occluder_up));

    const std::array<glm::vec3, 3> positions { glm::vec3(-1.0F, -1.0F, 0.5F), glm::vec3(1.0F, -1.0F, 0.5F),
        glm::vec3(0.0F, 1.0F, 0.5F) };
    const std::array<uint32_t, 5> indices { 0, 1, 9, 0, 1 };
    const Kinesis::TriangleOccluder broken(positions, indices);
    EXPECT_FALSE(broken.occluded(glm::vec3(0.0F), k_occluder_up));
    EXPECT_GT(none.clearance(), 0.0F);
}

TEST(RadiometryOccluderTest, HierarchyAgreesWithTestingEveryTriangle)
{
    const auto value = [](uint32_t i, uint32_t seed) { return Kinesis::Stochastic::hashed_unit(i, seed) * 4.0F - 2.0F; };
    std::vector<glm::vec3> positions;
    std::vector<uint32_t> indices;
    for (uint32_t i = 0; i < 600; ++i) {
        const glm::vec3 centre(value(i, 1), value(i, 2), value(i, 3));
        for (uint32_t c = 0; c < 3; ++c) {
            indices.push_back(static_cast<uint32_t>(positions.size()));
            positions.push_back(centre + 0.15F * glm::vec3(value(i, 4 + c), value(i, 7 + c), value(i, 10 + c)));
        }
    }
    const Kinesis::TriangleOccluder occluder(positions, indices);

    uint32_t hidden = 0;
    for (uint32_t i = 0; i < 500; ++i) {
        const glm::vec3 from(value(i, 20), value(i, 21), value(i, 22));
        const glm::vec3 to(value(i, 23), value(i, 24), value(i, 25));
        const auto first = Kinesis::ray_triangles_parameter(
            Kinesis::Ray { .origin = from, .direction = to - from }, positions, std::span<const uint32_t>(indices));
        const bool expected = first && first->second.x < 1.0F - 1.0e-4F;
        ASSERT_EQ(occluder.occluded(from, to), expected) << i;
        hidden += expected ? 1U : 0U;
    }
    EXPECT_GT(hidden, 20U);
    EXPECT_LT(hidden, 480U);
}

TEST(RadiometryVisibilityTest, OpenCoveredAndHalfCovered)
{
    const auto floor = occluder_plate(0.0F, { -100.0F, -100.0F }, { 100.0F, 100.0F });
    EXPECT_FLOAT_EQ(Kinesis::ambient_visibility(floor, glm::vec3(0.0F), k_occluder_up, 10.0F), 1.0F);

    const auto lid = occluder_plate(0.1F, { -100.0F, -100.0F }, { 100.0F, 100.0F });
    EXPECT_LT(Kinesis::ambient_visibility(lid, glm::vec3(0.0F), k_occluder_up, 50.0F, 256), 0.01F);
    EXPECT_FLOAT_EQ(Kinesis::ambient_visibility(lid, glm::vec3(0.0F), k_occluder_up, 0.05F), 1.0F);

    const auto half = occluder_plate(0.1F, { 0.0F, -100.0F }, { 100.0F, 100.0F });
    EXPECT_NEAR(Kinesis::ambient_visibility(half, glm::vec3(0.0F), k_occluder_up, 50.0F, 1024), 0.5F, 0.03F);

    EXPECT_FLOAT_EQ(Kinesis::ambient_visibility(lid, glm::vec3(0.0F), k_occluder_up, 0.0F), 1.0F);
    EXPECT_FLOAT_EQ(Kinesis::ambient_visibility(lid, glm::vec3(0.0F), k_occluder_up, 50.0F, 0), 1.0F);
}

TEST(RadiometryVisibilityTest, BulkMatchesOneAtATimeAndStopsAtTheShorterSpan)
{
    const auto half = occluder_plate(0.5F, { 0.0F, -3.0F }, { 3.0F, 3.0F });
    std::vector<glm::vec3> points;
    for (int i = 0; i < 200; ++i) {
        points.emplace_back(-2.0F + 0.02F * i, 0.0F, 0.0F);
    }
    const std::vector<glm::vec3> normals(150, k_occluder_up);

    const auto bulk = Kinesis::ambient_visibility(half, points, normals, 10.0F, 32);
    ASSERT_EQ(bulk.size(), 150U);
    for (size_t i = 0; i < bulk.size(); ++i) {
        ASSERT_EQ(bulk[i], Kinesis::ambient_visibility(half, points[i], k_occluder_up, 10.0F, 32, static_cast<uint32_t>(i)));
    }
}

TEST(RadiometryVisibilityTest, ASourceHalfBehindAnEdgeIsHalfSeen)
{
    const auto edge = occluder_plate(1.0F, { 0.0F, -10.0F }, { 10.0F, 10.0F });
    std::vector<glm::vec3> targets;
    for (int i = 0; i < 100; ++i) {
        targets.emplace_back(-1.0F + 0.02F * (i + 0.5F), 0.0F, 2.0F);
    }
    EXPECT_NEAR(Kinesis::source_visibility(edge, glm::vec3(0.0F), k_occluder_up, targets), 0.5F, 0.02F);
    EXPECT_EQ(Kinesis::source_visibility(edge, glm::vec3(0.0F), k_occluder_up, {}), 1.0F);
}

TEST(RadiometryVisibilityTest, SpecularOcclusionStaysInRange)
{
    for (const float roughness : { 0.0F, 0.5F, 1.0F }) {
        for (const float cos_view : { 0.0F, 0.5F, 1.0F }) {
            EXPECT_NEAR(Kinesis::specular_occlusion(cos_view, 1.0F, roughness), 1.0F, 1.0e-5F);
            EXPECT_EQ(Kinesis::specular_occlusion(cos_view, 0.0F, roughness), 0.0F);
            const float held = Kinesis::specular_occlusion(cos_view, 0.4F, roughness);
            EXPECT_GE(held, 0.0F);
            EXPECT_LE(held, 1.0F);
        }
    }
}

} // namespace MayaFlux::Test
