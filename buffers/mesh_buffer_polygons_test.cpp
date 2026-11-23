#include "gtest/gtest.h"
#include "../support/polyhedral_support.hpp"
#include "../support/test_metadata.hpp"

#include "MayaFlux/Buffers/Geometry/MeshBuffer.hpp"
#include "MayaFlux/Kakshya/NDData/MeshData.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::vulkan>() }))

namespace MayaFlux::Test {
namespace {

    Kakshya::MeshData cube_with_polygons()
    {
        const auto cube = Polyhedral::unit_cube();
        return Kakshya::MeshData::from_positions(
            cube.positions, Kakshya::FaceList { .sizes = cube.sizes, .corners = cube.corners })
            .value();
    }

    std::vector<uint8_t> vertex_bytes_of(const Kakshya::MeshData& mesh)
    {
        return std::get<std::vector<uint8_t>>(mesh.vertex_variant);
    }

}

TEST(MeshBufferPolygonsTest, ABufferKeepsThePolygonsItsMeshWasMadeFrom)
{
    const auto buffer = std::make_shared<Buffers::MeshBuffer>(cube_with_polygons());

    EXPECT_TRUE(buffer->get_mesh_data().has_polygons());
    EXPECT_EQ(buffer->get_mesh_data().polygon_count(), 6U);
    EXPECT_EQ(buffer->get_face_count(), 12U);
}

TEST(MeshBufferPolygonsTest, ReplacingTheTrianglesDropsThePolygonsTheyCameFrom)
{
    const auto buffer = std::make_shared<Buffers::MeshBuffer>(cube_with_polygons());

    buffer->set_index_data(std::vector<uint32_t> { 0, 1, 2 });

    EXPECT_FALSE(buffer->get_mesh_data().has_polygons());
    EXPECT_EQ(buffer->get_face_count(), 1U);
}

TEST(MeshBufferPolygonsTest, ReplacingTheVerticesWithTheSameCountKeepsThePolygons)
{
    const auto buffer = std::make_shared<Buffers::MeshBuffer>(cube_with_polygons());
    const auto bytes = vertex_bytes_of(buffer->get_mesh_data());

    buffer->set_vertex_data(bytes);

    EXPECT_TRUE(buffer->get_mesh_data().has_polygons());
}

TEST(MeshBufferPolygonsTest, ReplacingTheVerticesWithAnotherCountDropsThePolygons)
{
    const auto buffer = std::make_shared<Buffers::MeshBuffer>(cube_with_polygons());
    auto bytes = vertex_bytes_of(buffer->get_mesh_data());
    bytes.resize(bytes.size() / 2);

    buffer->set_vertex_data(bytes);

    EXPECT_FALSE(buffer->get_mesh_data().has_polygons());
}

TEST(MeshBufferPolygonsTest, ReplacingTheWholeMeshTakesItsPolygonsOrNone)
{
    const auto buffer = std::make_shared<Buffers::MeshBuffer>(cube_with_polygons());
    const auto grid = Polyhedral::unit_grid(2, 2);
    auto quads = Kakshya::MeshData::from_positions(
        grid.positions, Kakshya::FaceList { .sizes = grid.sizes, .corners = grid.corners })
                     .value();

    buffer->set_mesh_data(std::move(quads));
    EXPECT_EQ(buffer->get_mesh_data().polygon_count(), 4U);

    buffer->set_mesh_data(Kakshya::MeshData::from_positions(grid.positions, std::vector<uint32_t> { 0, 1, 4 }).value());
    EXPECT_FALSE(buffer->get_mesh_data().has_polygons());
}

}
