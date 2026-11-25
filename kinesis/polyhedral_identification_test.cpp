#include "gtest/gtest.h"
#include "../support/polyhedral_support.hpp"
#include "../support/test_metadata.hpp"

#include "MayaFlux/Kinesis/Polyhedral/Identification.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {

namespace {

    using Kinesis::AttributeSupport;
    using Kinesis::Incidence;

    std::vector<uint32_t> per_corner_face(const Incidence& incidence)
    {
        return incidence.corner_faces();
    }

    bool all_distinct(const std::vector<uint32_t>& values)
    {
        std::vector<uint32_t> sorted = values;
        std::ranges::sort(sorted);
        return std::ranges::adjacent_find(sorted) == sorted.end();
    }

}

TEST(PolyhedralIdentificationTest, CornersThatAgreeEverywhereKeepTheirVertices)
{
    const auto cube = Polyhedral::unit_cube();
    const auto incidence = Incidence::create(cube.sizes, cube.corners, cube.positions.size());
    ASSERT_TRUE(incidence.has_value());

    const std::vector<float> same(24, 1.0F);
    const auto result = Kinesis::identify_corners(*incidence, std::span<const float>(same));

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->source_vertex.size(), 8U);
    EXPECT_EQ(result->corner_vertex, cube.corners);
}

TEST(PolyhedralIdentificationTest, WithNoChannelEveryVertexStaysWhole)
{
    const auto cube = Polyhedral::unit_cube();
    const auto incidence = Incidence::create(cube.sizes, cube.corners, cube.positions.size());
    ASSERT_TRUE(incidence.has_value());

    const auto result = Kinesis::identify_corners(*incidence);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->corner_vertex, cube.corners);
}

TEST(PolyhedralIdentificationTest, AFaceDependentChannelGivesEveryCubeCornerItsOwnVertex)
{
    const auto cube = Polyhedral::unit_cube();
    const auto incidence = Incidence::create(cube.sizes, cube.corners, cube.positions.size());
    ASSERT_TRUE(incidence.has_value());

    const auto faces = per_corner_face(*incidence);
    const auto result = Kinesis::identify_corners(*incidence, std::span<const uint32_t>(faces));

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->source_vertex.size(), 24U);
    EXPECT_TRUE(all_distinct(result->corner_vertex));
    for (uint32_t corner = 0; corner < 24; ++corner) {
        EXPECT_EQ(result->source_vertex[result->corner_vertex[corner]], cube.corners[corner]);
    }
}

TEST(PolyhedralIdentificationTest, ASplitHappensWhereAnyOneChannelDisagrees)
{
    const auto cube = Polyhedral::unit_cube();
    const auto incidence = Incidence::create(cube.sizes, cube.corners, cube.positions.size());
    ASSERT_TRUE(incidence.has_value());

    const std::vector<glm::vec2> agree(24, glm::vec2(0.5F));
    std::vector<glm::vec3> by_axis(24);
    for (uint32_t corner = 0; corner < 24; ++corner) {
        by_axis[corner] = Polyhedral::unit_cube_normals()[corner / 4];
    }

    const auto one = Kinesis::identify_corners(*incidence, std::span<const glm::vec2>(agree));
    const auto both = Kinesis::identify_corners(*incidence, std::span<const glm::vec2>(agree), std::span<const glm::vec3>(by_axis));

    ASSERT_TRUE(one.has_value());
    ASSERT_TRUE(both.has_value());
    EXPECT_EQ(one->source_vertex.size(), 8U);
    EXPECT_EQ(both->source_vertex.size(), 24U);
}

TEST(PolyhedralIdentificationTest, FacesThatAgreeShareTheVertexTheyMeetAt)
{
    const std::vector<uint32_t> sizes { 3, 3, 3 };
    const std::vector<uint32_t> corners { 0, 1, 2, 0, 2, 3, 0, 3, 4 };
    const auto incidence = Incidence::create(sizes, corners, 5);
    ASSERT_TRUE(incidence.has_value());

    const std::vector<uint32_t> group { 0, 0, 0, 1, 1, 1, 1, 1, 1 };
    const auto result = Kinesis::identify_corners(*incidence, std::span<const uint32_t>(group));

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->source_vertex.size(), 7U);
    EXPECT_NE(result->corner_vertex[0], result->corner_vertex[3]);
    EXPECT_EQ(result->corner_vertex[3], result->corner_vertex[6]);
}

TEST(PolyhedralIdentificationTest, AVertexNoCornerNamesHasNoNewVertex)
{
    const auto cube = Polyhedral::unit_cube();
    const auto incidence = Incidence::create(cube.sizes, cube.corners, 9);
    ASSERT_TRUE(incidence.has_value());

    const std::vector<float> same(24, 1.0F);
    const auto result = Kinesis::identify_corners(*incidence, std::span<const float>(same));

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->source_vertex, (std::vector<uint32_t> { 0, 1, 2, 3, 4, 5, 6, 7 }));
}

TEST(PolyhedralIdentificationTest, AChannelWithoutOneValuePerCornerIsRefused)
{
    const auto cube = Polyhedral::unit_cube();
    const auto incidence = Incidence::create(cube.sizes, cube.corners, cube.positions.size());
    ASSERT_TRUE(incidence.has_value());

    const std::vector<float> short_channel(23, 1.0F);
    EXPECT_FALSE(Kinesis::identify_corners(*incidence, std::span<const float>(short_channel)).has_value());
}

TEST(PolyhedralIdentificationTest, ASplitChannelRoundTripsThroughExpandToCorners)
{
    const auto cube = Polyhedral::unit_cube();
    const auto incidence = Incidence::create(cube.sizes, cube.corners, cube.positions.size());
    ASSERT_TRUE(incidence.has_value());

    const auto normals = Polyhedral::unit_cube_normals();
    const auto per_corner = incidence->expand_to_corners(std::span<const glm::vec3>(normals), AttributeSupport::Face);
    ASSERT_TRUE(per_corner.has_value());

    const auto result = Kinesis::identify_corners(*incidence, std::span<const glm::vec3>(*per_corner));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->source_vertex.size(), 24U);
}

TEST(PolyhedralIdentificationTest, LargeSurfacesTakeTheParallelPath)
{
    const auto grid = Polyhedral::unit_grid(150, 150);
    const auto incidence = Incidence::create(grid.sizes, grid.corners, grid.positions.size());
    ASSERT_TRUE(incidence.has_value());

    const auto faces = per_corner_face(*incidence);
    const auto split = Kinesis::identify_corners(*incidence, std::span<const uint32_t>(faces));
    ASSERT_TRUE(split.has_value());
    EXPECT_EQ(split->source_vertex.size(), 90000U);
    EXPECT_TRUE(all_distinct(split->corner_vertex));

    const std::vector<float> same(90000, 1.0F);
    const auto whole = Kinesis::identify_corners(*incidence, std::span<const float>(same));
    ASSERT_TRUE(whole.has_value());
    EXPECT_EQ(whole->source_vertex.size(), 22801U);
    EXPECT_EQ(whole->corner_vertex, grid.corners);
}

TEST(PolyhedralWeldTest, DuplicatedCubeCornersMergeBackToEightVertices)
{
    const auto cube = Polyhedral::unit_cube();
    std::vector<glm::vec3> separate;
    separate.reserve(cube.corners.size());
    for (const uint32_t vertex : cube.corners) {
        separate.push_back(cube.positions[vertex]);
    }

    const auto welded = Kinesis::merge_coincident_positions(separate, 1.0e-4F);

    ASSERT_TRUE(welded.has_value());
    EXPECT_EQ(welded->source_vertex.size(), 8U);
    for (size_t a = 0; a < separate.size(); ++a) {
        for (size_t b = 0; b < separate.size(); ++b) {
            EXPECT_EQ(welded->vertex_map[a] == welded->vertex_map[b], cube.corners[a] == cube.corners[b]);
        }
    }
}

TEST(PolyhedralWeldTest, MergedVerticesAreNumberedByTheirLowestPointInOrder)
{
    const std::vector<glm::vec3> points {
        glm::vec3(5.0F, 0.0F, 0.0F), glm::vec3(0.0F), glm::vec3(5.0F, 0.0F, 0.00005F), glm::vec3(0.0F, 0.00002F, 0.0F), glm::vec3(9.0F)
    };

    const auto welded = Kinesis::merge_coincident_positions(points, 1.0e-4F);

    ASSERT_TRUE(welded.has_value());
    EXPECT_EQ(welded->source_vertex, (std::vector<uint32_t> { 0, 1, 4 }));
    EXPECT_EQ(welded->vertex_map, (std::vector<uint32_t> { 0, 1, 0, 1, 2 }));
}

TEST(PolyhedralWeldTest, ClosenessChainsThroughNeighbours)
{
    const std::vector<glm::vec3> chain { glm::vec3(0.0F), glm::vec3(0.00009F, 0.0F, 0.0F), glm::vec3(0.00018F, 0.0F, 0.0F) };

    const auto welded = Kinesis::merge_coincident_positions(chain, 1.0e-4F);

    ASSERT_TRUE(welded.has_value());
    EXPECT_EQ(welded->source_vertex.size(), 1U);
}

TEST(PolyhedralWeldTest, PointsFartherApartThanTheToleranceStaySeparate)
{
    const std::vector<glm::vec3> points { glm::vec3(0.0F), glm::vec3(0.0003F, 0.0F, 0.0F) };

    const auto welded = Kinesis::merge_coincident_positions(points, 1.0e-4F);

    ASSERT_TRUE(welded.has_value());
    EXPECT_EQ(welded->source_vertex.size(), 2U);
}

TEST(PolyhedralWeldTest, NeighbouringCellsAreSearchedInEveryDirection)
{
    const std::vector<glm::vec3> points {
        glm::vec3(0.99F, 0.99F, 0.99F), glm::vec3(1.01F, 1.01F, 1.01F),
        glm::vec3(1.01F, 0.99F, 1.01F), glm::vec3(0.99F, 1.01F, 0.99F)
    };

    const auto welded = Kinesis::merge_coincident_positions(points, 0.05F);

    ASSERT_TRUE(welded.has_value());
    EXPECT_EQ(welded->source_vertex.size(), 1U);
}

TEST(PolyhedralWeldTest, AToleranceOfZeroMergesOnlyEqualPositionsAndTreatsNegativeZeroAsZero)
{
    const std::vector<glm::vec3> points {
        glm::vec3(0.0F), glm::vec3(-0.0F, 0.0F, 0.0F), glm::vec3(0.0F, 0.0F, 1.0e-7F), glm::vec3(0.0F)
    };

    const auto welded = Kinesis::merge_coincident_positions(points, 0.0F);

    ASSERT_TRUE(welded.has_value());
    EXPECT_EQ(welded->vertex_map, (std::vector<uint32_t> { 0, 0, 1, 0 }));
}

TEST(PolyhedralWeldTest, APointThatIsNotFiniteMergesWithNothing)
{
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const std::vector<glm::vec3> points { glm::vec3(nan, 0.0F, 0.0F), glm::vec3(nan, 0.0F, 0.0F), glm::vec3(1.0F) };

    const auto exact = Kinesis::merge_coincident_positions(points, 0.0F);
    const auto within = Kinesis::merge_coincident_positions(points, 0.1F);

    ASSERT_TRUE(exact.has_value());
    ASSERT_TRUE(within.has_value());
    EXPECT_EQ(exact->source_vertex.size(), 3U);
    EXPECT_EQ(within->source_vertex.size(), 3U);
}

TEST(PolyhedralWeldTest, ANegativeOrNonFiniteToleranceIsRefused)
{
    const std::vector<glm::vec3> points { glm::vec3(0.0F) };

    EXPECT_FALSE(Kinesis::merge_coincident_positions(points, -1.0F).has_value());
    EXPECT_FALSE(Kinesis::merge_coincident_positions(points, std::numeric_limits<float>::infinity()).has_value());
    EXPECT_FALSE(Kinesis::merge_coincident_positions(points, std::numeric_limits<float>::quiet_NaN()).has_value());
}

TEST(PolyhedralWeldTest, AGridOfDuplicatedPointsWeldsInParallelSizedInput)
{
    const auto grid = Polyhedral::unit_grid(150, 150);
    std::vector<glm::vec3> separate;
    separate.reserve(grid.corners.size());
    for (const uint32_t vertex : grid.corners) {
        separate.push_back(grid.positions[vertex]);
    }

    const auto welded = Kinesis::merge_coincident_positions(separate, 0.25F);

    ASSERT_TRUE(welded.has_value());
    EXPECT_EQ(welded->source_vertex.size(), grid.positions.size());
}

}
