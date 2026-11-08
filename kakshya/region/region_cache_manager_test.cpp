#include "gtest/gtest.h"

#include "MayaFlux/Kakshya/Region/RegionCacheManager.hpp"

namespace MayaFlux::Test {
namespace {

    using Kakshya::DataVariant;
    using Kakshya::Region;
    using Kakshya::RegionCache;
    using Kakshya::RegionCacheManager;
    using Kakshya::RegionSegment;

    constexpr size_t k_entry_doubles = 8;
    constexpr size_t k_entry_bytes = k_entry_doubles * sizeof(double);

    Region region_at(uint64_t start)
    {
        return Region::time_span(start, start + 9);
    }

    RegionCache cache_of(const Region& region, size_t doubles = k_entry_doubles, double fill = 1.0)
    {
        RegionCache cache;
        cache.source_region = region;
        cache.data = { DataVariant(std::vector<double>(doubles, fill)) };
        return cache;
    }

    const RegionCache* lookup(RegionCacheManager& manager, const Region& region)
    {
        return manager.get_cached_segment(RegionSegment(region));
    }

    TEST(RegionCacheManagerTest, AStoredRegionIsFoundByASegmentOfIt)
    {
        RegionCacheManager manager(4 * k_entry_bytes);
        manager.cache_region(cache_of(region_at(0), k_entry_doubles, 3.0));

        const auto* found = lookup(manager, region_at(0));

        ASSERT_NE(found, nullptr);
        EXPECT_EQ(found->source_region, region_at(0));
        ASSERT_EQ(found->data.size(), 1U);
        EXPECT_EQ(std::get<std::vector<double>>(found->data[0]), std::vector<double>(k_entry_doubles, 3.0));
    }

    TEST(RegionCacheManagerTest, ARegionThatWasNeverStoredIsAMiss)
    {
        RegionCacheManager manager(4 * k_entry_bytes);
        manager.cache_region(cache_of(region_at(0)));

        EXPECT_EQ(lookup(manager, region_at(100)), nullptr);
    }

    TEST(RegionCacheManagerTest, EachLookupCountsAsAnAccess)
    {
        RegionCacheManager manager(4 * k_entry_bytes);
        manager.cache_region(cache_of(region_at(0)));

        EXPECT_EQ(lookup(manager, region_at(0))->access_count, 1U);
        EXPECT_EQ(lookup(manager, region_at(0))->access_count, 2U);
    }

    TEST(RegionCacheManagerTest, TheOldestEntryGoesFirstWhenTheByteBudgetIsExceeded)
    {
        RegionCacheManager manager(2 * k_entry_bytes);
        manager.cache_region(cache_of(region_at(0)));
        manager.cache_region(cache_of(region_at(100)));
        manager.cache_region(cache_of(region_at(200)));

        EXPECT_EQ(lookup(manager, region_at(0)), nullptr);
        EXPECT_NE(lookup(manager, region_at(100)), nullptr);
        EXPECT_NE(lookup(manager, region_at(200)), nullptr);
    }

    TEST(RegionCacheManagerTest, ALookupMakesAnEntryTheNewest)
    {
        RegionCacheManager manager(2 * k_entry_bytes);
        manager.cache_region(cache_of(region_at(0)));
        manager.cache_region(cache_of(region_at(100)));
        ASSERT_NE(lookup(manager, region_at(0)), nullptr);

        manager.cache_region(cache_of(region_at(200)));

        EXPECT_NE(lookup(manager, region_at(0)), nullptr);
        EXPECT_EQ(lookup(manager, region_at(100)), nullptr);
        EXPECT_NE(lookup(manager, region_at(200)), nullptr);
    }

    TEST(RegionCacheManagerTest, StoringARegionAgainReplacesItsEntryWithoutCountingItTwice)
    {
        RegionCacheManager manager(2 * k_entry_bytes);
        manager.cache_region(cache_of(region_at(0), k_entry_doubles, 1.0));
        manager.cache_region(cache_of(region_at(0), k_entry_doubles, 2.0));
        manager.cache_region(cache_of(region_at(100)));

        const auto* replaced = lookup(manager, region_at(0));
        ASSERT_NE(replaced, nullptr);
        EXPECT_EQ(std::get<std::vector<double>>(replaced->data[0]).front(), 2.0);
        EXPECT_NE(lookup(manager, region_at(100)), nullptr);
    }

    TEST(RegionCacheManagerTest, AnEntryLargerThanTheWholeBudgetIsNotStoredAndEvictsNothing)
    {
        RegionCacheManager manager(2 * k_entry_bytes);
        manager.cache_region(cache_of(region_at(0)));

        manager.cache_region(cache_of(region_at(100), 3 * k_entry_doubles));

        EXPECT_EQ(lookup(manager, region_at(100)), nullptr);
        EXPECT_NE(lookup(manager, region_at(0)), nullptr);
    }

    TEST(RegionCacheManagerTest, AnOversizedReplacementLeavesNoStaleEntryBehind)
    {
        RegionCacheManager manager(2 * k_entry_bytes);
        manager.cache_region(cache_of(region_at(0)));

        manager.cache_region(cache_of(region_at(0), 3 * k_entry_doubles));

        EXPECT_EQ(lookup(manager, region_at(0)), nullptr);
    }

    TEST(RegionCacheManagerTest, SeveralSmallEntriesAreEvictedToFitOneLargeOne)
    {
        RegionCacheManager manager(4 * k_entry_bytes);
        for (uint64_t i = 0; i < 4; ++i) {
            manager.cache_region(cache_of(region_at(i * 100)));
        }

        manager.cache_region(cache_of(region_at(1000), 3 * k_entry_doubles));

        EXPECT_NE(lookup(manager, region_at(1000)), nullptr);
        EXPECT_EQ(lookup(manager, region_at(0)), nullptr);
        EXPECT_EQ(lookup(manager, region_at(100)), nullptr);
        EXPECT_EQ(lookup(manager, region_at(200)), nullptr);
        EXPECT_NE(lookup(manager, region_at(300)), nullptr);
    }

} // namespace
} // namespace MayaFlux::Test
