#include "gtest/gtest.h"

#include "MayaFlux/Kinesis/Discrete/Sort.hpp"

namespace MayaFlux::Test {
namespace {

    namespace D = Kinesis::Discrete;

    using D::SortingAlgorithm;
    using D::SortingDirection;

    std::vector<double> unsorted()
    {
        return { 3.0, -1.0, 4.0, 1.0, -5.0, 9.0, 2.0, 6.0, -5.0, 3.0 };
    }

    std::vector<double> sorted_in(SortingDirection direction)
    {
        auto values = unsorted();
        std::ranges::sort(values, D::double_comparator(direction));
        return values;
    }

    std::vector<double> other_channel()
    {
        return { 0.5, 0.25, 0.75, 0.0, 1.0 };
    }

    std::string direction_name(SortingDirection direction)
    {
        return direction == SortingDirection::ASCENDING ? "Ascending" : "Descending";
    }

    std::string algorithm_name(SortingAlgorithm algorithm)
    {
        switch (algorithm) {
        case SortingAlgorithm::STANDARD:
            return "Standard";
        case SortingAlgorithm::STABLE:
            return "Stable";
        case SortingAlgorithm::PARTIAL:
            return "Partial";
        case SortingAlgorithm::NTH_ELEMENT:
            return "NthElement";
        case SortingAlgorithm::HEAP:
            return "Heap";
        case SortingAlgorithm::PARALLEL:
            return "Parallel";
        case SortingAlgorithm::RADIX:
            return "Radix";
        case SortingAlgorithm::COUNTING:
            return "Counting";
        case SortingAlgorithm::BUCKET:
            return "Bucket";
        case SortingAlgorithm::MERGE_EXTERNAL:
            return "MergeExternal";
        case SortingAlgorithm::QUICK_OPTIMIZED:
            return "QuickOptimized";
        case SortingAlgorithm::LAZY_STREAMING:
            return "LazyStreaming";
        case SortingAlgorithm::PREDICTIVE_ML:
            return "PredictiveMl";
        case SortingAlgorithm::EIGEN_OPTIMIZED:
            return "EigenOptimized";
        case SortingAlgorithm::GPU_ACCELERATED:
            return "GpuAccelerated";
        }
        return "Unknown";
    }

    // ------------------------------------------------------------------
    // Full sorts: every algorithm that sorts the whole span
    // ------------------------------------------------------------------

    using FullCase = std::tuple<SortingDirection, SortingAlgorithm>;

    class KinesisSortFullTest : public ::testing::TestWithParam<FullCase> { };

    INSTANTIATE_TEST_SUITE_P(Algorithms, KinesisSortFullTest,
        ::testing::Combine(
            ::testing::Values(SortingDirection::ASCENDING, SortingDirection::DESCENDING),
            ::testing::Values(
                SortingAlgorithm::STANDARD,
                SortingAlgorithm::STABLE,
                SortingAlgorithm::HEAP,
                SortingAlgorithm::PARALLEL,
                // Not implemented: each falls back to STANDARD.
                SortingAlgorithm::RADIX,
                SortingAlgorithm::COUNTING,
                SortingAlgorithm::BUCKET,
                SortingAlgorithm::MERGE_EXTERNAL,
                SortingAlgorithm::QUICK_OPTIMIZED,
                SortingAlgorithm::LAZY_STREAMING,
                SortingAlgorithm::PREDICTIVE_ML,
                SortingAlgorithm::EIGEN_OPTIMIZED,
                SortingAlgorithm::GPU_ACCELERATED)),
        [](const ::testing::TestParamInfo<FullCase>& info) {
            return direction_name(std::get<0>(info.param)) + algorithm_name(std::get<1>(info.param));
        });

    TEST_P(KinesisSortFullTest, SortSpanOrdersEveryElement)
    {
        const auto [direction, algorithm] = GetParam();
        auto values = unsorted();

        D::sort_span(values, direction, algorithm);

        EXPECT_EQ(values, sorted_in(direction));
    }

    // ------------------------------------------------------------------
    // Partial sort: the first half holds the leading half of the order
    // ------------------------------------------------------------------

    class KinesisSortPartialTest : public ::testing::TestWithParam<SortingDirection> { };

    INSTANTIATE_TEST_SUITE_P(Directions, KinesisSortPartialTest,
        ::testing::Values(SortingDirection::ASCENDING, SortingDirection::DESCENDING),
        [](const ::testing::TestParamInfo<SortingDirection>& info) {
            return direction_name(info.param);
        });

    TEST_P(KinesisSortPartialTest, FirstHalfIsTheLeadingHalfInOrder)
    {
        const auto direction = GetParam();
        auto values = unsorted();

        D::sort_span(values, direction, SortingAlgorithm::PARTIAL);

        const auto expected = sorted_in(direction);
        const auto half = values.size() / 2;
        EXPECT_TRUE(std::equal(values.begin(), values.begin() + half, expected.begin()));
    }

    TEST_P(KinesisSortPartialTest, NoElementIsLostOrInvented)
    {
        const auto direction = GetParam();
        auto values = unsorted();

        D::sort_span(values, direction, SortingAlgorithm::PARTIAL);

        auto actual = values;
        std::ranges::sort(actual);
        auto expected = unsorted();
        std::ranges::sort(expected);
        EXPECT_EQ(actual, expected);
    }

    // ------------------------------------------------------------------
    // Nth element: the midpoint holds its final value, with a partition either side
    // ------------------------------------------------------------------

    class KinesisSortNthTest : public ::testing::TestWithParam<SortingDirection> { };

    INSTANTIATE_TEST_SUITE_P(Directions, KinesisSortNthTest,
        ::testing::Values(SortingDirection::ASCENDING, SortingDirection::DESCENDING),
        [](const ::testing::TestParamInfo<SortingDirection>& info) {
            return direction_name(info.param);
        });

    TEST_P(KinesisSortNthTest, MidpointHoldsItsFinalValue)
    {
        const auto direction = GetParam();
        auto values = unsorted();

        D::sort_span(values, direction, SortingAlgorithm::NTH_ELEMENT);

        const auto expected = sorted_in(direction);
        const auto mid = values.size() / 2;
        EXPECT_EQ(values[mid], expected[mid]);
    }

    TEST_P(KinesisSortNthTest, ElementsOnEachSideArePartitionedAroundTheMidpoint)
    {
        const auto direction = GetParam();
        auto values = unsorted();

        D::sort_span(values, direction, SortingAlgorithm::NTH_ELEMENT);

        const auto comp = D::double_comparator(direction);
        const auto mid = values.size() / 2;
        for (size_t i = 0; i < mid; ++i) {
            EXPECT_FALSE(comp(values[mid], values[i])) << "left element " << i;
        }
        for (size_t i = mid + 1; i < values.size(); ++i) {
            EXPECT_FALSE(comp(values[i], values[mid])) << "right element " << i;
        }
    }

    // ------------------------------------------------------------------
    // Sorting into a caller-owned buffer
    // ------------------------------------------------------------------

    class KinesisSortIntoTest : public ::testing::TestWithParam<SortingDirection> { };

    INSTANTIATE_TEST_SUITE_P(Directions, KinesisSortIntoTest,
        ::testing::Values(SortingDirection::ASCENDING, SortingDirection::DESCENDING),
        [](const ::testing::TestParamInfo<SortingDirection>& info) {
            return direction_name(info.param);
        });

    TEST_P(KinesisSortIntoTest, OutputIsSortedAndTheSourceIsUntouched)
    {
        const auto direction = GetParam();
        const auto source = unsorted();
        std::vector<double> storage;

        const auto out = D::sort_span_into(source, storage, direction);

        EXPECT_EQ(std::vector<double>(out.begin(), out.end()), sorted_in(direction));
        EXPECT_EQ(source, unsorted());
    }

    TEST_P(KinesisSortIntoTest, ReturnedSpanViewsTheOutputStorage)
    {
        const auto direction = GetParam();
        const auto source = unsorted();
        std::vector<double> storage;

        const auto out = D::sort_span_into(source, storage, direction);

        EXPECT_EQ(out.data(), storage.data());
        EXPECT_EQ(out.size(), source.size());
    }

    // ------------------------------------------------------------------
    // Channels: each channel is sorted on its own
    // ------------------------------------------------------------------

    class KinesisSortChannelsTest : public ::testing::TestWithParam<SortingDirection> { };

    INSTANTIATE_TEST_SUITE_P(Directions, KinesisSortChannelsTest,
        ::testing::Values(SortingDirection::ASCENDING, SortingDirection::DESCENDING),
        [](const ::testing::TestParamInfo<SortingDirection>& info) {
            return direction_name(info.param);
        });

    TEST_P(KinesisSortChannelsTest, EachChannelIsSortedIndependently)
    {
        const auto direction = GetParam();
        auto first = unsorted();
        auto second = other_channel();
        std::vector<std::span<double>> channels { first, second };

        D::sort_channels(channels, direction);

        EXPECT_EQ(first, sorted_in(direction));
        auto expected_second = other_channel();
        std::ranges::sort(expected_second, D::double_comparator(direction));
        EXPECT_EQ(second, expected_second);
    }

    TEST_P(KinesisSortChannelsTest, IntoLeavesEverySourceUntouched)
    {
        const auto direction = GetParam();
        const auto first = unsorted();
        const auto second = other_channel();
        const std::vector<std::span<const double>> sources { first, second };
        std::vector<std::vector<double>> storage;

        const auto out = D::sort_channels_into(sources, storage, direction);

        ASSERT_EQ(out.size(), 2U);
        ASSERT_EQ(storage.size(), 2U);
        EXPECT_EQ(std::vector<double>(out[0].begin(), out[0].end()), sorted_in(direction));
        EXPECT_EQ(first, unsorted());
        EXPECT_EQ(second, other_channel());
    }

    TEST_P(KinesisSortChannelsTest, IntoReturnedSpansViewTheirOwnStorage)
    {
        const auto direction = GetParam();
        const auto first = unsorted();
        const auto second = other_channel();
        const std::vector<std::span<const double>> sources { first, second };
        std::vector<std::vector<double>> storage;

        const auto out = D::sort_channels_into(sources, storage, direction);

        ASSERT_EQ(out.size(), 2U);
        EXPECT_EQ(out[0].data(), storage[0].data());
        EXPECT_EQ(out[1].data(), storage[1].data());
    }

    // ------------------------------------------------------------------
    // Index generation: the indices reorder the data into sorted order
    // ------------------------------------------------------------------

    class KinesisSortIndicesTest : public ::testing::TestWithParam<SortingDirection> { };

    INSTANTIATE_TEST_SUITE_P(Directions, KinesisSortIndicesTest,
        ::testing::Values(SortingDirection::ASCENDING, SortingDirection::DESCENDING),
        [](const ::testing::TestParamInfo<SortingDirection>& info) {
            return direction_name(info.param);
        });

    TEST_P(KinesisSortIndicesTest, IndicesReorderTheDataIntoSortedOrder)
    {
        const auto direction = GetParam();
        auto values = unsorted();

        const auto indices = D::span_sort_indices(values, direction);

        ASSERT_EQ(indices.size(), values.size());
        std::vector<double> reordered;
        for (size_t i : indices) {
            reordered.push_back(values[i]);
        }
        EXPECT_EQ(reordered, sorted_in(direction));
    }

    TEST_P(KinesisSortIndicesTest, IndicesArePermutationOfTheWholeRange)
    {
        const auto direction = GetParam();
        auto values = unsorted();

        auto indices = D::span_sort_indices(values, direction);
        std::ranges::sort(indices);

        std::vector<size_t> identity(values.size());
        std::iota(identity.begin(), identity.end(), 0);
        EXPECT_EQ(indices, identity);
    }

    TEST_P(KinesisSortIndicesTest, SourceIsNotModified)
    {
        const auto direction = GetParam();
        auto values = unsorted();

        EXPECT_FALSE(D::span_sort_indices(values, direction).empty());

        EXPECT_EQ(values, unsorted());
    }

    TEST_P(KinesisSortIndicesTest, ChannelIndicesMatchTheIndicesOfEachChannel)
    {
        const auto direction = GetParam();
        auto first = unsorted();
        auto second = other_channel();
        std::vector<std::span<double>> channels { first, second };

        const auto per_channel = D::channels_sort_indices(channels, direction);

        ASSERT_EQ(per_channel.size(), 2U);
        EXPECT_EQ(per_channel[0], D::span_sort_indices(first, direction));
        EXPECT_EQ(per_channel[1], D::span_sort_indices(second, direction));
    }

    // ------------------------------------------------------------------
    // Empty input is a no-op everywhere
    // ------------------------------------------------------------------

    class KinesisSortEmptyTest : public ::testing::TestWithParam<SortingDirection> { };

    INSTANTIATE_TEST_SUITE_P(Directions, KinesisSortEmptyTest,
        ::testing::Values(SortingDirection::ASCENDING, SortingDirection::DESCENDING),
        [](const ::testing::TestParamInfo<SortingDirection>& info) {
            return direction_name(info.param);
        });

    TEST_P(KinesisSortEmptyTest, EmptySpanStaysEmpty)
    {
        const auto direction = GetParam();
        std::vector<double> empty;
        std::vector<double> storage;

        D::sort_span(empty, direction);
        EXPECT_TRUE(D::sort_span_into(empty, storage, direction).empty());
        EXPECT_TRUE(D::span_sort_indices(empty, direction).empty());
    }

    TEST_P(KinesisSortEmptyTest, NoChannelsGivesNoResults)
    {
        const auto direction = GetParam();
        std::vector<std::span<double>> channels;
        std::vector<std::span<const double>> sources;
        std::vector<std::vector<double>> storage;

        D::sort_channels(channels, direction);
        EXPECT_TRUE(D::sort_channels_into(sources, storage, direction).empty());
        EXPECT_TRUE(D::channels_sort_indices(channels, direction).empty());
    }

} // namespace
} // namespace MayaFlux::Test
