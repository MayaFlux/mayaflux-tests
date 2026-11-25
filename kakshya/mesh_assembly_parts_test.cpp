#include "gtest/gtest.h"
#include "../support/test_metadata.hpp"

#include "MayaFlux/Kakshya/Semantic/MeshAssemblyMotionFrames.hpp"
#include "MayaFlux/Kakshya/Semantic/MeshAssemblyParts.hpp"
#include "MayaFlux/Portal/Graphics/GraphicsUtils.hpp"

#include <glm/gtc/matrix_transform.hpp>

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {
namespace {

    using Kakshya::MeshAssemblyContainer;
    using Kakshya::MeshAssemblyPart;
    using Kakshya::MeshAssemblyPartContext;
    using Kakshya::MeshAssemblyPartKey;
    using Kakshya::MeshAssemblyRowKind;
    using Kakshya::partition_assembly;
    using Kakshya::ResidentFrames;
    using Portal::Graphics::PrimitiveTopology;
    namespace Partitions = Kakshya::Partitions;

    glm::mat4 slid(float x)
    {
        return glm::translate(glm::mat4(1.0F), glm::vec3(x, 0.0F, 0.0F));
    }

    Kakshya::MeshData part_triangle()
    {
        return Kakshya::MeshData::from_positions(
            std::vector<glm::vec3> { { 0.0F, 0.0F, 0.0F }, { 1.0F, 0.0F, 0.0F }, { 0.0F, 1.0F, 0.0F } },
            std::vector<uint32_t> { 0, 1, 2 })
            .value();
    }

    std::shared_ptr<const ResidentFrames<Kakshya::MeshData>> part_mesh_frames()
    {
        return ResidentFrames<Kakshya::MeshData>::create(
            *Kinesis::TimeSampling::uniform(1.0), std::vector<Kakshya::MeshData> { part_triangle(), part_triangle() });
    }

    std::shared_ptr<const ResidentFrames<Kakshya::SpatialData>> part_spatial_frames()
    {
        Kakshya::SpatialData shot;
        shot.topology = PrimitiveTopology::POINT_LIST;
        shot.positions = { glm::vec3(0.0F), glm::vec3(1.0F) };
        return ResidentFrames<Kakshya::SpatialData>::create(
            *Kinesis::TimeSampling::uniform(1.0), std::vector<Kakshya::SpatialData> { shot, shot });
    }

    /**
     * A level: row 0 a joint at x = 10, row 1 a wall of stone, rows 2 and 3 two wheels of
     * rubber sharing a mesh, row 4 a set of points and row 5 a curve.
     */
    struct PartLevel {
        std::shared_ptr<MeshAssemblyContainer> assembly = std::make_shared<MeshAssemblyContainer>();
        size_t wall {};
        size_t wheel {};
        size_t dust {};
        size_t rail {};

        PartLevel()
        {
            wall = assembly->add_source({ .geometry = part_triangle(), .name = "wall" });
            wheel = assembly->add_source({ .geometry = part_triangle(), .name = "wheel" });

            Kakshya::SpatialData points;
            points.topology = PrimitiveTopology::POINT_LIST;
            points.positions = { glm::vec3(0.0F), glm::vec3(1.0F), glm::vec3(2.0F) };
            dust = assembly->add_source({ .geometry = points, .name = "dust" });

            Kakshya::SpatialData curve;
            curve.topology = PrimitiveTopology::LINE_STRIP;
            curve.positions = { glm::vec3(0.0F), glm::vec3(1.0F), glm::vec3(2.0F) };
            curve.vertex_counts_per_curve = { 3 };
            rail = assembly->add_source({ .geometry = curve, .name = "rail" });

            const auto stone = assembly->add_material({ .name = "stone" });
            const auto rubber = assembly->add_material({ .name = "rubber" });

            const auto level = assembly->add_placement({ .name = "level", .local = slid(10.0F) }).value();
            std::ignore = assembly->add_placement({ .name = "wall", .parent = level, .source = wall, .material = stone });
            std::ignore = assembly->add_placement({ .name = "left", .parent = level, .source = wheel, .material = rubber, .local = slid(1.0F) });
            std::ignore = assembly->add_placement({ .name = "right", .parent = level, .source = wheel, .material = rubber, .local = slid(-1.0F) });
            std::ignore = assembly->add_placement({ .name = "dust", .source = dust });
            std::ignore = assembly->add_placement({ .name = "rail", .source = rail });
        }
    };

    using Rows = std::vector<uint64_t>;

    TEST(MeshAssemblyPartsTest, ARowIsKnownByWhatItShows)
    {
        const PartLevel level;

        EXPECT_EQ(row_kind(*level.assembly, 0), MeshAssemblyRowKind::Joint);
        EXPECT_EQ(row_kind(*level.assembly, 1), MeshAssemblyRowKind::Mesh);
        EXPECT_EQ(row_kind(*level.assembly, 4), MeshAssemblyRowKind::Points);
        EXPECT_EQ(row_kind(*level.assembly, 5), MeshAssemblyRowKind::Curves);
        EXPECT_EQ(row_kind(*level.assembly, 99), MeshAssemblyRowKind::Joint);
    }

    TEST(MeshAssemblyPartsTest, WithNoPartitionEachKindIsOnePartAndJointsAreInNone)
    {
        const PartLevel level;

        const auto parts = partition_assembly(*level.assembly);

        ASSERT_EQ(parts.size(), 3U);
        EXPECT_EQ(parts[0].kind, MeshAssemblyRowKind::Mesh);
        EXPECT_EQ(parts[0].rows, (Rows { 1, 2, 3 }));
        EXPECT_EQ(parts[1].kind, MeshAssemblyRowKind::Points);
        EXPECT_EQ(parts[1].rows, (Rows { 4 }));
        EXPECT_EQ(parts[2].kind, MeshAssemblyRowKind::Curves);
        EXPECT_EQ(parts[2].rows, (Rows { 5 }));
    }

    TEST(MeshAssemblyPartsTest, AnEmptyAssemblyHasNoParts)
    {
        const MeshAssemblyContainer assembly;

        EXPECT_TRUE(partition_assembly(assembly).empty());
    }

    TEST(MeshAssemblyPartsTest, MaterialsSplitTheMeshesAndLeaveTheRestAlone)
    {
        const PartLevel level;

        const auto parts = partition_assembly(*level.assembly, Partitions::by_material());

        ASSERT_EQ(parts.size(), 4U);
        EXPECT_EQ(parts[0].rows, (Rows { 1 }));
        EXPECT_EQ(parts[1].rows, (Rows { 2, 3 }));
        EXPECT_EQ(parts[2].kind, MeshAssemblyRowKind::Points);
        EXPECT_EQ(parts[3].kind, MeshAssemblyRowKind::Curves);
    }

    TEST(MeshAssemblyPartsTest, EverySourceCanHaveAPartOfItsOwn)
    {
        const PartLevel level;

        const auto parts = partition_assembly(*level.assembly, Partitions::by_source());

        ASSERT_EQ(parts.size(), 4U);
        EXPECT_EQ(parts[0].rows, (Rows { 1 }));
        EXPECT_EQ(parts[1].rows, (Rows { 2, 3 }));
    }

    TEST(MeshAssemblyPartsTest, GeometryThatHasFramesIsKeptApartFromGeometryThatNeverChanges)
    {
        const PartLevel level;
        ASSERT_TRUE(level.assembly->set_mesh_frames(level.wheel, part_mesh_frames()));
        ASSERT_TRUE(level.assembly->set_spatial_frames(level.dust, part_spatial_frames()));

        const auto parts = partition_assembly(*level.assembly, Partitions::by_geometry_change());

        ASSERT_EQ(parts.size(), 4U);
        EXPECT_EQ(parts[0].kind, MeshAssemblyRowKind::Mesh);
        EXPECT_EQ(parts[0].rows, (Rows { 1 }));
        EXPECT_EQ(parts[1].kind, MeshAssemblyRowKind::Mesh);
        EXPECT_EQ(parts[1].rows, (Rows { 2, 3 }));
        EXPECT_NE(parts[0].key, parts[1].key);
        EXPECT_EQ(parts[2].kind, MeshAssemblyRowKind::Points);
        EXPECT_NE(parts[2].key, parts[3].key);
    }

    TEST(MeshAssemblyPartsTest, MeshesShownInSeveralPlacesAreInstancedAndTheRestAreNot)
    {
        const PartLevel level;

        const auto parts = partition_assembly(*level.assembly, Partitions::instanced(2));

        ASSERT_EQ(parts.size(), 4U);
        EXPECT_EQ(parts[0].rows, (Rows { 1 }));
        EXPECT_FALSE(parts[0].key.instanced);
        EXPECT_EQ(parts[1].rows, (Rows { 2, 3 }));
        EXPECT_TRUE(parts[1].key.instanced);
        EXPECT_FALSE(parts[2].key.instanced);
        EXPECT_FALSE(parts[3].key.instanced);
    }

    TEST(MeshAssemblyPartsTest, AThresholdAboveTheGroupSizeInstancesNothing)
    {
        const PartLevel level;

        const auto parts = partition_assembly(*level.assembly, Partitions::instanced(3));

        ASSERT_EQ(parts.size(), 3U);
        EXPECT_EQ(parts[0].rows, (Rows { 1, 2, 3 }));
        EXPECT_FALSE(parts[0].key.instanced);
    }

    TEST(MeshAssemblyPartsTest, ThePartitionMarkOfInstancingIsKeptForMeshesAndPointsAndDroppedForCurves)
    {
        const PartLevel level;

        const auto parts = partition_assembly(*level.assembly, [](const MeshAssemblyPartContext&, size_t) {
            return MeshAssemblyPartKey { .key = 7, .instanced = true };
        });

        for (const auto& part : parts) {
            EXPECT_EQ(part.key.instanced, part.kind != MeshAssemblyRowKind::Curves);
        }
    }

    TEST(MeshAssemblyPartsTest, AnyCallableIsAPartitionAndPartsFollowTheirFirstRow)
    {
        const PartLevel level;

        const auto parts = partition_assembly(*level.assembly, [](const MeshAssemblyPartContext&, size_t row) {
            return MeshAssemblyPartKey { .key = row % 2 };
        });

        ASSERT_EQ(parts.size(), 4U);
        EXPECT_EQ(parts[0].kind, MeshAssemblyRowKind::Mesh);
        EXPECT_EQ(parts[0].rows, (Rows { 1, 3 }));
        EXPECT_EQ(parts[1].kind, MeshAssemblyRowKind::Mesh);
        EXPECT_EQ(parts[1].rows, (Rows { 2 }));
        EXPECT_EQ(parts[2].rows, (Rows { 4 }));
        EXPECT_EQ(parts[3].rows, (Rows { 5 }));
    }

    TEST(MeshAssemblyPartsTest, ThePartitionIsGivenTheGroupsOfTheAssemblyForEveryRow)
    {
        const PartLevel level;
        std::vector<std::optional<size_t>> seen;

        std::ignore = partition_assembly(*level.assembly, [&seen](const MeshAssemblyPartContext& context, size_t row) {
            EXPECT_EQ(context.group_of.size(), context.assembly.get_total_elements());
            seen.push_back(context.group_of[row]);
            return MeshAssemblyPartKey {};
        });

        ASSERT_EQ(seen.size(), 5U);
        EXPECT_TRUE(seen[0].has_value());
        EXPECT_EQ(seen[1], seen[2]);
        EXPECT_NE(seen[0], seen[1]);
    }

    TEST(MeshAssemblyPartsTest, ACameraIsAKindOfRowAndHasAPartOfItsOwn)
    {
        auto assembly = std::make_shared<MeshAssemblyContainer>();
        const auto lens = assembly->add_source({ .geometry = Kakshya::MeshAssemblyLens {}, .name = "lens" });
        const auto mesh = assembly->add_source({ .geometry = part_triangle(), .name = "tri" });
        std::ignore = assembly->add_placement({ .name = "camera", .source = lens });
        std::ignore = assembly->add_placement({ .name = "tri", .source = mesh });

        EXPECT_EQ(row_kind(*assembly, 0), MeshAssemblyRowKind::Camera);
        EXPECT_EQ(row_kind(*assembly, 1), MeshAssemblyRowKind::Mesh);
        EXPECT_NE(assembly->lens(lens), nullptr);
        EXPECT_EQ(assembly->mesh(lens), nullptr);
        EXPECT_EQ(assembly->lens(mesh), nullptr);

        const auto parts = partition_assembly(*assembly);
        ASSERT_EQ(parts.size(), 2U);
        EXPECT_EQ(parts[0].kind, MeshAssemblyRowKind::Camera);
        EXPECT_EQ(parts[0].rows, (Rows { 0 }));
    }

    TEST(MeshAssemblyPartsTest, ALightIsAKindOfRowAndIsInNoPart)
    {
        auto assembly = std::make_shared<MeshAssemblyContainer>();
        const auto light = assembly->add_source({ .geometry = Kakshya::MeshAssemblyLight {}, .name = "sun" });
        const auto mesh = assembly->add_source({ .geometry = part_triangle(), .name = "tri" });
        std::ignore = assembly->add_placement({ .name = "sun", .source = light });
        std::ignore = assembly->add_placement({ .name = "tri", .source = mesh });

        EXPECT_EQ(row_kind(*assembly, 0), MeshAssemblyRowKind::Light);
        EXPECT_NE(assembly->light(light), nullptr);
        EXPECT_EQ(assembly->light(mesh), nullptr);

        const auto parts = partition_assembly(*assembly);
        ASSERT_EQ(parts.size(), 1U);
        EXPECT_EQ(parts[0].kind, MeshAssemblyRowKind::Mesh);
        EXPECT_EQ(parts[0].rows, (Rows { 1 }));
    }

    TEST(MeshAssemblyFramesTest, ACameraSourceTakesLensFramesAndNothingElseDoes)
    {
        auto assembly = std::make_shared<MeshAssemblyContainer>();
        const auto lens = assembly->add_source({ .geometry = Kakshya::MeshAssemblyLens {}, .name = "lens" });
        const auto mesh = assembly->add_source({ .geometry = part_triangle(), .name = "tri" });
        const auto frames = ResidentFrames<Kakshya::MeshAssemblyLens>::create(*Kinesis::TimeSampling::uniform(1.0), std::vector<Kakshya::MeshAssemblyLens> { Kakshya::MeshAssemblyLens {}, Kakshya::MeshAssemblyLens {} });

        EXPECT_TRUE(assembly->set_lens_frames(lens, frames));
        EXPECT_FALSE(assembly->set_lens_frames(mesh, frames));
        EXPECT_FALSE(assembly->set_lens_frames(9, frames));
        EXPECT_EQ(assembly->lens_frames(lens), frames);
        EXPECT_EQ(assembly->lens_frames(mesh), nullptr);

        assembly->clear();
        EXPECT_EQ(assembly->lens_frames(lens), nullptr);
    }

    TEST(MeshAssemblyFramesTest, ASourceTakesFramesOfItsOwnKindOnly)
    {
        const PartLevel level;

        EXPECT_TRUE(level.assembly->set_mesh_frames(level.wall, part_mesh_frames()));
        EXPECT_FALSE(level.assembly->set_mesh_frames(level.dust, part_mesh_frames()));
        EXPECT_FALSE(level.assembly->set_mesh_frames(99, part_mesh_frames()));
        EXPECT_TRUE(level.assembly->set_spatial_frames(level.dust, part_spatial_frames()));
        EXPECT_FALSE(level.assembly->set_spatial_frames(level.wall, part_spatial_frames()));
        EXPECT_FALSE(level.assembly->set_spatial_frames(99, part_spatial_frames()));
    }

    TEST(MeshAssemblyFramesTest, TheFramesAreReadBackBySource)
    {
        const PartLevel level;
        const auto mesh_frames = part_mesh_frames();
        const auto spatial_frames = part_spatial_frames();
        ASSERT_TRUE(level.assembly->set_mesh_frames(level.wheel, mesh_frames));
        ASSERT_TRUE(level.assembly->set_spatial_frames(level.dust, spatial_frames, 9U));

        EXPECT_EQ(level.assembly->mesh_frames(level.wheel), mesh_frames);
        EXPECT_EQ(level.assembly->mesh_frames(level.wall), nullptr);
        EXPECT_EQ(level.assembly->mesh_frames(99), nullptr);
        EXPECT_EQ(level.assembly->spatial_frames(level.dust), spatial_frames);
        EXPECT_EQ(level.assembly->spatial_frames(level.rail), nullptr);
        EXPECT_EQ(level.assembly->spatial_max_vertices(level.dust), std::optional<uint32_t> { 9U });
        EXPECT_EQ(level.assembly->spatial_max_vertices(level.rail), std::nullopt);
    }

    TEST(MeshAssemblyFramesTest, ANullReferenceClearsTheFramesAndClearingTheAssemblyDropsThemAll)
    {
        const PartLevel level;
        ASSERT_TRUE(level.assembly->set_mesh_frames(level.wheel, part_mesh_frames()));
        ASSERT_TRUE(level.assembly->set_spatial_frames(level.dust, part_spatial_frames(), 3U));

        EXPECT_TRUE(level.assembly->set_mesh_frames(level.wheel, nullptr));
        EXPECT_EQ(level.assembly->mesh_frames(level.wheel), nullptr);

        level.assembly->clear();
        EXPECT_EQ(level.assembly->spatial_frames(level.dust), nullptr);
        EXPECT_EQ(level.assembly->spatial_max_vertices(level.dust), std::nullopt);
    }

    TEST(MeshAssemblyWorldTest, ResolvingComposesEachRowThroughItsParents)
    {
        const PartLevel level;
        auto locals = level.assembly->local_transforms();

        const auto frame = Kakshya::resolve_world(*level.assembly, locals);

        ASSERT_EQ(frame.matrices.size(), locals.size());
        ASSERT_EQ(frame.world.size(), locals.size());
        EXPECT_FLOAT_EQ(frame.world[2][3].x, 11.0F);
        EXPECT_FLOAT_EQ(frame.world[3][3].x, 9.0F);
        EXPECT_FLOAT_EQ(frame.world[4][3].x, 0.0F);
        EXPECT_FLOAT_EQ(frame.matrices[2][3].x, 1.0F);
    }

    TEST(MeshAssemblyWorldTest, LocalsOfTheWrongSizeResolveToNoWorld)
    {
        const PartLevel level;

        const auto frame = Kakshya::resolve_world(*level.assembly, std::vector<glm::mat4>(2, glm::mat4(1.0F)));

        EXPECT_TRUE(frame.world.empty());
        EXPECT_EQ(frame.matrices.size(), 2U);
    }

    TEST(MeshAssemblyWorldTest, AFrameOfAMotionCarriesTheWorldPlacementsToo)
    {
        const PartLevel level;
        const auto times = *Kinesis::TimeSampling::acyclic({ 0.0, 2.0 });
        auto slide = Kinesis::TimedValues<glm::vec3>::create({ .times = times,
            .values = { glm::vec3(0.0F), glm::vec3(4.0F, 0.0F, 0.0F) } });
        ASSERT_TRUE(level.assembly->add_motion({ .name = "slide", .channels = { { .row = 0, .translation = slide } } }).has_value());

        const auto frames = Kakshya::MeshAssemblyMotionFrames::create(level.assembly, 0);
        ASSERT_NE(frames, nullptr);
        const auto frame = frames->sample(1.0);

        ASSERT_NE(frame, nullptr);
        ASSERT_EQ(frame->world.size(), frame->matrices.size());
        EXPECT_FLOAT_EQ(frame->matrices[0][3].x, 2.0F);
        EXPECT_FLOAT_EQ(frame->world[2][3].x, 3.0F);
        EXPECT_FLOAT_EQ(frame->world[3][3].x, 1.0F);
    }

} // namespace
} // namespace MayaFlux::Test
