#include "gtest/gtest.h"
#include "../support/polyhedral_support.hpp"
#include "../support/test_metadata.hpp"

#include "MayaFlux/Kinesis/Polyhedral/Polygon.hpp"
#include "MayaFlux/Kinesis/Polyhedral/Triangulation.hpp"

#include <glm/gtc/matrix_transform.hpp>

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {

namespace {

    using Kinesis::Incidence;

    Kinesis::Triangulation triangulate(const Polyhedral::Surface& surface, bool shorter_quad_diagonal = true)
    {
        const auto incidence = Incidence::create(surface.sizes, surface.corners, surface.positions.size());
        EXPECT_TRUE(incidence.has_value());
        const auto result = Kinesis::triangulate_faces(surface.positions, *incidence, shorter_quad_diagonal);
        EXPECT_TRUE(result.has_value());
        return result.value_or(Kinesis::Triangulation {});
    }

    std::vector<glm::vec3> rhombus()
    {
        return { glm::vec3(0.0F, 0.0F, 0.0F), glm::vec3(4.0F, 0.0F, 0.0F), glm::vec3(5.0F, 2.0F, 0.0F), glm::vec3(0.0F, 1.0F, 0.0F) };
    }

    glm::vec3 triangle_normal(const Polyhedral::Surface& surface, const std::vector<uint32_t>& indices, size_t triangle)
    {
        const std::array<uint32_t, 3> ring { indices[3 * triangle], indices[3 * triangle + 1], indices[3 * triangle + 2] };
        return Kinesis::polygon_area_vector(surface.positions, ring);
    }

}

TEST(PolyhedralTriangulationTest, ATriangleIsKeptAsItIs)
{
    const auto surface = Polyhedral::single_face({ glm::vec3(0.0F), glm::vec3(1.0F, 0.0F, 0.0F), glm::vec3(0.0F, 1.0F, 0.0F) });
    const auto result = triangulate(surface);

    EXPECT_EQ(result.indices, (std::vector<uint32_t> { 0, 1, 2 }));
    EXPECT_EQ(result.triangle_face, (std::vector<uint32_t> { 0 }));
    EXPECT_TRUE(result.failed_faces.empty());
}

TEST(PolyhedralTriangulationTest, AConvexQuadSplitsAlongTheShorterDiagonalByDefault)
{
    const auto surface = Polyhedral::single_face(rhombus());
    const auto result = triangulate(surface);

    EXPECT_EQ(result.indices, (std::vector<uint32_t> { 1, 2, 3, 1, 3, 0 }));
    EXPECT_NEAR(Polyhedral::triangle_list_area(surface.positions, result.indices), 6.5, 1.0e-5);
}

TEST(PolyhedralTriangulationTest, TheFirstDiagonalCanBeAskedFor)
{
    const auto surface = Polyhedral::single_face(rhombus());
    const auto result = triangulate(surface, false);

    EXPECT_EQ(result.indices, (std::vector<uint32_t> { 0, 1, 2, 0, 2, 3 }));
    EXPECT_NEAR(Polyhedral::triangle_list_area(surface.positions, result.indices), 6.5, 1.0e-5);
}

TEST(PolyhedralTriangulationTest, ATieBetweenDiagonalsTakesTheFirst)
{
    const auto surface = Polyhedral::single_face(
        { glm::vec3(0.0F), glm::vec3(1.0F, 0.0F, 0.0F), glm::vec3(1.0F, 1.0F, 0.0F), glm::vec3(0.0F, 1.0F, 0.0F) });
    const auto result = triangulate(surface);

    EXPECT_EQ(result.indices, (std::vector<uint32_t> { 0, 1, 2, 0, 2, 3 }));
}

TEST(PolyhedralTriangulationTest, AConvexPolygonBeyondFourCornersIsAFanFromTheFirst)
{
    const auto surface = Polyhedral::single_face(Polyhedral::regular(6));
    const auto result = triangulate(surface);

    EXPECT_EQ(result.indices, (std::vector<uint32_t> { 0, 1, 2, 0, 2, 3, 0, 3, 4, 0, 4, 5 }));
    EXPECT_EQ(result.triangle_face, (std::vector<uint32_t> { 0, 0, 0, 0 }));
}

TEST(PolyhedralTriangulationTest, AConcaveHexagonIsTiledWithTheSameOrientation)
{
    const auto surface = Polyhedral::single_face(Polyhedral::l_shape());
    const auto result = triangulate(surface);

    ASSERT_EQ(result.indices.size(), 12U);
    EXPECT_TRUE(result.failed_faces.empty());
    EXPECT_NEAR(Polyhedral::triangle_list_area(surface.positions, result.indices), 3.0, 1.0e-5);
    for (size_t t = 0; t < 4; ++t) {
        EXPECT_GT(triangle_normal(surface, result.indices, t).z, 0.0F) << "triangle " << t;
    }
}

TEST(PolyhedralTriangulationTest, AConcaveQuadIsSplitInsideItself)
{
    const auto surface = Polyhedral::single_face(
        { glm::vec3(0.0F, 0.0F, 0.0F), glm::vec3(0.5F, 0.5F, 0.0F), glm::vec3(1.0F, 0.0F, 0.0F), glm::vec3(0.5F, 1.0F, 0.0F) });
    const auto result = triangulate(surface);

    ASSERT_EQ(result.indices.size(), 6U);
    EXPECT_NEAR(Polyhedral::triangle_list_area(surface.positions, result.indices), 0.25, 1.0e-5);
    for (size_t t = 0; t < 2; ++t) {
        EXPECT_GT(triangle_normal(surface, result.indices, t).z, 0.0F);
    }
}

TEST(PolyhedralTriangulationTest, AConcaveFaceInATiltedPlaneTriangulatesTheSame)
{
    const glm::mat4 frame = glm::translate(glm::mat4(1.0F), glm::vec3(5.0F, -3.0F, 2.0F))
        * glm::rotate(glm::mat4(1.0F), 0.7F, glm::normalize(glm::vec3(1.0F, 2.0F, 3.0F)));

    auto ring = Polyhedral::l_shape();
    for (auto& p : ring) {
        p = glm::vec3(frame * glm::vec4(p, 1.0F));
    }
    const auto surface = Polyhedral::single_face(ring);
    const auto result = triangulate(surface);

    ASSERT_EQ(result.indices.size(), 12U);
    EXPECT_NEAR(Polyhedral::triangle_list_area(surface.positions, result.indices), 3.0, 1.0e-4);

    const auto face_normal = Kinesis::polygon_normal(surface.positions, Polyhedral::ring_of(6));
    ASSERT_TRUE(face_normal.has_value());
    for (size_t t = 0; t < 4; ++t) {
        EXPECT_GT(glm::dot(glm::normalize(triangle_normal(surface, result.indices, t)), *face_normal), 0.99F);
    }
}

TEST(PolyhedralTriangulationTest, AQuadThatCrossesItselfStillBecomesTwoTriangles)
{
    const auto surface = Polyhedral::single_face(
        { glm::vec3(0.0F, 0.0F, 0.0F), glm::vec3(4.0F, 1.0F, 0.0F), glm::vec3(4.0F, 0.0F, 0.0F), glm::vec3(0.0F, 2.0F, 0.0F) });
    const auto shorter = triangulate(surface);
    const auto first = triangulate(surface, false);

    EXPECT_TRUE(shorter.failed_faces.empty());
    EXPECT_EQ(shorter.indices.size(), 6U);
    EXPECT_EQ(shorter.triangle_face, (std::vector<uint32_t> { 0, 0 }));
    EXPECT_EQ(first.indices, (std::vector<uint32_t> { 0, 1, 2, 0, 2, 3 }));
}

TEST(PolyhedralTriangulationTest, ABowtieWhoseHalvesCancelIsNotMistakenForAFaceWithNoArea)
{
    const auto surface = Polyhedral::single_face(
        { glm::vec3(0.0F, 0.0F, 0.0F), glm::vec3(1.0F, 1.0F, 0.0F), glm::vec3(1.0F, 0.0F, 0.0F), glm::vec3(0.0F, 1.0F, 0.0F) });
    const auto result = triangulate(surface);

    EXPECT_TRUE(result.failed_faces.empty());
    EXPECT_EQ(result.indices.size(), 6U);
}

TEST(PolyhedralTriangulationTest, FacesThatCannotBeTriangulatedAreReportedAndLeftOut)
{
    Polyhedral::Surface surface;
    surface.positions = Polyhedral::regular(5);
    for (const auto& p : std::vector<glm::vec3> { glm::vec3(10.0F, 0.0F, 0.0F), glm::vec3(11.0F, 0.0F, 0.0F),
             glm::vec3(11.0F, 1.0F, 0.0F), glm::vec3(10.0F, 1.0F, 0.0F) }) {
        surface.positions.push_back(p);
    }
    surface.sizes = { 5, 4 };
    surface.corners = { 0, 2, 4, 1, 3, 5, 6, 7, 8 };

    const auto result = triangulate(surface);

    EXPECT_EQ(result.failed_faces, (std::vector<uint32_t> { 0 }));
    EXPECT_EQ(result.indices.size(), 6U);
    EXPECT_EQ(result.triangle_face, (std::vector<uint32_t> { 1, 1 }));
}

TEST(PolyhedralTriangulationTest, ACollinearFaceFailsWithoutAffectingItsNeighbours)
{
    Polyhedral::Surface surface = Polyhedral::unit_grid(1, 1);
    surface.positions.insert(surface.positions.end(),
        { glm::vec3(0.0F, 5.0F, 0.0F), glm::vec3(1.0F, 5.0F, 0.0F), glm::vec3(2.0F, 5.0F, 0.0F), glm::vec3(3.0F, 5.0F, 0.0F) });
    surface.sizes.push_back(4);
    surface.corners.insert(surface.corners.end(), { 4, 5, 6, 7 });

    const auto result = triangulate(surface);

    EXPECT_EQ(result.failed_faces, (std::vector<uint32_t> { 1 }));
    EXPECT_EQ(result.indices.size(), 6U);
    EXPECT_EQ(result.triangle_face, (std::vector<uint32_t> { 0, 0 }));
}

TEST(PolyhedralTriangulationTest, ACubeBecomesTwelveTrianglesFacingOutward)
{
    const auto cube = Polyhedral::unit_cube();
    const auto result = triangulate(cube);

    ASSERT_EQ(result.indices.size(), 36U);
    ASSERT_EQ(result.triangle_face.size(), 12U);
    EXPECT_NEAR(Polyhedral::triangle_list_area(cube.positions, result.indices), 6.0, 1.0e-5);

    const auto normals = Polyhedral::unit_cube_normals();
    for (size_t t = 0; t < 12; ++t) {
        EXPECT_EQ(result.triangle_face[t], t / 2);
        EXPECT_GT(glm::dot(triangle_normal(cube, result.indices, t), normals[result.triangle_face[t]]), 0.0F);
    }
}

TEST(PolyhedralTriangulationTest, LargeSurfacesAreTriangulatedInParallelInFaceOrder)
{
    const auto grid = Polyhedral::unit_grid(150, 150);
    const auto result = triangulate(grid);

    ASSERT_EQ(result.indices.size(), 22500U * 6U);
    EXPECT_TRUE(result.failed_faces.empty());
    EXPECT_EQ(std::vector<uint32_t>(result.indices.begin(), result.indices.begin() + 6),
        (std::vector<uint32_t> { 0, 1, 152, 0, 152, 151 }));
    EXPECT_NEAR(Polyhedral::triangle_list_area(grid.positions, result.indices), 22500.0, 1.0e-3);

    bool in_order = true;
    for (uint32_t t = 0; t < result.triangle_face.size(); ++t) {
        in_order = in_order && result.triangle_face[t] == t / 2;
    }
    EXPECT_TRUE(in_order);
}

TEST(PolyhedralTriangulationTest, TooFewPositionsAreRefused)
{
    const auto cube = Polyhedral::unit_cube();
    const auto incidence = Incidence::create(cube.sizes, cube.corners, cube.positions.size());
    ASSERT_TRUE(incidence.has_value());

    EXPECT_FALSE(Kinesis::triangulate_faces(std::span<const glm::vec3>(cube.positions).first(5), *incidence).has_value());
}

}
