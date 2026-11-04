#include "MayaFlux/Yantra/Sorters/RegionKeySorter.hpp"

#include <gtest/gtest.h>

#include <optional>
#include <stdexcept>

namespace MayaFlux::Test {
namespace {

    using Yantra::RegionKeySorter;
    using Yantra::RegionKeySorterConfig;
    using Yantra::SortKey;
    using Yantra::SortingDirection;

    const Facet<double> first {};
    const Facet<double> second {};

    Kakshya::Region make_region(std::optional<double> a, std::optional<double> b)
    {
        Kakshya::Region region { { 0 }, { 5 } };
        if (a) {
            region.set(first, *a);
        }
        if (b) {
            region.set(second, *b);
        }
        return region;
    }

    Yantra::Datum<Kakshya::RegionGroup> make_group(const std::vector<Kakshya::Region>& regions)
    {
        Kakshya::RegionGroup group;
        group.name = "keys";
        for (const auto& region : regions) {
            group.add_region(region);
        }
        return Yantra::Datum<Kakshya::RegionGroup> { group };
    }

    RegionKeySorterConfig keyed(std::vector<SortKey> keys)
    {
        RegionKeySorterConfig cfg;
        cfg.keys = std::move(keys);
        return cfg;
    }

    std::vector<double> values_of(const Yantra::Datum<Kakshya::RegionGroup>& out, const Facet<double>& feature)
    {
        std::vector<double> values;
        for (const auto& region : out.data.regions) {
            values.push_back(*region.get(feature));
        }
        return values;
    }

    TEST(RegionKeySorterTest, AscendingKeyOrdersByFeature)
    {
        auto sorter = std::make_shared<RegionKeySorter>(
            keyed({ { first, SortingDirection::ASCENDING, 1.0, false } }));
        auto input = make_group({ make_region(3.0, 0.0), make_region(1.0, 0.0), make_region(2.0, 0.0) });

        auto result = sorter->apply_operation(input);

        EXPECT_EQ(values_of(result, first), (std::vector<double> { 1.0, 2.0, 3.0 }));
    }

    TEST(RegionKeySorterTest, DescendingKeyReversesTheOrder)
    {
        auto sorter = std::make_shared<RegionKeySorter>(
            keyed({ { first, SortingDirection::DESCENDING, 1.0, false } }));
        auto input = make_group({ make_region(1.0, 0.0), make_region(3.0, 0.0), make_region(2.0, 0.0) });

        auto result = sorter->apply_operation(input);

        EXPECT_EQ(values_of(result, first), (std::vector<double> { 3.0, 2.0, 1.0 }));
    }

    TEST(RegionKeySorterTest, WeightedKeysBreakTiesOnTheSecondTerm)
    {
        auto sorter = std::make_shared<RegionKeySorter>(keyed({
            { first, SortingDirection::ASCENDING, 1.0, false },
            { second, SortingDirection::DESCENDING, 1.0, false },
        }));
        auto input = make_group({ make_region(1.0, 1.0), make_region(1.0, 3.0), make_region(0.0, 5.0) });

        auto result = sorter->apply_operation(input);

        EXPECT_EQ(values_of(result, first), (std::vector<double> { 0.0, 1.0, 1.0 }));
        EXPECT_EQ(values_of(result, second), (std::vector<double> { 5.0, 3.0, 1.0 }));
    }

    TEST(RegionKeySorterTest, EmptyKeysLeaveTheGroupInItsOrder)
    {
        auto sorter = std::make_shared<RegionKeySorter>();
        auto input = make_group({ make_region(3.0, 0.0), make_region(1.0, 0.0), make_region(2.0, 0.0) });

        auto result = sorter->apply_operation(input);

        EXPECT_EQ(values_of(result, first), (std::vector<double> { 3.0, 1.0, 2.0 }));
    }

    TEST(RegionKeySorterTest, RegionMissingAKeyFeatureIsAnError)
    {
        auto sorter = std::make_shared<RegionKeySorter>(keyed({
            { first, SortingDirection::ASCENDING, 1.0, false },
            { second, SortingDirection::ASCENDING, 1.0, false },
        }));
        auto input = make_group({ make_region(1.0, 1.0), make_region(2.0, std::nullopt) });

        EXPECT_THROW(sorter->apply_operation(input), std::runtime_error);
    }

    TEST(RegionKeySorterTest, RejectsNonPositiveWeightAtConstruction)
    {
        EXPECT_THROW(
            std::make_shared<RegionKeySorter>(keyed({ { first, SortingDirection::ASCENDING, 0.0, false } })),
            std::invalid_argument);
    }

} // namespace
} // namespace MayaFlux::Test
