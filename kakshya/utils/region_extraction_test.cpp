#include "gtest/gtest.h"
#include "../../support/test_metadata.hpp"

#include "MayaFlux/Kakshya/Utils/DataUtils.hpp"
#include "MayaFlux/Kakshya/Utils/RegionUtils.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {
namespace {

    using Kakshya::DataDimension;
    using Kakshya::OrganizationStrategy;
    using Kakshya::Region;
    using Kakshya::RegionGroup;
    using Kakshya::RegionSegment;

    using Samples = std::vector<double>;
    using Channels = std::vector<Samples>;

    constexpr uint64_t k_frames = 5;

    std::vector<DataDimension> stereo_dimensions()
    {
        return { DataDimension::time(k_frames), DataDimension::channel(2) };
    }

    Samples interleaved_stereo()
    {
        Samples data(k_frames * 2);
        for (uint64_t frame = 0; frame < k_frames; ++frame) {
            for (uint64_t channel = 0; channel < 2; ++channel) {
                data[frame * 2 + channel] = static_cast<double>(10 * frame + channel);
            }
        }
        return data;
    }

    Channels planar_stereo()
    {
        Channels data(2, Samples(k_frames));
        for (uint64_t channel = 0; channel < 2; ++channel) {
            for (uint64_t frame = 0; frame < k_frames; ++frame) {
                data[channel][frame] = static_cast<double>(10 * frame + channel);
            }
        }
        return data;
    }

    std::vector<std::span<const double>> spans_of(const Channels& channels)
    {
        std::vector<std::span<const double>> spans;
        for (const auto& channel : channels) {
            spans.emplace_back(channel.data(), channel.size());
        }
        return spans;
    }

    std::span<const double> span_of(const Samples& samples)
    {
        return { samples.data(), samples.size() };
    }

    Kakshya::DataVariant variant_of(Samples samples)
    {
        return Kakshya::DataVariant(std::move(samples));
    }

    // ---- extract_region_data over one span -----------------------------------------------

    TEST(RegionExtractionSingleSpanTest, InclusiveEndsReturnEveryFrameInTheRange)
    {
        const auto data = interleaved_stereo();

        const auto out = Kakshya::extract_region_data<double>(
            span_of(data), Region::audio_span(1, 3, 0, 1), stereo_dimensions());

        EXPECT_EQ(out, (Samples { 10, 11, 20, 21, 30, 31 }));
    }

    TEST(RegionExtractionSingleSpanTest, APointRegionReturnsOneElement)
    {
        const auto data = interleaved_stereo();

        const auto out = Kakshya::extract_region_data<double>(
            span_of(data), Region::audio_point(2, 1), stereo_dimensions());

        EXPECT_EQ(out, (Samples { 21 }));
    }

    TEST(RegionExtractionSingleSpanTest, OneChannelWalksTheFramesInOrder)
    {
        const auto data = interleaved_stereo();

        const auto out = Kakshya::extract_region_data<double>(
            span_of(data), Region::audio_span(0, 4, 1, 1), stereo_dimensions());

        EXPECT_EQ(out, (Samples { 1, 11, 21, 31, 41 }));
    }

    TEST(RegionExtractionSingleSpanTest, TheWholeRegionReturnsTheWholeBuffer)
    {
        const auto data = interleaved_stereo();

        const auto out = Kakshya::extract_region_data<double>(
            span_of(data), Region::audio_span(0, 4, 0, 1), stereo_dimensions());

        EXPECT_EQ(out, data);
    }

    TEST(RegionExtractionSingleSpanTest, TheLastFrameIsInsideTheBuffer)
    {
        const auto data = interleaved_stereo();

        const auto out = Kakshya::extract_region_data<double>(
            span_of(data), Region::audio_span(4, 4, 0, 1), stereo_dimensions());

        EXPECT_EQ(out, (Samples { 40, 41 }));
    }

    TEST(RegionExtractionSingleSpanTest, ARegionPastTheEndThrows)
    {
        const auto data = interleaved_stereo();

        EXPECT_THROW((void)Kakshya::extract_region_data<double>(
                         span_of(data), Region::audio_span(3, 5, 0, 1), stereo_dimensions()),
            std::out_of_range);
    }

    TEST(RegionExtractionSingleSpanTest, AChannelPastTheLastThrows)
    {
        const auto data = interleaved_stereo();

        EXPECT_THROW((void)Kakshya::extract_region_data<double>(
                         span_of(data), Region::audio_span(0, 1, 0, 2), stereo_dimensions()),
            std::out_of_range);
    }

    // ---- extract_region_data over one span per channel -------------------------------------

    TEST(RegionExtractionPlanarTest, EachChannelSpanIsReadWithoutTheChannelDimension)
    {
        const auto data = planar_stereo();

        const auto out = Kakshya::extract_region_data<double>(
            spans_of(data), Region::audio_span(1, 3, 0, 1), stereo_dimensions());

        ASSERT_EQ(out.size(), 2U);
        EXPECT_EQ(out[0], (Samples { 10, 20, 30 }));
        EXPECT_EQ(out[1], (Samples { 11, 21, 31 }));
    }

    TEST(RegionExtractionPlanarTest, TheRegionChannelRangeSelectsTheChannels)
    {
        const auto data = planar_stereo();

        const auto out = Kakshya::extract_region_data<double>(
            spans_of(data), Region::audio_span(1, 3, 1, 1), stereo_dimensions());

        ASSERT_EQ(out.size(), 1U);
        EXPECT_EQ(out[0], (Samples { 11, 21, 31 }));
    }

    TEST(RegionExtractionPlanarTest, FlatteningJoinsTheChannelsEndToEnd)
    {
        const auto data = planar_stereo();

        const auto out = Kakshya::extract_region_data<double>(
            spans_of(data), Region::audio_span(1, 3, 0, 1), stereo_dimensions(), true);

        ASSERT_EQ(out.size(), 1U);
        EXPECT_EQ(out[0], (Samples { 10, 20, 30, 11, 21, 31 }));
    }

    TEST(RegionExtractionPlanarTest, TheOrganizationOverloadPicksTheLayout)
    {
        const auto planar = planar_stereo();
        const auto interleaved = interleaved_stereo();
        const auto region = Region::audio_span(1, 2, 0, 1);

        const auto from_planar = Kakshya::extract_region_data<double>(
            spans_of(planar), region, stereo_dimensions(), OrganizationStrategy::PLANAR);
        const auto from_interleaved = Kakshya::extract_region_data<double>(
            std::vector<std::span<const double>> { span_of(interleaved) },
            region, stereo_dimensions(), OrganizationStrategy::INTERLEAVED);

        ASSERT_EQ(from_planar.size(), 2U);
        EXPECT_EQ(from_planar[0], (Samples { 10, 20 }));
        EXPECT_EQ(from_planar[1], (Samples { 11, 21 }));
        ASSERT_EQ(from_interleaved.size(), 1U);
        EXPECT_EQ(from_interleaved[0], (Samples { 10, 11, 20, 21 }));
    }

    TEST(RegionExtractionPlanarTest, FlattenChannelsJoinsInOrder)
    {
        EXPECT_EQ(Kakshya::flatten_channels<double>(Channels { { 1, 2 }, {}, { 3 } }), (Samples { 1, 2, 3 }));
        EXPECT_TRUE(Kakshya::flatten_channels<double>(Channels {}).empty());
    }

    // ---- channel dimension helpers ---------------------------------------------------------

    TEST(RegionExtractionDimensionTest, TheChannelCoordinateIsRemovedWhereverItSits)
    {
        const std::vector<DataDimension> channel_first { DataDimension::channel(2), DataDimension::time(k_frames) };
        const Region region({ 0, 1 }, { 1, 3 });

        const auto without = Kakshya::remove_channel_dimension(region, channel_first);

        EXPECT_EQ(without.start_coordinates, (std::vector<uint64_t> { 1 }));
        EXPECT_EQ(without.end_coordinates, (std::vector<uint64_t> { 3 }));
    }

    TEST(RegionExtractionDimensionTest, ARegionWithoutAChannelDimensionIsUnchanged)
    {
        const std::vector<DataDimension> time_only { DataDimension::time(k_frames) };
        const auto region = Region::time_span(1, 3);

        EXPECT_EQ(Kakshya::remove_channel_dimension(region, time_only), region);
    }

    TEST(RegionExtractionDimensionTest, NonChannelDimensionsDropOnlyTheChannel)
    {
        const auto kept = Kakshya::get_non_channel_dimensions(stereo_dimensions());

        ASSERT_EQ(kept.size(), 1U);
        EXPECT_EQ(kept[0].role, DataDimension::Role::TIME);
        EXPECT_EQ(Kakshya::get_non_channel_dimensions({ DataDimension::time(3) }).size(), 1U);
    }

    // ---- extract_group_data ----------------------------------------------------------------

    RegionGroup late_then_early()
    {
        return RegionGroup("order", { Region::audio_span(3, 3, 0, 1), Region::audio_span(0, 0, 0, 1) });
    }

    TEST(RegionExtractionGroupTest, InterleavedRegionsJoinInGroupOrder)
    {
        const auto data = interleaved_stereo();

        const auto out = Kakshya::extract_group_data<double>(
            { span_of(data) }, late_then_early(), stereo_dimensions(), OrganizationStrategy::INTERLEAVED);

        ASSERT_EQ(out.size(), 1U);
        EXPECT_EQ(out[0], (Samples { 30, 31, 0, 1 }));
    }

    TEST(RegionExtractionGroupTest, PlanarRegionsJoinPerChannelInGroupOrder)
    {
        const auto data = planar_stereo();

        const auto out = Kakshya::extract_group_data<double>(
            spans_of(data), late_then_early(), stereo_dimensions(), OrganizationStrategy::PLANAR);

        ASSERT_EQ(out.size(), 2U);
        EXPECT_EQ(out[0], (Samples { 30, 0 }));
        EXPECT_EQ(out[1], (Samples { 31, 1 }));
    }

    TEST(RegionExtractionGroupTest, AnEmptyGroupGivesNothing)
    {
        const auto data = planar_stereo();

        EXPECT_TRUE(Kakshya::extract_group_data<double>(
            spans_of(data), RegionGroup("none"), stereo_dimensions(), OrganizationStrategy::PLANAR)
                .empty());
    }

    // ---- extract_segments_data -------------------------------------------------------------

    std::vector<RegionSegment> two_segments()
    {
        return { RegionSegment(Region::audio_span(1, 1, 0, 1)), RegionSegment(Region::audio_span(3, 3, 0, 1)) };
    }

    TEST(RegionExtractionSegmentTest, InterleavedSegmentsJoinInOrder)
    {
        const auto data = interleaved_stereo();

        const auto out = Kakshya::extract_segments_data<double>(
            two_segments(), { span_of(data) }, stereo_dimensions(), OrganizationStrategy::INTERLEAVED);

        ASSERT_EQ(out.size(), 1U);
        EXPECT_EQ(out[0], (Samples { 10, 11, 30, 31 }));
    }

    TEST(RegionExtractionSegmentTest, PlanarSegmentsJoinPerChannel)
    {
        const auto data = planar_stereo();

        const auto out = Kakshya::extract_segments_data<double>(
            two_segments(), spans_of(data), stereo_dimensions(), OrganizationStrategy::PLANAR);

        ASSERT_EQ(out.size(), 2U);
        EXPECT_EQ(out[0], (Samples { 10, 30 }));
        EXPECT_EQ(out[1], (Samples { 11, 31 }));
    }

    TEST(RegionExtractionSegmentTest, ACachedInterleavedSegmentIsUsedInsteadOfTheSource)
    {
        const auto data = interleaved_stereo();
        auto segments = two_segments();
        segments[0].is_cached = true;
        segments[0].cache.set(Kakshya::variant_cache(), { variant_of({ 7, 8, 9 }) });

        const auto out = Kakshya::extract_segments_data<double>(
            segments, { span_of(data) }, stereo_dimensions(), OrganizationStrategy::INTERLEAVED);

        ASSERT_EQ(out.size(), 1U);
        EXPECT_EQ(out[0], (Samples { 7, 8, 9, 30, 31 }));
    }

    TEST(RegionExtractionSegmentTest, ACacheHoldingOnlyAnotherPayloadFallsBackToTheSource)
    {
        const auto data = interleaved_stereo();
        auto segments = two_segments();
        const Facet<std::vector<int>> other;
        segments[0].mark_cached(other, { 7, 8, 9 });

        const auto out = Kakshya::extract_segments_data<double>(
            segments, { span_of(data) }, stereo_dimensions(), OrganizationStrategy::INTERLEAVED);

        ASSERT_EQ(out.size(), 1U);
        EXPECT_EQ(out[0], (Samples { 10, 11, 30, 31 }));
    }

    TEST(RegionExtractionSegmentTest, CachedPlanarSegmentsJoinPerChannel)
    {
        const auto data = interleaved_stereo();
        std::vector<RegionSegment> segments { RegionSegment(Region::audio_span(0, 1, 0, 1)), RegionSegment(Region::audio_span(2, 2, 0, 1)) };
        segments[0].is_cached = true;
        segments[0].cache.set(Kakshya::variant_cache(), { variant_of({ 1, 2 }), variant_of({ 3, 4 }) });
        segments[1].is_cached = true;
        segments[1].cache.set(Kakshya::variant_cache(), { variant_of({ 5 }), variant_of({ 6 }) });

        const auto out = Kakshya::extract_segments_data<double>(
            segments, { span_of(data) }, stereo_dimensions(), OrganizationStrategy::PLANAR);

        ASSERT_EQ(out.size(), 2U);
        EXPECT_EQ(out[0], (Samples { 1, 2, 5 }));
        EXPECT_EQ(out[1], (Samples { 3, 4, 6 }));
    }

    // ---- set_or_update_region_data ---------------------------------------------------------

    TEST(RegionExtractionWriteTest, WritesTheRegionAndLeavesTheRestUntouched)
    {
        Samples dest(k_frames * 2, -1.0);
        const Samples source { 1, 2, 3, 4 };

        Kakshya::set_or_update_region_data<double>(
            std::span<double>(dest), span_of(source), Region::audio_span(1, 2, 0, 1), stereo_dimensions());

        EXPECT_EQ(dest, (Samples { -1, -1, 1, 2, 3, 4, -1, -1, -1, -1 }));
    }

    TEST(RegionExtractionWriteTest, ASourceLongerThanTheRegionStopsAtTheRegion)
    {
        Samples dest(k_frames * 2, -1.0);
        const Samples source { 1, 2, 3, 4, 5, 6 };

        Kakshya::set_or_update_region_data<double>(
            std::span<double>(dest), span_of(source), Region::audio_span(1, 2, 0, 1), stereo_dimensions());

        EXPECT_EQ(dest, (Samples { -1, -1, 1, 2, 3, 4, -1, -1, -1, -1 }));
    }

    TEST(RegionExtractionWriteTest, ASourceShorterThanTheRegionFillsItsStart)
    {
        Samples dest(k_frames * 2, -1.0);
        const Samples source { 1, 2, 3 };

        Kakshya::set_or_update_region_data<double>(
            std::span<double>(dest), span_of(source), Region::audio_span(1, 2, 0, 1), stereo_dimensions());

        EXPECT_EQ(dest, (Samples { -1, -1, 1, 2, 3, -1, -1, -1, -1, -1 }));
    }

    TEST(RegionExtractionWriteTest, WhatIsWrittenIsWhatIsExtracted)
    {
        Samples dest(k_frames * 2, 0.0);
        const Samples source { 9, 8, 7, 6, 5, 4 };
        const auto region = Region::audio_span(0, 2, 0, 1);

        Kakshya::set_or_update_region_data<double>(std::span<double>(dest), span_of(source), region, stereo_dimensions());

        EXPECT_EQ(Kakshya::extract_region_data<double>(span_of(dest), region, stereo_dimensions()), source);
    }

    TEST(RegionExtractionWriteTest, PlanarChannelsReceiveTheirOwnSource)
    {
        Channels dest(2, Samples(k_frames, -1.0));
        std::vector<std::span<double>> dest_spans { std::span<double>(dest[0]), std::span<double>(dest[1]) };
        const Channels source { { 1, 2 }, { 3, 4 } };

        Kakshya::set_or_update_region_data<double>(
            dest_spans, spans_of(source), Region::audio_span(1, 2, 0, 1), stereo_dimensions());

        EXPECT_EQ(dest[0], (Samples { -1, 1, 2, -1, -1 }));
        EXPECT_EQ(dest[1], (Samples { -1, 3, 4, -1, -1 }));
    }

    // ---- positions and intersections -------------------------------------------------------

    TEST(RegionExtractionPositionTest, OutputRegionEndsOnTheLastFrameOfTheShape)
    {
        const auto region = Kakshya::calculate_output_region({ 10, 0 }, { 4, 2 });

        EXPECT_EQ(region.start_coordinates, (std::vector<uint64_t> { 10, 0 }));
        EXPECT_EQ(region.end_coordinates, (std::vector<uint64_t> { 13, 1 }));
    }

    TEST(RegionExtractionPositionTest, OutputRegionRejectsMismatchedRankAndZeroShape)
    {
        EXPECT_THROW((void)Kakshya::calculate_output_region({ 10 }, { 4, 2 }), std::invalid_argument);
        EXPECT_THROW((void)Kakshya::calculate_output_region({ 10, 0 }, { 4, 0 }), std::invalid_argument);
    }

    TEST(RegionExtractionPositionTest, TheFirstContainingRegionIsFound)
    {
        const std::vector<Region> regions { Region::time_span(0, 9), Region::time_span(5, 14) };

        EXPECT_EQ(Kakshya::find_region_for_position({ 7 }, regions), std::optional<size_t> { 0 });
        EXPECT_EQ(Kakshya::find_region_for_position({ 12 }, regions), std::optional<size_t> { 1 });
        EXPECT_EQ(Kakshya::find_region_for_position({ 9 }, regions), std::optional<size_t> { 0 });
        EXPECT_FALSE(Kakshya::find_region_for_position({ 20 }, regions).has_value());
    }

    TEST(RegionExtractionPositionTest, RectanglesIntersectOnTheirInclusiveEdges)
    {
        const auto a = Region::image_rect(0, 0, 4, 4);

        EXPECT_TRUE(Kakshya::regions_intersect(a, Region::image_rect(2, 2, 6, 6)));
        EXPECT_TRUE(Kakshya::regions_intersect(a, Region::image_rect(4, 4, 8, 8)));
        EXPECT_FALSE(Kakshya::regions_intersect(a, Region::image_rect(5, 5, 8, 8)));
        EXPECT_FALSE(Kakshya::regions_intersect(a, Region::image_rect(0, 5, 4, 8)));
    }

} // namespace
} // namespace MayaFlux::Test
