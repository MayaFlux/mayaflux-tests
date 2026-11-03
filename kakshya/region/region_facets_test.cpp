#include "gtest/gtest.h"

#include "MayaFlux/Kakshya/Region/OrganizedRegion.hpp"
#include "MayaFlux/Kakshya/Region/RegionGroup.hpp"

namespace MayaFlux::Test {
namespace {

    using Kakshya::OrganizedRegion;
    using Kakshya::Region;
    using Kakshya::RegionGroup;
    using Kakshya::RegionSegment;

    template <typename Carrier>
    Carrier make_carrier();

    template <>
    Region make_carrier<Region>()
    {
        return Region::time_span(0, 10);
    }

    template <>
    RegionGroup make_carrier<RegionGroup>()
    {
        return RegionGroup("group", { Region::time_span(0, 10) });
    }

    template <>
    RegionSegment make_carrier<RegionSegment>()
    {
        return RegionSegment(Region::time_span(0, 10));
    }

    template <>
    OrganizedRegion make_carrier<OrganizedRegion>()
    {
        return OrganizedRegion("group", 0);
    }

    struct CarrierNames {
        template <typename Carrier>
        static std::string GetName(int)
        {
            if constexpr (std::is_same_v<Carrier, Region>) {
                return "Region";
            } else if constexpr (std::is_same_v<Carrier, RegionGroup>) {
                return "RegionGroup";
            } else if constexpr (std::is_same_v<Carrier, RegionSegment>) {
                return "RegionSegment";
            } else {
                return "OrganizedRegion";
            }
        }
    };

    template <typename Carrier>
    class RegionFacetCarrierTest : public ::testing::Test { };

    using Carriers = ::testing::Types<Region, RegionGroup, RegionSegment, OrganizedRegion>;
    TYPED_TEST_SUITE(RegionFacetCarrierTest, Carriers, CarrierNames);

    TYPED_TEST(RegionFacetCarrierTest, AnAbsentKeyReadsAsNull)
    {
        const auto carrier = make_carrier<TypeParam>();
        const Facet<int> key;

        EXPECT_EQ(carrier.get(key), nullptr);
    }

    TYPED_TEST(RegionFacetCarrierTest, ASetValueReadsBack)
    {
        auto carrier = make_carrier<TypeParam>();
        const Facet<int> key;

        carrier.set(key, 7);

        ASSERT_NE(carrier.get(key), nullptr);
        EXPECT_EQ(*carrier.get(key), 7);
    }

    TYPED_TEST(RegionFacetCarrierTest, SettingAgainReplacesTheValue)
    {
        auto carrier = make_carrier<TypeParam>();
        const Facet<double> key;

        carrier.set(key, 0.25);
        carrier.set(key, 0.5);

        ASSERT_NE(carrier.get(key), nullptr);
        EXPECT_DOUBLE_EQ(*carrier.get(key), 0.5);
    }

    TYPED_TEST(RegionFacetCarrierTest, KeysOfTheSameTypeDoNotCollide)
    {
        auto carrier = make_carrier<TypeParam>();
        const Facet<int> first;
        const Facet<int> second;

        carrier.set(first, 1);

        EXPECT_EQ(carrier.get(second), nullptr);
        carrier.set(second, 2);
        ASSERT_NE(carrier.get(first), nullptr);
        ASSERT_NE(carrier.get(second), nullptr);
        EXPECT_EQ(*carrier.get(first), 1);
        EXPECT_EQ(*carrier.get(second), 2);
    }

    TYPED_TEST(RegionFacetCarrierTest, StringsAndVectorsAreStoredByValue)
    {
        auto carrier = make_carrier<TypeParam>();
        const Facet<std::string> name;
        const Facet<std::vector<double>> envelope;

        carrier.set(name, std::string("segment"));
        carrier.set(envelope, { 0.0, 0.5, 1.0 });

        ASSERT_NE(carrier.get(name), nullptr);
        ASSERT_NE(carrier.get(envelope), nullptr);
        EXPECT_EQ(*carrier.get(name), "segment");
        EXPECT_EQ(carrier.get(envelope)->size(), 3U);
        EXPECT_DOUBLE_EQ((*carrier.get(envelope))[2], 1.0);
    }

    TYPED_TEST(RegionFacetCarrierTest, ACopyCarriesTheValuesAndWritesStayApart)
    {
        auto carrier = make_carrier<TypeParam>();
        const Facet<int> key;
        carrier.set(key, 1);

        auto copy = carrier;
        copy.set(key, 2);

        ASSERT_NE(carrier.get(key), nullptr);
        ASSERT_NE(copy.get(key), nullptr);
        EXPECT_EQ(*carrier.get(key), 1);
        EXPECT_EQ(*copy.get(key), 2);
    }

    TEST(RegionFacetIsolationTest, AGroupValueIsNotVisibleOnItsRegions)
    {
        const Facet<int> key;
        auto group = make_carrier<RegionGroup>();

        group.set(key, 5);

        ASSERT_NE(group.get(key), nullptr);
        EXPECT_EQ(group.regions.front().get(key), nullptr);
    }

    TEST(RegionFacetIsolationTest, ASegmentValueIsNotVisibleOnItsSourceRegion)
    {
        const Facet<int> key;
        auto segment = make_carrier<RegionSegment>();

        segment.set(key, 5);

        ASSERT_NE(segment.get(key), nullptr);
        EXPECT_EQ(segment.source_region.get(key), nullptr);
    }

    TEST(RegionFacetIsolationTest, ASegmentSeesTheValuesItsRegionCarried)
    {
        const Facet<int> key;
        Region region = Region::time_span(0, 10);
        region.set(key, 9);

        const RegionSegment segment(region);

        ASSERT_NE(segment.source_region.get(key), nullptr);
        EXPECT_EQ(*segment.source_region.get(key), 9);
        EXPECT_EQ(segment.get(key), nullptr);
    }

} // namespace
} // namespace MayaFlux::Test
