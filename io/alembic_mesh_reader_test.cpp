#include "gtest/gtest.h"
#include "../support/test_metadata.hpp"

#include "../support/io_roundtrip_support.hpp"
#include "../support/scoped_working_dir.hpp"

#include "MayaFlux/IO/Spatial/MeshSequence.hpp"
#include "MayaFlux/IO/Spatial/SpatialReader.hpp"
#include "MayaFlux/Kakshya/NDData/VertexFormats.hpp"

#ifdef MAYAFLUX_PLATFORM_WINDOWS
#include <windows.h>
#endif

#include <Alembic/Abc/All.h>
#include <Alembic/AbcCoreOgawa/All.h>
#include <Alembic/AbcGeom/All.h>

#ifdef MAYAFLUX_PLATFORM_WINDOWS
#ifdef ERROR
#undef ERROR
#endif
#endif

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::writable_temp>() }))

namespace MayaFlux::Test {
namespace {

    using Kakshya::MeshData;
    using Kakshya::MeshVertex;
    using RoundTrip::path_in_cwd;

    std::vector<MeshVertex> vertices_of(const MeshData& mesh)
    {
        const auto& bytes = std::get<std::vector<uint8_t>>(mesh.vertex_variant);
        std::vector<MeshVertex> vertices(bytes.size() / sizeof(MeshVertex));
        std::memcpy(vertices.data(), bytes.data(), bytes.size());
        return vertices;
    }

    const std::vector<uint32_t>& indices_of(const MeshData& mesh)
    {
        return std::get<std::vector<uint32_t>>(mesh.index_variant);
    }

    glm::vec3 triangle_normal(const MeshData& mesh, size_t triangle)
    {
        const auto vertices = vertices_of(mesh);
        const auto& indices = indices_of(mesh);
        const auto& a = vertices[indices[triangle * 3]].position;
        const auto& b = vertices[indices[triangle * 3 + 1]].position;
        const auto& c = vertices[indices[triangle * 3 + 2]].position;
        return glm::normalize(glm::cross(b - a, c - a));
    }

    class AlembicMeshReaderTest : public ::testing::Test {
    protected:
        ScopedWorkingDir m_files;

        static void write_meshes(const std::string& path)
        {
            using namespace Alembic::AbcGeom;
            OArchive archive(Alembic::AbcCoreOgawa::WriteArchive(), path);
            const uint32_t each_second = archive.addTimeSampling(Alembic::AbcCoreAbstract::TimeSampling(1.0, 0.0));

            const auto write = [](OPolyMesh& mesh, const std::vector<V3f>& positions,
                                   const std::vector<int32_t>& indices, const std::vector<int32_t>& counts) {
                mesh.getSchema().set(OPolyMeshSchema::Sample(
                    P3fArraySample(positions.data(), positions.size()),
                    Int32ArraySample(indices.data(), indices.size()),
                    Int32ArraySample(counts.data(), counts.size())));
            };

            OPolyMesh tri(archive.getTop(), "tri");
            write(tri, { V3f(0, 0, 0), V3f(1, 0, 0), V3f(0, 1, 0) }, { 0, 1, 2 }, { 3 });

            OPolyMesh quad(archive.getTop(), "quad");
            write(quad, { V3f(0, 0, 0), V3f(1, 0, 0), V3f(1, 1, 0), V3f(0, 1, 0) }, { 0, 1, 2, 3 }, { 4 });

            OPolyMesh walker(archive.getTop(), "walker");
            walker.getSchema().setTimeSampling(each_second);
            for (int s = 0; s < 3; ++s) {
                const auto x = static_cast<float>(s);
                write(walker, { V3f(x, 0, 0), V3f(x + 1, 0, 0), V3f(x, 1, 0) }, { 0, 1, 2 }, { 3 });
            }

            OPolyMesh morph(archive.getTop(), "morph");
            morph.getSchema().setTimeSampling(each_second);
            write(morph, { V3f(0, 0, 0), V3f(1, 0, 0), V3f(0, 1, 0) }, { 0, 1, 2 }, { 3 });
            write(morph, { V3f(0, 0, 0), V3f(1, 0, 0), V3f(1, 1, 0), V3f(0, 1, 0) }, { 0, 1, 2, 3 }, { 4 });

            OPolyMesh strip(archive.getTop(), "strip");
            {
                const std::vector<V3f> positions { V3f(0, 0, 0), V3f(1, 0, 0), V3f(1, 1, 0), V3f(0, 1, 0) };
                const std::vector<int32_t> indices { 0, 1, 2, 0, 2, 3 };
                const std::vector<int32_t> counts { 3, 3 };
                const std::vector<V2f> uvs { V2f(0.00F, 0.0F), V2f(0.25F, 0.0F), V2f(0.50F, 0.5F),
                    V2f(0.75F, 0.0F), V2f(1.00F, 0.5F), V2f(0.50F, 1.0F) };
                strip.getSchema().set(OPolyMeshSchema::Sample(
                    P3fArraySample(positions.data(), positions.size()),
                    Int32ArraySample(indices.data(), indices.size()),
                    Int32ArraySample(counts.data(), counts.size()),
                    OV2fGeomParam::Sample(V2fArraySample(uvs.data(), uvs.size()), kFacevaryingScope)));
            }

            OPolyMesh lit(archive.getTop(), "lit");
            {
                const std::vector<V3f> positions { V3f(0, 0, 0), V3f(1, 0, 0), V3f(0, 1, 0) };
                const std::vector<int32_t> indices { 0, 1, 2 };
                const std::vector<int32_t> counts { 3 };
                const std::vector<N3f> normals { N3f(0, 1, 0), N3f(0, 1, 0), N3f(0, 1, 0) };
                lit.getSchema().set(OPolyMeshSchema::Sample(
                    P3fArraySample(positions.data(), positions.size()),
                    Int32ArraySample(indices.data(), indices.size()),
                    Int32ArraySample(counts.data(), counts.size()),
                    OV2fGeomParam::Sample(),
                    ON3fGeomParam::Sample(N3fArraySample(normals.data(), normals.size()), kVertexScope)));
            }

            OPolyMesh broken(archive.getTop(), "broken");
            write(broken, { V3f(0, 0, 0), V3f(1, 0, 0), V3f(0, 1, 0) }, { 0, 1, 5 }, { 3 });

            OPolyMesh flat(archive.getTop(), "flat");
            {
                const std::vector<V3f> positions { V3f(0, 0, 0), V3f(1, 0, 0), V3f(0, 1, 0) };
                const std::vector<int32_t> indices { 0, 1, 2 };
                const std::vector<int32_t> counts { 3 };
                const std::vector<N3f> normal { N3f(0, 1, 0) };
                flat.getSchema().set(OPolyMeshSchema::Sample(
                    P3fArraySample(positions.data(), positions.size()),
                    Int32ArraySample(indices.data(), indices.size()),
                    Int32ArraySample(counts.data(), counts.size()),
                    OV2fGeomParam::Sample(),
                    ON3fGeomParam::Sample(N3fArraySample(normal.data(), normal.size()), kConstantScope)));
            }

            OPolyMesh banded(archive.getTop(), "banded");
            {
                const std::vector<V3f> positions { V3f(0, 0, 0), V3f(1, 0, 0), V3f(1, 1, 0), V3f(0, 1, 0) };
                const std::vector<int32_t> indices { 0, 1, 2, 0, 2, 3 };
                const std::vector<int32_t> counts { 3, 3 };
                const std::vector<V2f> uvs { V2f(0.25F, 0.5F), V2f(0.75F, 0.5F) };
                banded.getSchema().set(OPolyMeshSchema::Sample(
                    P3fArraySample(positions.data(), positions.size()),
                    Int32ArraySample(indices.data(), indices.size()),
                    Int32ArraySample(counts.data(), counts.size()),
                    OV2fGeomParam::Sample(V2fArraySample(uvs.data(), uvs.size()), kUniformScope)));
            }

            OPolyMesh vanishing(archive.getTop(), "vanishing");
            vanishing.getSchema().setTimeSampling(each_second);
            write(vanishing, { V3f(0, 0, 0), V3f(1, 0, 0), V3f(0, 1, 0) }, { 0, 1, 2 }, { 3 });
            write(vanishing, {}, {}, {});

            OSubD cage(archive.getTop(), "cage");
            {
                const std::vector<V3f> positions { V3f(0, 0, 0), V3f(1, 0, 0), V3f(1, 1, 0), V3f(0, 1, 0) };
                const std::vector<int32_t> indices { 0, 1, 2, 3 };
                const std::vector<int32_t> counts { 4 };
                cage.getSchema().set(OSubDSchema::Sample(
                    P3fArraySample(positions.data(), positions.size()),
                    Int32ArraySample(indices.data(), indices.size()),
                    Int32ArraySample(counts.data(), counts.size())));
            }
        }

        std::string archive_path()
        {
            const auto path = path_in_cwd("meshes.abc");
            write_meshes(path);
            return path;
        }
    };

}

TEST_F(AlembicMeshReaderTest, MeshObjectsAreListedInFileOrderApartFromTheStreams)
{
    IO::SpatialReader reader;
    ASSERT_TRUE(reader.open(archive_path())) << reader.get_last_error();

    EXPECT_EQ(reader.get_mesh_names(),
        (std::vector<std::string> { "tri", "quad", "walker", "morph", "strip", "lit", "broken", "flat", "banded", "vanishing", "cage" }));
    EXPECT_TRUE(reader.get_stream_names().empty());
    EXPECT_EQ(reader.get_mesh_sample_count("walker"), 3U);
    EXPECT_EQ(reader.get_mesh_sample_count("absent"), 0U);
    EXPECT_EQ(reader.mesh_time("walker", 2), std::optional<double> { 2.0 });
    EXPECT_FALSE(reader.mesh_time("walker", 3).has_value());
}

TEST_F(AlembicMeshReaderTest, TheTopologyVarianceIsTheFilesOwn)
{
    IO::SpatialReader reader;
    ASSERT_TRUE(reader.open(archive_path()));

    EXPECT_EQ(reader.get_mesh_variance("tri"), std::optional { Kakshya::TopologyVariance::Constant });
    EXPECT_EQ(reader.get_mesh_variance("walker"), std::optional { Kakshya::TopologyVariance::Homogeneous });
    EXPECT_EQ(reader.get_mesh_variance("morph"), std::optional { Kakshya::TopologyVariance::Heterogeneous });
    EXPECT_FALSE(reader.get_mesh_variance("absent").has_value());
}

TEST_F(AlembicMeshReaderTest, AlembicsClockwiseFacesAreReversedToCounterClockwise)
{
    IO::SpatialReader reader;
    ASSERT_TRUE(reader.open(archive_path()));

    const auto mesh = reader.read_mesh("tri", 0);

    ASSERT_TRUE(mesh.has_value()) << reader.get_last_error();
    EXPECT_EQ(mesh->vertex_count(), 3U);
    EXPECT_EQ(mesh->face_count(), 1U);
    EXPECT_EQ(indices_of(*mesh), (std::vector<uint32_t> { 2, 1, 0 }));
    const auto normal = triangle_normal(*mesh, 0);
    EXPECT_NEAR(normal.z, -1.0F, 1e-5F);
    for (const auto& vertex : vertices_of(*mesh)) {
        EXPECT_NEAR(vertex.normal.z, -1.0F, 1e-5F);
    }
}

TEST_F(AlembicMeshReaderTest, AQuadIsTriangulatedIntoTwoTrianglesFacingTheSameWay)
{
    IO::SpatialReader reader;
    ASSERT_TRUE(reader.open(archive_path()));

    const auto mesh = reader.read_mesh("quad", 0);

    ASSERT_TRUE(mesh.has_value()) << reader.get_last_error();
    EXPECT_EQ(mesh->vertex_count(), 4U);
    EXPECT_EQ(mesh->face_count(), 2U);
    EXPECT_EQ(mesh->polygon_count(), 1U);
    EXPECT_NEAR(triangle_normal(*mesh, 0).z, -1.0F, 1e-5F);
    EXPECT_NEAR(triangle_normal(*mesh, 1).z, -1.0F, 1e-5F);
}

TEST_F(AlembicMeshReaderTest, FaceVaryingTextureCoordinatesGiveOneVertexPerCorner)
{
    IO::SpatialReader reader;
    ASSERT_TRUE(reader.open(archive_path()));

    const auto mesh = reader.read_mesh("strip", 0);

    ASSERT_TRUE(mesh.has_value()) << reader.get_last_error();
    EXPECT_EQ(mesh->vertex_count(), 6U);
    EXPECT_EQ(mesh->face_count(), 2U);
    const auto vertices = vertices_of(*mesh);
    EXPECT_EQ(vertices[0].uv, glm::vec2(0.00F, 0.0F));
    EXPECT_EQ(vertices[1].uv, glm::vec2(0.25F, 0.0F));
    EXPECT_EQ(vertices[4].uv, glm::vec2(1.00F, 0.5F));
    EXPECT_EQ(vertices[5].uv, glm::vec2(0.50F, 1.0F));
    EXPECT_EQ(vertices[3].position, glm::vec3(0.0F, 0.0F, 0.0F));
    EXPECT_EQ(vertices[4].position, glm::vec3(1.0F, 1.0F, 0.0F));
}

TEST_F(AlembicMeshReaderTest, NormalsTheFileGivesPerVertexAreKeptAsWritten)
{
    IO::SpatialReader reader;
    ASSERT_TRUE(reader.open(archive_path()));

    const auto mesh = reader.read_mesh("lit", 0);

    ASSERT_TRUE(mesh.has_value()) << reader.get_last_error();
    EXPECT_EQ(mesh->vertex_count(), 3U);
    for (const auto& vertex : vertices_of(*mesh)) {
        EXPECT_NEAR(vertex.normal.y, 1.0F, 1e-5F);
        EXPECT_NEAR(vertex.normal.z, 0.0F, 1e-5F);
    }
}

TEST_F(AlembicMeshReaderTest, ANormalOfConstantScopeIsGivenToEveryVertex)
{
    IO::SpatialReader reader;
    ASSERT_TRUE(reader.open(archive_path()));

    const auto mesh = reader.read_mesh("flat", 0);

    ASSERT_TRUE(mesh.has_value()) << reader.get_last_error();
    EXPECT_EQ(mesh->vertex_count(), 3U);
    for (const auto& vertex : vertices_of(*mesh)) {
        EXPECT_NEAR(vertex.normal.y, 1.0F, 1e-5F);
    }
}

TEST_F(AlembicMeshReaderTest, TextureCoordinatesOfUniformScopeBelongToEveryCornerOfTheirFace)
{
    IO::SpatialReader reader;
    ASSERT_TRUE(reader.open(archive_path()));

    const auto mesh = reader.read_mesh("banded", 0);

    ASSERT_TRUE(mesh.has_value()) << reader.get_last_error();
    ASSERT_EQ(mesh->vertex_count(), 6U);
    const auto vertices = vertices_of(*mesh);
    for (size_t corner = 0; corner < 3; ++corner) {
        EXPECT_EQ(vertices[corner].uv, glm::vec2(0.25F, 0.5F)) << corner;
        EXPECT_EQ(vertices[3 + corner].uv, glm::vec2(0.75F, 0.5F)) << corner;
    }
}

TEST_F(AlembicMeshReaderTest, ASampleWithNoFacesIsAnEmptyFrameNotAnError)
{
    IO::SpatialReader reader;
    ASSERT_TRUE(reader.open(archive_path()));

    const auto empty = reader.read_mesh("vanishing", 1);

    ASSERT_TRUE(empty.has_value()) << reader.get_last_error();
    EXPECT_EQ(empty->vertex_count(), 0U);
    EXPECT_EQ(empty->face_count(), 0U);
}

TEST_F(AlembicMeshReaderTest, AMeshFrameSourceServesAnEmptySampleAsAnEmptyFrame)
{
    const auto frames = IO::MeshFileFrames::create(archive_path(), "vanishing");
    ASSERT_NE(frames, nullptr);

    const auto frame = frames->read(1);

    ASSERT_NE(frame, nullptr);
    EXPECT_EQ(frame->vertex_count(), 0U);
}

TEST_F(AlembicMeshReaderTest, ASubdivisionSurfaceIsReadAsItsControlCage)
{
    IO::SpatialReader reader;
    ASSERT_TRUE(reader.open(archive_path()));

    const auto mesh = reader.read_mesh("cage", 0);

    ASSERT_TRUE(mesh.has_value()) << reader.get_last_error();
    EXPECT_EQ(mesh->vertex_count(), 4U);
    EXPECT_EQ(mesh->face_count(), 2U);
}

TEST_F(AlembicMeshReaderTest, AHomogeneousMeshKeepsItsIndicesAndMovesItsVertices)
{
    IO::SpatialReader reader;
    ASSERT_TRUE(reader.open(archive_path()));

    const auto first = reader.read_mesh("walker", 0);
    const auto last = reader.read_mesh("walker", 2);

    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(last.has_value());
    EXPECT_EQ(indices_of(*first), indices_of(*last));
    EXPECT_EQ(vertices_of(*first)[0].position, glm::vec3(0.0F, 0.0F, 0.0F));
    EXPECT_EQ(vertices_of(*last)[0].position, glm::vec3(2.0F, 0.0F, 0.0F));
}

TEST_F(AlembicMeshReaderTest, AHeterogeneousMeshMayChangeItsFaces)
{
    IO::SpatialReader reader;
    ASSERT_TRUE(reader.open(archive_path()));

    const auto first = reader.read_mesh("morph", 0);
    const auto second = reader.read_mesh("morph", 1);

    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(first->face_count(), 1U);
    EXPECT_EQ(second->face_count(), 2U);
}

TEST_F(AlembicMeshReaderTest, ASampleWhoseFacesDoNotFitItsVerticesIsRefusedWithAReason)
{
    IO::SpatialReader reader;
    ASSERT_TRUE(reader.open(archive_path()));

    EXPECT_FALSE(reader.read_mesh("broken", 0).has_value());
    EXPECT_NE(reader.get_last_error().find("vertex that does not exist"), std::string::npos);
    EXPECT_FALSE(reader.read_mesh("absent", 0).has_value());
    EXPECT_FALSE(reader.read_mesh("tri", 1).has_value());
}

TEST_F(AlembicMeshReaderTest, MeshFramesAreTimedByTheArchiveAndSharedWhileHeld)
{
    const auto frames = IO::MeshFileFrames::create(archive_path(), "walker");

    ASSERT_NE(frames, nullptr);
    EXPECT_EQ(frames->count(), 3U);
    EXPECT_EQ(frames->mesh(), "walker");
    EXPECT_EQ(frames->variance(), Kakshya::TopologyVariance::Homogeneous);
    EXPECT_EQ(frames->time_sampling().time(2), std::optional<double> { 2.0 });

    const auto held = frames->read(1);
    ASSERT_NE(held, nullptr);
    EXPECT_EQ(frames->read(1), held);
    EXPECT_EQ(vertices_of(*held)[0].position, glm::vec3(1.0F, 0.0F, 0.0F));
    EXPECT_EQ(frames->read(3), nullptr);
}

TEST_F(AlembicMeshReaderTest, AFileWithSeveralMeshesNeedsOneNamedAndAWrongNameFails)
{
    const auto path = archive_path();

    EXPECT_EQ(IO::MeshFileFrames::create(path), nullptr);
    EXPECT_EQ(IO::MeshFileFrames::create(path, "absent"), nullptr);
}

TEST_F(AlembicMeshReaderTest, AFrameThatCannotBeReadGivesNullAndTheOthersStillRead)
{
    const auto frames = IO::MeshFileFrames::create(archive_path(), "broken");

    ASSERT_NE(frames, nullptr);
    EXPECT_EQ(frames->read(0), nullptr);
}

}
