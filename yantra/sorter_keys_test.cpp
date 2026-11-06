#include "MayaFlux/Yantra/Sorters/StandardSorter.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <stdexcept>

namespace MayaFlux::Test {
namespace {

    using Yantra::SortKey;
    using Yantra::SortingDirection;

    const Facet<double> rms {};
    const Facet<double> brightness {};

    Kakshya::Region make_region(std::optional<double> rms_value, std::optional<double> brightness_value)
    {
        Kakshya::Region region { { 0 }, { 5 } };
        if (rms_value) {
            region.set(rms, *rms_value);
        }
        if (brightness_value) {
            region.set(brightness, *brightness_value);
        }
        return region;
    }

    TEST(SortKeyScoreTest, SingleAscendingKeyScoresItsValue)
    {
        auto region = make_region(2.0, std::nullopt);
        const std::vector<SortKey> keys { { rms, SortingDirection::ASCENDING, 1.0, false } };

        const auto score = Yantra::weighted_sort_score(region, keys);
        ASSERT_TRUE(score.has_value());
        EXPECT_DOUBLE_EQ(*score, 2.0);
    }

    TEST(SortKeyScoreTest, DescendingKeyNegatesAndScalesByWeight)
    {
        auto region = make_region(2.0, std::nullopt);
        const std::vector<SortKey> keys { { rms, SortingDirection::DESCENDING, 2.0, false } };

        const auto score = Yantra::weighted_sort_score(region, keys);
        ASSERT_TRUE(score.has_value());
        EXPECT_DOUBLE_EQ(*score, -4.0);
    }

    TEST(SortKeyScoreTest, TermsAddUpWithTheirSigns)
    {
        auto region = make_region(1.0, 3.0);
        const std::vector<SortKey> keys {
            { rms, SortingDirection::ASCENDING, 2.0, false },
            { brightness, SortingDirection::DESCENDING, 1.0, false },
        };

        const auto score = Yantra::weighted_sort_score(region, keys);
        ASSERT_TRUE(score.has_value());
        EXPECT_DOUBLE_EQ(*score, 2.0 * 1.0 - 1.0 * 3.0);
    }

    TEST(SortKeyScoreTest, NormalizeSquashesTheValueWithTanh)
    {
        auto region = make_region(2.0, std::nullopt);
        const std::vector<SortKey> keys { { rms, SortingDirection::ASCENDING, 3.0, true } };

        const auto score = Yantra::weighted_sort_score(region, keys);
        ASSERT_TRUE(score.has_value());
        EXPECT_DOUBLE_EQ(*score, 3.0 * std::tanh(2.0));
    }

    TEST(SortKeyScoreTest, MissingFeatureGivesNoScore)
    {
        auto region = make_region(1.0, std::nullopt);
        const std::vector<SortKey> keys {
            { rms, SortingDirection::ASCENDING, 1.0, false },
            { brightness, SortingDirection::ASCENDING, 1.0, false },
        };

        EXPECT_FALSE(Yantra::weighted_sort_score(region, keys).has_value());
    }

    TEST(SortKeyScoreTest, OrdersElementsByScore)
    {
        std::vector<Kakshya::Region> regions {
            make_region(1.0, 0.0),
            make_region(4.0, 0.0),
            make_region(2.0, 0.0),
        };
        const std::vector<SortKey> keys { { rms, SortingDirection::DESCENDING, 1.0, false } };

        std::vector<std::pair<double, size_t>> scored;
        for (size_t i = 0; i < regions.size(); ++i) {
            scored.emplace_back(*Yantra::weighted_sort_score(regions[i], keys), i);
        }
        std::ranges::sort(scored);

        ASSERT_EQ(scored.size(), 3U);
        EXPECT_EQ(scored[0].second, 1U);
        EXPECT_EQ(scored[1].second, 2U);
        EXPECT_EQ(scored[2].second, 0U);
    }

    TEST(SortKeyValidationTest, AcceptsPositiveFiniteWeights)
    {
        const std::vector<SortKey> keys {
            { rms, SortingDirection::ASCENDING, 0.5, false },
            { brightness, SortingDirection::DESCENDING, 10.0, true },
        };
        EXPECT_NO_THROW(Yantra::validate_sort_keys(keys));
    }

    TEST(SortKeyValidationTest, RejectsZeroWeight)
    {
        const std::vector<SortKey> keys { { rms, SortingDirection::ASCENDING, 0.0, false } };
        EXPECT_THROW(Yantra::validate_sort_keys(keys), std::invalid_argument);
    }

    TEST(SortKeyValidationTest, RejectsNegativeWeight)
    {
        const std::vector<SortKey> keys { { rms, SortingDirection::ASCENDING, -1.0, false } };
        EXPECT_THROW(Yantra::validate_sort_keys(keys), std::invalid_argument);
    }

    TEST(SortKeyValidationTest, RejectsNonFiniteWeight)
    {
        const std::vector<SortKey> nan_keys { { rms, SortingDirection::ASCENDING, std::numeric_limits<double>::quiet_NaN(), false } };
        EXPECT_THROW(Yantra::validate_sort_keys(nan_keys), std::invalid_argument);

        const std::vector<SortKey> inf_keys { { rms, SortingDirection::ASCENDING, std::numeric_limits<double>::infinity(), false } };
        EXPECT_THROW(Yantra::validate_sort_keys(inf_keys), std::invalid_argument);
    }

} // namespace
} // namespace MayaFlux::Test
