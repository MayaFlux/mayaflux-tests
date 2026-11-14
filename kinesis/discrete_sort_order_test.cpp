#include "gtest/gtest.h"
#include "../support/test_metadata.hpp"

#include "MayaFlux/Kinesis/Discrete/Sort.hpp"

#include <cmath>
#include <stdexcept>

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {
namespace {

    namespace D = Kinesis::Discrete;

    using D::SortingAlgorithm;
    using D::SortingDirection;

    std::vector<double> unsorted()
    {
        return { 3.0, -1.0, 4.0, 1.0, -5.0, 9.0, 2.0, 6.0, -5.0, 3.0 };
    }

    std::vector<double> apply_indices(const std::vector<double>& values, const std::vector<size_t>& indices)
    {
        std::vector<double> out;
        out.reserve(indices.size());
        for (size_t idx : indices) {
            out.push_back(values[idx]);
        }
        return out;
    }

    std::vector<double> absolute_order_key()
    {
        return { -3.0, 1.0, -2.0, 4.0, 0.0 };
    }

    D::DoubleComparator by_magnitude()
    {
        return [](double a, double b) { return std::abs(a) < std::abs(b); };
    }

    class KinesisSortOrderMountainTest : public ::testing::TestWithParam<SortingAlgorithm> {
    };

    TEST_P(KinesisSortOrderMountainTest, EvenLengthRisesToCentreAndFalls)
    {
        auto values = unsorted();
        D::sort_span(values, SortingDirection::BIDIRECTIONAL, GetParam());

        const std::vector<double> expected { -5.0, -1.0, 2.0, 3.0, 6.0, 9.0, 4.0, 3.0, 1.0, -5.0 };
        EXPECT_EQ(values, expected);
    }

    TEST_P(KinesisSortOrderMountainTest, OddLengthRisesToCentreAndFalls)
    {
        std::vector<double> values { 4.0, 1.0, 5.0, 2.0, 3.0 };
        D::sort_span(values, SortingDirection::BIDIRECTIONAL, GetParam());

        const std::vector<double> expected { 1.0, 3.0, 5.0, 4.0, 2.0 };
        EXPECT_EQ(values, expected);
    }

    INSTANTIATE_TEST_SUITE_P(
        Algorithms,
        KinesisSortOrderMountainTest,
        ::testing::Values(
            SortingAlgorithm::STANDARD,
            SortingAlgorithm::STABLE,
            SortingAlgorithm::HEAP,
            SortingAlgorithm::PARALLEL,
            SortingAlgorithm::PARTIAL,
            SortingAlgorithm::NTH_ELEMENT));

    TEST(KinesisSortOrderBidirectionalTest, IndicesAgreeForEveryAlgorithm)
    {
        const auto values = unsorted();
        const std::vector<SortingAlgorithm> algorithms {
            SortingAlgorithm::STANDARD, SortingAlgorithm::PARTIAL, SortingAlgorithm::NTH_ELEMENT
        };
        for (auto algorithm : algorithms) {
            auto sorted = values;
            D::sort_span(sorted, SortingDirection::BIDIRECTIONAL, algorithm);

            std::vector<double> source = values;
            const auto indices = D::span_sort_indices(source, SortingDirection::BIDIRECTIONAL);
            EXPECT_EQ(apply_indices(values, indices), sorted) << "algorithm " << static_cast<int>(algorithm);
        }
    }

    TEST(KinesisSortOrderBidirectionalTest, IndicesReproduceTheValueArrangement)
    {
        const auto values = unsorted();

        auto sorted = values;
        D::sort_span(sorted, SortingDirection::BIDIRECTIONAL, SortingAlgorithm::STANDARD);

        std::vector<double> source = values;
        const auto indices = D::span_sort_indices(source, SortingDirection::BIDIRECTIONAL);

        ASSERT_EQ(indices.size(), values.size());
        EXPECT_EQ(apply_indices(values, indices), sorted);
    }

    TEST(KinesisSortOrderBidirectionalTest, EachChannelIsArrangedIndependently)
    {
        std::vector<double> first = unsorted();
        std::vector<double> second { 0.5, 0.25, 0.75, 0.0, 1.0 };

        std::vector<std::span<double>> channels { first, second };
        D::sort_channels(channels, SortingDirection::BIDIRECTIONAL, SortingAlgorithm::STANDARD);

        EXPECT_EQ(first, (std::vector<double> { -5.0, -1.0, 2.0, 3.0, 6.0, 9.0, 4.0, 3.0, 1.0, -5.0 }));
        EXPECT_EQ(second, (std::vector<double> { 0.0, 0.5, 1.0, 0.75, 0.25 }));
    }

    TEST(KinesisSortOrderBidirectionalTest, SingleElementIsUnchanged)
    {
        std::vector<double> values { 7.0 };
        D::sort_span(values, SortingDirection::BIDIRECTIONAL, SortingAlgorithm::STANDARD);

        EXPECT_EQ(values, (std::vector<double> { 7.0 }));
    }

    TEST(KinesisSortOrderBidirectionalTest, ComparatorFactoryRejectsIt)
    {
        EXPECT_THROW((void)D::double_comparator(SortingDirection::BIDIRECTIONAL), std::invalid_argument);
    }

    TEST(KinesisMountainPositionsTest, RanksAlternateBetweenTheEndsAndFillInward)
    {
        EXPECT_TRUE(D::mountain_positions(0).empty());
        EXPECT_EQ(D::mountain_positions(1), (std::vector<size_t> { 0 }));
        EXPECT_EQ(D::mountain_positions(4), (std::vector<size_t> { 0, 3, 1, 2 }));
        EXPECT_EQ(D::mountain_positions(5), (std::vector<size_t> { 0, 4, 1, 3, 2 }));
    }

    TEST(KinesisMountainPositionsTest, IsAPermutationOfTheIndices)
    {
        for (const size_t count : { 2U, 7U, 64U, 101U }) {
            auto positions = D::mountain_positions(count);
            std::ranges::sort(positions);
            std::vector<size_t> expected(count);
            std::iota(expected.begin(), expected.end(), size_t { 0 });
            EXPECT_EQ(positions, expected) << "count " << count;
        }
    }

    TEST(KinesisSortOrderCustomTest, ComparatorOrdersValues)
    {
        auto values = absolute_order_key();
        D::sort_span(values, D::SortOrder { by_magnitude() }, SortingAlgorithm::STANDARD);

        EXPECT_EQ(values, (std::vector<double> { 0.0, 1.0, -2.0, -3.0, 4.0 }));
    }

    TEST(KinesisSortOrderCustomTest, IndicesFollowTheComparator)
    {
        const auto values = absolute_order_key();
        std::vector<double> source = values;
        const auto indices = D::span_sort_indices(source, D::SortOrder { by_magnitude() });

        EXPECT_EQ(apply_indices(values, indices), (std::vector<double> { 0.0, 1.0, -2.0, -3.0, 4.0 }));
    }

    TEST(KinesisSortOrderCustomTest, CustomWithoutComparatorIsRejected)
    {
        std::vector<double> values = absolute_order_key();
        EXPECT_THROW(D::sort_span(values, SortingDirection::CUSTOM, SortingAlgorithm::STANDARD), std::invalid_argument);
    }

} // namespace
} // namespace MayaFlux::Test
