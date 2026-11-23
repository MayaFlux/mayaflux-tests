#include "gtest/gtest.h"
#include "../support/polyhedral_support.hpp"
#include "../support/test_metadata.hpp"

#include "MayaFlux/Kinesis/Polyhedral/Incidence.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {

namespace {

    using Kinesis::AttributeSupport;
    using Kinesis::Incidence;

    std::vector<uint32_t> copy_of(std::span<const uint32_t> view)
    {
        return { view.begin(), view.end() };
    }

}

TEST(PolyhedralIncidenceTest, ACubeIsSixQuadsOverEightVertices)
{
    const auto cube = Polyhedral::unit_cube();
    const auto incidence = Incidence::create(cube.sizes, cube.corners, cube.positions.size());

    ASSERT_TRUE(incidence.has_value());
    EXPECT_EQ(incidence->face_count(), 6U);
    EXPECT_EQ(incidence->corner_count(), 24U);
    EXPECT_EQ(incidence->vertex_count(), 8U);
    EXPECT_EQ(copy_of(incidence->offsets()), (std::vector<uint32_t> { 0, 4, 8, 12, 16, 20, 24 }));
}

TEST(PolyhedralIncidenceTest, AFaceViewNamesItsOwnVerticesInOrder)
{
    const auto cube = Polyhedral::unit_cube();
    const auto incidence = Incidence::create(cube.sizes, cube.corners, cube.positions.size());

    ASSERT_TRUE(incidence.has_value());
    EXPECT_EQ(copy_of(incidence->face(2)), (std::vector<uint32_t> { 0, 1, 5, 4 }));
    EXPECT_EQ(copy_of(incidence->face(5)), (std::vector<uint32_t> { 1, 2, 6, 5 }));
}

TEST(PolyhedralIncidenceTest, MixedFaceSizesGetTheirOwnOffsets)
{
    const std::vector<uint32_t> sizes { 3, 5, 4 };
    const std::vector<uint32_t> corners { 0, 1, 2, 2, 3, 4, 5, 0, 1, 2, 3, 4 };
    const auto incidence = Incidence::create(sizes, corners, 6);

    ASSERT_TRUE(incidence.has_value());
    EXPECT_EQ(copy_of(incidence->offsets()), (std::vector<uint32_t> { 0, 3, 8, 12 }));
    EXPECT_EQ(incidence->face(1).size(), 5U);
}

TEST(PolyhedralIncidenceTest, ARefusalNeverLeavesAnIncidence)
{
    const std::vector<uint32_t> two { 2, 4 };
    const std::vector<uint32_t> six { 0, 1, 0, 1, 2, 3 };
    EXPECT_FALSE(Incidence::create(two, six, 4).has_value());

    const std::vector<uint32_t> sizes { 4, 4 };
    const std::vector<uint32_t> seven { 0, 1, 2, 3, 0, 1, 2 };
    EXPECT_FALSE(Incidence::create(sizes, seven, 4).has_value());

    const std::vector<uint32_t> outside { 0, 1, 2, 3, 0, 1, 2, 4 };
    EXPECT_FALSE(Incidence::create(sizes, outside, 4).has_value());
    EXPECT_TRUE(Incidence::create(sizes, outside, 5).has_value());
}

TEST(PolyhedralIncidenceTest, ANoFaceSetIsValidAndEmpty)
{
    const std::vector<uint32_t> none;
    const auto incidence = Incidence::create(none, none, 0);

    ASSERT_TRUE(incidence.has_value());
    EXPECT_EQ(incidence->face_count(), 0U);
    EXPECT_EQ(incidence->corner_count(), 0U);
}

TEST(PolyhedralIncidenceTest, CornerFacesInvertsTheOffsets)
{
    const auto cube = Polyhedral::unit_cube();
    const auto incidence = Incidence::create(cube.sizes, cube.corners, cube.positions.size());
    ASSERT_TRUE(incidence.has_value());

    const auto faces = incidence->corner_faces();
    ASSERT_EQ(faces.size(), 24U);
    for (uint32_t corner = 0; corner < faces.size(); ++corner) {
        EXPECT_EQ(faces[corner], corner / 4) << "corner " << corner;
    }
}

TEST(PolyhedralIncidenceTest, VertexCornersListsEveryCornerOfAVertexInAscendingOrder)
{
    const auto cube = Polyhedral::unit_cube();
    const auto incidence = Incidence::create(cube.sizes, cube.corners, cube.positions.size());
    ASSERT_TRUE(incidence.has_value());

    const auto around = incidence->vertex_corners();
    size_t total = 0;
    for (uint32_t vertex = 0; vertex < 8; ++vertex) {
        const auto list = around.around(vertex);
        EXPECT_EQ(list.size(), 3U) << "vertex " << vertex;
        EXPECT_TRUE(std::ranges::is_sorted(list));
        for (const uint32_t corner : list) {
            EXPECT_EQ(cube.corners[corner], vertex);
        }
        total += list.size();
    }
    EXPECT_EQ(total, 24U);
}

TEST(PolyhedralIncidenceTest, AVertexNoFaceNamesHasNoCorners)
{
    const std::vector<uint32_t> sizes { 3 };
    const std::vector<uint32_t> corners { 0, 2, 3 };
    const auto incidence = Incidence::create(sizes, corners, 5);
    ASSERT_TRUE(incidence.has_value());

    const auto around = incidence->vertex_corners();
    EXPECT_TRUE(around.around(1).empty());
    EXPECT_TRUE(around.around(4).empty());
    EXPECT_EQ(around.around(2).size(), 1U);
}

TEST(PolyhedralIncidenceTest, ExpandToCornersCarriesEverySupportToOneValuePerCorner)
{
    const auto cube = Polyhedral::unit_cube();
    const auto incidence = Incidence::create(cube.sizes, cube.corners, cube.positions.size());
    ASSERT_TRUE(incidence.has_value());

    const std::vector<float> constant { 7.0F };
    const auto from_constant = incidence->expand_to_corners(std::span<const float>(constant), AttributeSupport::Constant);
    ASSERT_TRUE(from_constant.has_value());
    EXPECT_EQ(from_constant->size(), 24U);
    EXPECT_TRUE(std::ranges::all_of(*from_constant, [](float v) { return v == 7.0F; }));

    const std::vector<float> per_face { 0.0F, 10.0F, 20.0F, 30.0F, 40.0F, 50.0F };
    const auto from_face = incidence->expand_to_corners(std::span<const float>(per_face), AttributeSupport::Face);
    ASSERT_TRUE(from_face.has_value());
    for (uint32_t corner = 0; corner < 24; ++corner) {
        const uint32_t face = corner / 4;
        EXPECT_EQ((*from_face)[corner], 10.0F * static_cast<float>(face));
    }

    std::vector<uint32_t> per_vertex(8);
    std::ranges::iota(per_vertex, 0U);
    const auto from_vertex = incidence->expand_to_corners(std::span<const uint32_t>(per_vertex), AttributeSupport::Vertex);
    ASSERT_TRUE(from_vertex.has_value());
    EXPECT_EQ(*from_vertex, cube.corners);

    std::vector<uint32_t> per_corner(24);
    std::ranges::iota(per_corner, 100U);
    const auto from_corner = incidence->expand_to_corners(std::span<const uint32_t>(per_corner), AttributeSupport::Corner);
    ASSERT_TRUE(from_corner.has_value());
    EXPECT_EQ(*from_corner, per_corner);
}

TEST(PolyhedralIncidenceTest, ExpandToCornersRefusesAWrongValueCount)
{
    const auto cube = Polyhedral::unit_cube();
    const auto incidence = Incidence::create(cube.sizes, cube.corners, cube.positions.size());
    ASSERT_TRUE(incidence.has_value());

    const auto expand = [&](size_t count, AttributeSupport support) {
        const std::vector<float> values(count, 1.0F);
        return incidence->expand_to_corners(std::span<const float>(values), support);
    };

    EXPECT_FALSE(expand(2, AttributeSupport::Constant).has_value());
    EXPECT_FALSE(expand(5, AttributeSupport::Face).has_value());
    EXPECT_FALSE(expand(7, AttributeSupport::Vertex).has_value());
    EXPECT_FALSE(expand(23, AttributeSupport::Corner).has_value());
}

TEST(PolyhedralIncidenceTest, LargeSurfacesTakeTheParallelPaths)
{
    const auto grid = Polyhedral::unit_grid(150, 150);
    const auto incidence = Incidence::create(grid.sizes, grid.corners, grid.positions.size());
    ASSERT_TRUE(incidence.has_value());
    ASSERT_EQ(incidence->face_count(), 22500U);
    ASSERT_EQ(incidence->corner_count(), 90000U);

    const auto faces = incidence->corner_faces();
    bool faces_match = true;
    for (uint32_t corner = 0; corner < faces.size(); ++corner) {
        faces_match = faces_match && faces[corner] == corner / 4;
    }
    EXPECT_TRUE(faces_match);

    std::vector<uint32_t> per_face(22500);
    std::ranges::iota(per_face, 0U);
    const auto expanded = incidence->expand_to_corners(std::span<const uint32_t>(per_face), AttributeSupport::Face);
    ASSERT_TRUE(expanded.has_value());
    EXPECT_EQ(*expanded, faces);

    const auto around = incidence->vertex_corners();
    EXPECT_EQ(around.around(0).size(), 1U);
    EXPECT_EQ(around.around(151 + 5).size(), 4U);
    EXPECT_EQ(around.corners.size(), 90000U);
}

TEST(PolyhedralIncidenceTest, AFaceListIsViewedNotCopied)
{
    const auto cube = Polyhedral::unit_cube();
    const Kakshya::FaceList list { .sizes = cube.sizes, .corners = cube.corners };
    const auto incidence = Incidence::create(list, cube.positions.size());

    ASSERT_TRUE(incidence.has_value());
    EXPECT_EQ(incidence->face_count(), 6U);
}

}
