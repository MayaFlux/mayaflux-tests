#include "gtest/gtest.h"
#include "../support/test_metadata.hpp"

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

    std::vector<MeshVertex> vertices_of(const MeshData& mesh)
    {
        const auto& bytes = std::get<std::vector<uint8_t>>(mesh.vertex_variant);
        std::vector<MeshVertex> vertices(bytes.size() / sizeof(MeshVertex));
        std::memcpy(vertices.data(), bytes.data(), vertices.size() * sizeof(MeshVertex));
        return vertices;
    }

    const std::vector<uint32_t>& indices_of(const MeshData& mesh)
    {
        return std::get<std::vector<uint32_t>>(mesh.index_variant);
    }

    size_t triangles_in(const std::vector<MeshData>& meshes)
    {
        size_t total = 0;
        for (const auto& mesh : meshes) {
            total += indices_of(mesh).size() / 3;
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
        EXPECT_TRUE(covers_positions(vertices_of(loaded.front()), vertices_of(source)));
        EXPECT_EQ(loaded.front().layout.stride_bytes, sizeof(MeshVertex));

        const auto [lo, hi] = bounds_of(vertices_of(loaded.front()));
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

    TEST_F(ModelFileTest, ObjRoundTripKeepsNormalsAndFlipsTheVerticalUvCoordinate)
    {
        const auto source = flat_cube("cube");
        const auto path = path_in_cwd("uv.obj");
        IO::AssimpModelWriter writer;
        ASSERT_TRUE(writer.write(path, { source })) << writer.get_last_error();

        IO::ModelReader reader;
        const auto loaded = reader.load(path);
        ASSERT_EQ(loaded.size(), 1U) << reader.get_last_error();

        const auto read_back = vertices_of(loaded.front());
        EXPECT_EQ(read_back.size(), 24U);
        for (const auto& original : vertices_of(source)) {
            const auto* match = find_vertex(read_back, original.position, &original.normal);
            ASSERT_NE(match, nullptr) << "no vertex at " << original.position.x << ' ' << original.position.y << ' ' << original.position.z;
            EXPECT_NEAR(match->uv.x, original.uv.x, 1e-4F);
            EXPECT_NEAR(match->uv.y, 1.0F - original.uv.y, 1e-4F);
        }
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

        const auto read_back = vertices_of(loaded.front());
        for (const auto& original : vertices_of(source)) {
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

        const auto read_back = vertices_of(loaded.front());
        for (const auto& original : vertices_of(source)) {
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
        EXPECT_EQ(indices_of(loaded.front()).size(), 6U);
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
            const auto [lo, hi] = bounds_of(vertices_of(mesh));
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

        EXPECT_FALSE(writer.write(path, {}));
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
        ASSERT_FALSE(writer.write(path_in_cwd("none.obj"), {}));
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
        EXPECT_EQ(vertices_of(via_extract.front()).size(), vertices_of(via_load.front()).size());
        EXPECT_EQ(indices_of(via_extract.front()), indices_of(via_load.front()));
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

    std::string two_node_gltf(bool shared_mesh)
    {
        const std::array<float, 9> triangle { 0, 0, 0, 1, 0, 0, 0, 1, 0 };
        const std::string payload(reinterpret_cast<const char*>(triangle.data()), sizeof(float) * triangle.size());

        std::string nodes = R"([{"name":"parent","translation":[0,5,0],"children":[1)";
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

    TEST_F(ModelFileTest, CurrentBehaviourExtractsVerticesWithoutTheNodeTransforms)
    {
        const auto path = path_in_cwd("graph_vertices.gltf");
        RoundTrip::write_bytes(path, two_node_gltf(false));

        IO::ModelReader reader;
        const auto loaded = reader.load(path);
        ASSERT_EQ(loaded.size(), 1U);

        const auto [lo, hi] = bounds_of(vertices_of(loaded.front()));
        EXPECT_TRUE(near(lo, glm::vec3(0.0F, 0.0F, 0.0F)));
        EXPECT_TRUE(near(hi, glm::vec3(1.0F, 1.0F, 0.0F)));
    }

    TEST_F(ModelFileTest, CurrentBehaviourBuildsOneIdentityRootSlotIgnoringTheNodeTree)
    {
        const auto path = path_in_cwd("graph_slots.gltf");
        RoundTrip::write_bytes(path, two_node_gltf(false));

        IO::ModelReader reader;
        ASSERT_TRUE(reader.open(path)) << reader.get_last_error();
        const auto network = reader.create_mesh_network();

        ASSERT_NE(network, nullptr);
        ASSERT_EQ(network->slot_count(), 1U);
        const auto& slot = network->get_slot(0);
        EXPECT_FALSE(slot.parent_index.has_value());
        EXPECT_EQ(slot.local_transform, glm::mat4(1.0F));
        EXPECT_EQ(slot.world_transform, glm::mat4(1.0F));
    }

    TEST_F(ModelFileTest, CurrentBehaviourBuildsOneSlotForAMeshUsedByTwoNodes)
    {
        const auto path = path_in_cwd("graph_shared.gltf");
        RoundTrip::write_bytes(path, two_node_gltf(true));

        IO::ModelReader reader;
        ASSERT_TRUE(reader.open(path)) << reader.get_last_error();
        const auto network = reader.create_mesh_network();

        ASSERT_NE(network, nullptr);
        EXPECT_EQ(network->slot_count(), 1U);
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
            const auto vertices = vertices_of(mesh);
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

    struct AssetCase {
        const char* relative;
    };

    class ModelAssetTest : public ::testing::TestWithParam<AssetCase> { };

    MAYAFLUX_TEST_WITH(TEST_P, ModelAssetTest, RealFilesLoadAsValidCanonicalMeshes,
        (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::repo_assets, ::MayaFlux::Test::Meta::Capability::writable_temp>() }))
    {
        const auto path = RoundTrip::repo_asset(GetParam().relative);
        if (!path) {
            GTEST_SKIP() << GetParam().relative << " is not present";
        }

        IO::ModelReader reader;
        const auto meshes = reader.load(*path);

        ASSERT_FALSE(meshes.empty()) << reader.get_last_error();
        for (const auto& mesh : meshes) {
            EXPECT_TRUE(mesh.is_valid());
            EXPECT_EQ(mesh.layout.stride_bytes, sizeof(MeshVertex));
            EXPECT_EQ(indices_of(mesh).size() % 3, 0U);
            const auto count = static_cast<uint32_t>(vertices_of(mesh).size());
            EXPECT_TRUE(std::ranges::all_of(indices_of(mesh), [count](uint32_t i) { return i < count; }));
            EXPECT_TRUE(std::ranges::all_of(vertices_of(mesh), [](const MeshVertex& v) {
                return std::isfinite(v.position.x) && std::isfinite(v.position.y) && std::isfinite(v.position.z);
            }));
        }
    }

    MAYAFLUX_TEST_WITH(TEST_P, ModelAssetTest, RealFilesBuildOneSlotPerExtractedMesh,
        (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::repo_assets, ::MayaFlux::Test::Meta::Capability::writable_temp>() }))
    {
        const auto path = RoundTrip::repo_asset(GetParam().relative);
        if (!path) {
            GTEST_SKIP() << GetParam().relative << " is not present";
        }

        IO::ModelReader reader;
        ASSERT_TRUE(reader.open(*path)) << reader.get_last_error();
        const auto meshes = reader.extract_meshes();
        const auto network = reader.create_mesh_network();

        ASSERT_NE(network, nullptr);
        EXPECT_EQ(network->slot_count(), meshes.size());
    }

    MAYAFLUX_TEST_WITH(TEST_P, ModelAssetTest, RealFilesReexportToObjWithTheSameTriangleCount,
        (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::repo_assets, ::MayaFlux::Test::Meta::Capability::writable_temp>() }))
    {
        const auto path = RoundTrip::repo_asset(GetParam().relative);
        if (!path) {
            GTEST_SKIP() << GetParam().relative << " is not present";
        }

        ScopedWorkingDir files;
        IO::ModelReader reader;
        const auto meshes = reader.load(*path);
        ASSERT_FALSE(meshes.empty());

        IO::AssimpModelWriter writer;
        const auto out = path_in_cwd("reexport.obj");
        ASSERT_TRUE(writer.write(out, meshes)) << writer.get_last_error();

        IO::ModelReader again;
        const auto reloaded = again.load(out);
        ASSERT_FALSE(reloaded.empty()) << again.get_last_error();
        EXPECT_EQ(triangles_in(reloaded), triangles_in(meshes));
    }

    INSTANTIATE_TEST_SUITE_P(
        Assets,
        ModelAssetTest,
        ::testing::Values(
            AssetCase { "res/fbx/Wolf.fbx" },
            AssetCase { "res/car.fbx" },
            AssetCase { "res/2.fbx" }),
        [](const ::testing::TestParamInfo<AssetCase>& info) {
            std::string name = info.param.relative;
            std::ranges::replace_if(name, [](char c) { return !std::isalnum(static_cast<unsigned char>(c)); }, '_');
            return name;
        });

}
}
