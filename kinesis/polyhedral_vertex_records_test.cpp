#include "gtest/gtest.h"
#include "../support/polyhedral_support.hpp"
#include "../support/test_metadata.hpp"

#include "MayaFlux/Kakshya/NDData/VertexFormats.hpp"
#include "MayaFlux/Kinesis/Polyhedral/Identification.hpp"
#include "MayaFlux/Kinesis/Polyhedral/Triangulation.hpp"
#include "MayaFlux/Kinesis/Polyhedral/VertexRecords.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {

namespace {

    using Kakshya::MeshVertex;
    using Kinesis::AttributePolicy;
    using Kinesis::FillOutcome;
    using Polyhedral::approx;

    constexpr size_t k_normal_offset = offsetof(MeshVertex, normal);
    constexpr size_t k_weight_offset = offsetof(MeshVertex, weight);

    std::vector<uint8_t> pack(const std::vector<MeshVertex>& vertices)
    {
        std::vector<uint8_t> bytes(vertices.size() * sizeof(MeshVertex));
        std::memcpy(bytes.data(), vertices.data(), bytes.size());
        return bytes;
    }

    MeshVertex unpack(const std::vector<uint8_t>& bytes, size_t index)
    {
        MeshVertex vertex {};
        std::memcpy(&vertex, bytes.data() + index * sizeof(MeshVertex), sizeof(MeshVertex));
        return vertex;
    }

    std::vector<MeshVertex> vertices_of(const std::vector<glm::vec3>& positions)
    {
        std::vector<MeshVertex> vertices(positions.size());
        for (size_t i = 0; i < positions.size(); ++i) {
            vertices[i].position = positions[i];
        }
        return vertices;
    }

    std::vector<uint32_t> triangles_of(const Polyhedral::Surface& surface)
    {
        const auto incidence = Kinesis::Incidence::create(surface.sizes, surface.corners, surface.positions.size());
        EXPECT_TRUE(incidence.has_value());
        const auto result = Kinesis::triangulate_faces(surface.positions, *incidence);
        EXPECT_TRUE(result.has_value());
        return result.value_or(Kinesis::Triangulation {}).indices;
    }

    Kakshya::MeshData cube_mesh()
    {
        const auto cube = Polyhedral::unit_cube();
        auto mesh = Kakshya::MeshData::empty();
        mesh.vertex_variant = pack(vertices_of(cube.positions));
        mesh.index_variant = triangles_of(cube);
        mesh.layout = Kakshya::VertexLayout::for_meshes();
        return mesh;
    }

    std::vector<uint8_t>& records_of(Kakshya::MeshData& mesh)
    {
        return std::get<std::vector<uint8_t>>(mesh.vertex_variant);
    }

    struct Quad {
        std::vector<uint8_t> records;
        std::vector<uint32_t> triangles;
    };

    Quad quad_with_uv(const std::array<glm::vec2, 4>& uv)
    {
        auto vertices = vertices_of({ glm::vec3(0.0F, 0.0F, 0.0F), glm::vec3(1.0F, 0.0F, 0.0F),
            glm::vec3(1.0F, 1.0F, 0.0F), glm::vec3(0.0F, 1.0F, 0.0F) });
        for (size_t i = 0; i < 4; ++i) {
            vertices[i].uv = uv[i];
        }
        return { .records = pack(vertices), .triangles = { 0, 1, 2, 0, 2, 3 } };
    }

}

TEST(PolyhedralVertexRecordsTest, NormalsAreDerivedForAMeshThatCameWithNone)
{
    auto mesh = cube_mesh();

    EXPECT_EQ(Kinesis::write_vertex_normals(mesh), FillOutcome::Written);

    const auto cube = Polyhedral::unit_cube();
    for (size_t vertex = 0; vertex < 8; ++vertex) {
        const glm::vec3 expected = glm::normalize(cube.positions[vertex] * 2.0F - glm::vec3(1.0F));
        EXPECT_TRUE(approx(unpack(records_of(mesh), vertex).normal, expected, 1.0e-5F)) << "vertex " << vertex;
    }
}

TEST(PolyhedralVertexRecordsTest, ANormalTheCreatorSuppliedIsLeftAloneByDefault)
{
    auto mesh = cube_mesh();
    auto vertices = vertices_of(Polyhedral::unit_cube().positions);
    vertices[3].normal = glm::vec3(0.0F, 1.0F, 0.0F);
    mesh.vertex_variant = pack(vertices);
    const auto before = records_of(mesh);

    EXPECT_EQ(Kinesis::write_vertex_normals(mesh), FillOutcome::Kept);
    EXPECT_EQ(records_of(mesh), before);
}

TEST(PolyhedralVertexRecordsTest, ReplaceOverwritesNormalsTheCreatorSupplied)
{
    auto mesh = cube_mesh();
    auto vertices = vertices_of(Polyhedral::unit_cube().positions);
    vertices[3].normal = glm::vec3(0.0F, 1.0F, 0.0F);
    mesh.vertex_variant = pack(vertices);

    EXPECT_EQ(Kinesis::write_vertex_normals(mesh, AttributePolicy::Replace), FillOutcome::Written);
    EXPECT_TRUE(approx(unpack(records_of(mesh), 3).normal, glm::normalize(glm::vec3(-1.0F, 1.0F, -1.0F)), 1.0e-5F));
}

TEST(PolyhedralVertexRecordsTest, AVertexNoTriangleNamesKeepsItsNormal)
{
    const auto cube = Polyhedral::unit_cube();
    auto positions = cube.positions;
    positions.emplace_back(9.0F, 9.0F, 9.0F);
    auto mesh = cube_mesh();
    mesh.vertex_variant = pack(vertices_of(positions));

    EXPECT_EQ(Kinesis::write_vertex_normals(mesh), FillOutcome::Written);
    EXPECT_EQ(unpack(records_of(mesh), 8).normal, glm::vec3(0.0F, 0.0F, 1.0F));
}

TEST(PolyhedralVertexRecordsTest, ExtraBytesBeyondTheKnownAttributesAreNeverTouched)
{
    const auto cube = Polyhedral::unit_cube();
    constexpr size_t stride = sizeof(MeshVertex) + 12;
    std::vector<uint8_t> records(8 * stride, 0xAB);
    for (size_t i = 0; i < 8; ++i) {
        const MeshVertex vertex = vertices_of(cube.positions)[i];
        std::memcpy(records.data() + i * stride, &vertex, sizeof(MeshVertex));
    }
    const auto layout = Kakshya::VertexLayout::for_meshes(stride);

    EXPECT_EQ(Kinesis::write_vertex_normals(records, layout, triangles_of(cube)), FillOutcome::Written);

    for (size_t i = 0; i < 8; ++i) {
        for (size_t byte = sizeof(MeshVertex); byte < stride; ++byte) {
            EXPECT_EQ(records[i * stride + byte], 0xAB);
        }
        glm::vec3 normal;
        std::memcpy(&normal, records.data() + i * stride + k_normal_offset, sizeof(normal));
        EXPECT_TRUE(approx(normal, glm::normalize(cube.positions[i] * 2.0F - glm::vec3(1.0F)), 1.0e-5F));
    }
}

TEST(PolyhedralVertexRecordsTest, ALayoutWithoutNormalsOrAnIndexListOffThreeIsUnsupported)
{
    const auto cube = Polyhedral::unit_cube();
    Kakshya::VertexLayout position_only;
    position_only.stride_bytes = sizeof(glm::vec3);
    position_only.attributes.push_back({ .component_modality = Kakshya::DataModality::VERTEX_POSITIONS_3D,
        .offset_in_vertex = 0,
        .name = "position" });

    std::vector<uint8_t> bare(8 * sizeof(glm::vec3));
    std::memcpy(bare.data(), cube.positions.data(), bare.size());
    EXPECT_EQ(Kinesis::write_vertex_normals(bare, position_only, triangles_of(cube)), FillOutcome::Unsupported);

    auto mesh = cube_mesh();
    std::get<std::vector<uint32_t>>(mesh.index_variant).pop_back();
    EXPECT_EQ(Kinesis::write_vertex_normals(mesh), FillOutcome::Unsupported);
}

TEST(PolyhedralVertexRecordsTest, TangentsFollowTheDirectionOfIncreasingU)
{
    auto quad = quad_with_uv({ glm::vec2(0.0F, 0.0F), glm::vec2(1.0F, 0.0F), glm::vec2(1.0F, 1.0F), glm::vec2(0.0F, 1.0F) });
    const auto layout = Kakshya::VertexLayout::for_meshes();

    EXPECT_EQ(Kinesis::write_vertex_tangents(quad.records, layout, quad.triangles), FillOutcome::Written);

    for (size_t vertex = 0; vertex < 4; ++vertex) {
        const MeshVertex v = unpack(quad.records, vertex);
        EXPECT_TRUE(approx(glm::vec3(v.tangent), glm::vec3(1.0F, 0.0F, 0.0F), 1.0e-5F));
        EXPECT_EQ(v.tangent.w, 1.0F);
    }
}

TEST(PolyhedralVertexRecordsTest, SwappingTheTextureAxesTurnsTheTangentAndFlipsTheHandedness)
{
    auto quad = quad_with_uv({ glm::vec2(0.0F, 0.0F), glm::vec2(0.0F, 1.0F), glm::vec2(1.0F, 1.0F), glm::vec2(1.0F, 0.0F) });
    const auto layout = Kakshya::VertexLayout::for_meshes();

    EXPECT_EQ(Kinesis::write_vertex_tangents(quad.records, layout, quad.triangles), FillOutcome::Written);

    const MeshVertex v = unpack(quad.records, 1);
    EXPECT_TRUE(approx(glm::vec3(v.tangent), glm::vec3(0.0F, 1.0F, 0.0F), 1.0e-5F));
    EXPECT_EQ(v.tangent.w, -1.0F);
}

TEST(PolyhedralVertexRecordsTest, AMirroredTextureReversesTheTangentAndFlipsTheHandedness)
{
    auto quad = quad_with_uv({ glm::vec2(1.0F, 0.0F), glm::vec2(0.0F, 0.0F), glm::vec2(0.0F, 1.0F), glm::vec2(1.0F, 1.0F) });
    const auto layout = Kakshya::VertexLayout::for_meshes();

    EXPECT_EQ(Kinesis::write_vertex_tangents(quad.records, layout, quad.triangles), FillOutcome::Written);

    const MeshVertex v = unpack(quad.records, 2);
    EXPECT_TRUE(approx(glm::vec3(v.tangent), glm::vec3(-1.0F, 0.0F, 0.0F), 1.0e-5F));
    EXPECT_EQ(v.tangent.w, -1.0F);
}

TEST(PolyhedralVertexRecordsTest, ATangentTheCreatorSuppliedIsLeftAloneByDefault)
{
    auto quad = quad_with_uv({ glm::vec2(0.0F, 0.0F), glm::vec2(1.0F, 0.0F), glm::vec2(1.0F, 1.0F), glm::vec2(0.0F, 1.0F) });
    auto vertices = std::vector<MeshVertex>(4);
    std::memcpy(vertices.data(), quad.records.data(), quad.records.size());
    vertices[2].tangent = glm::vec4(0.0F, 0.0F, 1.0F, 1.0F);
    quad.records = pack(vertices);
    const auto before = quad.records;
    const auto layout = Kakshya::VertexLayout::for_meshes();

    EXPECT_EQ(Kinesis::write_vertex_tangents(quad.records, layout, quad.triangles), FillOutcome::Kept);
    EXPECT_EQ(quad.records, before);
    EXPECT_EQ(Kinesis::write_vertex_tangents(quad.records, layout, quad.triangles, AttributePolicy::Replace), FillOutcome::Written);
}

TEST(PolyhedralVertexRecordsTest, TangentsNeedTextureCoordinates)
{
    auto quad = quad_with_uv({ glm::vec2(0.0F), glm::vec2(0.0F), glm::vec2(0.0F), glm::vec2(0.0F) });
    const auto layout = Kakshya::VertexLayout::for_meshes();

    EXPECT_EQ(Kinesis::write_vertex_tangents(quad.records, layout, quad.triangles), FillOutcome::Unsupported);
}

TEST(PolyhedralVertexRecordsTest, ARecordCanBeGatheredFromTheVerticesNamedAndInThatOrder)
{
    const auto cube = Polyhedral::unit_cube();
    const auto records = pack(vertices_of(cube.positions));
    const std::vector<uint32_t> source { 7, 0, 0 };

    const auto gathered = Kinesis::gather_records(records, sizeof(MeshVertex), source);

    ASSERT_EQ(gathered.size(), 3 * sizeof(MeshVertex));
    EXPECT_EQ(unpack(gathered, 0).position, cube.positions[7]);
    EXPECT_EQ(unpack(gathered, 1).position, cube.positions[0]);
    EXPECT_EQ(unpack(gathered, 2).position, cube.positions[0]);
}

TEST(PolyhedralVertexRecordsTest, GatheringRefusesAnOutOfRangeVertexOrABadStride)
{
    const auto records = pack(vertices_of(Polyhedral::unit_cube().positions));

    EXPECT_TRUE(Kinesis::gather_records(records, sizeof(MeshVertex), std::vector<uint32_t> { 8 }).empty());
    EXPECT_TRUE(Kinesis::gather_records(records, 0, std::vector<uint32_t> { 0 }).empty());
    EXPECT_TRUE(Kinesis::gather_records(records, sizeof(MeshVertex) - 1, std::vector<uint32_t> { 0 }).empty());
}

TEST(PolyhedralVertexRecordsTest, CornerValuesLandInTheRecordOfTheVertexTheirCornerNames)
{
    auto records = pack(std::vector<MeshVertex>(3));
    const std::vector<uint32_t> corner_vertex { 0, 1, 1, 2 };
    const std::vector<float> values { 1.0F, 2.0F, 2.0F, 3.0F };

    ASSERT_TRUE(Kinesis::scatter_corner_values(std::span<uint8_t>(records), sizeof(MeshVertex), k_weight_offset,
        corner_vertex, std::span<const float>(values)));

    EXPECT_EQ(unpack(records, 0).weight, 1.0F);
    EXPECT_EQ(unpack(records, 1).weight, 2.0F);
    EXPECT_EQ(unpack(records, 2).weight, 3.0F);
}

TEST(PolyhedralVertexRecordsTest, ScatteringWritesNothingWhenTheInputsDisagree)
{
    auto records = pack(std::vector<MeshVertex>(2));
    const auto before = records;
    const std::vector<float> values { 1.0F, 2.0F };

    EXPECT_FALSE(Kinesis::scatter_corner_values(std::span<uint8_t>(records), sizeof(MeshVertex), k_weight_offset,
        std::vector<uint32_t> { 0 }, std::span<const float>(values)));
    EXPECT_FALSE(Kinesis::scatter_corner_values(std::span<uint8_t>(records), sizeof(MeshVertex), k_weight_offset,
        std::vector<uint32_t> { 0, 2 }, std::span<const float>(values)));
    EXPECT_FALSE(Kinesis::scatter_corner_values(std::span<uint8_t>(records), sizeof(MeshVertex), sizeof(MeshVertex) - 2,
        std::vector<uint32_t> { 0, 1 }, std::span<const float>(values)));
    EXPECT_EQ(records, before);
}

TEST(PolyhedralVertexRecordsTest, FlatShadingACubeSplitsItsCornersAndCarriesEveryAttribute)
{
    const auto cube = Polyhedral::unit_cube();
    const auto incidence = Kinesis::Incidence::create(cube.sizes, cube.corners, cube.positions.size());
    ASSERT_TRUE(incidence.has_value());

    const auto face_directions = Polyhedral::unit_cube_normals();
    const auto per_corner = incidence->expand_to_corners(
        std::span<const glm::vec3>(face_directions), Kinesis::AttributeSupport::Face);
    ASSERT_TRUE(per_corner.has_value());

    const auto split = Kinesis::identify_corners(*incidence, std::span<const glm::vec3>(*per_corner));
    ASSERT_TRUE(split.has_value());
    ASSERT_EQ(split->source_vertex.size(), 24U);

    const auto original = pack(vertices_of(cube.positions));
    auto records = Kinesis::gather_records(original, sizeof(MeshVertex), split->source_vertex);
    ASSERT_TRUE(Kinesis::scatter_corner_values(std::span<uint8_t>(records), sizeof(MeshVertex), k_normal_offset,
        split->corner_vertex, std::span<const glm::vec3>(*per_corner)));

    for (uint32_t corner = 0; corner < 24; ++corner) {
        const MeshVertex vertex = unpack(records, split->corner_vertex[corner]);
        EXPECT_EQ(vertex.position, cube.positions[cube.corners[corner]]);
        EXPECT_EQ(vertex.normal, face_directions[corner / 4]);
    }
}

}
