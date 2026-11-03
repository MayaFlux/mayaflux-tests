#include "gtest/gtest.h"

#include "MayaFlux/Kakshya/Region/RegionGroup.hpp"

namespace MayaFlux::Test {
namespace {

    using Kakshya::Region;
    using Kakshya::RegionGroup;
    using Kakshya::RegionSelectionPattern;
    using Kakshya::RegionState;
    using Kakshya::RegionTransition;

    using Coordinates = std::vector<uint64_t>;
    using Labels = std::vector<std::string>;

    RegionGroup standard_group()
    {
        return RegionGroup("test_group",
            { Region::time_point(100, "onset"),
                Region::time_span(200, 300, "sustain"),
                Region::time_point(400, "release") });
    }

    Labels labels_of(const std::vector<Region>& regions)
    {
        Labels labels;
        for (const auto& region : regions) {
            labels.push_back(region.label.value_or(""));
        }
        return labels;
    }

    TEST(RegionGroupTest, DefaultsAreIdleSequentialAndEmpty)
    {
        RegionGroup group;

        EXPECT_TRUE(group.name.empty());
        EXPECT_TRUE(group.regions.empty());
        EXPECT_EQ(group.current_region_index, 0U);
        EXPECT_TRUE(group.active_indices.empty());
        EXPECT_EQ(group.state, RegionState::IDLE);
        EXPECT_EQ(group.transition_type, RegionTransition::IMMEDIATE);
        EXPECT_EQ(group.region_selection_pattern, RegionSelectionPattern::SEQUENTIAL);
        EXPECT_DOUBLE_EQ(group.transition_duration_ms, 0.0);
    }

    TEST(RegionGroupTest, ConstructionKeepsTheNameAndRegionsInOrder)
    {
        const RegionGroup group = standard_group();

        EXPECT_EQ(group.name, "test_group");
        EXPECT_EQ(labels_of(group.regions), (Labels { "onset", "sustain", "release" }));
    }

    TEST(RegionGroupTest, AddRegionAppends)
    {
        RegionGroup group = standard_group();

        group.add_region(Region::time_point(500, "fade_out"));

        EXPECT_EQ(labels_of(group.regions), (Labels { "onset", "sustain", "release", "fade_out" }));
    }

    struct InsertCase {
        std::string_view name;
        size_t index;
        Labels expected;
    };

    class RegionGroupInsertTest : public ::testing::TestWithParam<InsertCase> { };

    INSTANTIATE_TEST_SUITE_P(Insert, RegionGroupInsertTest,
        ::testing::Values(
            InsertCase { "AtTheFront", 0, { "new", "onset", "sustain", "release" } },
            InsertCase { "InTheMiddle", 1, { "onset", "new", "sustain", "release" } },
            InsertCase { "AtTheEnd", 3, { "onset", "sustain", "release", "new" } },
            InsertCase { "PastTheEndAppends", 100, { "onset", "sustain", "release", "new" } }),
        [](const ::testing::TestParamInfo<InsertCase>& info) { return std::string(info.param.name); });

    TEST_P(RegionGroupInsertTest, PlacesTheRegionAndShiftsTheRest)
    {
        const auto& c = GetParam();
        RegionGroup group = standard_group();

        group.insert_region(c.index, Region::time_point(150, "new"));

        EXPECT_EQ(labels_of(group.regions), c.expected);
    }

    struct RemoveCase {
        std::string_view name;
        size_t index;
        size_t cursor_before;
        Labels expected;
        size_t cursor_after;
    };

    class RegionGroupRemoveTest : public ::testing::TestWithParam<RemoveCase> { };

    INSTANTIATE_TEST_SUITE_P(Remove, RegionGroupRemoveTest,
        ::testing::Values(
            RemoveCase { "Middle", 1, 0, { "onset", "release" }, 0 },
            RemoveCase { "LastUnderTheCursor", 2, 2, { "onset", "sustain" }, 1 },
            RemoveCase { "FirstAheadOfTheCursor", 0, 2, { "sustain", "release" }, 1 },
            RemoveCase { "OutOfRangeIsIgnored", 100, 1, { "onset", "sustain", "release" }, 1 }),
        [](const ::testing::TestParamInfo<RemoveCase>& info) { return std::string(info.param.name); });

    TEST_P(RegionGroupRemoveTest, DropsTheRegionAndKeepsTheCursorValid)
    {
        const auto& c = GetParam();
        RegionGroup group = standard_group();
        group.current_region_index = c.cursor_before;

        group.remove_region(c.index);

        EXPECT_EQ(labels_of(group.regions), c.expected);
        EXPECT_EQ(group.current_region_index, c.cursor_after);
    }

    TEST(RegionGroupTest, RemovingTheLastRegionLeavesAnEmptyGroup)
    {
        RegionGroup group("single", { Region::time_point(1, "only") });

        group.remove_region(0);

        EXPECT_TRUE(group.regions.empty());
        EXPECT_EQ(group.current_region_index, 0U);
    }

    TEST(RegionGroupTest, ClearResetsTheRegionsCursorAndActiveSet)
    {
        RegionGroup group = standard_group();
        group.current_region_index = 2;
        group.active_indices = { 0, 2 };

        group.clear_regions();

        EXPECT_TRUE(group.regions.empty());
        EXPECT_EQ(group.current_region_index, 0U);
        EXPECT_TRUE(group.active_indices.empty());
    }

    struct SortDimensionCase {
        std::string_view name;
        size_t dimension;
        std::vector<Region> regions;
        Labels expected;
    };

    class RegionGroupSortDimensionTest : public ::testing::TestWithParam<SortDimensionCase> { };

    INSTANTIATE_TEST_SUITE_P(SortByDimension, RegionGroupSortDimensionTest,
        ::testing::Values(
            SortDimensionCase { "TimeOrder", 0, { Region::time_point(300, "third"), Region::time_point(100, "first"), Region::time_point(200, "second") }, { "first", "second", "third" } },
            SortDimensionCase { "AlreadyOrdered", 0, { Region::time_point(1, "a"), Region::time_point(2, "b") }, { "a", "b" } },
            SortDimensionCase { "ChannelOrder", 1, { Region::audio_point(0, 2, "high"), Region::audio_point(0, 0, "low"), Region::audio_point(0, 1, "mid") }, { "low", "mid", "high" } },
            SortDimensionCase { "Empty", 0, {}, {} }),
        [](const ::testing::TestParamInfo<SortDimensionCase>& info) { return std::string(info.param.name); });

    TEST_P(RegionGroupSortDimensionTest, OrdersByTheStartCoordinate)
    {
        const auto& c = GetParam();
        RegionGroup group("sorted", c.regions);

        group.sort_by_dimension(c.dimension);

        EXPECT_EQ(labels_of(group.regions), c.expected);
    }

    TEST(RegionGroupTest, SortingByAnAbsentDimensionKeepsEveryRegion)
    {
        RegionGroup group = standard_group();

        group.sort_by_dimension(10);

        Labels labels = labels_of(group.regions);
        std::ranges::sort(labels);
        EXPECT_EQ(labels, (Labels { "onset", "release", "sustain" }));
    }

    struct SortFacetCase {
        std::string_view name;
        std::vector<std::optional<double>> priorities;
        Labels expected;
    };

    class RegionGroupSortFacetTest : public ::testing::TestWithParam<SortFacetCase> { };

    INSTANTIATE_TEST_SUITE_P(SortByFacet, RegionGroupSortFacetTest,
        ::testing::Values(
            SortFacetCase { "AllPresent", { 3.0, 1.0, 2.0 }, { "b", "c", "a" } },
            SortFacetCase { "MissingTrails", { std::nullopt, 2.0, 1.0 }, { "c", "b", "a" } },
            SortFacetCase { "MissingKeepRelativeOrder", { std::nullopt, std::nullopt, 1.0 }, { "c", "a", "b" } },
            SortFacetCase { "AllMissing", { std::nullopt, std::nullopt, std::nullopt }, { "a", "b", "c" } },
            SortFacetCase { "EqualValuesKeepOrder", { 1.0, 1.0, 1.0 }, { "a", "b", "c" } }),
        [](const ::testing::TestParamInfo<SortFacetCase>& info) { return std::string(info.param.name); });

    TEST_P(RegionGroupSortFacetTest, OrdersByTheValueWithMissingLast)
    {
        const auto& c = GetParam();
        const Facet<double> priority;
        RegionGroup group("sorted");
        const Labels names { "a", "b", "c" };
        for (size_t i = 0; i < names.size(); ++i) {
            Region region = Region::time_point(i, names[i]);
            if (c.priorities[i]) {
                region.set(priority, *c.priorities[i]);
            }
            group.add_region(region);
        }

        group.sort(priority);

        EXPECT_EQ(labels_of(group.regions), c.expected);
    }

    TEST(RegionGroupTest, SortingAnEmptyGroupByAFacetIsHarmless)
    {
        const Facet<double> priority;
        RegionGroup group;

        group.sort(priority);

        EXPECT_TRUE(group.regions.empty());
    }

    struct LabelSearchCase {
        std::string_view name;
        std::string label;
        size_t count;
    };

    class RegionGroupLabelSearchTest : public ::testing::TestWithParam<LabelSearchCase> { };

    INSTANTIATE_TEST_SUITE_P(LabelSearch, RegionGroupLabelSearchTest,
        ::testing::Values(
            LabelSearchCase { "Unique", "sustain", 1 },
            LabelSearchCase { "Repeated", "onset", 2 },
            LabelSearchCase { "Absent", "nonexistent", 0 }),
        [](const ::testing::TestParamInfo<LabelSearchCase>& info) { return std::string(info.param.name); });

    TEST_P(RegionGroupLabelSearchTest, FindsEveryRegionWithTheLabel)
    {
        const auto& c = GetParam();
        RegionGroup group = standard_group();
        group.add_region(Region::time_point(500, "onset"));

        const auto found = group.find_regions_with_label(c.label);

        EXPECT_EQ(found.size(), c.count);
    }

    struct FacetSearchCase {
        std::string_view name;
        std::string value;
        Labels expected;
    };

    class RegionGroupFacetSearchTest : public ::testing::TestWithParam<FacetSearchCase> { };

    INSTANTIATE_TEST_SUITE_P(FacetSearch, RegionGroupFacetSearchTest,
        ::testing::Values(
            FacetSearchCase { "Shared", "percussive", { "onset", "release" } },
            FacetSearchCase { "Single", "tonal", { "sustain" } },
            FacetSearchCase { "Absent", "ambient", {} }),
        [](const ::testing::TestParamInfo<FacetSearchCase>& info) { return std::string(info.param.name); });

    TEST_P(RegionGroupFacetSearchTest, FindsRegionsWhoseValueMatches)
    {
        const auto& c = GetParam();
        const Facet<std::string> type;
        RegionGroup group = standard_group();
        group.regions[0].set(type, std::string("percussive"));
        group.regions[1].set(type, std::string("tonal"));
        group.regions[2].set(type, std::string("percussive"));
        group.add_region(Region::time_point(500, "untyped"));

        const auto found = group.find_regions(type, c.value);

        EXPECT_EQ(labels_of(found), c.expected);
    }

    struct CoordinateSearchCase {
        std::string_view name;
        Coordinates position;
        Labels expected;
    };

    class RegionGroupCoordinateSearchTest : public ::testing::TestWithParam<CoordinateSearchCase> { };

    INSTANTIATE_TEST_SUITE_P(CoordinateSearch, RegionGroupCoordinateSearchTest,
        ::testing::Values(
            CoordinateSearchCase { "InsideASpan", { 250 }, { "sustain" } },
            CoordinateSearchCase { "OnAPoint", { 100 }, { "onset" } },
            CoordinateSearchCase { "InNothing", { 1000 }, {} },
            CoordinateSearchCase { "InAMultiDimensionalSpan", { 650, 0 }, { "multi" } },
            CoordinateSearchCase { "RankMismatch", { 250, 0, 0 }, {} }),
        [](const ::testing::TestParamInfo<CoordinateSearchCase>& info) { return std::string(info.param.name); });

    TEST_P(RegionGroupCoordinateSearchTest, FindsRegionsContainingThePosition)
    {
        const auto& c = GetParam();
        RegionGroup group = standard_group();
        group.add_region(Region::audio_span(600, 700, 0, 1, "multi"));

        const auto found = group.find_regions_containing_coordinates(c.position);

        EXPECT_EQ(labels_of(found), c.expected);
    }

    struct BoundsCase {
        std::string_view name;
        RegionGroup group;
        Coordinates start;
        Coordinates end;
    };

    class RegionGroupBoundsTest : public ::testing::TestWithParam<BoundsCase> { };

    INSTANTIATE_TEST_SUITE_P(Bounds, RegionGroupBoundsTest,
        ::testing::Values(
            BoundsCase { "TimeRegions", standard_group(), { 100 }, { 400 } },
            BoundsCase { "MultiDimensional", RegionGroup("multi_group", { Region::audio_span(0, 100, 0, 1, "a"), Region::audio_span(50, 200, 1, 3, "b"), Region::audio_span(25, 150, 2, 2, "c") }), { 0, 0 }, { 200, 3 } },
            BoundsCase { "SinglePoint", RegionGroup("single", { Region::time_point(500, "single") }), { 500 }, { 500 } },
            BoundsCase { "Empty", RegionGroup("empty"), {}, {} }),
        [](const ::testing::TestParamInfo<BoundsCase>& info) { return std::string(info.param.name); });

    TEST_P(RegionGroupBoundsTest, BoundingRegionSpansEveryMember)
    {
        const auto& c = GetParam();

        const Region bounds = c.group.get_bounding_region();

        EXPECT_EQ(bounds.start_coordinates, c.start);
        EXPECT_EQ(bounds.end_coordinates, c.end);
    }

    TEST_P(RegionGroupBoundsTest, BoundingRegionIsLabelledWithTheGroupWhenNotEmpty)
    {
        const auto& c = GetParam();

        const Region bounds = c.group.get_bounding_region();

        if (c.group.regions.empty()) {
            EXPECT_FALSE(bounds.label.has_value());
        } else {
            EXPECT_EQ(bounds.label, c.group.name);
        }
    }

    TEST(RegionGroupTest, TypedValuesReplaceOnSet)
    {
        RegionGroup group("onsets");
        const Facet<uint32_t> sample_rate;

        group.set(sample_rate, 44100U);
        group.set(sample_rate, 48000U);

        ASSERT_NE(group.get(sample_rate), nullptr);
        EXPECT_EQ(*group.get(sample_rate), 48000U);
    }

} // namespace
} // namespace MayaFlux::Test
