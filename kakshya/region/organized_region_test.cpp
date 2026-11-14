#include "gtest/gtest.h"
#include "../../support/test_metadata.hpp"

#include "MayaFlux/Kakshya/Region/OrganizedRegion.hpp"
#include "MayaFlux/Kakshya/Region/RegionGroup.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {
namespace {

    using Kakshya::OrganizedRegion;
    using Kakshya::Region;
    using Kakshya::RegionGroup;
    using Kakshya::RegionSegment;
    using Kakshya::RegionSelectionPattern;
    using Kakshya::RegionState;
    using Kakshya::RegionTransition;

    using Coordinates = std::vector<uint64_t>;

    OrganizedRegion windowed()
    {
        OrganizedRegion organized("windows", 3);
        const Region source = Region::time_span(0, 999);
        organized.segments.emplace_back(source, Coordinates { 100 }, Coordinates { 50 });
        organized.segments.emplace_back(source, Coordinates { 200 }, Coordinates { 50 });
        return organized;
    }

    TEST(OrganizedRegionTest, ConstructionRecordsItsPlaceInTheGroup)
    {
        const OrganizedRegion organized("chapters", 4);

        EXPECT_EQ(organized.group_name, "chapters");
        EXPECT_EQ(organized.region_index, 4U);
    }

    TEST(OrganizedRegionTest, DefaultsAreIdleSequentialAndImmediate)
    {
        const OrganizedRegion organized("chapters", 0);

        EXPECT_TRUE(organized.segments.empty());
        EXPECT_EQ(organized.state, RegionState::IDLE);
        EXPECT_EQ(organized.transition_type, RegionTransition::IMMEDIATE);
        EXPECT_EQ(organized.selection_pattern, RegionSelectionPattern::SEQUENTIAL);
        EXPECT_DOUBLE_EQ(organized.transition_duration_ms, 0.0);
        EXPECT_EQ(organized.active_segment_index, 0U);
        EXPECT_FALSE(organized.looping_enabled);
    }

    struct VolumeCase {
        std::string_view name;
        std::vector<Region> regions;
        uint64_t volume;
    };

    class OrganizedRegionVolumeTest : public ::testing::TestWithParam<VolumeCase> { };

    INSTANTIATE_TEST_SUITE_P(Volume, OrganizedRegionVolumeTest,
        ::testing::Values(
            VolumeCase { "NoSegments", {}, 0 },
            VolumeCase { "OneSpan", { Region::time_span(0, 9) }, 10 },
            VolumeCase { "TwoSpans", { Region::time_span(0, 9), Region::time_span(100, 119) }, 30 },
            VolumeCase { "AudioSpan", { Region::audio_span(0, 9, 0, 1) }, 20 }),
        [](const ::testing::TestParamInfo<VolumeCase>& info) { return std::string(info.param.name); });

    TEST_P(OrganizedRegionVolumeTest, TotalVolumeSumsTheSegments)
    {
        const auto& c = GetParam();
        OrganizedRegion organized("volume", 0);
        for (const auto& region : c.regions) {
            organized.segments.emplace_back(region);
        }

        EXPECT_EQ(organized.get_total_volume(), c.volume);
    }

    struct PositionCase {
        std::string_view name;
        Coordinates position;
        std::optional<size_t> segment;
    };

    class OrganizedRegionPositionTest : public ::testing::TestWithParam<PositionCase> { };

    INSTANTIATE_TEST_SUITE_P(Position, OrganizedRegionPositionTest,
        ::testing::Values(
            PositionCase { "InTheFirstWindow", { 120 }, 0 },
            PositionCase { "InTheSecondWindow", { 220 }, 1 },
            PositionCase { "FirstWindowStart", { 100 }, 0 },
            PositionCase { "FirstWindowLastElement", { 149 }, 0 },
            PositionCase { "BetweenWindows", { 160 }, std::nullopt },
            PositionCase { "BeforeAllWindows", { 99 }, std::nullopt },
            PositionCase { "WrongRank", { 120, 0 }, std::nullopt }),
        [](const ::testing::TestParamInfo<PositionCase>& info) { return std::string(info.param.name); });

    TEST_P(OrganizedRegionPositionTest, FindsTheSegmentHoldingThePosition)
    {
        const auto& c = GetParam();

        const OrganizedRegion organized = windowed();

        EXPECT_EQ(organized.find_segment_for_position(c.position), c.segment);
    }

    TEST_P(OrganizedRegionPositionTest, ContainsPositionAgreesWithTheSearch)
    {
        const auto& c = GetParam();

        const OrganizedRegion organized = windowed();

        EXPECT_EQ(organized.contains_position(c.position), c.segment.has_value());
    }

    struct ActiveCase {
        std::string_view name;
        size_t index;
        bool present;
    };

    class OrganizedRegionActiveTest : public ::testing::TestWithParam<ActiveCase> { };

    INSTANTIATE_TEST_SUITE_P(Active, OrganizedRegionActiveTest,
        ::testing::Values(
            ActiveCase { "First", 0, true },
            ActiveCase { "Last", 1, true },
            ActiveCase { "PastTheEnd", 2, false },
            ActiveCase { "FarPastTheEnd", 100, false }),
        [](const ::testing::TestParamInfo<ActiveCase>& info) { return std::string(info.param.name); });

    TEST_P(OrganizedRegionActiveTest, ActiveSegmentFollowsTheIndex)
    {
        const auto& c = GetParam();
        OrganizedRegion organized = windowed();
        organized.active_segment_index = c.index;

        const RegionSegment* active = organized.get_active_segment();

        if (c.present) {
            ASSERT_NE(active, nullptr);
            EXPECT_EQ(active, &organized.segments[c.index]);
        } else {
            EXPECT_EQ(active, nullptr);
        }
    }

    TEST(OrganizedRegionTest, AnEmptyRegionHasNoActiveSegment)
    {
        const OrganizedRegion organized("empty", 0);

        EXPECT_EQ(organized.get_active_segment(), nullptr);
    }

    struct MergeOrderCase {
        std::string_view name;
        bool group_first;
        int shared;
    };

    class OrganizedRegionMergeTest : public ::testing::TestWithParam<MergeOrderCase> { };

    INSTANTIATE_TEST_SUITE_P(MergeOrder, OrganizedRegionMergeTest,
        ::testing::Values(
            MergeOrderCase { "GroupThenRegion", true, 4 },
            MergeOrderCase { "RegionThenGroup", false, 2 }),
        [](const ::testing::TestParamInfo<MergeOrderCase>& info) { return std::string(info.param.name); });

    TEST_P(OrganizedRegionMergeTest, TheLaterSourceWinsOnSharedKeys)
    {
        const auto& c = GetParam();
        const Facet<int> group_only;
        const Facet<int> region_only;
        const Facet<int> shared;
        RegionGroup group("chapters");
        group.set(group_only, 1);
        group.set(shared, 2);
        Region region(Coordinates { 10 });
        region.set(region_only, 3);
        region.set(shared, 4);
        OrganizedRegion organized("chapters", 0);

        if (c.group_first) {
            organized.merge(group);
            organized.merge(region);
        } else {
            organized.merge(region);
            organized.merge(group);
        }

        ASSERT_NE(organized.get(group_only), nullptr);
        ASSERT_NE(organized.get(region_only), nullptr);
        ASSERT_NE(organized.get(shared), nullptr);
        EXPECT_EQ(*organized.get(group_only), 1);
        EXPECT_EQ(*organized.get(region_only), 3);
        EXPECT_EQ(*organized.get(shared), c.shared);
    }

    TEST(OrganizedRegionTest, MergingLeavesTheSourcesAndIdentityUntouched)
    {
        const Facet<int> shared;
        RegionGroup group("chapters");
        group.set(shared, 2);
        OrganizedRegion organized("kept", 6);

        organized.merge(group);
        organized.set(shared, 9);

        ASSERT_NE(group.get(shared), nullptr);
        EXPECT_EQ(*group.get(shared), 2);
        EXPECT_EQ(organized.group_name, "kept");
        EXPECT_EQ(organized.region_index, 6U);
    }

    TEST(OrganizedRegionTest, MergingAnEmptySourceChangesNothing)
    {
        const Facet<int> key;
        OrganizedRegion organized("chapters", 0);
        organized.set(key, 5);

        organized.merge(RegionGroup("empty"));
        organized.merge(Region(Coordinates { 0 }));

        ASSERT_NE(organized.get(key), nullptr);
        EXPECT_EQ(*organized.get(key), 5);
    }

} // namespace
} // namespace MayaFlux::Test
