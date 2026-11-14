#include "gtest/gtest.h"
#include "../../support/test_metadata.hpp"

#include "MayaFlux/Kakshya/Region/Region.hpp"
#include "MayaFlux/Kakshya/Region/RegionHash.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {
namespace {

    using Kakshya::DataModality;
    using Kakshya::Region;
    using Kakshya::RegionHash;

    using Coordinates = std::vector<uint64_t>;
    using Offset = std::vector<int64_t>;

    Region span_of(Coordinates start, Coordinates end)
    {
        return Region(std::move(start), std::move(end));
    }

    struct FactoryCase {
        std::string_view name;
        Region region;
        Coordinates start;
        Coordinates end;
        std::optional<std::string> label;
        std::optional<DataModality> modality;
        bool point;
    };

    class RegionFactoryTest : public ::testing::TestWithParam<FactoryCase> { };

    INSTANTIATE_TEST_SUITE_P(Factories, RegionFactoryTest,
        ::testing::Values(
            FactoryCase { "TimePoint", Region::time_point(500, "onset"), { 500 }, { 500 }, "onset", std::nullopt, true },
            FactoryCase { "TimeSpan", Region::time_span(50, 150, "span"), { 50 }, { 150 }, "span", std::nullopt, false },
            FactoryCase { "AudioPoint", Region::audio_point(100, 1, "transient"), { 100, 1 }, { 100, 1 }, "transient", DataModality::AUDIO_1D, true },
            FactoryCase { "AudioSpan", Region::audio_span(0, 1000, 0, 1, "audio"), { 0, 0 }, { 1000, 1 }, "audio", DataModality::AUDIO_MULTICHANNEL, false },
            FactoryCase { "ImageRect", Region::image_rect(10, 20, 100, 200, "roi"), { 10, 20 }, { 100, 200 }, "roi", std::nullopt, false },
            FactoryCase { "VideoRegion", Region::video_region(0, 30, 50, 60, 150, 160, "scene"), { 0, 50, 60 }, { 30, 150, 160 }, "scene", std::nullopt, false },
            FactoryCase { "UnlabeledTimePoint", Region::time_point(7), { 7 }, { 7 }, std::nullopt, std::nullopt, true },
            FactoryCase { "UnlabeledAudioSpan", Region::audio_span(0, 9, 0, 0), { 0, 0 }, { 9, 0 }, std::nullopt, DataModality::AUDIO_MULTICHANNEL, false }),
        [](const ::testing::TestParamInfo<FactoryCase>& info) { return std::string(info.param.name); });

    TEST_P(RegionFactoryTest, CoordinatesFollowTheArguments)
    {
        const auto& c = GetParam();

        EXPECT_EQ(c.region.start_coordinates, c.start);
        EXPECT_EQ(c.region.end_coordinates, c.end);
    }

    TEST_P(RegionFactoryTest, LabelIsSetOnlyWhenGiven)
    {
        const auto& c = GetParam();

        EXPECT_EQ(c.region.label, c.label);
    }

    TEST_P(RegionFactoryTest, ModalityMatchesTheFactory)
    {
        const auto& c = GetParam();

        EXPECT_EQ(c.region.modality, c.modality);
    }

    TEST_P(RegionFactoryTest, IsPointWhenStartEqualsEnd)
    {
        const auto& c = GetParam();

        EXPECT_EQ(c.region.is_point(), c.point);
    }

    struct GeometryCase {
        std::string_view name;
        Region region;
        Coordinates spans;
        uint64_t volume;
    };

    class RegionGeometryTest : public ::testing::TestWithParam<GeometryCase> { };

    INSTANTIATE_TEST_SUITE_P(Geometry, RegionGeometryTest,
        ::testing::Values(
            GeometryCase { "Point", Region(Coordinates { 100 }), { 1 }, 1 },
            GeometryCase { "TimeSpan", Region::time_span(50, 150), { 101 }, 101 },
            GeometryCase { "AudioSpan", Region::audio_span(0, 1000, 0, 1), { 1001, 2 }, 2002 },
            GeometryCase { "ThreeDimensional", span_of({ 10, 20, 30 }, { 50, 60, 70 }), { 41, 41, 41 }, 68921 }),
        [](const ::testing::TestParamInfo<GeometryCase>& info) { return std::string(info.param.name); });

    TEST_P(RegionGeometryTest, SpanAndDurationCountInclusiveElements)
    {
        const auto& c = GetParam();

        for (size_t i = 0; i < c.spans.size(); ++i) {
            EXPECT_EQ(c.region.get_span(static_cast<uint32_t>(i)), c.spans[i]);
            EXPECT_EQ(c.region.get_duration(static_cast<uint32_t>(i)), c.spans[i]);
        }
    }

    TEST_P(RegionGeometryTest, VolumeIsTheProductOfSpans)
    {
        const auto& c = GetParam();

        EXPECT_EQ(c.region.get_volume(), c.volume);
    }

    TEST_P(RegionGeometryTest, DimensionsPastTheEndHaveNoExtent)
    {
        const auto& c = GetParam();
        const auto past = static_cast<uint32_t>(c.spans.size());

        EXPECT_EQ(c.region.get_span(past), 0U);
        EXPECT_EQ(c.region.get_duration(past), 0U);
    }

    struct ContainmentCase {
        std::string_view name;
        Region region;
        Coordinates position;
        bool contained;
    };

    class RegionContainmentTest : public ::testing::TestWithParam<ContainmentCase> { };

    INSTANTIATE_TEST_SUITE_P(Containment, RegionContainmentTest,
        ::testing::Values(
            ContainmentCase { "PointItself", Region(Coordinates { 100 }), { 100 }, true },
            ContainmentCase { "PointBefore", Region(Coordinates { 100 }), { 99 }, false },
            ContainmentCase { "PointAfter", Region(Coordinates { 100 }), { 101 }, false },
            ContainmentCase { "PointWrongRank", Region(Coordinates { 100 }), { 100, 200 }, false },
            ContainmentCase { "SpanInterior", Region::time_span(50, 150), { 75 }, true },
            ContainmentCase { "SpanStart", Region::time_span(50, 150), { 50 }, true },
            ContainmentCase { "SpanEnd", Region::time_span(50, 150), { 150 }, true },
            ContainmentCase { "SpanBefore", Region::time_span(50, 150), { 49 }, false },
            ContainmentCase { "SpanAfter", Region::time_span(50, 150), { 151 }, false },
            ContainmentCase { "BoxInterior", span_of({ 10, 20, 30 }, { 50, 60, 70 }), { 25, 40, 50 }, true },
            ContainmentCase { "BoxLowerCorner", span_of({ 10, 20, 30 }, { 50, 60, 70 }), { 10, 20, 30 }, true },
            ContainmentCase { "BoxUpperCorner", span_of({ 10, 20, 30 }, { 50, 60, 70 }), { 50, 60, 70 }, true },
            ContainmentCase { "BoxOutsideFirst", span_of({ 10, 20, 30 }, { 50, 60, 70 }), { 9, 40, 50 }, false },
            ContainmentCase { "BoxOutsideSecond", span_of({ 10, 20, 30 }, { 50, 60, 70 }), { 25, 19, 50 }, false },
            ContainmentCase { "BoxOutsideThird", span_of({ 10, 20, 30 }, { 50, 60, 70 }), { 25, 40, 71 }, false },
            ContainmentCase { "BoxMissingDimension", span_of({ 10, 20, 30 }, { 50, 60, 70 }), { 25, 40 }, false },
            ContainmentCase { "AudioInsideChannels", Region::audio_span(100, 200, 0, 2), { 150, 1 }, true },
            ContainmentCase { "AudioOutsideChannels", Region::audio_span(100, 200, 0, 2), { 150, 3 }, false }),
        [](const ::testing::TestParamInfo<ContainmentCase>& info) { return std::string(info.param.name); });

    TEST_P(RegionContainmentTest, ContainsFollowsInclusiveBounds)
    {
        const auto& c = GetParam();

        EXPECT_EQ(c.region.contains(c.position), c.contained);
    }

    struct OverlapCase {
        std::string_view name;
        Region first;
        Region second;
        bool overlapping;
    };

    class RegionOverlapTest : public ::testing::TestWithParam<OverlapCase> { };

    INSTANTIATE_TEST_SUITE_P(Overlap, RegionOverlapTest,
        ::testing::Values(
            OverlapCase { "SpanWithItself", Region::time_span(50, 150), Region::time_span(50, 150), true },
            OverlapCase { "PartialOverlap", Region::time_span(50, 150), Region::time_span(75, 125), true },
            OverlapCase { "TouchingAtEnd", Region::time_span(50, 150), Region::time_span(150, 175), true },
            OverlapCase { "Disjoint", Region::time_span(50, 150), Region::time_span(200, 250), false },
            OverlapCase { "PointInsideSpan", Region::time_point(125), Region::time_span(100, 200), true },
            OverlapCase { "PointOutsideSpan", Region::time_point(350), Region::time_span(100, 200), false },
            OverlapCase { "PointWithItself", Region::time_point(125), Region::time_point(125), true },
            OverlapCase { "BoxesOverlap", span_of({ 10, 20, 30 }, { 50, 60, 70 }), span_of({ 40, 50, 60 }, { 80, 90, 100 }), true },
            OverlapCase { "BoxesApart", span_of({ 10, 20, 30 }, { 50, 60, 70 }), span_of({ 100, 100, 100 }, { 200, 200, 200 }), false },
            OverlapCase { "RankMismatch", Region::time_span(50, 150), span_of({ 50, 0 }, { 150, 1 }), false }),
        [](const ::testing::TestParamInfo<OverlapCase>& info) { return std::string(info.param.name); });

    TEST_P(RegionOverlapTest, OverlapIsSymmetric)
    {
        const auto& c = GetParam();

        EXPECT_EQ(c.first.overlaps(c.second), c.overlapping);
        EXPECT_EQ(c.second.overlaps(c.first), c.overlapping);
    }

    struct TranslateCase {
        std::string_view name;
        Region region;
        Offset offset;
        Coordinates start;
        Coordinates end;
    };

    class RegionTranslateTest : public ::testing::TestWithParam<TranslateCase> { };

    INSTANTIATE_TEST_SUITE_P(Translate, RegionTranslateTest,
        ::testing::Values(
            TranslateCase { "Forward", Region::time_span(50, 150), { 10 }, { 60 }, { 160 } },
            TranslateCase { "Backward", Region::time_span(100, 200), { -50 }, { 50 }, { 150 } },
            TranslateCase { "ClampsAtZero", Region::time_span(5, 10), { -10 }, { 0 }, { 0 } },
            TranslateCase { "MixedDirections", span_of({ 10, 20, 30 }, { 50, 60, 70 }), { 5, -5, 10 }, { 15, 15, 40 }, { 55, 55, 80 } },
            TranslateCase { "ShortOffsetLeavesTrailingDimensions", span_of({ 10, 20, 30 }, { 50, 60, 70 }), { 5 }, { 15, 20, 30 }, { 55, 60, 70 } },
            TranslateCase { "AudioChannelDown", Region::audio_span(100, 200, 1, 2), { 10, -1 }, { 110, 0 }, { 210, 1 } }),
        [](const ::testing::TestParamInfo<TranslateCase>& info) { return std::string(info.param.name); });

    TEST_P(RegionTranslateTest, MovesBothCorners)
    {
        const auto& c = GetParam();

        const Region moved = c.region.translate(c.offset);

        EXPECT_EQ(moved.start_coordinates, c.start);
        EXPECT_EQ(moved.end_coordinates, c.end);
    }

    TEST_P(RegionTranslateTest, LeavesTheSourceUntouched)
    {
        const auto& c = GetParam();
        const Region before = c.region;

        const Region moved = c.region.translate(c.offset);

        EXPECT_EQ(c.region, before);
    }

    TEST(RegionTranslateKeepsMembersTest, LabelModalityAndTypedValuesTravel)
    {
        const Facet<double> energy;
        Region region = Region::audio_span(0, 100, 0, 1, "tagged");
        region.set(energy, 0.5);

        const Region moved = region.translate({ 10, 0 });

        EXPECT_EQ(moved.label, "tagged");
        EXPECT_EQ(moved.modality, DataModality::AUDIO_MULTICHANNEL);
        ASSERT_NE(moved.get(energy), nullptr);
        EXPECT_DOUBLE_EQ(*moved.get(energy), 0.5);
    }

    struct ScaleCase {
        std::string_view name;
        Region region;
        std::vector<double> factors;
        Coordinates start;
        Coordinates end;
    };

    class RegionScaleTest : public ::testing::TestWithParam<ScaleCase> { };

    INSTANTIATE_TEST_SUITE_P(Scale, RegionScaleTest,
        ::testing::Values(
            ScaleCase { "Identity", Region::time_span(50, 150), { 1.0 }, { 50 }, { 150 } },
            ScaleCase { "Doubled", Region::time_span(50, 150), { 2.0 }, { 0 }, { 200 } },
            ScaleCase { "Halved", Region::time_span(50, 150), { 0.5 }, { 75 }, { 125 } },
            ScaleCase { "DoubledAroundCentre", Region::time_span(100, 200), { 2.0 }, { 50 }, { 250 } },
            ScaleCase { "HalvedAroundCentre", Region::time_span(100, 200), { 0.5 }, { 125 }, { 175 } },
            ScaleCase { "PerDimension", span_of({ 100, 100 }, { 200, 200 }), { 2.0, 0.5 }, { 50, 125 }, { 250, 175 } }),
        [](const ::testing::TestParamInfo<ScaleCase>& info) { return std::string(info.param.name); });

    TEST_P(RegionScaleTest, ScalesAboutTheCentre)
    {
        const auto& c = GetParam();

        const Region scaled = c.region.scale(c.factors);

        EXPECT_EQ(scaled.start_coordinates, c.start);
        EXPECT_EQ(scaled.end_coordinates, c.end);
    }

    struct EqualityCase {
        std::string_view name;
        Region first;
        Region second;
        bool equal;
    };

    class RegionEqualityTest : public ::testing::TestWithParam<EqualityCase> { };

    INSTANTIATE_TEST_SUITE_P(Equality, RegionEqualityTest,
        ::testing::Values(
            EqualityCase { "SamePoint", Region(Coordinates { 100 }), Region(Coordinates { 100 }), true },
            EqualityCase { "DifferentPoint", Region(Coordinates { 100 }), Region(Coordinates { 101 }), false },
            EqualityCase { "SameSpan", Region::time_span(50, 150), Region::time_span(50, 150), true },
            EqualityCase { "DifferentEnd", Region::time_span(50, 150), Region::time_span(50, 151), false },
            EqualityCase { "SameBox", span_of({ 10, 20, 30 }, { 50, 60, 70 }), span_of({ 10, 20, 30 }, { 50, 60, 70 }), true },
            EqualityCase { "DifferentBoxCorner", span_of({ 10, 20, 30 }, { 50, 60, 70 }), span_of({ 10, 20, 30 }, { 50, 60, 71 }), false },
            EqualityCase { "DifferentRank", Region(Coordinates { 100 }), Region(Coordinates { 100, 0 }), false },
            EqualityCase { "LabelsAreIgnored", Region::time_point(100, "a"), Region::time_point(100, "b"), true }),
        [](const ::testing::TestParamInfo<EqualityCase>& info) { return std::string(info.param.name); });

    TEST_P(RegionEqualityTest, EqualityComparesCoordinates)
    {
        const auto& c = GetParam();

        EXPECT_EQ(c.first == c.second, c.equal);
        EXPECT_EQ(c.first != c.second, !c.equal);
    }

    TEST_P(RegionEqualityTest, EqualRegionsHashAlike)
    {
        const auto& c = GetParam();

        if (c.equal) {
            EXPECT_EQ(RegionHash {}(c.first), RegionHash {}(c.second));
        }
    }

    TEST(RegionFacetTest, WritesOnACopyLeaveTheOriginalAndIdentityUnchanged)
    {
        const Facet<int> priority;
        Region region(Coordinates { 100 });
        region.set(priority, 1);
        Region copy = region;
        copy.set(priority, 2);

        ASSERT_NE(region.get(priority), nullptr);
        ASSERT_NE(copy.get(priority), nullptr);
        EXPECT_EQ(*region.get(priority), 1);
        EXPECT_EQ(*copy.get(priority), 2);
        EXPECT_EQ(copy, region);
        EXPECT_EQ(RegionHash {}(copy), RegionHash {}(region));
    }

    TEST(RegionFacetTest, ReplaceTransfersAllTypedKeysAndLeavesOtherMembersUntouched)
    {
        const Facet<int> previous;
        const Facet<int> first;
        const Facet<std::string> second;
        Region region(Coordinates { 100 });
        region.label = "marker";
        region.modality = DataModality::AUDIO_1D;
        region.set(previous, 1);
        Facets source;
        source.set(first, 2);
        source.set(second, std::string { "chapter" });

        region.replace(source);

        EXPECT_EQ(region.get(previous), nullptr);
        ASSERT_NE(region.get(first), nullptr);
        ASSERT_NE(region.get(second), nullptr);
        EXPECT_EQ(*region.get(first), 2);
        EXPECT_EQ(*region.get(second), "chapter");
        EXPECT_EQ(region.label, "marker");
        EXPECT_EQ(region.modality, DataModality::AUDIO_1D);
        EXPECT_EQ(region.start_coordinates, (Coordinates { 100 }));
    }

    TEST(RegionFacetTest, ReplaceWithAnEmptyBagClearsEveryTypedValue)
    {
        const Facet<int> key;
        Region region(Coordinates { 1 });
        region.set(key, 3);

        region.replace(Facets {});

        EXPECT_EQ(region.get(key), nullptr);
    }

} // namespace
} // namespace MayaFlux::Test
