#include "gtest/gtest.h"
#include "../support/test_metadata.hpp"

#include "MayaFlux/Kakshya/Semantic/MeshAssemblyAccess.hpp"
#include "MayaFlux/Kakshya/Semantic/MeshAssemblyInsertion.hpp"
#include "MayaFlux/Portal/Graphics/GraphicsUtils.hpp"

#include <glm/gtc/matrix_transform.hpp>

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {
namespace {

    using Kakshya::MeshAssemblyAccess;
    using Kakshya::MeshAssemblyContainer;
    using Kakshya::MeshAssemblyInsertion;
    using Kakshya::MeshAssemblyRowKind;
    using Kakshya::ResidentFrames;
    using Portal::Graphics::PrimitiveTopology;

    glm::mat4 adapter_shift(float x)
    {
        return glm::translate(glm::mat4(1.0F), glm::vec3(x, 0.0F, 0.0F));
    }

    Kakshya::MeshData adapter_triangle()
    {
        return Kakshya::MeshData::from_positions(
            std::vector<glm::vec3> { { 0.0F, 0.0F, 0.0F }, { 1.0F, 0.0F, 0.0F }, { 0.0F, 1.0F, 0.0F } },
            std::vector<uint32_t> { 0, 1, 2 })
            .value();
    }

    Kakshya::SpatialData adapter_points()
    {
        Kakshya::SpatialData shot;
        shot.topology = PrimitiveTopology::POINT_LIST;
        shot.positions = { glm::vec3(0.0F), glm::vec3(1.0F) };
        return shot;
    }

    std::shared_ptr<const ResidentFrames<Kakshya::MeshData>> adapter_frames()
    {
        return ResidentFrames<Kakshya::MeshData>::create(
            *Kinesis::TimeSampling::uniform(1.0), std::vector<Kakshya::MeshData> { adapter_triangle(), adapter_triangle() });
    }

    using Rows = std::vector<uint64_t>;

    /**
     * Rows: 0 world, 1 car at x = 10 under it, 2 and 3 wheels at x = 1 and -1 under the car,
     * 4 a body under the car, 5 a set of points under the world, 6 another row called wheel_l
     * under the world.
     */
    struct AdapterLevel {
        MeshAssemblyContainer assembly;
        size_t wheel {};
        size_t body {};
        size_t dust {};

        AdapterLevel()
        {
            wheel = assembly.add_source({ .geometry = adapter_triangle(), .name = "wheel" });
            body = assembly.add_source({ .geometry = adapter_triangle(), .name = "body" });
            dust = assembly.add_source({ .geometry = adapter_points(), .name = "dust" });
            const auto rubber = assembly.add_material({ .name = "rubber" });

            const auto world = assembly.add_placement({ .name = "world" }).value();
            const auto car = assembly.add_placement({ .name = "car", .parent = world, .local = adapter_shift(10.0F) }).value();
            std::ignore = assembly.add_placement({ .name = "wheel_l", .parent = car, .source = wheel, .material = rubber, .local = adapter_shift(1.0F) });
            std::ignore = assembly.add_placement({ .name = "wheel_r", .parent = car, .source = wheel, .material = rubber, .local = adapter_shift(-1.0F) });
            std::ignore = assembly.add_placement({ .name = "body", .parent = car, .source = body });
            std::ignore = assembly.add_placement({ .name = "dust", .parent = world, .source = dust });
            std::ignore = assembly.add_placement({ .name = "wheel_l", .parent = world });
        }
    };

    TEST(AssemblyAccessTest, ItCountsWhatTheAssemblyHolds)
    {
        const AdapterLevel level;
        const MeshAssemblyAccess access(level.assembly);

        EXPECT_EQ(access.row_count(), 7U);
        EXPECT_EQ(access.source_count(), 3U);
        EXPECT_EQ(access.material_count(), 1U);
        EXPECT_EQ(access.motion_count(), 0U);
    }

    TEST(AssemblyAccessTest, ARowComesWithItsPlacementAndWhatItShows)
    {
        const AdapterLevel level;
        const MeshAssemblyAccess access(level.assembly);

        const auto wheel = access.row(2);
        ASSERT_TRUE(wheel.has_value());
        EXPECT_EQ(wheel->index, 2U);
        EXPECT_EQ(wheel->placement.name, "wheel_l");
        EXPECT_EQ(wheel->kind, MeshAssemblyRowKind::Mesh);
        EXPECT_EQ(access.row(0)->kind, MeshAssemblyRowKind::Joint);
        EXPECT_EQ(access.row(5)->kind, MeshAssemblyRowKind::Points);
        EXPECT_FALSE(access.row(99).has_value());
    }

    TEST(AssemblyAccessTest, TheHierarchyIsIndexedOnce)
    {
        const AdapterLevel level;
        const MeshAssemblyAccess access(level.assembly);

        EXPECT_EQ(access.roots(), (Rows { 0 }));
        const auto kids = access.children(1);
        EXPECT_EQ(Rows(kids.begin(), kids.end()), (Rows { 2, 3, 4 }));
        EXPECT_TRUE(access.children(2).empty());
        EXPECT_TRUE(access.children(99).empty());
    }

    TEST(AssemblyAccessTest, ASubtreeIsTheRowAndItsDescendantsParentsFirst)
    {
        const AdapterLevel level;
        const MeshAssemblyAccess access(level.assembly);

        EXPECT_EQ(access.subtree(1), (Rows { 1, 2, 3, 4 }));
        EXPECT_EQ(access.subtree(0), (Rows { 0, 1, 2, 3, 4, 5, 6 }));
        EXPECT_EQ(access.subtree(2), (Rows { 2 }));
        EXPECT_TRUE(access.subtree(99).empty());
    }

    TEST(AssemblyAccessTest, AncestorsRunFromTheNearestParentToTheRoot)
    {
        const AdapterLevel level;
        const MeshAssemblyAccess access(level.assembly);

        EXPECT_EQ(access.ancestors(2), (Rows { 1, 0 }));
        EXPECT_TRUE(access.ancestors(0).empty());
        EXPECT_TRUE(access.ancestors(99).empty());
        EXPECT_EQ(access.depth(2), 2U);
        EXPECT_EQ(access.depth(0), 0U);
    }

    TEST(AssemblyAccessTest, RowsAreFoundByNameAndByPath)
    {
        const AdapterLevel level;
        const MeshAssemblyAccess access(level.assembly);

        EXPECT_EQ(access.path(2), "world/car/wheel_l");
        EXPECT_EQ(access.path(0), "world");
        EXPECT_TRUE(access.path(99).empty());
        EXPECT_EQ(access.find("wheel_l"), std::optional<uint64_t> { 2 });
        EXPECT_EQ(access.find_all("wheel_l"), (Rows { 2, 6 }));
        EXPECT_EQ(access.find_path("world/wheel_l"), std::optional<uint64_t> { 6 });
        EXPECT_EQ(access.find_path("world/car/wheel_l"), std::optional<uint64_t> { 2 });
        EXPECT_EQ(access.find("missing"), std::nullopt);
        EXPECT_EQ(access.find_path("world/missing"), std::nullopt);
    }

    TEST(AssemblyAccessTest, RowsAreListedBySourceAndByKind)
    {
        const AdapterLevel level;
        const MeshAssemblyAccess access(level.assembly);

        EXPECT_EQ(access.rows_of_source(level.wheel), (Rows { 2, 3 }));
        EXPECT_EQ(access.rows_of_kind(MeshAssemblyRowKind::Mesh), (Rows { 2, 3, 4 }));
        EXPECT_EQ(access.rows_of_kind(MeshAssemblyRowKind::Points), (Rows { 5 }));
        EXPECT_EQ(access.rows_of_kind(MeshAssemblyRowKind::Joint), (Rows { 0, 1, 6 }));
        EXPECT_EQ(access.source_of(2), std::optional<uint64_t> { level.wheel });
        EXPECT_EQ(access.source_of(1), std::nullopt);
    }

    TEST(AssemblyAccessTest, TheWorldPlacementOfARowComposesItsParents)
    {
        const AdapterLevel level;
        const MeshAssemblyAccess access(level.assembly);

        ASSERT_TRUE(access.world(2).has_value());
        EXPECT_FLOAT_EQ((*access.world(2))[3].x, 11.0F);
        EXPECT_FLOAT_EQ((*access.world(3))[3].x, 9.0F);
        EXPECT_FALSE(access.world(99).has_value());
    }

    TEST(AssemblyAccessTest, SourcesMaterialsAndMotionsAreReadByIndex)
    {
        AdapterLevel level;
        const auto times = *Kinesis::TimeSampling::acyclic({ 0.0, 1.0 });
        auto slide = Kinesis::TimedValues<glm::vec3>::create({ .times = times, .values = { glm::vec3(0.0F), glm::vec3(1.0F, 0.0F, 0.0F) } });
        ASSERT_TRUE(level.assembly.add_motion({ .name = "slide", .channels = { { .row = 1, .translation = slide } } }).has_value());
        const MeshAssemblyAccess access(level.assembly);

        ASSERT_NE(access.source(level.wheel), nullptr);
        EXPECT_EQ(access.source(level.wheel)->name, "wheel");
        EXPECT_EQ(access.source(99), nullptr);
        ASSERT_NE(access.material(0), nullptr);
        EXPECT_EQ(access.material(0)->name, "rubber");
        EXPECT_EQ(access.material(5), nullptr);
        ASSERT_NE(access.motion(0), nullptr);
        EXPECT_EQ(access.motion(0)->name, "slide");
        EXPECT_NE(access.channel(0, 1), nullptr);
        EXPECT_EQ(access.channel(0, 2), nullptr);
        EXPECT_EQ(access.channel(5, 1), nullptr);
    }

    TEST(AssemblyAccessTest, AMeshSourceIsAViewOverItsTrianglesAndPointsAreNot)
    {
        const AdapterLevel level;
        const MeshAssemblyAccess access(level.assembly);

        const auto mesh = access.mesh(level.wheel);
        ASSERT_TRUE(mesh.has_value());
        EXPECT_EQ(mesh->vertex_count(), 3U);
        EXPECT_EQ(mesh->face_count(), 1U);
        EXPECT_FALSE(access.mesh(level.dust).has_value());
        ASSERT_NE(access.spatial(level.dust), nullptr);
        EXPECT_EQ(access.spatial(level.dust)->positions.size(), 2U);
        EXPECT_EQ(access.spatial(level.wheel), nullptr);
    }

    TEST(AssemblyAccessTest, ASourceWithFramesAnimates)
    {
        AdapterLevel level;
        ASSERT_TRUE(level.assembly.set_mesh_frames(level.body, adapter_frames()));
        const MeshAssemblyAccess access(level.assembly);

        EXPECT_TRUE(access.animates(level.body));
        EXPECT_FALSE(access.animates(level.wheel));
    }

    TEST(AssemblyAccessTest, AnEmptyAssemblyHasNothingToAnswer)
    {
        const MeshAssemblyContainer assembly;
        const MeshAssemblyAccess access(assembly);

        EXPECT_EQ(access.row_count(), 0U);
        EXPECT_TRUE(access.roots().empty());
        EXPECT_EQ(access.find("x"), std::nullopt);
    }

    TEST(AssemblyInsertionTest, SourcesAreAddedWithWhatTheFileSaidOfThem)
    {
        MeshAssemblyContainer assembly;
        MeshAssemblyInsertion put(assembly);
        const auto times = *Kinesis::TimeSampling::uniform(0.5);

        const auto mesh = put.add_mesh(adapter_triangle(), "tri", times, Kakshya::TopologyVariance::Homogeneous);
        const auto points = put.add_points(adapter_points(), "pts");

        EXPECT_EQ(mesh, 0U);
        EXPECT_EQ(points, 1U);
        EXPECT_EQ(assembly.get_data().sources[0].name, "tri");
        EXPECT_TRUE(assembly.get_data().sources[0].time.has_value());
        EXPECT_EQ(assembly.get_data().sources[0].variance, Kakshya::TopologyVariance::Homogeneous);
        EXPECT_NE(assembly.spatial(points), nullptr);
    }

    TEST(AssemblyInsertionTest, JointsAndPlacesAreAddedUnderParentsThatExist)
    {
        MeshAssemblyContainer assembly;
        MeshAssemblyInsertion put(assembly);
        const auto source = put.add_mesh(adapter_triangle(), "tri");

        const auto root = put.add_node("root");
        ASSERT_TRUE(root.has_value());
        const auto child = put.add_instance("child", source, root, adapter_shift(2.0F));
        ASSERT_TRUE(child.has_value());

        EXPECT_FALSE(put.add_node("orphan", 40).has_value());
        EXPECT_FALSE(put.add_instance("bad", 40, root).has_value());
        EXPECT_FALSE(put.add_instance("bad", source, root, glm::mat4(1.0F), 40).has_value());
        EXPECT_EQ(assembly.get_total_elements(), 2U);
        EXPECT_FLOAT_EQ(assembly.world_transforms()[*child][3].x, 2.0F);
    }

    TEST(AssemblyInsertionTest, OneSourceIsShownInManyPlacesAtOnce)
    {
        MeshAssemblyContainer assembly;
        MeshAssemblyInsertion put(assembly);
        const auto source = put.add_mesh(adapter_triangle(), "wheel");
        const auto car = put.add_node("car", std::nullopt, adapter_shift(10.0F)).value();
        const std::array placements { adapter_shift(1.0F), adapter_shift(-1.0F), adapter_shift(3.0F) };

        const auto rows = put.instance(source, placements, car, "wheel");

        ASSERT_EQ(rows.size(), 3U);
        EXPECT_EQ(assembly.placement(rows[0])->name, "wheel_0");
        EXPECT_EQ(assembly.placement(rows[2])->name, "wheel_2");
        EXPECT_FLOAT_EQ(assembly.world_transforms()[rows[1]][3].x, 9.0F);
        EXPECT_EQ(assembly.groups().size(), 1U);
        EXPECT_EQ(assembly.groups()[0].rows.size(), 3U);
    }

    TEST(AssemblyInsertionTest, ABadInstanceAddsNoRows)
    {
        MeshAssemblyContainer assembly;
        MeshAssemblyInsertion put(assembly);
        const auto source = put.add_mesh(adapter_triangle(), "wheel");
        const std::array placements { adapter_shift(1.0F), adapter_shift(2.0F) };

        EXPECT_TRUE(put.instance(source + 5, placements).empty());
        EXPECT_TRUE(put.instance(source, placements, 9).empty());
        EXPECT_TRUE(put.instance(source, placements, std::nullopt, "x", 9).empty());
        EXPECT_EQ(assembly.get_total_elements(), 0U);
    }

    /** A prop: a joint at x = 2 holding a mesh at x = 1, a material, a motion on the joint and frames for the mesh. */
    struct AdapterProp {
        MeshAssemblyContainer assembly;
        std::shared_ptr<const ResidentFrames<Kakshya::MeshData>> frames = adapter_frames();

        AdapterProp()
        {
            MeshAssemblyInsertion put(assembly);
            const auto mesh = put.add_mesh(adapter_triangle(), "gem");
            const auto stone = assembly.add_material({ .name = "stone" });
            const auto joint = put.add_node("joint", std::nullopt, adapter_shift(2.0F)).value();
            std::ignore = put.add_instance("gem", mesh, joint, adapter_shift(1.0F), stone);
            std::ignore = assembly.set_mesh_frames(mesh, frames);
            const auto times = *Kinesis::TimeSampling::acyclic({ 0.0, 1.0 });
            auto spin = Kinesis::TimedValues<glm::vec3>::create({ .times = times, .values = { glm::vec3(0.0F), glm::vec3(1.0F, 0.0F, 0.0F) } });
            std::ignore = assembly.add_motion({ .name = "spin", .channels = { { .row = joint, .translation = spin } } });
        }
    };

    TEST(AssemblyInsertionTest, AnotherAssemblyIsGraftedUnderARowInsideAJointOfItsOwn)
    {
        AdapterLevel level;
        const AdapterProp prop;
        MeshAssemblyInsertion put(level.assembly);
        const size_t rows_before = level.assembly.get_total_elements();

        const auto graft = put.graft(prop.assembly, 1, adapter_shift(5.0F), "prop");

        ASSERT_TRUE(graft.has_value());
        ASSERT_TRUE(graft->joint.has_value());
        ASSERT_EQ(graft->rows.size(), 2U);
        EXPECT_EQ(*graft->joint, rows_before);
        EXPECT_EQ(level.assembly.placement(*graft->joint)->name, "prop");
        EXPECT_EQ(level.assembly.placement(graft->rows[0])->parent, graft->joint);
        EXPECT_EQ(level.assembly.placement(graft->rows[1])->parent, std::optional<uint64_t> { graft->rows[0] });
        EXPECT_FLOAT_EQ(level.assembly.world_transforms()[graft->rows[1]][3].x, 10.0F + 5.0F + 2.0F + 1.0F);
    }

    TEST(AssemblyInsertionTest, WithNoJointTheGraftPlacementIsComposedIntoItsRoots)
    {
        AdapterLevel level;
        const AdapterProp prop;
        MeshAssemblyInsertion put(level.assembly);

        const auto graft = put.graft(prop.assembly, 1, adapter_shift(5.0F));

        ASSERT_TRUE(graft.has_value());
        EXPECT_FALSE(graft->joint.has_value());
        EXPECT_EQ(level.assembly.placement(graft->rows[0])->parent, std::optional<uint64_t> { 1 });
        EXPECT_FLOAT_EQ(level.assembly.world_transforms()[graft->rows[1]][3].x, 10.0F + 5.0F + 2.0F + 1.0F);
    }

    TEST(AssemblyInsertionTest, SourcesMaterialsMotionsAndFramesFollowTheGraftedRows)
    {
        AdapterLevel level;
        const AdapterProp prop;
        MeshAssemblyInsertion put(level.assembly);

        const auto graft = put.graft(prop.assembly, std::nullopt, glm::mat4(1.0F), "prop");

        ASSERT_TRUE(graft.has_value());
        ASSERT_EQ(graft->sources.size(), 1U);
        ASSERT_EQ(graft->materials.size(), 1U);
        ASSERT_EQ(graft->motions.size(), 1U);
        EXPECT_EQ(level.assembly.get_data().sources[graft->sources[0]].name, "gem");
        EXPECT_EQ(level.assembly.get_data().materials[graft->materials[0]].name, "stone");
        EXPECT_EQ(level.assembly.placement(graft->rows[1])->source, std::optional<uint64_t> { graft->sources[0] });
        EXPECT_EQ(level.assembly.placement(graft->rows[1])->material, std::optional<uint64_t> { graft->materials[0] });
        EXPECT_EQ(level.assembly.mesh_frames(graft->sources[0]), prop.frames);
        ASSERT_EQ(level.assembly.get_data().motions[graft->motions[0]].channels.size(), 1U);
        EXPECT_EQ(level.assembly.get_data().motions[graft->motions[0]].channels[0].row, graft->rows[0]);
    }

    TEST(AssemblyInsertionTest, TheSameAssemblyMayBeGraftedMoreThanOnce)
    {
        AdapterLevel level;
        const AdapterProp prop;
        MeshAssemblyInsertion put(level.assembly);

        const auto first = put.graft(prop.assembly, std::nullopt, adapter_shift(1.0F), "a");
        const auto second = put.graft(prop.assembly, std::nullopt, adapter_shift(2.0F), "b");

        ASSERT_TRUE(first.has_value());
        ASSERT_TRUE(second.has_value());
        EXPECT_NE(first->sources[0], second->sources[0]);
        EXPECT_NE(first->rows[0], second->rows[0]);
        EXPECT_EQ(level.assembly.get_data().motions.size(), 2U);
    }

    TEST(AssemblyInsertionTest, AParentThatIsNotThereGraftsNothing)
    {
        AdapterLevel level;
        const AdapterProp prop;
        MeshAssemblyInsertion put(level.assembly);
        const size_t rows = level.assembly.get_total_elements();
        const size_t sources = level.assembly.get_data().sources.size();

        EXPECT_FALSE(put.graft(prop.assembly, 99).has_value());
        EXPECT_EQ(level.assembly.get_total_elements(), rows);
        EXPECT_EQ(level.assembly.get_data().sources.size(), sources);
    }

    TEST(AssemblyInsertionTest, WhatWasInsertedIsReadBackThroughTheAccess)
    {
        MeshAssemblyContainer assembly;
        MeshAssemblyInsertion put(assembly);
        const auto source = put.add_mesh(adapter_triangle(), "wheel");
        const auto car = put.add_node("car").value();
        const std::array placements { adapter_shift(1.0F), adapter_shift(-1.0F) };
        std::ignore = put.instance(source, placements, car, "wheel");

        const MeshAssemblyAccess access(assembly);

        EXPECT_EQ(access.find_path("car/wheel_1").has_value(), true);
        EXPECT_EQ(access.rows_of_source(source), (Rows { 1, 2 }));
        EXPECT_EQ(access.children(car).size(), 2U);
    }

} // namespace
} // namespace MayaFlux::Test
