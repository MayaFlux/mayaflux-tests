#include "gtest/gtest.h"
#include "../support/test_metadata.hpp"

#include "../support/io_roundtrip_support.hpp"
#include "../support/scoped_working_dir.hpp"

#include "MayaFlux/IO/Model/AlembicModelWriter.hpp"
#include "MayaFlux/IO/Spatial/SpatialReader.hpp"
#include "MayaFlux/Kakshya/NDData/VertexFormats.hpp"
#include "MayaFlux/Kakshya/Semantic/MeshAssemblyContainer.hpp"
#include "MayaFlux/Portal/Graphics/GraphicsUtils.hpp"

#include <glm/gtc/matrix_transform.hpp>

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::writable_temp>() }))

namespace MayaFlux::Test {
namespace {

    using Kakshya::MeshAssemblyContainer;
    using Kakshya::MeshData;
    using Kakshya::MeshVertex;
    using RoundTrip::path_in_cwd;

    Kakshya::MeshAssemblySource source_of(auto geometry, const std::string& name)
    {
        Kakshya::MeshAssemblySource source;
        source.geometry = std::move(geometry);
        source.name = name;
        return source;
    }

    std::vector<MeshVertex> vertices_of(const MeshData& mesh)
    {
        const auto& bytes = std::get<std::vector<uint8_t>>(mesh.vertex_variant);
        std::vector<MeshVertex> out(bytes.size() / sizeof(MeshVertex));
        std::memcpy(out.data(), bytes.data(), out.size() * sizeof(MeshVertex));
        return out;
    }

    /** Two quads sharing an edge, one vertex per point, with a uv and colour on each. */
    MeshData two_quads(float lift = 0.0F)
    {
        std::vector<MeshVertex> vertices(6);
        const std::array<glm::vec3, 6> positions { glm::vec3(0, 0, 0), glm::vec3(1, 0, 0), glm::vec3(2, 0, 0),
            glm::vec3(0, 1, 0), glm::vec3(1, 1, 0), glm::vec3(2, 1, 0) };
        for (size_t v = 0; v < vertices.size(); ++v) {
            vertices[v].position = positions[v] + glm::vec3(0.0F, 0.0F, lift);
            vertices[v].uv = glm::vec2(positions[v].x * 0.5F, positions[v].y);
            vertices[v].color = glm::vec4(positions[v].y, 0.25F, 0.5F, 1.0F);
            vertices[v].weight = static_cast<float>(v) * 0.1F;
        }
        return *MeshData::from_vertices(vertices, Kakshya::FaceList { .sizes = { 4, 4 }, .corners = { 0, 1, 4, 3, 1, 2, 5, 4 } });
    }

    class AlembicModelWriterTest : public ::testing::Test {
    protected:
        ScopedWorkingDir m_files;
        IO::AlembicModelWriter m_writer;
        IO::SpatialReader m_reader;

        std::shared_ptr<MeshAssemblyContainer> round_trip(const std::shared_ptr<MeshAssemblyContainer>& assembly)
        {
            const auto path = path_in_cwd("assembly.abc");
            if (!m_writer.write(path, std::shared_ptr<const MeshAssemblyContainer>(assembly))) {
                ADD_FAILURE() << m_writer.get_last_error();
                return nullptr;
            }
            if (!m_reader.open(path)) {
                ADD_FAILURE() << m_reader.get_last_error();
                return nullptr;
            }
            return m_reader.create_mesh_assembly();
        }
    };

} // namespace

TEST_F(AlembicModelWriterTest, OnlyAbcIsClaimed)
{
    EXPECT_TRUE(m_writer.can_write("a.abc"));
    EXPECT_TRUE(m_writer.can_write("a.ABC"));
    EXPECT_FALSE(m_writer.can_write("a.fbx"));
}

TEST_F(AlembicModelWriterTest, HierarchyAndMotionComeBack)
{
    auto assembly = std::make_shared<MeshAssemblyContainer>();
    const auto mesh = assembly->add_source(source_of(two_quads(), "plate"));
    const auto rig = assembly->add_placement({ .name = "rig" });
    const auto arm = assembly->add_placement(
        { .name = "arm", .parent = rig, .local = glm::translate(glm::mat4(1.0F), glm::vec3(1.0F, 0.0F, 0.0F)) });
    ASSERT_TRUE(assembly->add_placement({ .name = "plate", .parent = arm, .source = mesh }));

    Kakshya::MeshAssemblyChannel channel { .row = *arm };
    channel.translation = Kinesis::TimedValues<glm::vec3>::create(
        { .times = *Kinesis::TimeSampling::acyclic({ 0.0, 1.0 }), .values = { glm::vec3(1, 0, 0), glm::vec3(3, 0, 0) } });
    ASSERT_TRUE(assembly->add_motion({ .name = "slide", .channels = { channel } }));

    const auto read = round_trip(assembly);
    ASSERT_NE(read, nullptr);
    ASSERT_EQ(read->get_total_elements(), 3U);
    EXPECT_EQ(read->placement(0)->name, "rig");
    EXPECT_EQ(read->placement(1)->parent, std::optional<uint64_t> { 0 });
    EXPECT_EQ(read->placement(2)->name, "plate");
    EXPECT_TRUE(read->placement(2)->source.has_value());

    const auto& motions = read->get_data().motions;
    ASSERT_EQ(motions.size(), 1U);
    ASSERT_EQ(motions[0].channels.size(), 1U);
    const auto middle = motions[0].channels[0].translation->sample(0.5);
    ASSERT_TRUE(middle.has_value());
    EXPECT_NEAR(middle->x, 2.0F, 1.0e-5F);
}

TEST_F(AlembicModelWriterTest, PolygonsAndVertexDataComeBack)
{
    auto assembly = std::make_shared<MeshAssemblyContainer>();
    const auto written = two_quads();
    auto source = source_of(written, "plate");
    source.facets.set(Kakshya::MeshAssemblyVertexSets {
        .uvs = { std::vector<glm::vec2>(6, glm::vec2(0.25F, 0.75F)) },
        .colors = { std::vector<glm::vec4>(6, glm::vec4(0.0F, 1.0F, 0.0F, 1.0F)) } });
    source.facets.set(Kakshya::MeshAssemblyAttributes { .values = {
                                                            { .name = "heat",
                                                                .scope = Kakshya::SpatialScope::Varying,
                                                                .values = std::vector<float> { 0, 1, 2, 3, 4, 5 } },
                                                        } });
    const auto index = assembly->add_source(std::move(source));
    ASSERT_TRUE(assembly->add_placement({ .name = "plate", .source = index }));

    const auto read = round_trip(assembly);
    ASSERT_NE(read, nullptr);
    const auto* mesh = read->mesh(0);
    ASSERT_NE(mesh, nullptr);
    ASSERT_TRUE(mesh->faces.has_value());
    EXPECT_EQ(mesh->faces->sizes, written.faces->sizes);
    EXPECT_EQ(mesh->faces->corners, written.faces->corners);

    const auto expected = vertices_of(written);
    const auto actual = vertices_of(*mesh);
    ASSERT_EQ(actual.size(), expected.size());
    for (size_t v = 0; v < actual.size(); ++v) {
        EXPECT_EQ(actual[v].position, expected[v].position);
        EXPECT_EQ(actual[v].uv, expected[v].uv);
        EXPECT_EQ(actual[v].color, expected[v].color);
        EXPECT_EQ(actual[v].weight, expected[v].weight);
    }

    const auto& facets = read->get_data().sources[0].facets;
    const auto* sets = facets.get<Kakshya::MeshAssemblyVertexSets>();
    ASSERT_NE(sets, nullptr);
    ASSERT_EQ(sets->uvs.size(), 1U);
    ASSERT_EQ(sets->colors.size(), 1U);
    EXPECT_EQ(sets->colors[0][5], glm::vec4(0.0F, 1.0F, 0.0F, 1.0F));

    const auto* attributes = facets.get<Kakshya::MeshAssemblyAttributes>();
    ASSERT_NE(attributes, nullptr);
    ASSERT_EQ(attributes->values.size(), 1U);
    EXPECT_EQ(attributes->values[0].name, "heat");
    EXPECT_EQ(std::get<std::vector<float>>(attributes->values[0].values), (std::vector<float> { 0, 1, 2, 3, 4, 5 }));
}

TEST_F(AlembicModelWriterTest, SeamVerticesShareTheirPointInTheFile)
{
    std::vector<MeshVertex> vertices(6);
    const std::array<glm::vec3, 6> positions { glm::vec3(0, 0, 0), glm::vec3(1, 0, 0), glm::vec3(0, 1, 0),
        glm::vec3(0, 1, 0), glm::vec3(1, 0, 0), glm::vec3(1, 1, 0) };
    for (size_t v = 0; v < vertices.size(); ++v) {
        vertices[v].position = positions[v];
        vertices[v].uv = glm::vec2(static_cast<float>(v) * 0.1F, 0.5F);
    }
    auto source = source_of(*MeshData::from_vertices(vertices,
                                Kakshya::FaceList { .sizes = { 3, 3 }, .corners = { 0, 1, 2, 3, 4, 5 } }),
        "seamed");
    source.facets.set(Kakshya::MeshAssemblyPointMap { .of_vertex = { 0, 1, 2, 2, 1, 3 }, .point_count = 4 });

    auto assembly = std::make_shared<MeshAssemblyContainer>();
    const auto index = assembly->add_source(std::move(source));
    ASSERT_TRUE(assembly->add_placement({ .name = "seamed", .source = index }));

    const auto read = round_trip(assembly);
    ASSERT_NE(read, nullptr);
    const auto* map = read->get_data().sources[0].facets.get<Kakshya::MeshAssemblyPointMap>();
    ASSERT_NE(map, nullptr);
    EXPECT_EQ(map->point_count, 4U);

    const auto* mesh = read->mesh(0);
    ASSERT_NE(mesh, nullptr);
    const auto actual = vertices_of(*mesh);
    ASSERT_EQ(mesh->faces->corners.size(), 6U);
    for (size_t corner = 0; corner < 6; ++corner) {
        const auto& vertex = actual[mesh->faces->corners[corner]];
        EXPECT_EQ(vertex.position, positions[corner]);
        EXPECT_EQ(vertex.uv, vertices[corner].uv);
    }
}

TEST_F(AlembicModelWriterTest, MaterialsAndFaceSetsComeBack)
{
    auto assembly = std::make_shared<MeshAssemblyContainer>();

    Kakshya::MeshAssemblyMaterial material { .name = "paint" };
    material.facets.set(Kakshya::MeshAssemblySurface {
        .base_color = glm::vec4(0.5F, 0.25F, 0.125F, 1.0F), .metallic = 0.75F, .double_sided = true });
    material.facets.set(Kakshya::MeshAssemblyMaps {
        .base_color = Kakshya::MeshAssemblyTexture { .path = "paint.png", .wrap_u = Kakshya::MeshAssemblyWrap::Clamp } });
    material.facets.set(Kakshya::MeshAssemblyProperties {
        .entries = { { .key = "gloss", .value = std::vector<float> { 3.0F } } } });
    const auto painted = assembly->add_material(std::move(material));

    auto source = source_of(two_quads(), "plate");
    source.facets.set(Kakshya::MeshAssemblyFaceSets {
        .sets = { { .name = "right", .faces = { 1 }, .exclusive = true, .material = painted } } });
    const auto index = assembly->add_source(std::move(source));
    ASSERT_TRUE(assembly->add_placement({ .name = "plate", .source = index, .material = painted }));

    const auto read = round_trip(assembly);
    ASSERT_NE(read, nullptr);
    EXPECT_EQ(read->get_total_elements(), 1U);
    ASSERT_EQ(read->get_data().materials.size(), 1U);
    EXPECT_EQ(read->placement(0)->material, std::optional<uint64_t> { 0 });

    const auto& held = read->get_data().materials[0];
    EXPECT_EQ(held.name, "paint");
    EXPECT_EQ(held.diffuse_path, "paint.png");
    const auto* surface = held.facets.get<Kakshya::MeshAssemblySurface>();
    ASSERT_NE(surface, nullptr);
    EXPECT_EQ(surface->base_color, glm::vec4(0.5F, 0.25F, 0.125F, 1.0F));
    EXPECT_EQ(surface->metallic, 0.75F);
    EXPECT_TRUE(surface->double_sided);
    const auto* maps = held.facets.get<Kakshya::MeshAssemblyMaps>();
    ASSERT_NE(maps, nullptr);
    EXPECT_EQ(maps->base_color->wrap_u, Kakshya::MeshAssemblyWrap::Clamp);
    const auto* properties = held.facets.get<Kakshya::MeshAssemblyProperties>();
    ASSERT_NE(properties, nullptr);
    ASSERT_EQ(properties->entries.size(), 1U);
    EXPECT_EQ(properties->entries[0].key, "gloss");

    const auto* face_sets = read->get_data().sources[0].facets.get<Kakshya::MeshAssemblyFaceSets>();
    ASSERT_NE(face_sets, nullptr);
    ASSERT_EQ(face_sets->sets.size(), 1U);
    EXPECT_EQ(face_sets->sets[0].name, "right");
    EXPECT_EQ(face_sets->sets[0].faces, (std::vector<uint32_t> { 1 }));
    EXPECT_TRUE(face_sets->sets[0].exclusive);
    EXPECT_EQ(face_sets->sets[0].material, std::optional<uint64_t> { 0 });
}

TEST_F(AlembicModelWriterTest, ASubdivisionCageKeepsItsCreases)
{
    auto source = source_of(two_quads(), "cage");
    source.facets.set(Kakshya::MeshAssemblySubdivision {
        .crease_indices = { 1, 4 }, .crease_lengths = { 2 }, .crease_sharpness = { 2.5F } });

    auto assembly = std::make_shared<MeshAssemblyContainer>();
    const auto index = assembly->add_source(std::move(source));
    ASSERT_TRUE(assembly->add_placement({ .name = "cage", .source = index }));

    const auto read = round_trip(assembly);
    ASSERT_NE(read, nullptr);
    const auto* subdivision = read->get_data().sources[0].facets.get<Kakshya::MeshAssemblySubdivision>();
    ASSERT_NE(subdivision, nullptr);
    EXPECT_EQ(subdivision->scheme, "catmull-clark");
    EXPECT_EQ(subdivision->crease_indices, (std::vector<int32_t> { 1, 4 }));
    EXPECT_EQ(subdivision->crease_sharpness, (std::vector<float> { 2.5F }));
}

TEST_F(AlembicModelWriterTest, CamerasAndLightsComeBack)
{
    auto assembly = std::make_shared<MeshAssemblyContainer>();
    Kakshya::MeshAssemblyLens lens;
    lens.focal_length = 50.0;
    lens.window = { .min = { -0.5F, -0.5F }, .max = { 1.5F, 0.75F } };
    lens.near = 0.5;
    lens.far = 250.0;
    lens.orthographic_width = 4.0;
    const auto camera = assembly->add_source(source_of(lens, "eye"));
    const auto light = assembly->add_source(source_of(Kakshya::MeshAssemblyLight {
                                                          .kind = Kakshya::MeshAssemblyLightKind::Spot,
                                                          .color = glm::vec3(1.0F, 0.5F, 0.25F),
                                                          .outer_cone = 0.8F },
        "lamp"));
    ASSERT_TRUE(assembly->add_placement({ .name = "eye", .source = camera }));
    ASSERT_TRUE(assembly->add_placement({ .name = "lamp", .source = light }));

    const auto read = round_trip(assembly);
    ASSERT_NE(read, nullptr);
    ASSERT_EQ(read->get_total_elements(), 2U);

    const auto* held = read->lens(*read->placement(0)->source);
    ASSERT_NE(held, nullptr);
    EXPECT_DOUBLE_EQ(held->focal_length, 50.0);
    EXPECT_NEAR(held->window.min.x, -0.5, 1.0e-9);
    EXPECT_NEAR(held->window.max.x, 1.5, 1.0e-9);
    EXPECT_NEAR(held->window.min.y, -0.5, 1.0e-9);
    EXPECT_NEAR(held->window.max.y, 0.75, 1.0e-9);
    EXPECT_DOUBLE_EQ(held->far, 250.0);
    EXPECT_DOUBLE_EQ(held->orthographic_width, 4.0);

    const auto* lamp = read->light(*read->placement(1)->source);
    ASSERT_NE(lamp, nullptr);
    EXPECT_EQ(lamp->kind, Kakshya::MeshAssemblyLightKind::Spot);
    EXPECT_EQ(lamp->color, glm::vec3(1.0F, 0.5F, 0.25F));
    EXPECT_EQ(lamp->outer_cone, 0.8F);
}

TEST_F(AlembicModelWriterTest, PointsAndCurvesKeepWidthsAndForm)
{
    auto assembly = std::make_shared<MeshAssemblyContainer>();
    Kakshya::SpatialData points;
    points.topology = Portal::Graphics::PrimitiveTopology::POINT_LIST;
    points.positions = { glm::vec3(0.0F), glm::vec3(1.0F) };
    points.ids = { 7, 9 };
    points.attributes = { { .name = "width", .scope = Kakshya::SpatialScope::Varying, .values = std::vector<float> { 0.1F, 0.2F } } };
    const auto dots = assembly->add_source(source_of(points, "dots"));

    Kakshya::SpatialData curve;
    curve.topology = Portal::Graphics::PrimitiveTopology::LINE_STRIP;
    curve.positions = { glm::vec3(0, 0, 0), glm::vec3(1, 1, 0), glm::vec3(2, 1, 0), glm::vec3(3, 0, 0) };
    curve.vertex_counts_per_curve = { 4 };
    auto bezier = source_of(curve, "arc");
    bezier.facets.set(Kakshya::MeshAssemblyCurveForm {
        .type = Kakshya::MeshAssemblyCurveType::Cubic, .basis = Kakshya::MeshAssemblyCurveBasis::Bezier });
    const auto arc = assembly->add_source(std::move(bezier));

    ASSERT_TRUE(assembly->add_placement({ .name = "dots", .source = dots }));
    ASSERT_TRUE(assembly->add_placement({ .name = "arc", .source = arc }));

    const auto read = round_trip(assembly);
    ASSERT_NE(read, nullptr);

    const auto* read_points = read->spatial(*read->placement(0)->source);
    ASSERT_NE(read_points, nullptr);
    EXPECT_EQ(read_points->ids, points.ids);
    ASSERT_EQ(read_points->attributes.size(), 1U);
    EXPECT_EQ(read_points->attributes[0].name, "width");

    const auto* form = read->get_data().sources[*read->placement(1)->source].facets.get<Kakshya::MeshAssemblyCurveForm>();
    ASSERT_NE(form, nullptr);
    EXPECT_EQ(form->type, Kakshya::MeshAssemblyCurveType::Cubic);
    EXPECT_EQ(form->basis, Kakshya::MeshAssemblyCurveBasis::Bezier);
}

TEST_F(AlembicModelWriterTest, ASharedSourceIsWrittenOnceAndInstanced)
{
    auto assembly = std::make_shared<MeshAssemblyContainer>();
    const auto mesh = assembly->add_source(source_of(two_quads(), "plate"));
    ASSERT_TRUE(assembly->add_placement({ .name = "first", .source = mesh }));
    ASSERT_TRUE(assembly->add_placement({ .name = "second", .source = mesh }));

    const auto read = round_trip(assembly);
    ASSERT_NE(read, nullptr);
    EXPECT_EQ(read->get_data().sources.size(), 1U);
    EXPECT_EQ(read->get_total_elements(), 2U);
    EXPECT_TRUE(std::ranges::any_of(m_reader.get_objects(),
        [](const auto& object) { return !object.instance_source.empty(); }));
}

TEST_F(AlembicModelWriterTest, MetadataAndVisibilityComeBack)
{
    auto assembly = std::make_shared<MeshAssemblyContainer>();
    ASSERT_TRUE(assembly->add_placement({ .name = "rig" }));

    Facets facets;
    facets.set(Kakshya::MeshAssemblyMetadata { .entries = {
                                                   { .row = {}, .key = "author", .value = std::string("someone") },
                                                   { .row = 0, .key = "tags/count", .value = int64_t { 3 } },
                                               } });
    facets.set(Kakshya::MeshAssemblyVisibility { .entries = { { .row = 0, .visible = false, .keys = {} } } });
    assembly->set_facets(std::move(facets));

    const auto read = round_trip(assembly);
    ASSERT_NE(read, nullptr);

    const auto* metadata = read->get_data().facets.get<Kakshya::MeshAssemblyMetadata>();
    ASSERT_NE(metadata, nullptr);
    ASSERT_EQ(metadata->entries.size(), 2U);
    EXPECT_FALSE(metadata->entries[0].row.has_value());
    EXPECT_EQ(metadata->entries[1].key, "tags/count");
    EXPECT_EQ(std::get<int64_t>(metadata->entries[1].value), 3);

    const auto* visibility = read->get_data().facets.get<Kakshya::MeshAssemblyVisibility>();
    ASSERT_NE(visibility, nullptr);
    ASSERT_EQ(visibility->entries.size(), 1U);
    EXPECT_FALSE(visibility->entries[0].visible);
}

TEST_F(AlembicModelWriterTest, MeshFramesAreWrittenAtTheirTimes)
{
    auto assembly = std::make_shared<MeshAssemblyContainer>();
    const auto mesh = assembly->add_source(source_of(two_quads(), "plate"));
    ASSERT_TRUE(assembly->add_placement({ .name = "plate", .source = mesh }));
    ASSERT_TRUE(assembly->set_mesh_frames(mesh,
        Kakshya::ResidentFrames<MeshData>::create(
            *Kinesis::TimeSampling::acyclic({ 0.0, 0.5 }), std::vector<MeshData> { two_quads(), two_quads(1.0F) })));

    ASSERT_NE(round_trip(assembly), nullptr);
    ASSERT_EQ(m_reader.get_mesh_sample_count("plate"), 2U);
    EXPECT_DOUBLE_EQ(m_reader.mesh_time("plate", 1).value_or(0.0), 0.5);
    const auto later = m_reader.read_mesh("plate", 1);
    ASSERT_TRUE(later.has_value());
    EXPECT_EQ(vertices_of(*later)[0].position.z, 1.0F);
}

} // namespace MayaFlux::Test
