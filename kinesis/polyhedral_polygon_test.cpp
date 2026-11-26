#include "gtest/gtest.h"
#include "../support/polyhedral_support.hpp"
#include "../support/test_metadata.hpp"

#include "MayaFlux/Kinesis/Polyhedral/Polygon.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {

namespace {

    using Kinesis::Incidence;
    using Polyhedral::approx;

    std::vector<glm::vec3> unit_square(float z = 0.0F)
    {
        return { glm::vec3(0.0F, 0.0F, z), glm::vec3(1.0F, 0.0F, z), glm::vec3(1.0F, 1.0F, z), glm::vec3(0.0F, 1.0F, z) };
    }

    std::vector<glm::vec3> polyhedral_polygon_dart()
    {
        return { glm::vec3(0.0F, 0.0F, 0.0F), glm::vec3(0.5F, 0.5F, 0.0F), glm::vec3(1.0F, 0.0F, 0.0F), glm::vec3(0.5F, 1.0F, 0.0F) };
    }

}

TEST(PolyhedralPolygonTest, ATriangleHasHalfTheCrossProductAsItsAreaVector)
{
    const std::vector<glm::vec3> ring { glm::vec3(0.0F), glm::vec3(2.0F, 0.0F, 0.0F), glm::vec3(0.0F, 3.0F, 0.0F) };
    const auto order = Polyhedral::ring_of(3);

    EXPECT_TRUE(approx(Kinesis::polygon_area_vector(ring, order), glm::vec3(0.0F, 0.0F, 3.0F), 1.0e-6F));
    EXPECT_FLOAT_EQ(Kinesis::polygon_area(ring, order), 3.0F);
}

TEST(PolyhedralPolygonTest, ReversingTheWindingFlipsTheNormalAndKeepsTheArea)
{
    const auto square = unit_square();
    const std::vector<uint32_t> forward { 0, 1, 2, 3 };
    const std::vector<uint32_t> backward { 3, 2, 1, 0 };

    const auto up = Kinesis::polygon_normal(square, forward);
    const auto down = Kinesis::polygon_normal(square, backward);
    ASSERT_TRUE(up.has_value());
    ASSERT_TRUE(down.has_value());
    EXPECT_TRUE(approx(*up, glm::vec3(0.0F, 0.0F, 1.0F), 1.0e-6F));
    EXPECT_TRUE(approx(*down, glm::vec3(0.0F, 0.0F, -1.0F), 1.0e-6F));
    EXPECT_FLOAT_EQ(Kinesis::polygon_area(square, forward), Kinesis::polygon_area(square, backward));
}

TEST(PolyhedralPolygonTest, TheResultDoesNotDependOnWhereTheRingStarts)
{
    const auto shape = Polyhedral::l_shape();
    const std::vector<uint32_t> from_start { 0, 1, 2, 3, 4, 5 };
    const std::vector<uint32_t> rotated { 3, 4, 5, 0, 1, 2 };

    EXPECT_TRUE(approx(Kinesis::polygon_area_vector(shape, from_start), Kinesis::polygon_area_vector(shape, rotated), 1.0e-6F));
}

TEST(PolyhedralPolygonTest, AUnitSquareFarFromTheOriginKeepsAnExactArea)
{
    std::vector<glm::vec3> square = unit_square();
    for (auto& p : square) {
        p += glm::vec3(4096.0F, 4096.0F, 4096.0F);
    }

    EXPECT_EQ(Kinesis::polygon_area(square, Polyhedral::ring_of(4)), 1.0F);
}

TEST(PolyhedralPolygonTest, ALShapedHexagonHasAreaThreeAndAnAreaWeightedCentroid)
{
    const auto shape = Polyhedral::l_shape();
    const auto order = Polyhedral::ring_of(6);

    EXPECT_NEAR(Kinesis::polygon_area(shape, order), 3.0F, 1.0e-6F);

    const glm::vec3 centroid = Kinesis::polygon_centroid(shape, order);
    EXPECT_NEAR(centroid.x, 5.0F / 6.0F, 1.0e-6F);
    EXPECT_NEAR(centroid.y, 5.0F / 6.0F, 1.0e-6F);
    EXPECT_NEAR(centroid.z, 0.0F, 1.0e-6F);
}

TEST(PolyhedralPolygonTest, AFlatPolygonHasNoPlanarityDeviationAndAWarpedOneDoes)
{
    const auto flat = unit_square();
    auto warped = unit_square();
    warped[3].z = 0.2F;
    const auto order = Polyhedral::ring_of(4);

    const auto flat_deviation = Kinesis::polygon_planarity(flat, order);
    const auto warped_deviation = Kinesis::polygon_planarity(warped, order);
    ASSERT_TRUE(flat_deviation.has_value());
    ASSERT_TRUE(warped_deviation.has_value());
    EXPECT_LT(*flat_deviation, 1.0e-6F);
    EXPECT_GT(*warped_deviation, 0.01F);
}

TEST(PolyhedralPolygonTest, ATriangleIsAlwaysPlanar)
{
    const std::vector<glm::vec3> triangle { glm::vec3(0.0F), glm::vec3(1.0F, 0.0F, 3.0F), glm::vec3(0.0F, 2.0F, 1.0F) };
    EXPECT_EQ(Kinesis::polygon_planarity(triangle, Polyhedral::ring_of(3)).value_or(-1.0F), 0.0F);
}

TEST(PolyhedralPolygonTest, ACollinearRingHasNoNormalNoAreaAndTheVertexMeanAsCentroid)
{
    const std::vector<glm::vec3> line {
        glm::vec3(0.0F), glm::vec3(1.0F, 0.0F, 0.0F), glm::vec3(2.0F, 0.0F, 0.0F), glm::vec3(3.0F, 0.0F, 0.0F)
    };
    const auto order = Polyhedral::ring_of(4);

    EXPECT_FALSE(Kinesis::polygon_normal(line, order).has_value());
    EXPECT_FALSE(Kinesis::polygon_planarity(line, order).has_value());
    EXPECT_FALSE(Kinesis::polygon_is_convex(line, order));
    EXPECT_EQ(Kinesis::polygon_area(line, order), 0.0F);
    EXPECT_TRUE(approx(Kinesis::polygon_centroid(line, order), glm::vec3(1.5F, 0.0F, 0.0F), 1.0e-6F));
}

TEST(PolyhedralPolygonTest, ARingOfFewerThanThreeIsNotAPolygon)
{
    const std::vector<glm::vec3> pair { glm::vec3(0.0F), glm::vec3(1.0F, 0.0F, 0.0F) };
    const auto order = Polyhedral::ring_of(2);

    EXPECT_EQ(Kinesis::polygon_area(pair, order), 0.0F);
    EXPECT_FALSE(Kinesis::polygon_normal(pair, order).has_value());
    EXPECT_FALSE(Kinesis::polygon_is_convex(pair, order));
}

TEST(PolyhedralPolygonTest, ConvexOutlinesAreConvex)
{
    const auto square = unit_square();
    const auto hexagon = Polyhedral::regular(6);
    const std::vector<glm::vec3> triangle { glm::vec3(0.0F), glm::vec3(1.0F, 0.0F, 0.0F), glm::vec3(0.0F, 1.0F, 0.0F) };

    EXPECT_TRUE(Kinesis::polygon_is_convex(square, Polyhedral::ring_of(4)));
    EXPECT_TRUE(Kinesis::polygon_is_convex(hexagon, Polyhedral::ring_of(6)));
    EXPECT_TRUE(Kinesis::polygon_is_convex(triangle, Polyhedral::ring_of(3)));
}

TEST(PolyhedralPolygonTest, ACollinearVertexOnAConvexOutlineKeepsItConvex)
{
    const std::vector<glm::vec3> ring {
        glm::vec3(0.0F), glm::vec3(0.5F, 0.0F, 0.0F), glm::vec3(1.0F, 0.0F, 0.0F),
        glm::vec3(1.0F, 1.0F, 0.0F), glm::vec3(0.0F, 1.0F, 0.0F)
    };
    EXPECT_TRUE(Kinesis::polygon_is_convex(ring, Polyhedral::ring_of(5)));
}

TEST(PolyhedralPolygonTest, ConcaveOutlinesAreNotConvex)
{
    const auto shape = Polyhedral::l_shape();
    const auto arrow = polyhedral_polygon_dart();

    EXPECT_FALSE(Kinesis::polygon_is_convex(shape, Polyhedral::ring_of(6)));
    EXPECT_FALSE(Kinesis::polygon_is_convex(arrow, Polyhedral::ring_of(4)));
}

TEST(PolyhedralPolygonTest, AStarPolygonTurnsTheSameWayAtEveryVertexYetIsNotConvex)
{
    const auto pentagon = Polyhedral::regular(5);
    const std::vector<uint32_t> star { 0, 2, 4, 1, 3 };

    EXPECT_TRUE(Kinesis::polygon_is_convex(pentagon, Polyhedral::ring_of(5)));
    EXPECT_FALSE(Kinesis::polygon_is_convex(pentagon, star));
}

TEST(PolyhedralPolygonTest, BatchNormalsAndAreasOfACubeAreOutwardAndUnit)
{
    const auto cube = Polyhedral::unit_cube();
    const auto incidence = Incidence::create(cube.sizes, cube.corners, cube.positions.size());
    ASSERT_TRUE(incidence.has_value());

    const auto normals = Kinesis::face_normals(cube.positions, *incidence);
    const auto areas = Kinesis::face_areas(cube.positions, *incidence);
    const auto vectors = Kinesis::face_area_vectors(cube.positions, *incidence);
    ASSERT_TRUE(normals.has_value());
    ASSERT_TRUE(areas.has_value());
    ASSERT_TRUE(vectors.has_value());

    const auto expected = Polyhedral::unit_cube_normals();
    for (size_t face = 0; face < 6; ++face) {
        EXPECT_TRUE(approx((*normals)[face], expected[face], 1.0e-6F)) << "face " << face;
        EXPECT_NEAR((*areas)[face], 1.0F, 1.0e-6F);
        EXPECT_TRUE(approx((*vectors)[face], expected[face], 1.0e-6F));
    }
}

TEST(PolyhedralPolygonTest, ADegenerateFaceGetsTheZeroNormalInABatch)
{
    const std::vector<glm::vec3> positions {
        glm::vec3(0.0F), glm::vec3(1.0F, 0.0F, 0.0F), glm::vec3(2.0F, 0.0F, 0.0F), glm::vec3(3.0F, 0.0F, 0.0F)
    };
    const std::vector<uint32_t> sizes { 4 };
    const std::vector<uint32_t> corners { 0, 1, 2, 3 };
    const auto incidence = Incidence::create(sizes, corners, 4);
    ASSERT_TRUE(incidence.has_value());

    const auto normals = Kinesis::face_normals(positions, *incidence);
    ASSERT_TRUE(normals.has_value());
    EXPECT_EQ((*normals)[0], glm::vec3(0.0F));
}

TEST(PolyhedralPolygonTest, BatchFunctionsRefuseTooFewPositions)
{
    const auto cube = Polyhedral::unit_cube();
    const auto incidence = Incidence::create(cube.sizes, cube.corners, cube.positions.size());
    ASSERT_TRUE(incidence.has_value());

    const std::span<const glm::vec3> short_positions = std::span<const glm::vec3>(cube.positions).first(7);
    EXPECT_FALSE(Kinesis::face_normals(short_positions, *incidence).has_value());
    EXPECT_FALSE(Kinesis::face_areas(short_positions, *incidence).has_value());
    EXPECT_FALSE(Kinesis::face_area_vectors(short_positions, *incidence).has_value());
}

TEST(PolyhedralPolygonTest, LargeSurfacesTakeTheParallelPath)
{
    const auto grid = Polyhedral::unit_grid(150, 150);
    const auto incidence = Incidence::create(grid.sizes, grid.corners, grid.positions.size());
    ASSERT_TRUE(incidence.has_value());

    const auto normals = Kinesis::face_normals(grid.positions, *incidence);
    const auto areas = Kinesis::face_areas(grid.positions, *incidence);
    ASSERT_TRUE(normals.has_value());
    ASSERT_TRUE(areas.has_value());
    ASSERT_EQ(normals->size(), 22500U);

    EXPECT_TRUE(std::ranges::all_of(*normals, [](const glm::vec3& n) { return n == glm::vec3(0.0F, 0.0F, 1.0F); }));
    EXPECT_TRUE(std::ranges::all_of(*areas, [](float a) { return a == 1.0F; }));
}

}
