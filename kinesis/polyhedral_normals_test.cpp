#include "gtest/gtest.h"
#include "../support/polyhedral_support.hpp"
#include "../support/test_metadata.hpp"

#include "MayaFlux/Kinesis/Polyhedral/Normals.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {

namespace {

    using Kinesis::Incidence;
    using Kinesis::NormalWeighting;
    using Polyhedral::approx;

    glm::vec3 cube_corner_direction(const glm::vec3& position)
    {
        return glm::normalize(position * 2.0F - glm::vec3(1.0F));
    }

    struct Pair {
        std::vector<glm::vec3> positions;
        std::vector<uint32_t> sizes;
        std::vector<uint32_t> corners;
    };

    Pair large_and_small_quad()
    {
        return {
            .positions = { glm::vec3(0.0F, 0.0F, 0.0F), glm::vec3(2.0F, 0.0F, 0.0F), glm::vec3(2.0F, 2.0F, 0.0F),
                glm::vec3(0.0F, 2.0F, 0.0F), glm::vec3(0.0F, 1.0F, 0.0F), glm::vec3(0.0F, 1.0F, 1.0F),
                glm::vec3(0.0F, 0.0F, 1.0F) },
            .sizes = { 4, 4 },
            .corners = { 0, 1, 2, 3, 0, 4, 5, 6 }
        };
    }

    Pair wide_and_narrow_triangle()
    {
        return {
            .positions = { glm::vec3(0.0F), glm::vec3(1.0F, 0.0F, 0.0F), glm::vec3(0.0F, 1.0F, 0.0F),
                glm::vec3(10.0F, 0.0F, 0.0F), glm::vec3(10.0F, 0.0F, 1.0F) },
            .sizes = { 3, 3 },
            .corners = { 0, 1, 2, 0, 3, 4 }
        };
    }

    std::vector<glm::vec3> normals_of(const Pair& pair, NormalWeighting weighting)
    {
        const auto incidence = Incidence::create(pair.sizes, pair.corners, pair.positions.size());
        EXPECT_TRUE(incidence.has_value());
        return Kinesis::weighted_vertex_normals(pair.positions, *incidence, weighting).value_or(std::vector<glm::vec3> {});
    }

}

TEST(PolyhedralNormalsTest, EveryWeightingGivesACubeCornerItsDiagonalDirection)
{
    const auto cube = Polyhedral::unit_cube();
    const auto incidence = Incidence::create(cube.sizes, cube.corners, cube.positions.size());
    ASSERT_TRUE(incidence.has_value());

    for (const auto weighting : { NormalWeighting::Uniform, NormalWeighting::Area, NormalWeighting::Angle }) {
        const auto normals = Kinesis::weighted_vertex_normals(cube.positions, *incidence, weighting);
        ASSERT_TRUE(normals.has_value());
        for (size_t vertex = 0; vertex < 8; ++vertex) {
            EXPECT_TRUE(approx((*normals)[vertex], cube_corner_direction(cube.positions[vertex]), 1.0e-5F))
                << "vertex " << vertex << " weighting " << static_cast<int>(weighting);
        }
    }
}

TEST(PolyhedralNormalsTest, AreaWeightingFavoursTheLargerFaceAndUniformDoesNot)
{
    const auto pair = large_and_small_quad();

    const auto uniform = normals_of(pair, NormalWeighting::Uniform);
    const auto area = normals_of(pair, NormalWeighting::Area);
    ASSERT_EQ(uniform.size(), 7U);

    const float half = std::sqrt(0.5F);
    EXPECT_TRUE(approx(uniform[0], glm::vec3(half, 0.0F, half), 1.0e-6F));
    const float root = std::sqrt(17.0F);
    EXPECT_TRUE(approx(area[0], glm::vec3(1.0F / root, 0.0F, 4.0F / root), 1.0e-6F));
}

TEST(PolyhedralNormalsTest, AngleWeightingFollowsTheInteriorAngleAtTheVertex)
{
    const auto pair = wide_and_narrow_triangle();

    const auto angle = normals_of(pair, NormalWeighting::Angle);
    const auto area = normals_of(pair, NormalWeighting::Area);
    ASSERT_EQ(angle.size(), 5U);

    const float expected_ratio = -std::atan(0.1F) / (std::numbers::pi_v<float> / 2.0F);
    EXPECT_NEAR(angle[0].y / angle[0].z, expected_ratio, 1.0e-4F);
    EXPECT_NEAR(angle[0].x, 0.0F, 1.0e-6F);
    EXPECT_NEAR(area[0].y / area[0].z, -10.0F, 1.0e-4F);
}

TEST(PolyhedralNormalsTest, ANormalIsUnitLengthWhereAnyFaceTouches)
{
    const auto pair = wide_and_narrow_triangle();
    for (const auto weighting : { NormalWeighting::Uniform, NormalWeighting::Area, NormalWeighting::Angle }) {
        const auto normals = normals_of(pair, weighting);
        ASSERT_EQ(normals.size(), 5U);
        for (const auto& n : normals) {
            EXPECT_NEAR(glm::length(n), 1.0F, 1.0e-5F);
        }
    }
}

TEST(PolyhedralNormalsTest, AVertexNoFaceNamesGetsTheZeroVector)
{
    const std::vector<glm::vec3> positions { glm::vec3(0.0F), glm::vec3(1.0F, 0.0F, 0.0F), glm::vec3(0.0F, 1.0F, 0.0F), glm::vec3(5.0F) };
    const std::vector<uint32_t> sizes { 3 };
    const std::vector<uint32_t> corners { 0, 1, 2 };
    const auto incidence = Incidence::create(sizes, corners, 4);
    ASSERT_TRUE(incidence.has_value());

    const auto normals = Kinesis::weighted_vertex_normals(positions, *incidence);
    ASSERT_TRUE(normals.has_value());
    EXPECT_EQ((*normals)[3], glm::vec3(0.0F));
    EXPECT_TRUE(approx((*normals)[0], glm::vec3(0.0F, 0.0F, 1.0F), 1.0e-6F));
}

TEST(PolyhedralNormalsTest, TooFewPositionsAreRefused)
{
    const auto cube = Polyhedral::unit_cube();
    const auto incidence = Incidence::create(cube.sizes, cube.corners, cube.positions.size());
    ASSERT_TRUE(incidence.has_value());

    EXPECT_FALSE(Kinesis::weighted_vertex_normals(std::span<const glm::vec3>(cube.positions).first(4), *incidence).has_value());
}

TEST(PolyhedralNormalsTest, LargeSurfacesTakeTheParallelPath)
{
    const auto grid = Polyhedral::unit_grid(150, 150);
    const auto incidence = Incidence::create(grid.sizes, grid.corners, grid.positions.size());
    ASSERT_TRUE(incidence.has_value());

    const auto normals = Kinesis::weighted_vertex_normals(grid.positions, *incidence);
    ASSERT_TRUE(normals.has_value());
    ASSERT_EQ(normals->size(), grid.positions.size());
    EXPECT_TRUE(std::ranges::all_of(*normals, [](const glm::vec3& n) { return glm::length(n - glm::vec3(0.0F, 0.0F, 1.0F)) < 1.0e-6F; }));
}

}
