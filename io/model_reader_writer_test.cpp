#include "gtest/gtest.h"
#include "../support/test_metadata.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include "../support/io_roundtrip_support.hpp"
#include "../support/scoped_working_dir.hpp"

#include "MayaFlux/IO/Model/AssimpModelWriter.hpp"
#include "MayaFlux/IO/Model/ModelExport.hpp"
#include "MayaFlux/IO/Model/ModelReader.hpp"
#include "MayaFlux/Kakshya/NDData/VertexFormats.hpp"
#include "MayaFlux/Nodes/Graphics/MeshWriterNode.hpp"
#include "MayaFlux/Nodes/Network/MeshNetwork.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::writable_temp>() }))

namespace MayaFlux::Test {
namespace {

    using Kakshya::MeshData;
    using Kakshya::MeshVertex;
    using RoundTrip::near;
    using RoundTrip::path_in_cwd;

    MeshData make_mesh(
        const std::vector<MeshVertex>& vertices,
        const std::vector<uint32_t>& indices,
        const std::string& name,
        const std::string& material = {},
        const std::string& diffuse_path = {})
    {
        auto data = MeshData::empty();
        auto& bytes = std::get<std::vector<uint8_t>>(data.vertex_variant);
        bytes.resize(vertices.size() * sizeof(MeshVertex));
        std::memcpy(bytes.data(), vertices.data(), bytes.size());
        std::get<std::vector<uint32_t>>(data.index_variant) = indices;

        Kakshya::MeshSubrange sub;
        sub.index_start = 0;
        sub.index_count = static_cast<uint32_t>(indices.size());
        sub.name = name;
        sub.material_name = material;
        sub.diffuse_path = diffuse_path;

        Kakshya::RegionGroup group("submeshes");
        group.add_region(sub.to_region());
        data.submeshes = std::move(group);
        return data;
    }

    MeshData flat_cube(
        const std::string& name,
        const glm::vec3& center = glm::vec3(0.0F),
        float half = 1.0F,
        const std::string& material = {},
        const std::string& diffuse_path = {})
    {
        struct Face {
            glm::vec3 normal;
            glm::vec3 u;
            glm::vec3 v;
        };
        const std::array<Face, 6> faces { {
            { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } },
            { { -1, 0, 0 }, { 0, 0, 1 }, { 0, 1, 0 } },
            { { 0, 1, 0 }, { 0, 0, 1 }, { 1, 0, 0 } },
            { { 0, -1, 0 }, { 1, 0, 0 }, { 0, 0, 1 } },
            { { 0, 0, 1 }, { 1, 0, 0 }, { 0, 1, 0 } },
            { { 0, 0, -1 }, { 0, 1, 0 }, { 1, 0, 0 } },
        } };
        constexpr std::array<std::pair<float, float>, 4> corners { {
            { -1.0F, -1.0F }, { 1.0F, -1.0F }, { 1.0F, 1.0F }, { -1.0F, 1.0F } } };

        std::vector<MeshVertex> vertices;
        std::vector<uint32_t> indices;
        for (size_t f = 0; f < faces.size(); ++f) {
            const auto base = static_cast<uint32_t>(vertices.size());
            const auto& face = faces[f];
            for (const auto& [a, b] : corners) {
                MeshVertex vertex;
                vertex.position = center + half * (face.normal + a * face.u + b * face.v);
                vertex.normal = face.normal;
                vertex.uv = { a > 0.0F ? 0.9F : 0.1F, b > 0.0F ? 0.65F : 0.15F };
                vertex.color = {
                    0.2F * static_cast<float>(1 + (f % 4)),
                    0.2F * static_cast<float>(1 + ((f + 1) % 4)),
                    0.2F * static_cast<float>(1 + ((f + 2) % 4)),
                    1.0F
                };
                vertex.tangent = glm::vec4(face.u, 1.0F);
                vertex.weight = 0.0F;
                vertices.push_back(vertex);
            }
            for (const uint32_t i : { 0U, 1U, 2U, 0U, 2U, 3U }) {
                indices.push_back(base + i);
            }
        }
        return make_mesh(vertices, indices, name, material, diffuse_path);
    }

    MeshData polygon_mesh(const std::vector<glm::vec3>& positions, Kakshya::FaceList faces)
    {
        return MeshData::from_positions(positions, std::move(faces)).value();
    }

    std::vector<glm::vec3> pentagon_at(float x)
    {
        std::vector<glm::vec3> ring;
        for (int i = 0; i < 5; ++i) {
            const float angle = 2.0F * std::numbers::pi_v<float> * static_cast<float>(i) / 5.0F;
            ring.emplace_back(x + std::cos(angle), std::sin(angle), 0.0F);
        }
        return ring;
    }

    std::vector<MeshVertex> model_reader_writer_vertices_of(const MeshData& mesh)
    {
        const auto& bytes = std::get<std::vector<uint8_t>>(mesh.vertex_variant);
        std::vector<MeshVertex> vertices(bytes.size() / sizeof(MeshVertex));
        std::memcpy(vertices.data(), bytes.data(), vertices.size() * sizeof(MeshVertex));
        return vertices;
    }

    const std::vector<uint32_t>& model_reader_writer_indices_of(const MeshData& mesh)
    {
        return std::get<std::vector<uint32_t>>(mesh.index_variant);
    }

    size_t triangles_in(const std::vector<MeshData>& meshes)
    {
        size_t total = 0;
        for (const auto& mesh : meshes) {
            total += model_reader_writer_indices_of(mesh).size() / 3;
        }
        return total;
    }

    std::pair<std::string, std::string> names_of(const MeshData& mesh)
    {
        if (!mesh.submeshes || mesh.submeshes->regions.empty()) {
            return {};
        }
        const auto* sub = mesh.submeshes->regions.front().get<Kakshya::MeshSubrange>();
        return sub ? std::pair { sub->name, sub->material_name } : std::pair<std::string, std::string> {};
    }

    const Kakshya::MeshSubrange* subrange_of(const MeshData& mesh)
    {
        if (!mesh.submeshes || mesh.submeshes->regions.empty()) {
            return nullptr;
        }
        return mesh.submeshes->regions.front().get<Kakshya::MeshSubrange>();
    }

    const MeshVertex* find_vertex(
        const std::vector<MeshVertex>& vertices,
        const glm::vec3& position,
        const glm::vec3* normal = nullptr)
    {
        for (const auto& vertex : vertices) {
            if (near(vertex.position, position) && (!normal || near(vertex.normal, *normal))) {
                return &vertex;
            }
        }
        return nullptr;
    }

    bool covers_positions(const std::vector<MeshVertex>& haystack, const std::vector<MeshVertex>& needles)
    {
        return std::ranges::all_of(needles, [&](const MeshVertex& needle) {
            return find_vertex(haystack, needle.position) != nullptr;
        });
    }

    std::pair<glm::vec3, glm::vec3> bounds_of(const std::vector<MeshVertex>& vertices)
    {
        glm::vec3 lo(std::numeric_limits<float>::max());
        glm::vec3 hi(std::numeric_limits<float>::lowest());
        for (const auto& vertex : vertices) {
            lo = glm::min(lo, vertex.position);
            hi = glm::max(hi, vertex.position);
        }
        return { lo, hi };
    }

    class ModelFileTest : public ::testing::Test {
    protected:
        void SetUp() override { IO::AssimpModelWriter::register_with_registry(); }

        ScopedWorkingDir m_files;
    };

    struct FormatCase {
        const char* extension;
    };

    class ModelFormatTest
        : public ModelFileTest,
          public ::testing::WithParamInterface<FormatCase> { };

    TEST_P(ModelFormatTest, RoundTripKeepsTheTrianglesAndEveryPosition)
    {
        const auto source = flat_cube("cube");
        const auto path = path_in_cwd(std::string("cube.") + GetParam().extension);

        IO::AssimpModelWriter writer;
        ASSERT_TRUE(writer.write(path, { source })) << writer.get_last_error();
        ASSERT_TRUE(RoundTrip::exists(path));

        IO::ModelReader reader;
        const auto loaded = reader.load(path);

        ASSERT_EQ(loaded.size(), 1U) << reader.get_last_error();
        EXPECT_EQ(triangles_in(loaded), 12U);
        EXPECT_TRUE(covers_positions(model_reader_writer_vertices_of(loaded.front()), model_reader_writer_vertices_of(source)));
        EXPECT_EQ(loaded.front().layout.stride_bytes, sizeof(MeshVertex));

        const auto [lo, hi] = bounds_of(model_reader_writer_vertices_of(loaded.front()));
        EXPECT_TRUE(near(lo, glm::vec3(-1.0F)));
        EXPECT_TRUE(near(hi, glm::vec3(1.0F)));
    }

    TEST_P(ModelFormatTest, WriterAndReaderBothClaimTheExtension)
    {
        const auto path = std::string("model.") + GetParam().extension;

        EXPECT_TRUE(IO::AssimpModelWriter {}.can_write(path));
        EXPECT_TRUE(IO::ModelReader {}.can_read(path));
    }

    INSTANTIATE_TEST_SUITE_P(
        Formats,
        ModelFormatTest,
        ::testing::Values(
            FormatCase { "gltf" },
            FormatCase { "glb" },
            FormatCase { "obj" },
            FormatCase { "ply" },
            FormatCase { "stl" },
            FormatCase { "dae" },
            FormatCase { "fbx" },
            FormatCase { "3ds" }),
        [](const ::testing::TestParamInfo<FormatCase>& info) { return std::string(info.param.extension); });

    TEST_F(ModelFileTest, ObjRoundTripKeepsNormalsAndUvCoordinates)
    {
        const auto source = flat_cube("cube");
        const auto path = path_in_cwd("uv.obj");
        IO::AssimpModelWriter writer;
        ASSERT_TRUE(writer.write(path, { source })) << writer.get_last_error();

        IO::ModelReader reader;
        const auto loaded = reader.load(path);
        ASSERT_EQ(loaded.size(), 1U) << reader.get_last_error();

        const auto read_back = model_reader_writer_vertices_of(loaded.front());
        EXPECT_EQ(read_back.size(), 24U);
        for (const auto& original : model_reader_writer_vertices_of(source)) {
            const auto* match = find_vertex(read_back, original.position, &original.normal);
            ASSERT_NE(match, nullptr) << "no vertex at " << original.position.x << ' ' << original.position.y << ' ' << original.position.z;
            EXPECT_NEAR(match->uv.x, original.uv.x, 1e-4F);
            EXPECT_NEAR(match->uv.y, original.uv.y, 1e-4F);
        }
    }

    TEST_F(ModelFileTest, KeepingPolygonsReadsAQuadAsOneFaceAndItsTwoTriangles)
    {
        const std::vector<glm::vec3> positions {
            glm::vec3(0.0F, 0.0F, 0.0F), glm::vec3(1.0F, 0.0F, 0.0F), glm::vec3(1.0F, 1.0F, 0.0F), glm::vec3(0.0F, 1.0F, 0.0F)
        };
        const auto source = polygon_mesh(positions, { .sizes = { 4 }, .corners = { 0, 1, 2, 3 } });
        const auto path = path_in_cwd("quad.obj");
        IO::AssimpModelWriter writer;
        ASSERT_TRUE(writer.write(path, { source })) << writer.get_last_error();

        IO::ModelReader reader;
        reader.set_keep_polygons(true);
        const auto loaded = reader.load(path);

        ASSERT_EQ(loaded.size(), 1U) << reader.get_last_error();
        const auto& mesh = loaded.front();
        EXPECT_TRUE(mesh.has_polygons());
        EXPECT_EQ(mesh.polygon_count(), 1U);
        EXPECT_EQ(mesh.face_count(), 2U);
        EXPECT_EQ(mesh.faces->sizes, (std::vector<uint32_t> { 4 }));
        EXPECT_TRUE(mesh.is_valid());
        EXPECT_TRUE(covers_positions(model_reader_writer_vertices_of(mesh), model_reader_writer_vertices_of(source)));
    }

    TEST_F(ModelFileTest, PolygonsAreOffByDefaultAndTheSameFileComesBackAsTriangles)
    {
        const std::vector<glm::vec3> positions {
            glm::vec3(0.0F, 0.0F, 0.0F), glm::vec3(1.0F, 0.0F, 0.0F), glm::vec3(1.0F, 1.0F, 0.0F), glm::vec3(0.0F, 1.0F, 0.0F)
        };
        const auto source = polygon_mesh(positions, { .sizes = { 4 }, .corners = { 0, 1, 2, 3 } });
        const auto path = path_in_cwd("quad.obj");
        IO::AssimpModelWriter writer;
        ASSERT_TRUE(writer.write(path, { source })) << writer.get_last_error();

        IO::ModelReader reader;
        const auto loaded = reader.load(path);

        ASSERT_EQ(loaded.size(), 1U) << reader.get_last_error();
        EXPECT_FALSE(loaded.front().has_polygons());
        EXPECT_EQ(loaded.front().face_count(), 2U);
    }

    TEST_F(ModelFileTest, AMeshOfTrianglesAndAPentagonStaysOneMeshWhenPolygonsAreKept)
    {
        auto positions = std::vector<glm::vec3> { glm::vec3(0.0F, 0.0F, 0.0F), glm::vec3(1.0F, 0.0F, 0.0F), glm::vec3(0.0F, 1.0F, 0.0F) };
        const auto pentagon = pentagon_at(5.0F);
        positions.insert(positions.end(), pentagon.begin(), pentagon.end());
        const auto source = polygon_mesh(positions, { .sizes = { 3, 5 }, .corners = { 0, 1, 2, 3, 4, 5, 6, 7 } });
        const auto path = path_in_cwd("mixed.obj");
        IO::AssimpModelWriter writer;
        ASSERT_TRUE(writer.write(path, { source })) << writer.get_last_error();

        IO::ModelReader kept;
        kept.set_keep_polygons(true);
        const auto polygons = kept.load(path);
        IO::ModelReader triangulated;
        const auto triangles = triangulated.load(path);

        ASSERT_EQ(polygons.size(), 1U) << kept.get_last_error();
        EXPECT_EQ(polygons.front().polygon_count(), 2U);
        EXPECT_EQ(polygons.front().face_count(), 4U);
        EXPECT_EQ(polygons.front().faces->sizes, (std::vector<uint32_t> { 3, 5 }));
        ASSERT_EQ(triangles.size(), 1U) << triangulated.get_last_error();
        EXPECT_EQ(triangles_in(triangles), 4U);
    }

    TEST_F(ModelFileTest, KeepingPolygonsDerivesTheNormalsAFileDoesNotCarry)
    {
        const std::vector<glm::vec3> positions {
            glm::vec3(0.0F, 0.0F, 0.0F), glm::vec3(0.0F, 0.0F, 1.0F), glm::vec3(1.0F, 0.0F, 1.0F), glm::vec3(1.0F, 0.0F, 0.0F)
        };
        const auto source = polygon_mesh(positions, { .sizes = { 4 }, .corners = { 0, 1, 2, 3 } });
        const auto path = path_in_cwd("tilted.obj");
        IO::AssimpModelWriter writer;
        ASSERT_TRUE(writer.write(path, { source })) << writer.get_last_error();

        IO::ModelReader reader;
        reader.set_keep_polygons(true);
        const auto loaded = reader.load(path);

        ASSERT_EQ(loaded.size(), 1U) << reader.get_last_error();
        for (const auto& vertex : model_reader_writer_vertices_of(loaded.front())) {
            EXPECT_TRUE(near(vertex.normal, glm::vec3(0.0F, 1.0F, 0.0F)));
        }
    }

    TEST_F(ModelFileTest, KeepingPolygonsLeavesTheNormalsAFileDoesCarry)
    {
        std::vector<MeshVertex> vertices(4);
        const std::array<glm::vec3, 4> corners {
            glm::vec3(0.0F, 0.0F, 0.0F), glm::vec3(1.0F, 0.0F, 0.0F), glm::vec3(1.0F, 1.0F, 0.0F), glm::vec3(0.0F, 1.0F, 0.0F)
        };
        for (size_t i = 0; i < 4; ++i) {
            vertices[i].position = corners[i];
            vertices[i].normal = glm::vec3(0.0F, 1.0F, 0.0F);
        }
        const auto source = MeshData::from_vertices(vertices, Kakshya::FaceList { .sizes = { 4 }, .corners = { 0, 1, 2, 3 } }).value();
        const auto path = path_in_cwd("authored.obj");
        IO::AssimpModelWriter writer;
        ASSERT_TRUE(writer.write(path, { source })) << writer.get_last_error();

        IO::ModelReader reader;
        reader.set_keep_polygons(true);
        const auto loaded = reader.load(path);

        ASSERT_EQ(loaded.size(), 1U) << reader.get_last_error();
        for (const auto& vertex : model_reader_writer_vertices_of(loaded.front())) {
            EXPECT_TRUE(near(vertex.normal, glm::vec3(0.0F, 1.0F, 0.0F)));
        }
    }

    TEST_F(ModelFileTest, TheSwitchAppliesToTheNextOpenOnly)
    {
        const std::vector<glm::vec3> positions {
            glm::vec3(0.0F, 0.0F, 0.0F), glm::vec3(1.0F, 0.0F, 0.0F), glm::vec3(1.0F, 1.0F, 0.0F), glm::vec3(0.0F, 1.0F, 0.0F)
        };
        const auto source = polygon_mesh(positions, { .sizes = { 4 }, .corners = { 0, 1, 2, 3 } });
        const auto path = path_in_cwd("quad.obj");
        IO::AssimpModelWriter writer;
        ASSERT_TRUE(writer.write(path, { source })) << writer.get_last_error();

        IO::ModelReader reader;
        reader.set_keep_polygons(true);
        ASSERT_TRUE(reader.open(path));
        reader.set_keep_polygons(false);
        const auto still_polygons = reader.extract_meshes();
        reader.close();
        const auto triangles = reader.load(path);

        ASSERT_EQ(still_polygons.size(), 1U);
        EXPECT_TRUE(still_polygons.front().has_polygons());
        ASSERT_EQ(triangles.size(), 1U);
        EXPECT_FALSE(triangles.front().has_polygons());
    }

    TEST_F(ModelFileTest, PlyRoundTripCurrentlyReadsEveryVertexColorBackAsZero)
    {
        const auto source = flat_cube("cube");
        const auto path = path_in_cwd("colors.ply");
        IO::AssimpModelWriter writer;
        ASSERT_TRUE(writer.write(path, { source })) << writer.get_last_error();

        IO::ModelReader reader;
        const auto loaded = reader.load(path);
        ASSERT_EQ(loaded.size(), 1U) << reader.get_last_error();

        const auto read_back = model_reader_writer_vertices_of(loaded.front());
        for (const auto& original : model_reader_writer_vertices_of(source)) {
            const auto* match = find_vertex(read_back, original.position, &original.normal);
            ASSERT_NE(match, nullptr);
            EXPECT_TRUE(near(match->color, glm::vec4(0.0F)));
        }
    }

    TEST_F(ModelFileTest, ObjRoundTripKeepsVertexColorsExactly)
    {
        const auto source = flat_cube("cube");
        const auto path = path_in_cwd("colored.obj");
        IO::AssimpModelWriter writer;
        ASSERT_TRUE(writer.write(path, { source })) << writer.get_last_error();

        IO::ModelReader reader;
        const auto loaded = reader.load(path);
        ASSERT_EQ(loaded.size(), 1U);

        const auto read_back = model_reader_writer_vertices_of(loaded.front());
        for (const auto& original : model_reader_writer_vertices_of(source)) {
            const auto* match = find_vertex(read_back, original.position, &original.normal);
            ASSERT_NE(match, nullptr);
            EXPECT_TRUE(near(match->color, original.color, 1e-5F));
        }
    }

    TEST_F(ModelFileTest, ReaderJoinsIdenticalVertices)
    {
        MeshVertex a;
        a.position = { 0, 0, 0 };
        MeshVertex b;
        b.position = { 1, 0, 0 };
        MeshVertex c;
        c.position = { 1, 1, 0 };
        MeshVertex d;
        d.position = { 0, 1, 0 };
        const auto source = make_mesh({ a, b, c, a, c, d }, { 0, 1, 2, 3, 4, 5 }, "quad");

        const auto path = path_in_cwd("quad.obj");
        IO::AssimpModelWriter writer;
        ASSERT_TRUE(writer.write(path, { source })) << writer.get_last_error();

        IO::ModelReader reader;
        const auto loaded = reader.load(path);

        ASSERT_EQ(loaded.size(), 1U) << reader.get_last_error();
        EXPECT_EQ(loaded.front().vertex_count(), 4U);
        EXPECT_EQ(model_reader_writer_indices_of(loaded.front()).size(), 6U);
    }

    TEST_F(ModelFileTest, ObjRoundTripKeepsMeshAndMaterialNames)
    {
        const std::vector<MeshData> source {
            flat_cube("left", { -3.0F, 0.0F, 0.0F }, 1.0F, "mat_a"),
            flat_cube("right", { 3.0F, 0.0F, 0.0F }, 1.0F, "mat_b"),
        };
        const auto path = path_in_cwd("pair.obj");
        IO::AssimpModelWriter writer;
        ASSERT_TRUE(writer.write(path, source)) << writer.get_last_error();

        IO::ModelReader reader;
        const auto loaded = reader.load(path);
        ASSERT_EQ(loaded.size(), 2U) << reader.get_last_error();

        std::set<std::string> meshes;
        std::set<std::string> materials;
        for (const auto& mesh : loaded) {
            const auto [name, material] = names_of(mesh);
            meshes.insert(name);
            materials.insert(material);
        }
        EXPECT_EQ(meshes, (std::set<std::string> { "left", "right" }));
        EXPECT_EQ(materials, (std::set<std::string> { "mat_a", "mat_b" }));
        EXPECT_EQ(triangles_in(loaded), 24U);
    }

    TEST_F(ModelFileTest, MeshesKeepTheirOwnPositionsWhenSeveralAreWritten)
    {
        const std::vector<MeshData> source {
            flat_cube("left", { -3.0F, 0.0F, 0.0F }, 1.0F, "mat_a"),
            flat_cube("right", { 3.0F, 0.0F, 0.0F }, 1.0F, "mat_b"),
        };
        const auto path = path_in_cwd("pair_positions.obj");
        IO::AssimpModelWriter writer;
        ASSERT_TRUE(writer.write(path, source)) << writer.get_last_error();

        IO::ModelReader reader;
        const auto loaded = reader.load(path);
        ASSERT_EQ(loaded.size(), 2U);

        for (const auto& mesh : loaded) {
            const auto [lo, hi] = bounds_of(model_reader_writer_vertices_of(mesh));
            const float center_x = 0.5F * (lo.x + hi.x);
            EXPECT_TRUE(near(center_x, -3.0F) || near(center_x, 3.0F)) << center_x;
            EXPECT_TRUE(near(hi.x - lo.x, 2.0F));
        }
    }

    TEST_F(ModelFileTest, MaterialNameOverrideReplacesTheSubmeshMaterial)
    {
        const auto path = path_in_cwd("override.obj");
        IO::AssimpModelWriter writer;
        IO::ModelWriteOptions options;
        options.material_name_override = "paint";
        ASSERT_TRUE(writer.write(path, { flat_cube("cube", glm::vec3(0.0F), 1.0F, "steel") }, options))
            << writer.get_last_error();

        IO::ModelReader reader;
        const auto loaded = reader.load(path);
        ASSERT_EQ(loaded.size(), 1U);
        EXPECT_EQ(names_of(loaded.front()).second, "paint");
    }

    TEST_F(ModelFileTest, DiffusePathIsWrittenVerbatimAndReadBack)
    {
        const auto path = path_in_cwd("textured.obj");
        IO::AssimpModelWriter writer;
        ASSERT_TRUE(writer.write(path,
            { flat_cube("cube", glm::vec3(0.0F), 1.0F, "wood", "wood.png") }))
            << writer.get_last_error();

        IO::ModelReader reader;
        const auto loaded = reader.load(path);
        ASSERT_EQ(loaded.size(), 1U);

        const auto* sub = subrange_of(loaded.front());
        ASSERT_NE(sub, nullptr);
        EXPECT_EQ(sub->diffuse_path, "wood.png");
        EXPECT_FALSE(sub->diffuse_embedded);
    }

    TEST_F(ModelFileTest, ObjWithAMaterialWritesASidecarAndObjnomtlDoesNot)
    {
        IO::AssimpModelWriter writer;
        ASSERT_TRUE(writer.write(path_in_cwd("with_mtl.obj"), { flat_cube("cube") }));
        EXPECT_TRUE(RoundTrip::exists(path_in_cwd("with_mtl.mtl")));

        IO::ModelWriteOptions options;
        options.format_id = "objnomtl";
        ASSERT_TRUE(writer.write(path_in_cwd("without_mtl.obj"), { flat_cube("cube") }, options));
        EXPECT_FALSE(RoundTrip::exists(path_in_cwd("without_mtl.mtl")));
    }

    TEST_F(ModelFileTest, FormatIdOverridesTheExtension)
    {
        IO::AssimpModelWriter writer;
        IO::ModelWriteOptions options;
        options.format_id = "stl";
        const auto path = path_in_cwd("cube.dat");

        EXPECT_FALSE(writer.can_write(path));
        ASSERT_TRUE(writer.write(path, { flat_cube("cube") }, options)) << writer.get_last_error();
        EXPECT_TRUE(RoundTrip::exists(path));
    }

    TEST_F(ModelFileTest, AssimpJsonDumpIsWrittenButNotReadable)
    {
        IO::AssimpModelWriter writer;
        const auto path = path_in_cwd("scene.json");

        ASSERT_TRUE(writer.write(path, { flat_cube("cube") })) << writer.get_last_error();
        EXPECT_TRUE(RoundTrip::exists(path));
        EXPECT_FALSE(IO::ModelReader {}.can_read(path));
    }

    TEST_F(ModelFileTest, TrailingIndicesThatDoNotFormATriangleAreDropped)
    {
        auto source = flat_cube("cube");
        std::get<std::vector<uint32_t>>(source.index_variant).push_back(0U);

        const auto path = path_in_cwd("ragged.obj");
        IO::AssimpModelWriter writer;
        ASSERT_TRUE(writer.write(path, { source })) << writer.get_last_error();

        IO::ModelReader reader;
        const auto loaded = reader.load(path);
        ASSERT_EQ(loaded.size(), 1U);
        EXPECT_EQ(triangles_in(loaded), 12U);
    }

    TEST_F(ModelFileTest, EmptyMeshListIsRejected)
    {
        IO::AssimpModelWriter writer;
        const auto path = path_in_cwd("none.obj");

        EXPECT_FALSE(writer.write(path, std::vector<Kakshya::MeshData> {}));
        EXPECT_NE(writer.get_last_error().find("No meshes"), std::string::npos);
        EXPECT_FALSE(RoundTrip::exists(path));
    }

    TEST_F(ModelFileTest, InvalidMeshIsRejectedBeforeAnythingIsWritten)
    {
        IO::AssimpModelWriter writer;
        const auto path = path_in_cwd("invalid.obj");

        EXPECT_FALSE(writer.write(path, { MeshData::empty() }));
        EXPECT_NE(writer.get_last_error().find("is_valid"), std::string::npos);
        EXPECT_FALSE(RoundTrip::exists(path));
    }

    TEST_F(ModelFileTest, OneInvalidMeshAmongValidOnesRejectsTheWholeWrite)
    {
        IO::AssimpModelWriter writer;
        const auto path = path_in_cwd("mixed.obj");

        EXPECT_FALSE(writer.write(path, { flat_cube("cube"), MeshData::empty() }));
        EXPECT_FALSE(RoundTrip::exists(path));
    }

    TEST_F(ModelFileTest, ANonCanonicalVertexStrideIsRejected)
    {
        auto source = flat_cube("cube");
        source.layout.stride_bytes = 32;

        IO::AssimpModelWriter writer;
        const auto path = path_in_cwd("stride.obj");

        EXPECT_FALSE(writer.write(path, { source }));
        EXPECT_NE(writer.get_last_error().find("stride"), std::string::npos);
        EXPECT_FALSE(RoundTrip::exists(path));
    }

    TEST_F(ModelFileTest, AnExtensionWithoutADefaultFormatNeedsAnExplicitFormatId)
    {
        IO::AssimpModelWriter writer;
        const auto path = path_in_cwd("cube.xyz");

        EXPECT_FALSE(writer.can_write(path));
        EXPECT_FALSE(writer.write(path, { flat_cube("cube") }));
        EXPECT_NE(writer.get_last_error().find("format_id"), std::string::npos);
        EXPECT_FALSE(RoundTrip::exists(path));
    }

    TEST_F(ModelFileTest, AnUnknownFormatIdFailsWithAnError)
    {
        IO::AssimpModelWriter writer;
        IO::ModelWriteOptions options;
        options.format_id = "not_a_format";

        EXPECT_FALSE(writer.write(path_in_cwd("cube.obj"), { flat_cube("cube") }, options));
        EXPECT_FALSE(writer.get_last_error().empty());
    }

    TEST_F(ModelFileTest, WriterErrorIsClearedByTheNextSuccessfulWrite)
    {
        IO::AssimpModelWriter writer;
        ASSERT_FALSE(writer.write(path_in_cwd("none.obj"), std::vector<Kakshya::MeshData> {}));
        ASSERT_FALSE(writer.get_last_error().empty());

        ASSERT_TRUE(writer.write(path_in_cwd("cube.obj"), { flat_cube("cube") }));
        EXPECT_TRUE(writer.get_last_error().empty());
    }

    TEST_F(ModelFileTest, WriterSupportsTheDocumentedExtensionSet)
    {
        const auto extensions = IO::AssimpModelWriter {}.get_supported_extensions();

        for (const auto* expected : { "dae", "obj", "stl", "ply", "3ds", "gltf", "glb", "fbx", "x3d", "json" }) {
            EXPECT_NE(std::ranges::find(extensions, std::string(expected)), extensions.end()) << expected;
        }
    }

    TEST_F(ModelFileTest, WriterExtensionMatchIsCaseInsensitive)
    {
        EXPECT_TRUE(IO::AssimpModelWriter {}.can_write("MODEL.GLB"));
        EXPECT_FALSE(IO::AssimpModelWriter {}.can_write("model"));
    }

    TEST_F(ModelFileTest, RegistryDispatchesByExtensionAfterRegistration)
    {
        auto& registry = IO::ModelWriterRegistry::instance();

        EXPECT_NE(registry.create_writer("a.glb"), nullptr);
        EXPECT_NE(registry.create_writer("a.obj"), nullptr);
        EXPECT_EQ(registry.create_writer("a.unknown"), nullptr);
    }

    TEST_F(ModelFileTest, ReaderExtensionMatchIsCaseInsensitive)
    {
        IO::ModelReader reader;

        EXPECT_TRUE(reader.can_read("model.GLB"));
        EXPECT_TRUE(reader.can_read("model.Obj"));
        EXPECT_FALSE(reader.can_read("model.wav"));
        EXPECT_FALSE(reader.can_read("model"));
    }

    TEST_F(ModelFileTest, ReaderListsEveryExtensionItAdvertises)
    {
        const auto extensions = IO::ModelReader {}.get_supported_extensions();

        for (const auto* expected : { "gltf", "glb", "obj", "fbx", "ply", "stl", "dae", "3ds", "x3d", "blend" }) {
            EXPECT_NE(std::ranges::find(extensions, std::string(expected)), extensions.end()) << expected;
        }
    }

    TEST_F(ModelFileTest, MissingFileFailsWithAnError)
    {
        IO::ModelReader reader;

        EXPECT_TRUE(reader.load(path_in_cwd("absent.obj")).empty());
        EXPECT_FALSE(reader.is_open());
        EXPECT_NE(reader.get_last_error().find("File not found"), std::string::npos);
    }

    TEST_F(ModelFileTest, GarbageGlbFailsToOpen)
    {
        const auto path = path_in_cwd("garbage.glb");
        RoundTrip::write_bytes(path, RoundTrip::garbage(4096));

        IO::ModelReader reader;
        EXPECT_FALSE(reader.open(path));
        EXPECT_TRUE(reader.load(path).empty());
        EXPECT_FALSE(reader.get_last_error().empty());
    }

    TEST_F(ModelFileTest, GlbWithValidMagicButCorruptBodyFailsToOpen)
    {
        const auto path = path_in_cwd("magic.glb");
        RoundTrip::write_bytes(path, RoundTrip::with_prefix("glTF", 512));

        IO::ModelReader reader;
        EXPECT_FALSE(reader.open(path));
    }

    TEST_F(ModelFileTest, TruncatedGlbFailsToOpen)
    {
        const auto good = path_in_cwd("good.glb");
        IO::AssimpModelWriter writer;
        ASSERT_TRUE(writer.write(good, { flat_cube("cube") }));
        const auto bytes = RoundTrip::read_bytes(good);
        ASSERT_GT(bytes.size(), 64U);

        const auto cut = path_in_cwd("cut.glb");
        RoundTrip::write_bytes(cut, RoundTrip::truncated(bytes, bytes.size() / 2));

        IO::ModelReader reader;
        EXPECT_FALSE(reader.open(cut));
        EXPECT_TRUE(reader.load(cut).empty());
    }

    TEST_F(ModelFileTest, EmptyObjYieldsNoMeshes)
    {
        const auto path = path_in_cwd("empty.obj");
        RoundTrip::write_bytes(path, "");

        IO::ModelReader reader;
        EXPECT_TRUE(reader.load(path).empty());
    }

    TEST_F(ModelFileTest, UnsupportedExtensionFailsToOpen)
    {
        const auto path = path_in_cwd("notes.txt");
        RoundTrip::write_bytes(path, "not a model\n");

        IO::ModelReader reader;
        EXPECT_FALSE(reader.open(path));
        EXPECT_TRUE(reader.load(path).empty());
    }

    TEST_F(ModelFileTest, PlyWithoutFacesYieldsNoMeshes)
    {
        const auto path = path_in_cwd("points.ply");
        RoundTrip::write_bytes(path,
            "ply\nformat ascii 1.0\nelement vertex 3\nproperty float x\nproperty float y\nproperty float z\n"
            "end_header\n0 0 0\n1 0 0\n0 1 0\n");

        IO::ModelReader reader;
        EXPECT_TRUE(reader.load(path).empty());
    }

    TEST_F(ModelFileTest, ObjWithOnlyLinesYieldsNoMeshes)
    {
        const auto path = path_in_cwd("lines.obj");
        RoundTrip::write_bytes(path, "v 0 0 0\nv 1 0 0\nv 1 1 0\nl 1 2 3\n");

        IO::ModelReader reader;
        EXPECT_TRUE(reader.load(path).empty());
    }

    TEST_F(ModelFileTest, OpenReportsSceneCountsAndCloseDropsThem)
    {
        const auto path = path_in_cwd("counted.obj");
        IO::AssimpModelWriter writer;
        ASSERT_TRUE(writer.write(path, { flat_cube("a", { -3, 0, 0 }, 1.0F, "m1"), flat_cube("b", { 3, 0, 0 }, 1.0F, "m2") }));

        IO::ModelReader reader;
        ASSERT_TRUE(reader.open(path)) << reader.get_last_error();
        EXPECT_TRUE(reader.is_open());

        const auto metadata = reader.get_metadata();
        ASSERT_TRUE(metadata.has_value());
        const auto* info = metadata->get<IO::ModelInfo>();
        ASSERT_NE(info, nullptr);
        EXPECT_EQ(info->mesh_count, 2U);
        EXPECT_GE(info->material_count, 2U);
        EXPECT_EQ(info->animation_count, 0U);
        EXPECT_EQ(info->texture_count, 0U);

        reader.close();
        EXPECT_FALSE(reader.is_open());
        EXPECT_FALSE(reader.get_metadata().has_value());
        EXPECT_TRUE(reader.extract_meshes().empty());
        EXPECT_NE(reader.get_last_error().find("No scene loaded"), std::string::npos);
    }

    TEST_F(ModelFileTest, ExtractMeshesAfterOpenMatchesLoad)
    {
        const auto path = path_in_cwd("extract.obj");
        IO::AssimpModelWriter writer;
        ASSERT_TRUE(writer.write(path, { flat_cube("cube") }));

        IO::ModelReader opened;
        ASSERT_TRUE(opened.open(path));
        const auto via_extract = opened.extract_meshes();

        IO::ModelReader loader;
        const auto via_load = loader.load(path);

        ASSERT_EQ(via_extract.size(), via_load.size());
        EXPECT_EQ(model_reader_writer_vertices_of(via_extract.front()).size(), model_reader_writer_vertices_of(via_load.front()).size());
        EXPECT_EQ(model_reader_writer_indices_of(via_extract.front()), model_reader_writer_indices_of(via_load.front()));
    }

    TEST_F(ModelFileTest, OpeningASecondFileReplacesTheFirst)
    {
        IO::AssimpModelWriter writer;
        ASSERT_TRUE(writer.write(path_in_cwd("one.obj"), { flat_cube("a") }));
        ASSERT_TRUE(writer.write(path_in_cwd("two.obj"), { flat_cube("a", { -3, 0, 0 }, 1.0F, "m1"), flat_cube("b", { 3, 0, 0 }, 1.0F, "m2") }));

        IO::ModelReader reader;
        ASSERT_TRUE(reader.open(path_in_cwd("one.obj")));
        ASSERT_TRUE(reader.open(path_in_cwd("two.obj")));

        EXPECT_EQ(reader.extract_meshes().size(), 2U);
    }

    TEST_F(ModelFileTest, FailedOpenLeavesNothingOpen)
    {
        IO::AssimpModelWriter writer;
        ASSERT_TRUE(writer.write(path_in_cwd("one.obj"), { flat_cube("a") }));

        IO::ModelReader reader;
        ASSERT_TRUE(reader.open(path_in_cwd("one.obj")));
        EXPECT_FALSE(reader.open(path_in_cwd("absent.obj")));

        EXPECT_FALSE(reader.is_open());
        EXPECT_TRUE(reader.extract_meshes().empty());
    }

    TEST_F(ModelFileTest, ContainerStyleInterfaceIsInertForMeshes)
    {
        IO::ModelReader reader;

        EXPECT_EQ(reader.create_container(), nullptr);
        EXPECT_NE(reader.get_last_error().find("SignalSourceContainer"), std::string::npos);
        EXPECT_FALSE(reader.load_into_container(nullptr));
        EXPECT_TRUE(reader.read_all().empty());
        EXPECT_TRUE(reader.get_regions().empty());
        EXPECT_TRUE(reader.seek({ 3 }));
        EXPECT_EQ(reader.get_read_position(), std::vector<uint64_t> { 0 });
        EXPECT_FALSE(reader.supports_streaming());
    }

    TEST_F(ModelFileTest, NetworkAndBufferFactoriesNeedAnOpenScene)
    {
        IO::ModelReader reader;

        EXPECT_EQ(reader.create_mesh_network(), nullptr);
        EXPECT_TRUE(reader.create_mesh_buffers().empty());
        EXPECT_NE(reader.get_last_error().find("No scene loaded"), std::string::npos);
    }

    TEST_F(ModelFileTest, ObjLoadsAsOneRootSlotPerMesh)
    {
        const auto path = path_in_cwd("slots.obj");
        IO::AssimpModelWriter writer;
        ASSERT_TRUE(writer.write(path, { flat_cube("left", { -3, 0, 0 }, 1.0F, "m1"), flat_cube("right", { 3, 0, 0 }, 1.0F, "m2") }));

        IO::ModelReader reader;
        ASSERT_TRUE(reader.open(path));
        const auto network = reader.create_mesh_network();

        ASSERT_NE(network, nullptr);
        ASSERT_EQ(network->slot_count(), 2U);
        for (const auto& slot : network->slots()) {
            EXPECT_FALSE(slot.parent_index.has_value());
            EXPECT_EQ(slot.local_transform, glm::mat4(1.0F));
            EXPECT_FALSE(slot.name.empty());
            ASSERT_NE(slot.node, nullptr);
        }
    }

    std::string two_node_gltf(bool shared_mesh, const std::string& parent_transform = R"("translation":[0,5,0])")
    {
        const std::array<float, 9> triangle { 0, 0, 0, 1, 0, 0, 0, 1, 0 };
        const std::string payload(reinterpret_cast<const char*>(triangle.data()), sizeof(float) * triangle.size());

        std::string nodes = R"([{"name":"parent",)" + parent_transform + R"(,"children":[1)";
        nodes += shared_mesh ? ",2]}," : "]},";
        nodes += R"({"name":"wheel_fl","mesh":0,"translation":[1,0,0]})";
        if (shared_mesh) {
            nodes += R"(,{"name":"wheel_fr","mesh":0,"translation":[-1,0,0]})";
        }
        nodes += "]";

        return R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],"nodes":)" + nodes
            + R"(,"meshes":[{"name":"tri","primitives":[{"attributes":{"POSITION":0},"mode":4}]}],)"
              R"("accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[1,1,0]}],)"
              R"("bufferViews":[{"buffer":0,"byteLength":36,"target":34962}],)"
              R"("buffers":[{"byteLength":36,"uri":"data:application/octet-stream;base64,)"
            + RoundTrip::base64(payload) + R"("}]})";
    }

    TEST_F(ModelFileTest, SceneGraphFileLoadsThroughAssimp)
    {
        const auto path = path_in_cwd("graph.gltf");
        RoundTrip::write_bytes(path, two_node_gltf(false));

        IO::ModelReader reader;
        const auto loaded = reader.load(path);

        ASSERT_EQ(loaded.size(), 1U) << reader.get_last_error();
        EXPECT_EQ(triangles_in(loaded), 1U);
    }

    TEST_F(ModelFileTest, ExtractedMeshVerticesStayInTheirOwnLocalSpace)
    {
        const auto path = path_in_cwd("graph_vertices.gltf");
        RoundTrip::write_bytes(path, two_node_gltf(false));

        IO::ModelReader reader;
        const auto loaded = reader.load(path);
        ASSERT_EQ(loaded.size(), 1U);

        const auto [lo, hi] = bounds_of(model_reader_writer_vertices_of(loaded.front()));
        EXPECT_TRUE(near(lo, glm::vec3(0.0F, 0.0F, 0.0F)));
        EXPECT_TRUE(near(hi, glm::vec3(1.0F, 1.0F, 0.0F)));
    }

    TEST_F(ModelFileTest, NetworkFollowsTheNodeTreeWithAJointForAMeshlessParent)
    {
        const auto path = path_in_cwd("graph_slots.gltf");
        RoundTrip::write_bytes(path, two_node_gltf(false));

        IO::ModelReader reader;
        ASSERT_TRUE(reader.open(path)) << reader.get_last_error();
        const auto network = reader.create_mesh_network();

        ASSERT_NE(network, nullptr);
        ASSERT_EQ(network->slot_count(), 2U);

        const auto* parent = network->find_slot("parent");
        const auto* wheel = network->find_slot("wheel_fl");
        ASSERT_NE(parent, nullptr);
        ASSERT_NE(wheel, nullptr);
        EXPECT_EQ(parent->node, nullptr);
        EXPECT_FALSE(parent->parent_index.has_value());
        EXPECT_EQ(wheel->parent_index, parent->index);
        EXPECT_NE(wheel->node, nullptr);
        EXPECT_TRUE(near(glm::vec3(parent->local_transform[3]), glm::vec3(0.0F, 5.0F, 0.0F)));
        EXPECT_TRUE(near(glm::vec3(wheel->local_transform[3]), glm::vec3(1.0F, 0.0F, 0.0F)));
        EXPECT_TRUE(near(glm::vec3(wheel->world_transform[3]), glm::vec3(1.0F, 5.0F, 0.0F)));
    }

    TEST_F(ModelFileTest, ARotatedParentPlacesTheChildThroughTheComposedWorldTransform)
    {
        const auto path = path_in_cwd("graph_rotated.gltf");
        RoundTrip::write_bytes(path, two_node_gltf(false, R"("rotation":[0,0.70710678,0,0.70710678])"));

        IO::ModelReader reader;
        ASSERT_TRUE(reader.open(path)) << reader.get_last_error();
        const auto network = reader.create_mesh_network();

        ASSERT_NE(network, nullptr);
        const auto* wheel = network->find_slot("wheel_fl");
        ASSERT_NE(wheel, nullptr);
        EXPECT_TRUE(near(glm::vec3(wheel->world_transform[3]), glm::vec3(0.0F, 0.0F, -1.0F), 1e-5F));
    }

    TEST_F(ModelFileTest, AMeshUsedByTwoNodesBecomesTwoSlotsWithTheirOwnPlacement)
    {
        const auto path = path_in_cwd("graph_shared.gltf");
        RoundTrip::write_bytes(path, two_node_gltf(true));

        IO::ModelReader reader;
        ASSERT_TRUE(reader.open(path)) << reader.get_last_error();
        const auto network = reader.create_mesh_network();

        ASSERT_NE(network, nullptr);
        ASSERT_EQ(network->slot_count(), 3U);
        const auto* front_left = network->find_slot("wheel_fl");
        const auto* front_right = network->find_slot("wheel_fr");
        ASSERT_NE(front_left, nullptr);
        ASSERT_NE(front_right, nullptr);
        EXPECT_NE(front_left->node, front_right->node);
        EXPECT_TRUE(near(glm::vec3(front_left->world_transform[3]), glm::vec3(1.0F, 5.0F, 0.0F)));
        EXPECT_TRUE(near(glm::vec3(front_right->world_transform[3]), glm::vec3(-1.0F, 5.0F, 0.0F)));
        EXPECT_EQ(reader.extract_meshes().size(), 1U);
    }

    TEST_F(ModelFileTest, SaveMeshBakesEverySlotsWorldTransformIntoTheExport)
    {
        auto triangle = [] {
            auto node = std::make_shared<Nodes::GpuSync::MeshWriterNode>(16);
            std::vector<MeshVertex> vertices(3);
            vertices[0].position = { 0, 0, 0 };
            vertices[1].position = { 1, 0, 0 };
            vertices[2].position = { 0, 1, 0 };
            node->set_mesh(vertices, std::vector<uint32_t> { 0, 1, 2 });
            node->compute_frame();
            return node;
        };
        auto translation = [](float x, float y, float z) {
            glm::mat4 m(1.0F);
            m[3] = glm::vec4(x, y, z, 1.0F);
            return m;
        };

        auto network = std::make_shared<Nodes::Network::MeshNetwork>();
        network->add_slot("root", triangle());
        network->add_slot("child", triangle(), 0U);
        network->get_slot(0).local_transform = translation(10.0F, 0.0F, 0.0F);
        network->get_slot(1).local_transform = translation(0.0F, 5.0F, 0.0F);
        network->process_batch(1);

        const auto path = path_in_cwd("network.obj");
        ASSERT_TRUE(IO::save_mesh(network, path, {}));

        IO::ModelReader reader;
        const auto loaded = reader.load(path);
        ASSERT_EQ(loaded.size(), 2U) << reader.get_last_error();

        std::vector<MeshVertex> all;
        for (const auto& mesh : loaded) {
            const auto vertices = model_reader_writer_vertices_of(mesh);
            all.insert(all.end(), vertices.begin(), vertices.end());
        }
        EXPECT_NE(find_vertex(all, { 10.0F, 0.0F, 0.0F }), nullptr);
        EXPECT_NE(find_vertex(all, { 11.0F, 0.0F, 0.0F }), nullptr);
        EXPECT_NE(find_vertex(all, { 10.0F, 5.0F, 0.0F }), nullptr);
        EXPECT_NE(find_vertex(all, { 11.0F, 5.0F, 0.0F }), nullptr);
    }

    TEST_F(ModelFileTest, SaveMeshRejectsNullInputsAndAnEmptyNetwork)
    {
        EXPECT_FALSE(IO::save_mesh(std::shared_ptr<Nodes::Network::MeshNetwork> {}, path_in_cwd("a.obj"), {}));
        EXPECT_FALSE(IO::save_mesh(std::shared_ptr<Nodes::GpuSync::MeshWriterNode> {}, path_in_cwd("b.obj"), {}));
        EXPECT_FALSE(IO::save_mesh(std::make_shared<Nodes::Network::MeshNetwork>(), path_in_cwd("c.obj"), {}));
        EXPECT_FALSE(RoundTrip::exists(path_in_cwd("c.obj")));
    }

    TEST_F(ModelFileTest, SaveMeshWithoutARegisteredExtensionFails)
    {
        auto node = std::make_shared<Nodes::GpuSync::MeshWriterNode>(16);
        std::vector<MeshVertex> vertices(3);
        vertices[1].position = { 1, 0, 0 };
        vertices[2].position = { 0, 1, 0 };
        node->set_mesh(vertices, std::vector<uint32_t> { 0, 1, 2 });
        node->compute_frame();

        EXPECT_FALSE(IO::save_mesh(node, path_in_cwd("node.unknown"), {}));
        EXPECT_TRUE(IO::save_mesh(node, path_in_cwd("node.obj"), {}));
        EXPECT_TRUE(RoundTrip::exists(path_in_cwd("node.obj")));
    }

    struct SceneNode {
        std::string name;
        std::optional<glm::mat4> matrix;
        glm::vec3 translation { 0.0F };
        float yaw { 0.0F };
        glm::vec3 scale { 1.0F };
        int mesh { -1 };
        std::vector<size_t> children;
    };

    struct MeshSpec {
        size_t triangles { 1 };
        bool points { false };
    };

    struct SceneSpec {
        std::vector<SceneNode> nodes;
        std::vector<size_t> roots;
        std::vector<MeshSpec> meshes;
    };

    size_t add_node(SceneSpec& scene, SceneNode node)
    {
        scene.nodes.push_back(std::move(node));
        return scene.nodes.size() - 1;
    }

    std::string number(float value)
    {
        return std::format("{:.9g}", value);
    }

    glm::mat4 local_of(const SceneNode& node)
    {
        if (node.matrix) {
            return *node.matrix;
        }
        return glm::translate(glm::mat4(1.0F), node.translation)
            * glm::rotate(glm::mat4(1.0F), node.yaw, glm::vec3(0.0F, 1.0F, 0.0F))
            * glm::scale(glm::mat4(1.0F), node.scale);
    }

    std::string scene_json(const SceneSpec& spec)
    {
        std::string payload;
        std::string accessors;
        std::string views;
        std::string meshes;
        size_t primitive = 0;

        for (size_t m = 0; m < spec.meshes.size(); ++m) {
            std::string primitives;
            const auto add_primitive = [&](int mode) {
                const float base = static_cast<float>(m * 10 + primitive);
                const std::array<float, 9> triangle { base, 0.0F, 0.0F, base + 1.0F, 0.0F, 0.0F, base, 1.0F, 0.0F };
                payload.append(reinterpret_cast<const char*>(triangle.data()), sizeof(triangle));
                if (primitive > 0) {
                    accessors += ',';
                    views += ',';
                }
                if (!primitives.empty()) {
                    primitives += ',';
                }
                accessors += std::format(R"({{"bufferView":{},"componentType":5126,"count":3,"type":"VEC3","min":[{},0,0],"max":[{},1,0]}})",
                    primitive, number(base), number(base + 1.0F));
                views += std::format(R"({{"buffer":0,"byteOffset":{},"byteLength":36,"target":34962}})", primitive * 36);
                primitives += std::format(R"({{"attributes":{{"POSITION":{}}},"mode":{}}})", primitive, mode);
                ++primitive;
            };
            for (size_t t = 0; t < spec.meshes[m].triangles; ++t) {
                add_primitive(4);
            }
            if (spec.meshes[m].points) {
                add_primitive(0);
            }
            if (m > 0) {
                meshes += ',';
            }
            meshes += std::format(R"({{"name":"mesh_{}","primitives":[{}]}})", m, primitives);
        }

        std::string nodes;
        for (size_t i = 0; i < spec.nodes.size(); ++i) {
            const auto& node = spec.nodes[i];
            std::string json = "{";
            if (!node.name.empty()) {
                json += "\"name\":\"" + node.name + "\",";
            }
            if (node.matrix) {
                json += "\"matrix\":[";
                for (int c = 0; c < 4; ++c) {
                    for (int r = 0; r < 4; ++r) {
                        json += number((*node.matrix)[c][r]);
                        json += (c == 3 && r == 3) ? "]," : ",";
                    }
                }
            } else {
                json += std::format(R"("translation":[{},{},{}],)", number(node.translation.x), number(node.translation.y), number(node.translation.z));
                json += std::format(R"("rotation":[0,{},0,{}],)", number(std::sin(node.yaw * 0.5F)), number(std::cos(node.yaw * 0.5F)));
                json += std::format(R"("scale":[{},{},{}],)", number(node.scale.x), number(node.scale.y), number(node.scale.z));
            }
            if (node.mesh >= 0) {
                json += std::format("\"mesh\":{},", node.mesh);
            }
            if (!node.children.empty()) {
                json += "\"children\":[";
                for (size_t c = 0; c < node.children.size(); ++c) {
                    json += (c ? "," : "") + std::to_string(node.children[c]);
                }
                json += "],";
            }
            json.pop_back();
            json += "}";
            nodes += (i ? "," : "") + json;
        }

        std::string roots;
        for (size_t r = 0; r < spec.roots.size(); ++r) {
            roots += (r ? "," : "") + std::to_string(spec.roots[r]);
        }

        return R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[)" + roots + R"(]}],"nodes":[)" + nodes
            + R"(],"meshes":[)" + meshes + R"(],"accessors":[)" + accessors + R"(],"bufferViews":[)" + views
            + R"(],"buffers":[{"byteLength":)" + std::to_string(payload.size())
            + R"(,"uri":"data:application/octet-stream;base64,)" + RoundTrip::base64(payload) + R"("}]})";
    }

    struct ExpectedSlot {
        std::string name;
        bool name_known { false };
        std::optional<size_t> parent;
        bool geometry { false };
        glm::mat4 world { 1.0F };
    };

    void expect_walk(
        const SceneSpec& spec,
        size_t index,
        std::optional<size_t> parent,
        const glm::mat4& parent_world,
        std::vector<ExpectedSlot>& out)
    {
        const auto& node = spec.nodes[index];
        const glm::mat4 world = parent_world * local_of(node);
        const size_t triangles = node.mesh >= 0 ? spec.meshes[static_cast<size_t>(node.mesh)].triangles : 0;

        const size_t own = out.size();
        out.push_back({ .name = node.name,
            .name_known = !node.name.empty() && node.name.size() < 1024,
            .parent = parent,
            .geometry = triangles > 0,
            .world = world });
        for (size_t extra = 1; extra < triangles; ++extra) {
            out.push_back({ .name = {}, .name_known = false, .parent = own, .geometry = true, .world = world });
        }
        for (const size_t child : node.children) {
            expect_walk(spec, child, own, world, out);
        }
    }

    std::vector<ExpectedSlot> expected_slots(const SceneSpec& spec)
    {
        std::vector<ExpectedSlot> out;
        for (const size_t root : spec.roots) {
            expect_walk(spec, root, std::nullopt, glm::mat4(1.0F), out);
        }
        return out;
    }

    bool matrices_near(const glm::mat4& a, const glm::mat4& b, float tolerance)
    {
        for (int c = 0; c < 4; ++c) {
            for (int r = 0; r < 4; ++r) {
                if (!(std::abs(a[c][r] - b[c][r]) <= tolerance)) {
                    return false;
                }
            }
        }
        return true;
    }

    SceneSpec hostile_rig()
    {
        SceneSpec spec;
        spec.meshes = { { 1, false }, { 1, false }, { 2, false }, { 0, true }, { 1, true } };

        size_t previous = 0;
        for (int i = 0; i < 600; ++i) {
            SceneNode bone;
            bone.name = "bone_" + std::to_string(i);
            bone.translation = { 0.01F, 0.02F, 0.0F };
            bone.yaw = 0.01F;
            if (i % 50 == 0) {
                bone.mesh = (i / 50) % 2;
            }
            const size_t index = add_node(spec, bone);
            if (i == 0) {
                spec.roots.push_back(index);
            } else {
                spec.nodes[previous].children.push_back(index);
            }
            previous = index;
        }

        SceneNode hand;
        hand.name = "hand";
        hand.translation = { 0.0F, 0.0F, 1.0F };
        hand.mesh = 0;
        const size_t hand_index = add_node(spec, hand);
        SceneNode forearm;
        forearm.name = "forearm";
        forearm.translation = { 0.0F, 1.0F, 0.0F };
        forearm.yaw = 0.5F;
        forearm.children = { hand_index };
        const size_t forearm_index = add_node(spec, forearm);
        SceneNode upper;
        upper.name = "upper_arm";
        upper.translation = { 2.0F, 0.0F, 0.0F };
        upper.children = { forearm_index };
        spec.roots.push_back(add_node(spec, upper));

        SceneNode hub;
        hub.name = "hub";
        hub.translation = { 3.0F, 0.0F, 0.0F };
        hub.scale = { 2.0F, 2.0F, 2.0F };
        const size_t hub_index = add_node(spec, hub);
        spec.roots.push_back(hub_index);
        for (int k = 0; k < 300; ++k) {
            SceneNode spoke;
            spoke.name = "spoke_" + std::to_string(k);
            spoke.translation = { static_cast<float>(k) * 0.01F, 0.0F, 0.0F };
            if (k % 100 == 0) {
                spoke.mesh = 2;
            }
            const size_t spoke_index = add_node(spec, spoke);
            spec.nodes[hub_index].children.push_back(spoke_index);
        }

        SceneNode mirror;
        mirror.name = "mirror";
        mirror.scale = { -1.0F, 1.0F, 1.0F };
        SceneNode mirrored;
        mirrored.name = "mirrored";
        mirrored.translation = { 1.0F, 0.0F, 0.0F };
        mirrored.mesh = 0;
        mirror.children = { add_node(spec, mirrored) };
        spec.roots.push_back(add_node(spec, mirror));

        glm::mat4 shear(1.0F);
        shear[1][0] = 0.5F;
        shear[3] = glm::vec4(0.0F, 0.0F, 4.0F, 1.0F);
        SceneNode skew;
        skew.name = "skew";
        skew.matrix = shear;
        SceneNode skewed;
        skewed.name = "skewed";
        skewed.translation = { 0.0F, 2.0F, 0.0F };
        skewed.mesh = 0;
        skew.children = { add_node(spec, skewed) };
        spec.roots.push_back(add_node(spec, skew));

        SceneNode dup_a;
        dup_a.name = "dup";
        dup_a.translation = { 1.0F, 0.0F, 0.0F };
        dup_a.mesh = 0;
        SceneNode dup_b = dup_a;
        dup_b.translation = { 2.0F, 0.0F, 0.0F };
        SceneNode dupes;
        dupes.name = "dupes";
        dupes.children = { add_node(spec, dup_a), add_node(spec, dup_b) };
        spec.roots.push_back(add_node(spec, dupes));

        SceneNode after;
        after.name = "after_unnamed";
        after.translation = { 0.0F, 0.0F, 3.0F };
        after.mesh = 0;
        SceneNode unnamed_joint;
        unnamed_joint.translation = { 0.0F, 1.0F, 0.0F };
        unnamed_joint.children = { add_node(spec, after) };
        SceneNode unnamed;
        unnamed.mesh = 1;
        unnamed.children = { add_node(spec, unnamed_joint) };
        spec.roots.push_back(add_node(spec, unnamed));

        SceneNode long_name;
        long_name.name = std::string(3000, 'L');
        long_name.mesh = 0;
        spec.roots.push_back(add_node(spec, long_name));

        SceneNode unicode;
        unicode.name = "joint_\xC3\xA9\xE2\x82\xAC";
        unicode.mesh = 1;
        spec.roots.push_back(add_node(spec, unicode));

        SceneNode cloud;
        cloud.name = "cloud_node";
        cloud.mesh = 3;
        spec.roots.push_back(add_node(spec, cloud));

        SceneNode mixed;
        mixed.name = "mixed";
        mixed.mesh = 4;
        spec.roots.push_back(add_node(spec, mixed));

        SceneNode under;
        under.name = "under_collapsed";
        under.translation = { 1.0F, 0.0F, 0.0F };
        under.mesh = 0;
        SceneNode collapsed;
        collapsed.name = "collapsed";
        collapsed.translation = { 7.0F, 0.0F, 0.0F };
        collapsed.scale = { 0.0F, 0.0F, 0.0F };
        collapsed.children = { add_node(spec, under) };
        spec.roots.push_back(add_node(spec, collapsed));

        return spec;
    }

    std::shared_ptr<Nodes::Network::MeshNetwork> network_from(IO::ModelReader& reader, const std::string& path)
    {
        EXPECT_TRUE(reader.open(path)) << reader.get_last_error();
        return reader.create_mesh_network();
    }

    TEST_F(ModelFileTest, HostileRigFollowsTheSpecSlotForSlotInDepthFirstOrder)
    {
        const auto spec = hostile_rig();
        const auto path = path_in_cwd("rig.gltf");
        RoundTrip::write_bytes(path, scene_json(spec));

        IO::ModelReader reader;
        const auto network = network_from(reader, path);
        ASSERT_NE(network, nullptr);

        const auto expected = expected_slots(spec);
        ASSERT_EQ(network->slot_count(), expected.size());
        for (size_t i = 0; i < expected.size(); ++i) {
            const auto& slot = network->get_slot(static_cast<uint32_t>(i));
            EXPECT_EQ(slot.index, i);
            EXPECT_EQ(slot.parent_index.has_value(), expected[i].parent.has_value()) << i;
            if (slot.parent_index && expected[i].parent) {
                EXPECT_EQ(*slot.parent_index, *expected[i].parent) << i;
            }
            EXPECT_EQ(slot.node != nullptr, expected[i].geometry) << i << ' ' << slot.name;
            EXPECT_FALSE(slot.name.empty()) << i;
            if (expected[i].name_known) {
                EXPECT_EQ(slot.name, expected[i].name) << i;
            }
            EXPECT_TRUE(matrices_near(slot.world_transform, expected[i].world, 2e-3F)) << i << ' ' << slot.name;
        }
    }

    TEST_F(ModelFileTest, HostileRigParentsAlwaysPrecedeTheirChildren)
    {
        const auto path = path_in_cwd("rig_order.gltf");
        RoundTrip::write_bytes(path, scene_json(hostile_rig()));

        IO::ModelReader reader;
        const auto network = network_from(reader, path);
        ASSERT_NE(network, nullptr);

        for (const auto& slot : network->slots()) {
            if (slot.parent_index) {
                EXPECT_LT(*slot.parent_index, slot.index) << slot.name;
                const auto& children = network->get_slot(*slot.parent_index).child_indices;
                EXPECT_NE(std::ranges::find(children, slot.index), children.end()) << slot.name;
            }
        }
    }

    TEST_F(ModelFileTest, HostileRigGivesEveryGeometrySlotItsOwnNodeAndTheSharedMeshesSameVertices)
    {
        const auto path = path_in_cwd("rig_nodes.gltf");
        RoundTrip::write_bytes(path, scene_json(hostile_rig()));

        IO::ModelReader reader;
        const auto network = network_from(reader, path);
        ASSERT_NE(network, nullptr);

        std::set<const void*> nodes;
        size_t geometry = 0;
        for (const auto& slot : network->slots()) {
            if (slot.node) {
                ++geometry;
                nodes.insert(slot.node.get());
                EXPECT_EQ(slot.node->get_mesh_vertex_count(), 3U) << slot.name;
            }
        }
        EXPECT_GT(geometry, 20U);
        EXPECT_EQ(nodes.size(), geometry);
    }

    TEST_F(ModelFileTest, HostileRigUnrenderableMeshesLeaveJointsAndMixedMeshesKeepTheirTriangle)
    {
        const auto path = path_in_cwd("rig_points.gltf");
        RoundTrip::write_bytes(path, scene_json(hostile_rig()));

        IO::ModelReader reader;
        const auto network = network_from(reader, path);
        ASSERT_NE(network, nullptr);

        const auto* cloud = network->find_slot("cloud_node");
        const auto* mixed = network->find_slot("mixed");
        ASSERT_NE(cloud, nullptr);
        ASSERT_NE(mixed, nullptr);
        EXPECT_EQ(cloud->node, nullptr);
        ASSERT_NE(mixed->node, nullptr);
        EXPECT_EQ(mixed->node->get_mesh_indices().size(), 3U);
        EXPECT_TRUE(mixed->child_indices.empty());
    }

    TEST_F(ModelFileTest, HostileRigWorldTransformsSurviveANetworkBatch)
    {
        const auto spec = hostile_rig();
        const auto path = path_in_cwd("rig_batch.gltf");
        RoundTrip::write_bytes(path, scene_json(spec));

        IO::ModelReader reader;
        const auto network = network_from(reader, path);
        ASSERT_NE(network, nullptr);
        network->process_batch(1);

        const auto expected = expected_slots(spec);
        ASSERT_EQ(network->slot_count(), expected.size());
        for (size_t i = 0; i < expected.size(); ++i) {
            EXPECT_TRUE(matrices_near(network->get_slot(static_cast<uint32_t>(i)).world_transform, expected[i].world, 2e-3F)) << i;
        }
    }

    TEST_F(ModelFileTest, HostileRigDuplicateNamesKeepEveryPlacement)
    {
        const auto path = path_in_cwd("rig_dup.gltf");
        RoundTrip::write_bytes(path, scene_json(hostile_rig()));

        IO::ModelReader reader;
        const auto network = network_from(reader, path);
        ASSERT_NE(network, nullptr);

        std::vector<float> placements;
        for (const auto& slot : network->slots()) {
            if (slot.name == "dup") {
                placements.push_back(slot.world_transform[3].x);
            }
        }
        std::ranges::sort(placements);
        ASSERT_EQ(placements.size(), 2U);
        EXPECT_NEAR(placements[0], 1.0F, 1e-4F);
        EXPECT_NEAR(placements[1], 2.0F, 1e-4F);
    }

    TEST_F(ModelFileTest, HostileRigMirroredAndShearedAndCollapsedParentsStayFinite)
    {
        const auto path = path_in_cwd("rig_degenerate.gltf");
        RoundTrip::write_bytes(path, scene_json(hostile_rig()));

        IO::ModelReader reader;
        const auto network = network_from(reader, path);
        ASSERT_NE(network, nullptr);

        const auto* mirrored = network->find_slot("mirrored");
        const auto* skewed = network->find_slot("skewed");
        const auto* under = network->find_slot("under_collapsed");
        ASSERT_NE(mirrored, nullptr);
        ASSERT_NE(skewed, nullptr);
        ASSERT_NE(under, nullptr);
        EXPECT_NEAR(mirrored->world_transform[3].x, -1.0F, 1e-5F);
        EXPECT_NEAR(skewed->world_transform[3].x, 1.0F, 1e-5F);
        EXPECT_NEAR(skewed->world_transform[3].z, 4.0F, 1e-5F);
        EXPECT_NEAR(under->world_transform[3].x, 7.0F, 1e-5F);
        for (const auto& slot : network->slots()) {
            for (int c = 0; c < 4; ++c) {
                for (int r = 0; r < 4; ++r) {
                    ASSERT_TRUE(std::isfinite(slot.world_transform[c][r])) << slot.name;
                }
            }
        }
    }

    TEST_F(ModelFileTest, HostileRigExportBakesEveryGeometrySlotsWorldPositions)
    {
        const auto spec = hostile_rig();
        const auto path = path_in_cwd("rig.gltf");
        RoundTrip::write_bytes(path, scene_json(spec));

        IO::ModelReader reader;
        const auto network = network_from(reader, path);
        ASSERT_NE(network, nullptr);

        const auto exported = path_in_cwd("rig_baked.obj");
        ASSERT_TRUE(IO::save_mesh(network, exported, {}));

        IO::ModelReader again;
        const auto loaded = again.load(exported);
        ASSERT_FALSE(loaded.empty()) << again.get_last_error();

        std::vector<MeshVertex> baked;
        for (const auto& mesh : loaded) {
            const auto vertices = model_reader_writer_vertices_of(mesh);
            baked.insert(baked.end(), vertices.begin(), vertices.end());
        }

        size_t geometry = 0;
        for (const auto& slot : network->slots()) {
            if (!slot.node) {
                continue;
            }
            ++geometry;
            for (const auto& vertex : slot.node->get_mesh_vertices()) {
                const glm::vec3 world = glm::vec3(slot.world_transform * glm::vec4(vertex.position, 1.0F));
                bool found = false;
                for (const auto& candidate : baked) {
                    if (near(candidate.position, world, 5e-3F)) {
                        found = true;
                        break;
                    }
                }
                EXPECT_TRUE(found) << slot.name;
            }
        }
        EXPECT_EQ(triangles_in(loaded), geometry);
    }

    TEST_F(ModelFileTest, ADeepChainOfNodesKeepsItsOrderWithoutOverflowingTheStack)
    {
        SceneSpec spec;
        spec.meshes = { { 1, false } };
        constexpr int depth = 2000;
        size_t previous = 0;
        for (int i = 0; i < depth; ++i) {
            SceneNode bone;
            bone.name = "link_" + std::to_string(i);
            bone.translation = { 0.001F, 0.0F, 0.0F };
            if (i == depth - 1) {
                bone.mesh = 0;
            }
            const size_t index = add_node(spec, bone);
            if (i == 0) {
                spec.roots.push_back(index);
            } else {
                spec.nodes[previous].children.push_back(index);
            }
            previous = index;
        }
        const auto path = path_in_cwd("deep.gltf");
        RoundTrip::write_bytes(path, scene_json(spec));

        IO::ModelReader reader;
        const auto network = network_from(reader, path);

        ASSERT_NE(network, nullptr);
        ASSERT_EQ(network->slot_count(), static_cast<size_t>(depth));
        const auto& leaf = network->get_slot(depth - 1);
        EXPECT_NE(leaf.node, nullptr);
        EXPECT_EQ(leaf.parent_index, static_cast<uint32_t>(depth - 2));
        EXPECT_NEAR(leaf.world_transform[3].x, 2.0F, 1e-2F);
    }

    TEST_F(ModelFileTest, AWideFanOfNodesKeepsEveryChild)
    {
        SceneSpec spec;
        spec.meshes = { { 1, false } };
        SceneNode root;
        root.name = "root";
        root.translation = { 1.0F, 0.0F, 0.0F };
        const size_t root_index = add_node(spec, root);
        spec.roots.push_back(root_index);
        for (int k = 0; k < 5000; ++k) {
            SceneNode leaf;
            leaf.name = "leaf_" + std::to_string(k);
            leaf.translation = { 0.0F, static_cast<float>(k), 0.0F };
            leaf.mesh = 0;
            const size_t leaf_index = add_node(spec, leaf);
            spec.nodes[root_index].children.push_back(leaf_index);
        }
        const auto path = path_in_cwd("wide.gltf");
        RoundTrip::write_bytes(path, scene_json(spec));

        IO::ModelReader reader;
        const auto network = network_from(reader, path);

        ASSERT_NE(network, nullptr);
        ASSERT_EQ(network->slot_count(), 5001U);
        EXPECT_EQ(network->get_slot(0).child_indices.size(), 5000U);
        EXPECT_NEAR(network->get_slot(5000).world_transform[3].y, 4999.0F, 1e-2F);
    }

    TEST_F(ModelFileTest, AMeshNoNodeReferencesStillGetsASlot)
    {
        SceneSpec spec;
        spec.meshes = { { 1, false }, { 1, false } };
        SceneNode used;
        used.name = "used";
        used.mesh = 0;
        spec.roots.push_back(add_node(spec, used));
        const auto path = path_in_cwd("orphan.gltf");
        RoundTrip::write_bytes(path, scene_json(spec));

        IO::ModelReader reader;
        const auto network = network_from(reader, path);
        const auto meshes = reader.extract_meshes();

        ASSERT_NE(network, nullptr);
        size_t geometry = 0;
        for (const auto& slot : network->slots()) {
            geometry += slot.node ? 1U : 0U;
        }
        EXPECT_EQ(geometry, meshes.size());
    }

    struct CorruptCase {
        const char* name;
        std::function<void(SceneSpec&, std::string&)> corrupt;
    };

    class CorruptSceneTest
        : public ModelFileTest,
          public ::testing::WithParamInterface<CorruptCase> { };

    TEST_P(CorruptSceneTest, FailsToOpenOrYieldsAFiniteNetwork)
    {
        SceneSpec spec;
        spec.meshes = { { 1, false } };
        SceneNode parent;
        parent.name = "parent";
        SceneNode child;
        child.name = "child";
        child.mesh = 0;
        parent.children = { add_node(spec, child) };
        spec.roots.push_back(add_node(spec, parent));

        std::string json;
        GetParam().corrupt(spec, json);
        if (json.empty()) {
            json = scene_json(spec);
        }
        const auto path = path_in_cwd("corrupt.gltf");
        RoundTrip::write_bytes(path, json);

        IO::ModelReader reader;
        if (!reader.open(path)) {
            EXPECT_FALSE(reader.get_last_error().empty());
            return;
        }
        const auto network = reader.create_mesh_network();
        ASSERT_NE(network, nullptr);
        for (const auto& slot : network->slots()) {
            for (int c = 0; c < 4; ++c) {
                for (int r = 0; r < 4; ++r) {
                    EXPECT_TRUE(std::isfinite(slot.world_transform[c][r])) << slot.name;
                }
            }
            if (slot.parent_index) {
                EXPECT_LT(*slot.parent_index, slot.index);
            }
        }
    }

    INSTANTIATE_TEST_SUITE_P(
        Corruption,
        CorruptSceneTest,
        ::testing::Values(
            CorruptCase { "ChildIndexOutOfRange", [](SceneSpec& spec, std::string&) { spec.nodes[1].children.push_back(9999); } },
            CorruptCase { "MeshIndexOutOfRange", [](SceneSpec& spec, std::string&) { spec.nodes[0].mesh = 42; } },
            CorruptCase { "BufferShorterThanItsAccessor", [](SceneSpec& spec, std::string& json) {
                              json = scene_json(spec);
                              const auto at = json.rfind("\"}]}");
                              json.erase(at - 24, 24);
                          } },
            CorruptCase { "RootListIsEmpty", [](SceneSpec& spec, std::string&) { spec.roots.clear(); } },
            CorruptCase { "EmptyNodeArray", [](SceneSpec& spec, std::string&) {
                              spec.nodes.clear();
                              spec.roots.clear();
                          } }),
        [](const ::testing::TestParamInfo<CorruptCase>& info) { return std::string(info.param.name); });

}
}
