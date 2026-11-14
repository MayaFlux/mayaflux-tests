#include "gtest/gtest.h"
#include "../../support/test_metadata.hpp"

#include "MayaFlux/Kakshya/Region/RegionGroup.hpp"
#include "MayaFlux/Kakshya/Utils/RegionUtils.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {
namespace {

    using Kakshya::Region;
    using Kakshya::RegionGroup;

    using Coordinates = std::vector<uint64_t>;
    using Labels = std::vector<std::string>;

    RegionGroup utility_group()
    {
        return RegionGroup("utility_test",
            { Region::time_span(100, 200, "region1"),
                Region::time_span(150, 250, "region2"),
                Region::time_span(300, 400, "region3"),
                Region::time_point(125, "point1"),
                Region::time_point(350, "point2") });
    }

    struct TranslateCase {
        std::string_view name;
        Region region;
        std::vector<int64_t> offset;
        Coordinates start;
        Coordinates end;
    };

    class RegionUtilsTranslateTest : public ::testing::TestWithParam<TranslateCase> { };

    INSTANTIATE_TEST_SUITE_P(Translate, RegionUtilsTranslateTest,
        ::testing::Values(
            TranslateCase { "Forward", Region::time_span(100, 200), { 50 }, { 150 }, { 250 } },
            TranslateCase { "Backward", Region::time_span(100, 200), { -50 }, { 50 }, { 150 } },
            TranslateCase { "MultiDimensional", Region::audio_span(100, 200, 1, 2), { 10, -1 }, { 110, 0 }, { 210, 1 } }),
        [](const ::testing::TestParamInfo<TranslateCase>& info) { return std::string(info.param.name); });

    TEST_P(RegionUtilsTranslateTest, MatchesTheRegionMember)
    {
        const auto& c = GetParam();

        const Region moved = Kakshya::translate_region(c.region, c.offset);

        EXPECT_EQ(moved.start_coordinates, c.start);
        EXPECT_EQ(moved.end_coordinates, c.end);
        EXPECT_EQ(moved, c.region.translate(c.offset));
    }

    struct ScaleCase {
        std::string_view name;
        std::vector<double> factors;
        Coordinates start;
        Coordinates end;
    };

    class RegionUtilsScaleTest : public ::testing::TestWithParam<ScaleCase> { };

    INSTANTIATE_TEST_SUITE_P(Scale, RegionUtilsScaleTest,
        ::testing::Values(
            ScaleCase { "Doubled", { 2.0 }, { 50 }, { 250 } },
            ScaleCase { "Halved", { 0.5 }, { 125 }, { 175 } },
            ScaleCase { "Identity", { 1.0 }, { 100 }, { 200 } }),
        [](const ::testing::TestParamInfo<ScaleCase>& info) { return std::string(info.param.name); });

    TEST_P(RegionUtilsScaleTest, ScalesAboutTheCentre)
    {
        const auto& c = GetParam();

        const Region scaled = Kakshya::scale_region(Region::time_span(100, 200), c.factors);

        EXPECT_EQ(scaled.start_coordinates, c.start);
        EXPECT_EQ(scaled.end_coordinates, c.end);
    }

    struct BoundsCase {
        std::string_view name;
        RegionGroup group;
        Coordinates start;
        Coordinates end;
    };

    class RegionUtilsBoundsTest : public ::testing::TestWithParam<BoundsCase> { };

    INSTANTIATE_TEST_SUITE_P(Bounds, RegionUtilsBoundsTest,
        ::testing::Values(
            BoundsCase { "MixedSpansAndPoints", utility_group(), { 100 }, { 400 } },
            BoundsCase { "SinglePoint", RegionGroup("single", { Region::time_point(125, "point1") }), { 125 }, { 125 } },
            BoundsCase { "Empty", RegionGroup("empty"), {}, {} }),
        [](const ::testing::TestParamInfo<BoundsCase>& info) { return std::string(info.param.name); });

    TEST_P(RegionUtilsBoundsTest, BoundingRegionSpansEveryMember)
    {
        const auto& c = GetParam();

        const Region bounds = Kakshya::get_bounding_region(c.group);

        EXPECT_EQ(bounds.start_coordinates, c.start);
        EXPECT_EQ(bounds.end_coordinates, c.end);
    }

    TEST_P(RegionUtilsBoundsTest, AgreesWithTheGroupMember)
    {
        const auto& c = GetParam();

        EXPECT_EQ(Kakshya::get_bounding_region(c.group), c.group.get_bounding_region());
    }

    struct SortCase {
        std::string_view name;
        size_t dimension;
        Labels expected;
    };

    class RegionUtilsSortTest : public ::testing::TestWithParam<SortCase> { };

    INSTANTIATE_TEST_SUITE_P(Sort, RegionUtilsSortTest,
        ::testing::Values(
            SortCase { "ByTime", 0, { "region1", "point1", "region2", "region3", "point2" } }),
        [](const ::testing::TestParamInfo<SortCase>& info) { return std::string(info.param.name); });

    TEST_P(RegionUtilsSortTest, OrdersByTheStartCoordinate)
    {
        const auto& c = GetParam();
        const RegionGroup source = utility_group();
        std::vector<Region> regions { source.regions[2], source.regions[0], source.regions[1], source.regions[4], source.regions[3] };

        Kakshya::sort_regions_by_dimension(regions, c.dimension);

        Labels labels;
        for (const auto& region : regions) {
            labels.push_back(region.label.value_or(""));
        }
        EXPECT_EQ(labels, c.expected);
    }

    struct FindCase {
        std::string_view name;
        std::string label;
        Coordinates position;
        size_t by_label;
        size_t by_position;
    };

    class RegionUtilsFindTest : public ::testing::TestWithParam<FindCase> { };

    INSTANTIATE_TEST_SUITE_P(Find, RegionUtilsFindTest,
        ::testing::Values(
            FindCase { "SharedSpan", "region1", { 175 }, 1, 2 },
            FindCase { "PointOnly", "point1", { 125 }, 1, 2 },
            FindCase { "Nothing", "missing", { 1000 }, 0, 0 }),
        [](const ::testing::TestParamInfo<FindCase>& info) { return std::string(info.param.name); });

    TEST_P(RegionUtilsFindTest, FindsByLabelAndByPosition)
    {
        const auto& c = GetParam();
        const RegionGroup group = utility_group();

        EXPECT_EQ(Kakshya::find_regions_with_label(group, c.label).size(), c.by_label);
        EXPECT_EQ(Kakshya::find_regions_containing_coordinates(group, c.position).size(), c.by_position);
    }

} // namespace
} // namespace MayaFlux::Test
