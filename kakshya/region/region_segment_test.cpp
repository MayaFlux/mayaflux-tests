#include "gtest/gtest.h"

#include "MayaFlux/Kakshya/Region/RegionSegment.hpp"

namespace MayaFlux::Test {
namespace {

    using Kakshya::DataVariant;
    using Kakshya::Region;
    using Kakshya::RegionCache;
    using Kakshya::RegionSegment;
    using Kakshya::RegionState;

    using Coordinates = std::vector<uint64_t>;

    struct FullSegmentCase {
        std::string_view name;
        Region region;
        Coordinates size;
        uint64_t total;
    };

    class RegionSegmentFullTest : public ::testing::TestWithParam<FullSegmentCase> { };

    INSTANTIATE_TEST_SUITE_P(FullSegment, RegionSegmentFullTest,
        ::testing::Values(
            FullSegmentCase { "TimeSpan", Region::time_span(10, 19), { 10 }, 10 },
            FullSegmentCase { "AudioSpan", Region::audio_span(0, 1000, 0, 1), { 1001, 2 }, 2002 },
            FullSegmentCase { "Point", Region::time_point(5), { 1 }, 1 },
            FullSegmentCase { "VideoRegion", Region::video_region(0, 9, 0, 0, 3, 1), { 10, 4, 2 }, 80 }),
        [](const ::testing::TestParamInfo<FullSegmentCase>& info) { return std::string(info.param.name); });

    TEST_P(RegionSegmentFullTest, CoversTheWholeRegionFromItsOrigin)
    {
        const auto& c = GetParam();

        const RegionSegment segment(c.region);

        EXPECT_EQ(segment.source_region, c.region);
        EXPECT_EQ(segment.offset_in_region, Coordinates(c.size.size(), 0));
        EXPECT_EQ(segment.segment_size, c.size);
        EXPECT_EQ(segment.get_total_elements(), c.total);
    }

    TEST_P(RegionSegmentFullTest, StartsIdleUncachedAndAtTheOrigin)
    {
        const auto& c = GetParam();

        const RegionSegment segment(c.region);

        EXPECT_FALSE(segment.is_cached);
        EXPECT_FALSE(segment.is_active);
        EXPECT_EQ(segment.state, RegionState::IDLE);
        EXPECT_EQ(segment.current_position, Coordinates(c.size.size(), 0));
    }

    struct WindowCase {
        std::string_view name;
        Coordinates position;
        bool contained;
    };

    class RegionSegmentWindowTest : public ::testing::TestWithParam<WindowCase> { };

    INSTANTIATE_TEST_SUITE_P(Window, RegionSegmentWindowTest,
        ::testing::Values(
            WindowCase { "Interior", { 200, 1 }, true },
            WindowCase { "LowerCorner", { 100, 0 }, true },
            WindowCase { "UpperCorner", { 599, 1 }, true },
            WindowCase { "BeforeOffset", { 99, 0 }, false },
            WindowCase { "PastSize", { 600, 0 }, false },
            WindowCase { "PastSecondDimension", { 200, 2 }, false },
            WindowCase { "TooFewDimensions", { 200 }, false },
            WindowCase { "TooManyDimensions", { 200, 1, 0 }, false }),
        [](const ::testing::TestParamInfo<WindowCase>& info) { return std::string(info.param.name); });

    TEST_P(RegionSegmentWindowTest, ContainsPositionsInsideTheOffsetWindow)
    {
        const auto& c = GetParam();
        const RegionSegment segment(Region::audio_span(0, 1000, 0, 1), { 100, 0 }, { 500, 2 });

        EXPECT_EQ(segment.contains_position(c.position), c.contained);
    }

    TEST(RegionSegmentTest, AWindowedSegmentCountsOnlyItsOwnElements)
    {
        const RegionSegment segment(Region::audio_span(0, 1000, 0, 1), { 100, 0 }, { 500, 2 });

        EXPECT_EQ(segment.offset_in_region, (Coordinates { 100, 0 }));
        EXPECT_EQ(segment.segment_size, (Coordinates { 500, 2 }));
        EXPECT_EQ(segment.get_total_elements(), 1000U);
    }

    TEST(RegionSegmentTest, MarkActiveAndInactiveMoveTheState)
    {
        RegionSegment segment(Region::time_span(0, 10));

        segment.mark_active();
        EXPECT_TRUE(segment.is_active);
        EXPECT_EQ(segment.state, RegionState::ACTIVE);

        segment.mark_inactive();
        EXPECT_FALSE(segment.is_active);
        EXPECT_EQ(segment.state, RegionState::IDLE);
    }

    TEST(RegionSegmentTest, AnUncachedSegmentReportsNoCacheAge)
    {
        const RegionSegment segment(Region::time_span(0, 10));

        EXPECT_DOUBLE_EQ(segment.get_cache_age_seconds(), -1.0);
    }

    TEST(RegionSegmentTest, MarkCachedStoresTheDataAndBecomesReady)
    {
        RegionSegment segment(Region::time_span(0, 2));

        segment.mark_cached({ DataVariant { std::vector<double> { 1.0, 2.0, 3.0 } } });

        EXPECT_TRUE(segment.is_cached);
        EXPECT_EQ(segment.state, RegionState::READY);
        EXPECT_FALSE(segment.cache.is_dirty);
        EXPECT_GE(segment.get_cache_age_seconds(), 0.0);
        ASSERT_EQ(segment.cache.data.size(), 1U);
        EXPECT_EQ(segment.cache.source_region, segment.source_region);
    }

    TEST(RegionSegmentTest, ClearCacheReturnsAReadySegmentToIdle)
    {
        RegionSegment segment(Region::time_span(0, 2));
        segment.mark_cached({ DataVariant { std::vector<double> { 1.0 } } });

        segment.clear_cache();

        EXPECT_FALSE(segment.is_cached);
        EXPECT_EQ(segment.state, RegionState::IDLE);
        EXPECT_TRUE(segment.cache.data.empty());
        EXPECT_DOUBLE_EQ(segment.get_cache_age_seconds(), -1.0);
    }

    TEST(RegionSegmentTest, ClearCacheLeavesAnActiveStateAlone)
    {
        RegionSegment segment(Region::time_span(0, 2));
        segment.mark_cached({ DataVariant { std::vector<double> { 1.0 } } });
        segment.mark_active();

        segment.clear_cache();

        EXPECT_EQ(segment.state, RegionState::ACTIVE);
    }

    struct AdvanceCase {
        std::string_view name;
        Coordinates from;
        uint64_t steps;
        uint32_t dimension;
        bool more;
        Coordinates to;
    };

    class RegionSegmentAdvanceTest : public ::testing::TestWithParam<AdvanceCase> { };

    INSTANTIATE_TEST_SUITE_P(Advance, RegionSegmentAdvanceTest,
        ::testing::Values(
            AdvanceCase { "WithinTheFirstDimension", { 0, 0 }, 10, 0, true, { 10, 0 } },
            AdvanceCase { "WrapsIntoTheNextDimension", { 1000, 0 }, 1, 0, true, { 0, 1 } },
            AdvanceCase { "RunsOffTheEnd", { 0, 1 }, 1001, 0, false, { 0, 2 } },
            AdvanceCase { "NoSuchDimension", { 0, 0 }, 1, 10, false, { 0, 0 } }),
        [](const ::testing::TestParamInfo<AdvanceCase>& info) { return std::string(info.param.name); });

    TEST_P(RegionSegmentAdvanceTest, StepsThroughTheSegmentLikeACounter)
    {
        const auto& c = GetParam();
        RegionSegment segment(Region::audio_span(0, 1000, 0, 1));
        segment.current_position = c.from;

        const bool more = segment.advance_position(c.steps, c.dimension);

        EXPECT_EQ(more, c.more);
        EXPECT_EQ(segment.current_position, c.to);
    }

    struct EndCase {
        std::string_view name;
        Coordinates position;
        bool at_end;
    };

    class RegionSegmentEndTest : public ::testing::TestWithParam<EndCase> { };

    INSTANTIATE_TEST_SUITE_P(End, RegionSegmentEndTest,
        ::testing::Values(
            EndCase { "AtTheOrigin", { 0, 0 }, false },
            EndCase { "OnTheLastRow", { 5, 1 }, false },
            EndCase { "PastTheLastRow", { 0, 2 }, true }),
        [](const ::testing::TestParamInfo<EndCase>& info) { return std::string(info.param.name); });

    TEST_P(RegionSegmentEndTest, EndIsDecidedByTheLastDimension)
    {
        const auto& c = GetParam();
        RegionSegment segment(Region::audio_span(0, 1000, 0, 1));
        segment.current_position = c.position;

        EXPECT_EQ(segment.is_at_end(), c.at_end);
    }

    TEST(RegionSegmentTest, ResetPositionReturnsToTheOrigin)
    {
        RegionSegment segment(Region::audio_span(0, 1000, 0, 1));
        segment.current_position = { 50, 1 };

        segment.reset_position();

        EXPECT_EQ(segment.current_position, (Coordinates { 0, 0 }));
    }

    TEST(RegionSegmentTest, TypedValuesReplaceOnSet)
    {
        RegionSegment segment(Region::time_span(0, 10));
        const Facet<double> gain;

        segment.set(gain, 0.25);
        segment.set(gain, 0.5);

        ASSERT_NE(segment.get(gain), nullptr);
        EXPECT_DOUBLE_EQ(*segment.get(gain), 0.5);
    }

    struct CacheDataCase {
        std::string_view name;
        DataVariant data;
        size_t size;
    };

    class RegionCacheDataTest : public ::testing::TestWithParam<CacheDataCase> { };

    INSTANTIATE_TEST_SUITE_P(CacheData, RegionCacheDataTest,
        ::testing::Values(
            CacheDataCase { "Doubles", DataVariant { std::vector<double> { 1.0, 2.0, 3.0, 4.0, 5.0 } }, 5 },
            CacheDataCase { "Floats", DataVariant { std::vector<float> { 1.0F, 2.0F, 3.0F } }, 3 },
            CacheDataCase { "Complex", DataVariant { std::vector<std::complex<double>> { { 1.0, 0.0 }, { 0.0, 1.0 }, { -1.0, 0.0 } } }, 3 },
            CacheDataCase { "Integers", DataVariant { std::vector<uint16_t> { 100, 200, 300 } }, 3 }),
        [](const ::testing::TestParamInfo<CacheDataCase>& info) { return std::string(info.param.name); });

    TEST_P(RegionCacheDataTest, TheCacheKeepsTheVariantItWasGiven)
    {
        const auto& c = GetParam();
        RegionCache cache;

        cache.data = { c.data };
        cache.source_region = Region::time_span(100, 200, "cached_region");

        ASSERT_EQ(cache.data.size(), 1U);
        EXPECT_EQ(cache.data[0].index(), c.data.index());
        const size_t size = std::visit([](const auto& values) { return static_cast<size_t>(values.size()); }, cache.data[0]);
        EXPECT_EQ(size, c.size);
    }

    TEST(RegionCacheTest, AccessAndDirtyFlagsAccumulate)
    {
        RegionCache cache;

        EXPECT_EQ(cache.access_count, 0U);
        EXPECT_FALSE(cache.is_dirty);

        cache.mark_accessed();
        cache.mark_accessed();
        cache.mark_dirty();

        EXPECT_EQ(cache.access_count, 2U);
        EXPECT_TRUE(cache.is_dirty);
    }

    TEST(RegionCacheTest, AgeGrowsFromTheLoadTime)
    {
        RegionCache cache;
        cache.load_time = std::chrono::steady_clock::now() - std::chrono::seconds(2);

        const auto age = cache.age();

        EXPECT_GE(age.count(), 2.0);
        EXPECT_LT(age.count(), 60.0);
    }

} // namespace
} // namespace MayaFlux::Test
