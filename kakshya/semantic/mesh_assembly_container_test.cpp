#include "gtest/gtest.h"
#include "../../support/test_metadata.hpp"

#include "MayaFlux/Kakshya/NDData/CompositeDescribe.hpp"
#include "MayaFlux/Kakshya/Region/RegionSegment.hpp"
#include "MayaFlux/Kakshya/Semantic/MeshAssemblyContainer.hpp"
#include "MayaFlux/Portal/Graphics/GraphicsUtils.hpp"
#include "MayaFlux/Kinesis/MatrixTransforms.hpp"

#include <glm/gtc/matrix_transform.hpp>

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {
namespace {

    using Kakshya::MeshAssemblyChannel;
    using Kakshya::MeshAssemblyClip;
    using Kakshya::MeshAssemblyContainer;
    using Kakshya::MeshAssemblyData;
    using Kakshya::MeshAssemblyMaterial;
    using Kakshya::MeshAssemblyPlacement;
    using Kakshya::MeshAssemblySource;
    using Kakshya::MeshData;
    using Kakshya::Region;
    using Kakshya::SpatialData;

    bool close(const glm::vec3& a, const glm::vec3& b, float tolerance = 1e-4F)
    {
        return glm::length(a - b) <= tolerance;
    }

    glm::mat4 moved(float x, float y = 0.0F, float z = 0.0F)
    {
        return glm::translate(glm::mat4(1.0F), glm::vec3(x, y, z));
    }

    MeshData triangle()
    {
        return MeshData::from_positions(
            std::vector<glm::vec3> { { 0.0F, 0.0F, 0.0F }, { 1.0F, 0.0F, 0.0F }, { 0.0F, 1.0F, 0.0F } },
            std::vector<uint32_t> { 0, 1, 2 })
            .value();
    }

    MeshAssemblySource mesh_source(std::string name)
    {
        return MeshAssemblySource { .geometry = triangle(), .name = std::move(name) };
    }

    /** car (0) with two wheels (1, 2) sharing one source, and a separate lamp (3). */
    struct CarFixture {
        MeshAssemblyContainer assembly;
        size_t wheel {};
        size_t lamp {};
        size_t rubber {};

        CarFixture()
        {
            wheel = assembly.add_source(mesh_source("wheel"));
            lamp = assembly.add_source(mesh_source("lamp"));
            rubber = assembly.add_material({ .name = "rubber", .diffuse_path = "rubber.png" });

            const auto car = assembly.add_placement({ .name = "car", .local = moved(10.0F) }).value();
            std::ignore = assembly.add_placement({ .name = "front_left", .parent = car, .source = wheel, .material = rubber, .local = moved(1.0F, 0.0F, 1.0F) });
            std::ignore = assembly.add_placement({ .name = "front_right", .parent = car, .source = wheel, .material = rubber, .local = moved(-1.0F, 0.0F, 1.0F) });
            std::ignore = assembly.add_placement({ .name = "lamp", .source = lamp, .local = moved(0.0F, 5.0F) });
        }
    };

    Kinesis::TimedValues<glm::vec3> slide(glm::vec3 from, glm::vec3 to)
    {
        return Kinesis::TimedValues<glm::vec3>::create({ .times = Kinesis::TimeSampling::acyclic({ 0.0, 1.0 }).value(),
                                                           .values = { from, to } })
            .value();
    }

    Kinesis::TimedValues<glm::vec4> turn_about_z(float radians)
    {
        const float half = radians * 0.5F;
        return Kinesis::TimedValues<glm::vec4>::create({ .times = Kinesis::TimeSampling::acyclic({ 0.0, 1.0 }).value(),
                                                           .values = { glm::vec4(0.0F, 0.0F, 0.0F, 1.0F),
                                                               glm::vec4(0.0F, 0.0F, std::sin(half), std::cos(half)) },
                                                           .kind = Kinesis::ValueKind::Rotation })
            .value();
    }

}

TEST(MeshAssemblyContainerTest, RowsAreAddedInOrderAndCounted)
{
    const CarFixture car;

    EXPECT_EQ(car.assembly.get_total_elements(), 4U);
    EXPECT_EQ(car.assembly.get_data().sources.size(), 2U);
    EXPECT_EQ(car.assembly.get_data().materials.size(), 1U);
    EXPECT_TRUE(car.assembly.has_data());
}

TEST(MeshAssemblyContainerTest, AParentThatDoesNotPrecedeItsChildIsRefused)
{
    MeshAssemblyContainer assembly;

    EXPECT_FALSE(assembly.add_placement({ .name = "orphan", .parent = 0 }).has_value());

    const auto root = assembly.add_placement({ .name = "root" }).value();
    EXPECT_FALSE(assembly.add_placement({ .name = "self", .parent = root + 1 }).has_value());
    EXPECT_TRUE(assembly.add_placement({ .name = "child", .parent = root }).has_value());
}

TEST(MeshAssemblyContainerTest, ASourceOrMaterialOutOfRangeIsRefused)
{
    MeshAssemblyContainer assembly;
    const auto source = assembly.add_source(mesh_source("one"));

    EXPECT_FALSE(assembly.add_placement({ .name = "a", .source = source + 1 }).has_value());
    EXPECT_FALSE(assembly.add_placement({ .name = "b", .source = source, .material = 0 }).has_value());
    EXPECT_EQ(assembly.get_total_elements(), 0U);
}

TEST(MeshAssemblyContainerTest, DataThatBreaksTheRulesIsRejectedAtConstruction)
{
    MeshAssemblyData data;
    data.sources.push_back(mesh_source("one"));

    const MeshAssemblyContainer valid(data);
    EXPECT_EQ(valid.get_data().sources.size(), 1U);

    std::string reason;
    EXPECT_TRUE(MeshAssemblyContainer::validate(data, reason));

    MeshAssemblyData wrong_layout;
    Kakshya::CompositeLayout other;
    other.add_field<double>("value");
    wrong_layout.placements = Kakshya::CompositeArray(other);
    EXPECT_FALSE(MeshAssemblyContainer::validate(wrong_layout, reason));
    EXPECT_THROW(MeshAssemblyContainer { std::move(wrong_layout) }, std::invalid_argument);
}

TEST(MeshAssemblyContainerTest, ARowWhoseParentComesLaterIsRejected)
{
    MeshAssemblyData data;
    Kakshya::CompositeInsertion insertion(data.placements);
    std::string reason;
    ASSERT_TRUE(Kakshya::write_described(insertion, insertion.append(), MeshAssemblyPlacement { .parent = 5 }, reason));

    EXPECT_FALSE(MeshAssemblyContainer::validate(data, reason));
    EXPECT_FALSE(reason.empty());
}

TEST(MeshAssemblyContainerTest, ADataSourceIndexOutOfRangeIsRejected)
{
    MeshAssemblyData data;
    Kakshya::CompositeInsertion insertion(data.placements);
    std::string reason;
    ASSERT_TRUE(Kakshya::write_described(insertion, insertion.append(), MeshAssemblyPlacement { .source = 0 }, reason));

    EXPECT_FALSE(MeshAssemblyContainer::validate(data, reason));
}

TEST(MeshAssemblyContainerTest, ADataMaterialIndexOutOfRangeIsRejected)
{
    MeshAssemblyData data;
    Kakshya::CompositeInsertion insertion(data.placements);
    std::string reason;
    ASSERT_TRUE(Kakshya::write_described(insertion, insertion.append(), MeshAssemblyPlacement { .material = 0 }, reason));

    EXPECT_FALSE(MeshAssemblyContainer::validate(data, reason));
}

TEST(MeshAssemblyPlacementTest, TheLayoutIsDerivedFromTheDescribedMembers)
{
    const auto layout = MeshAssemblyData::placement_layout();

    EXPECT_EQ(layout.fields().size(), 5U);
    EXPECT_TRUE(layout.find_field("name").has_value());
    EXPECT_TRUE(layout.find_field("parent").has_value());
    EXPECT_TRUE(layout.find_field("source").has_value());
    EXPECT_TRUE(layout.find_field("material").has_value());
    EXPECT_TRUE(layout.find_field("local").has_value());
    EXPECT_TRUE(layout.same_structure(Kakshya::described_composite_layout<MeshAssemblyPlacement>()));
}

TEST(MeshAssemblyPlacementTest, TheParentColumnIsNamedByTheParentMember)
{
    EXPECT_EQ(MeshAssemblyData::parent_column(), "parent");
    EXPECT_TRUE(MeshAssemblyData::placement_layout().find_field(MeshAssemblyData::parent_column()).has_value());
}

TEST(MeshAssemblyPlacementTest, ARowReadsBackAsItWasAdded)
{
    MeshAssemblyContainer assembly;
    const auto source = assembly.add_source(mesh_source("one"));
    const auto material = assembly.add_material({ .name = "m" });
    const auto root = assembly.add_placement({ .name = "root" }).value();
    const auto child = assembly.add_placement({ .name = "child", .parent = root, .source = source, .material = material, .local = moved(1.0F, 2.0F, 3.0F) }).value();

    const auto read = assembly.placement(child);

    ASSERT_TRUE(read.has_value());
    EXPECT_EQ(read->name, "child");
    EXPECT_EQ(read->parent, std::optional<uint64_t> { root });
    EXPECT_EQ(read->source, std::optional<uint64_t> { source });
    EXPECT_EQ(read->material, std::optional<uint64_t> { material });
    EXPECT_TRUE(close(glm::vec3(read->local[3]), glm::vec3(1.0F, 2.0F, 3.0F)));
}

TEST(MeshAssemblyPlacementTest, AnAbsentParentSourceAndMaterialReadBackAbsentAndTheLocalDefaultsToIdentity)
{
    MeshAssemblyContainer assembly;
    const auto row = assembly.add_placement({}).value();

    const auto read = assembly.placement(row);

    ASSERT_TRUE(read.has_value());
    EXPECT_TRUE(read->name.empty());
    EXPECT_FALSE(read->parent.has_value());
    EXPECT_FALSE(read->source.has_value());
    EXPECT_FALSE(read->material.has_value());
    EXPECT_EQ(read->local, glm::mat4(1.0F));
}

TEST(MeshAssemblyPlacementTest, ARowOutOfRangeReadsAsNothing)
{
    const CarFixture car;

    EXPECT_FALSE(car.assembly.placement(4).has_value());
}

TEST(MeshAssemblyPlacementTest, ARowSurvivesAMeshAssemblyCopyWithItsIndicesRemapped)
{
    CarFixture car;

    const auto copies = car.assembly.get_region_data(
        Region(std::vector<uint64_t> { 3 }, std::vector<uint64_t> { 3 }));

    ASSERT_EQ(copies.size(), 1U);
    const MeshAssemblyContainer copy(copies.front());
    const auto lamp = copy.placement(0);
    ASSERT_TRUE(lamp.has_value());
    EXPECT_EQ(lamp->name, "lamp");
    EXPECT_EQ(lamp->source, std::optional<uint64_t> { 0 });
    EXPECT_FALSE(lamp->parent.has_value());
    EXPECT_EQ(copy.get_data().sources[0].name, "lamp");
}

TEST(MeshAssemblyContainerTest, TheMeshAndSpatialAccessorsReportWhatASourceIs)
{
    MeshAssemblyContainer assembly;
    const auto mesh_index = assembly.add_source(mesh_source("mesh"));
    const auto points_index = assembly.add_source(MeshAssemblySource {
        .geometry = SpatialData { .topology = Portal::Graphics::PrimitiveTopology::POINT_LIST,
            .positions = { { 0.0F, 0.0F, 0.0F } } },
        .name = "points" });

    EXPECT_NE(assembly.mesh(mesh_index), nullptr);
    EXPECT_EQ(assembly.spatial(mesh_index), nullptr);
    EXPECT_NE(assembly.spatial(points_index), nullptr);
    EXPECT_EQ(assembly.mesh(points_index), nullptr);
    EXPECT_EQ(assembly.mesh(99), nullptr);
}

TEST(MeshAssemblyContainerTest, TheForestIndexesTheParentLinks)
{
    const CarFixture car;

    const auto index = car.assembly.index_forest();

    EXPECT_EQ(index.size(), 4U);
    ASSERT_EQ(index.roots().size(), 2U);
    EXPECT_EQ(index.roots()[0], 0U);
    EXPECT_EQ(index.roots()[1], 3U);
    EXPECT_EQ(index.children(0).size(), 2U);
    EXPECT_EQ(index.parent(1), std::optional<uint64_t> { 0 });
    EXPECT_FALSE(index.parent(3).has_value());
}

TEST(MeshAssemblyContainerTest, ASubtreeIsARegionOfRowsWhenTheRowsAreInPreorder)
{
    const CarFixture car;
    const auto index = car.assembly.index_forest();

    const auto region = car.assembly.subtree_region(index, 0);

    ASSERT_TRUE(region.has_value());
    EXPECT_EQ(region->start_coordinates[0], 0U);
    EXPECT_EQ(region->end_coordinates[0], 2U);
}

TEST(MeshAssemblyContainerTest, WorldTransformsComposeEachRowWithItsAncestors)
{
    const CarFixture car;

    const auto world = car.assembly.world_transforms();

    ASSERT_EQ(world.size(), 4U);
    EXPECT_TRUE(close(glm::vec3(world[0][3]), glm::vec3(10.0F, 0.0F, 0.0F)));
    EXPECT_TRUE(close(glm::vec3(world[1][3]), glm::vec3(11.0F, 0.0F, 1.0F)));
    EXPECT_TRUE(close(glm::vec3(world[2][3]), glm::vec3(9.0F, 0.0F, 1.0F)));
    EXPECT_TRUE(close(glm::vec3(world[3][3]), glm::vec3(0.0F, 5.0F, 0.0F)));
}

TEST(MeshAssemblyContainerTest, WorldTransformsOfTheWrongNumberOfLocalsAreEmpty)
{
    const CarFixture car;
    const std::vector<glm::mat4> too_few(2, glm::mat4(1.0F));

    EXPECT_TRUE(car.assembly.world_transforms(too_few).empty());
}

TEST(MeshAssemblyContainerTest, RowsSharingASourceAndMaterialFormOneGroupAndJointsAreLeftOut)
{
    CarFixture car;
    ASSERT_TRUE(car.assembly.add_placement({ .name = "axle", .parent = 0 }).has_value());

    const auto groups = car.assembly.groups();

    ASSERT_EQ(groups.size(), 2U);
    EXPECT_EQ(groups[0].source, car.wheel);
    EXPECT_EQ(groups[0].material, std::optional<uint64_t> { car.rubber });
    EXPECT_EQ(groups[0].rows, (std::vector<uint64_t> { 1, 2 }));
    EXPECT_EQ(groups[1].source, car.lamp);
    EXPECT_FALSE(groups[1].material.has_value());
}

TEST(MeshAssemblyContainerTest, ASharedSourceWithDifferentMaterialsSplitsIntoGroups)
{
    CarFixture car;
    const auto chrome = car.assembly.add_material({ .name = "chrome" });
    ASSERT_TRUE(car.assembly.add_placement({ .name = "spare", .parent = 0, .source = car.wheel, .material = chrome }).has_value());

    const auto groups = car.assembly.groups();

    ASSERT_EQ(groups.size(), 3U);
    EXPECT_EQ(groups[2].source, car.wheel);
    EXPECT_EQ(groups[2].material, std::optional<uint64_t> { chrome });
    EXPECT_EQ(groups[2].rows, (std::vector<uint64_t> { 4 }));
}

TEST(MeshAssemblyContainerTest, ARegionCopyIsASelfContainedMeshAssembly)
{
    CarFixture car;
    ASSERT_TRUE(car.assembly.add_clip({ .name = "roll", .channels = { { .row = 1, .translation = slide({ 0, 0, 0 }, { 0, 0, 5 }) }, { .row = 3, .translation = slide({ 0, 0, 0 }, { 1, 0, 0 }) } } }).has_value());

    const auto copies = car.assembly.get_region_data(
        Region(std::vector<uint64_t> { 1 }, std::vector<uint64_t> { 2 }));

    ASSERT_EQ(copies.size(), 1U);
    const MeshAssemblyContainer copy(copies.front());
    EXPECT_EQ(copy.get_total_elements(), 2U);
    EXPECT_EQ(copy.get_data().sources.size(), 1U);
    EXPECT_EQ(copy.get_data().sources[0].name, "wheel");
    EXPECT_EQ(copy.get_data().materials.size(), 1U);
    EXPECT_EQ(copy.index_forest().roots().size(), 2U);
    ASSERT_EQ(copy.get_data().clips.size(), 1U);
    ASSERT_EQ(copy.get_data().clips[0].channels.size(), 1U);
    EXPECT_EQ(copy.get_data().clips[0].channels[0].row, 0U);
}

TEST(MeshAssemblyContainerTest, ASubtreeCopyRebasesItsParentLinksAndSourceIndices)
{
    CarFixture car;
    const auto index = car.assembly.index_forest();
    const auto region = car.assembly.subtree_region(index, 0);
    ASSERT_TRUE(region.has_value());

    const auto copies = car.assembly.get_region_data(*region);

    ASSERT_EQ(copies.size(), 1U);
    const MeshAssemblyContainer copy(copies.front());
    const auto copy_index = copy.index_forest();
    ASSERT_EQ(copy_index.roots().size(), 1U);
    EXPECT_EQ(copy_index.children(0).size(), 2U);
    const auto groups = copy.groups();
    ASSERT_EQ(groups.size(), 1U);
    EXPECT_EQ(groups[0].source, 0U);
    EXPECT_EQ(groups[0].rows.size(), 2U);
}

TEST(MeshAssemblyContainerTest, AnInvalidRegionCopiesNothing)
{
    const CarFixture car;

    EXPECT_TRUE(car.assembly.get_region_data(
                              Region(std::vector<uint64_t> { 2 }, std::vector<uint64_t> { 9 }))
                    .empty());
}

TEST(MeshAssemblyContainerTest, SegmentsCopyOneMeshAssemblyEach)
{
    const CarFixture car;
    Kakshya::RegionSegment segment;
    segment.source_region = Region(std::vector<uint64_t> { 0 }, std::vector<uint64_t> { 2 });
    segment.offset_in_region = { 1 };
    segment.segment_size = { 2 };

    const auto copies = car.assembly.get_segments_data({ segment });

    ASSERT_EQ(copies.size(), 1U);
    EXPECT_EQ(copies[0].placements.size(), 2U);
}

TEST(MeshAssemblyContainerTest, FacetsOnASourceAMaterialAndTheWholeSurviveACopy)
{
    const Facet<std::string> author("none");
    const Facet<float> weight(0.0F);

    MeshAssemblyContainer assembly;
    MeshAssemblySource source = mesh_source("body");
    source.facets.set(weight, 2.5F);
    const auto body = assembly.add_source(std::move(source));
    MeshAssemblyMaterial paint { .name = "paint" };
    paint.facets.set(author, "studio");
    const auto paint_index = assembly.add_material(std::move(paint));
    ASSERT_TRUE(assembly.add_placement({ .name = "body", .source = body, .material = paint_index }).has_value());

    MeshAssemblyData with_file = assembly.get_data();
    with_file.facets.set(author, "exporter");
    const MeshAssemblyContainer holder(std::move(with_file));

    const auto copies = holder.get_region_data(
        Region(std::vector<uint64_t> { 0 }, std::vector<uint64_t> { 0 }));

    ASSERT_EQ(copies.size(), 1U);
    const auto* kept_weight = copies[0].sources[0].facets.get(weight);
    ASSERT_NE(kept_weight, nullptr);
    EXPECT_FLOAT_EQ(*kept_weight, 2.5F);
    const auto* kept_author = copies[0].materials[0].facets.get(author);
    ASSERT_NE(kept_author, nullptr);
    EXPECT_EQ(*kept_author, "studio");
    const auto* file_author = copies[0].facets.get(author);
    ASSERT_NE(file_author, nullptr);
    EXPECT_EQ(*file_author, "exporter");
}

TEST(MeshAssemblyContainerTest, AClipTargetingARowTwiceOrAMissingRowIsRefused)
{
    CarFixture car;

    EXPECT_FALSE(car.assembly.add_clip({ .name = "twice", .channels = { { .row = 1 }, { .row = 1 } } }).has_value());
    EXPECT_FALSE(car.assembly.add_clip({ .name = "missing", .channels = { { .row = 40 } } }).has_value());
    EXPECT_TRUE(car.assembly.add_clip({ .name = "fine", .channels = { { .row = 1 }, { .row = 2 } } }).has_value());
}

TEST(MeshAssemblyClipTest, ASampledClipMovesOnlyTheRowsItTargets)
{
    CarFixture car;
    const auto clip = car.assembly.add_clip({ .name = "drive", .channels = { { .row = 0, .translation = slide({ 10, 0, 0 }, { 30, 0, 0 }) } } }).value();

    const auto locals = car.assembly.sample(clip, 0.5).value();

    ASSERT_EQ(locals.size(), 4U);
    EXPECT_TRUE(close(glm::vec3(locals[0][3]), glm::vec3(20.0F, 0.0F, 0.0F)));
    EXPECT_TRUE(close(glm::vec3(locals[1][3]), glm::vec3(1.0F, 0.0F, 1.0F)));
    EXPECT_TRUE(close(glm::vec3(locals[3][3]), glm::vec3(0.0F, 5.0F, 0.0F)));

    const auto world = car.assembly.world_transforms(locals);
    EXPECT_TRUE(close(glm::vec3(world[1][3]), glm::vec3(21.0F, 0.0F, 1.0F)));
}

TEST(MeshAssemblyClipTest, AComponentTheChannelDoesNotKeepKeepsTheRowsRestValue)
{
    MeshAssemblyContainer assembly;
    const auto row = assembly.add_placement({ .name = "row", .local = Kinesis::compose_transform({ 1.0F, 2.0F, 3.0F }, glm::quat(1.0F, 0.0F, 0.0F, 0.0F), { 2.0F, 2.0F, 2.0F }) })
                         .value();
    const auto clip = assembly.add_clip({ .name = "slide", .channels = { { .row = row, .translation = slide({ 0, 0, 0 }, { 4, 0, 0 }) } } }).value();

    const auto locals = assembly.sample(clip, 1.0).value();

    EXPECT_TRUE(close(glm::vec3(locals[0][3]), glm::vec3(4.0F, 0.0F, 0.0F)));
    EXPECT_NEAR(glm::length(glm::vec3(locals[0][0])), 2.0F, 1e-4F);
}

TEST(MeshAssemblyClipTest, ARotationChannelTurnsTheRow)
{
    MeshAssemblyContainer assembly;
    const auto row = assembly.add_placement({ .name = "row" }).value();
    const auto clip = assembly.add_clip({ .name = "turn", .channels = { { .row = row, .rotation = turn_about_z(glm::half_pi<float>()) } } }).value();

    const auto locals = assembly.sample(clip, 1.0).value();

    EXPECT_TRUE(close(glm::vec3(locals[0] * glm::vec4(1.0F, 0.0F, 0.0F, 1.0F)), glm::vec3(0.0F, 1.0F, 0.0F)));
}

TEST(MeshAssemblyClipTest, AClipThatDoesNotExistGivesNothing)
{
    const CarFixture car;

    EXPECT_FALSE(car.assembly.sample(0, 0.0).has_value());
}

TEST(MeshAssemblyClipTest, AbsentOutsideTheKeysLeavesTheRestPlacement)
{
    CarFixture car;
    const auto clip = car.assembly.add_clip({ .name = "late", .channels = { { .row = 0, .translation = slide({ 0, 0, 0 }, { 5, 0, 0 }) } } }).value();

    const auto locals = car.assembly.sample(clip, 9.0, Kinesis::OutOfRange::Absent).value();

    EXPECT_TRUE(close(glm::vec3(locals[0][3]), glm::vec3(10.0F, 0.0F, 0.0F)));
}

TEST(MeshAssemblyContainerTest, TheContainerFaceHasOneRowAxisAndNoFrames)
{
    CarFixture car;

    const auto dimensions = car.assembly.get_dimensions();

    ASSERT_EQ(dimensions.size(), 1U);
    EXPECT_EQ(dimensions[0].size, 4U);
    EXPECT_EQ(car.assembly.get_frame_size(), 0U);
    EXPECT_EQ(car.assembly.get_num_frames(), 0U);
    EXPECT_EQ(car.assembly.get_memory_layout(), Kakshya::MemoryLayout::ROW_MAJOR);
    EXPECT_NE(car.assembly.get_raw_data(), nullptr);
    EXPECT_EQ(car.assembly.coordinates_to_linear_index({ 3 }), 3U);
    EXPECT_EQ(car.assembly.linear_index_to_coordinates(3), (std::vector<uint64_t> { 3 }));
    EXPECT_TRUE(car.assembly.is_region_loaded(Region(std::vector<uint64_t> { 0 }, std::vector<uint64_t> { 3 })));
    EXPECT_FALSE(car.assembly.is_region_loaded(Region(std::vector<uint64_t> { 0 }, std::vector<uint64_t> { 4 })));
}

TEST(MeshAssemblyContainerTest, SettingRegionDataIsRefused)
{
    CarFixture car;
    const auto copies = car.assembly.get_region_data(
        Region(std::vector<uint64_t> { 0 }, std::vector<uint64_t> { 0 }));

    car.assembly.set_region_data(Region(std::vector<uint64_t> { 1 }, std::vector<uint64_t> { 1 }), copies);

    EXPECT_EQ(car.assembly.get_total_elements(), 4U);
}

TEST(MeshAssemblyContainerTest, RegionGroupsAreKeptByTheSemanticBase)
{
    CarFixture car;
    Kakshya::RegionGroup group;
    group.name = "wheels";

    car.assembly.add_region_group(group);

    EXPECT_EQ(car.assembly.get_region_group("wheels").name, "wheels");
    car.assembly.clear();
    EXPECT_TRUE(car.assembly.get_all_region_groups().empty());
}

TEST(MeshAssemblyContainerTest, ClearRemovesEverythingAndTheContainerIsUsableAgain)
{
    CarFixture car;

    car.assembly.clear();

    EXPECT_FALSE(car.assembly.has_data());
    EXPECT_EQ(car.assembly.get_total_elements(), 0U);
    EXPECT_EQ(car.assembly.get_raw_data(), nullptr);
    EXPECT_TRUE(car.assembly.add_placement({ .name = "again" }).has_value());
}

}
