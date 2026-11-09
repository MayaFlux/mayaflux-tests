#include "gtest/gtest.h"

#include "../../support/gpu_dispatch_test_support.hpp"

#include "MayaFlux/Yantra/Sorters/RegionKeySorter.hpp"

#include <cmath>
#include <limits>

namespace MayaFlux::Test {
namespace {

    using namespace GpuDispatch;
    using Yantra::Datum;
    using Yantra::RegionKeySorter;
    using Yantra::RegionKeySorterConfig;
    using Yantra::SortingDirection;
    using Yantra::SortKey;

    constexpr double k_nan = std::numeric_limits<double>::quiet_NaN();

    const Facet<double> k_feature {};
    const Facet<double> k_marker {};

    Kakshya::Region make_region(double value, double marker)
    {
        Kakshya::Region region { { 0 }, { 5 } };
        region.set(k_feature, value);
        region.set(k_marker, marker);
        return region;
    }

    Datum<Kakshya::RegionGroup> make_group(const std::vector<Kakshya::Region>& regions)
    {
        Kakshya::RegionGroup group;
        group.name = "device";
        for (const auto& region : regions) {
            group.add_region(region);
        }
        return Datum<Kakshya::RegionGroup> { group };
    }

    RegionKeySorterConfig keyed(uint32_t gpu_sort_threshold)
    {
        RegionKeySorterConfig cfg;
        cfg.keys = { SortKey { k_feature, SortingDirection::ASCENDING, 1.0, false } };
        cfg.gpu_sort_threshold = gpu_sort_threshold;
        return cfg;
    }

    std::vector<double> column(const Datum<Kakshya::RegionGroup>& out, const Facet<double>& facet)
    {
        std::vector<double> values;
        for (const auto& region : out.data.regions) {
            values.push_back(*region.get(facet));
        }
        return values;
    }

    template <typename Fixture>
    class RegionKeySorterDeviceTest : public Fixture { };

    using HardwareOptions = ::testing::Types<GpuDispatchFixture<true>, GpuDispatchFixture<false>>;
    TYPED_TEST_SUITE(RegionKeySorterDeviceTest, HardwareOptions);

    TYPED_TEST(RegionKeySorterDeviceTest, GpuOrderMatchesTheCpuOrderIncludingTies)
    {
        std::vector<Kakshya::Region> regions;
        for (int i = 0; i < 37; ++i) {
            regions.push_back(make_region(static_cast<double>((i * 7) % 5), static_cast<double>(i)));
        }

        const auto cpu = std::make_shared<RegionKeySorter>(keyed(0))->apply_operation(make_group(regions));
        const auto gpu = std::make_shared<RegionKeySorter>(keyed(1))->apply_operation(make_group(regions));

        EXPECT_EQ(column(gpu, k_feature), column(cpu, k_feature));
        EXPECT_EQ(column(gpu, k_marker), column(cpu, k_marker));
    }

    TYPED_TEST(RegionKeySorterDeviceTest, GpuOrderKeepsDifferencesThatFloatWouldMerge)
    {
        const double close = 1.0 + std::ldexp(1.0, -40);
        const auto gpu = std::make_shared<RegionKeySorter>(keyed(1))
                             ->apply_operation(make_group({ make_region(close, 0.0), make_region(1.0, 1.0) }));

        EXPECT_EQ(column(gpu, k_marker), (std::vector<double> { 1.0, 0.0 }));
    }

    TYPED_TEST(RegionKeySorterDeviceTest, GpuOrderPutsNaNLastAndKeepsItsTiesInInputOrder)
    {
        const auto gpu = std::make_shared<RegionKeySorter>(keyed(1))
                             ->apply_operation(make_group({ make_region(k_nan, 0.0), make_region(2.0, 1.0),
                                 make_region(-1.0, 2.0), make_region(k_nan, 3.0) }));

        EXPECT_EQ(column(gpu, k_marker), (std::vector<double> { 2.0, 1.0, 0.0, 3.0 }));
    }

} // namespace
} // namespace MayaFlux::Test
