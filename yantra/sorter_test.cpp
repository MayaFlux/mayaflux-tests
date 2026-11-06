#include "../mock_signalsourcecontainer.hpp"
#include "MayaFlux/Yantra/Sorters/StandardSorter.hpp"

#include "MayaFlux/Kinesis/Discrete/Sort.hpp"

using namespace MayaFlux::Yantra;
using namespace MayaFlux::Kakshya;

namespace MayaFlux::Test {

namespace {

using ChannelSorter = StandardSorter<std::vector<Kakshya::DataVariant>>;
using ChannelDatum = Datum<std::vector<Kakshya::DataVariant>>;

std::vector<double> unsorted_values()
{
    return { 5.0, 2.0, 8.0, 1.0, 9.0, 3.0, 7.0, 4.0, 6.0, 0.0 };
}

std::vector<double> sorted_values()
{
    auto values = unsorted_values();
    std::ranges::sort(values);
    return values;
}

ChannelDatum single_channel_input(const std::vector<double>& values)
{
    std::vector<Kakshya::DataVariant> channels { Kakshya::DataVariant { values } };
    return ChannelDatum { channels };
}

std::vector<double> channel_of(const ChannelDatum& out, size_t index)
{
    return std::get<std::vector<double>>(out.data[index]);
}

StandardSorterConfig make_config(SortingDirection direction,
    SortingStrategy strategy,
    SortingAlgorithm algorithm = SortingAlgorithm::STANDARD,
    size_t chunk_size = 1024)
{
    StandardSorterConfig cfg;
    cfg.direction = direction;
    cfg.strategy = strategy;
    cfg.algorithm = algorithm;
    cfg.chunk_size = chunk_size;
    return cfg;
}

} // namespace

// ============================================================================
// Settings
// ============================================================================

TEST(StandardSorterWiringConfigTest, StoresSettingsAsGiven)
{
    auto sorter = std::make_shared<ChannelSorter>(
        make_config(SortingDirection::DESCENDING, SortingStrategy::PARTIAL_SORT, SortingAlgorithm::HEAP, 64));

    const auto& stored = sorter->config<StandardSorterConfig>();
    EXPECT_EQ(stored.direction, SortingDirection::DESCENDING);
    EXPECT_EQ(stored.strategy, SortingStrategy::PARTIAL_SORT);
    EXPECT_EQ(stored.algorithm, SortingAlgorithm::HEAP);
    EXPECT_EQ(stored.chunk_size, 64U);
}

TEST(StandardSorterWiringConfigTest, DefaultsWhenNoSettingsGiven)
{
    auto sorter = std::make_shared<ChannelSorter>();

    const auto& stored = sorter->config<StandardSorterConfig>();
    EXPECT_EQ(stored.direction, SortingDirection::ASCENDING);
    EXPECT_EQ(stored.strategy, SortingStrategy::COPY_SORT);
    EXPECT_EQ(stored.algorithm, SortingAlgorithm::STANDARD);
    EXPECT_EQ(stored.chunk_size, 1024U);
    EXPECT_EQ(sorter->get_sorting_type(), SortingType::STANDARD);
}

TEST(StandardSorterWiringConfigTest, RejectsZeroChunkSize)
{
    EXPECT_THROW(
        std::make_shared<ChannelSorter>(make_config(SortingDirection::ASCENDING, SortingStrategy::CHUNKED_SORT, SortingAlgorithm::STANDARD, 0)),
        std::invalid_argument);
}

// ============================================================================
// Direction
// ============================================================================

class StandardSorterWiringDirectionTest : public ::testing::TestWithParam<SortingDirection> {
};

TEST_P(StandardSorterWiringDirectionTest, OrdersChannelInRequestedDirection)
{
    const auto direction = GetParam();
    auto sorter = std::make_shared<ChannelSorter>(make_config(direction, SortingStrategy::COPY_SORT));

    auto result = sorter->apply_operation(single_channel_input(unsorted_values()));

    ASSERT_EQ(result.data.size(), 1U);
    auto expected = sorted_values();
    if (direction == SortingDirection::DESCENDING) {
        std::ranges::reverse(expected);
    }
    EXPECT_EQ(channel_of(result, 0), expected);
}

INSTANTIATE_TEST_SUITE_P(
    Directions,
    StandardSorterWiringDirectionTest,
    ::testing::Values(SortingDirection::ASCENDING, SortingDirection::DESCENDING));

// ============================================================================
// Algorithm
// ============================================================================

class StandardSorterWiringAlgorithmTest : public ::testing::TestWithParam<SortingAlgorithm> {
};

TEST_P(StandardSorterWiringAlgorithmTest, SortsWithConfiguredAlgorithm)
{
    auto sorter = std::make_shared<ChannelSorter>(
        make_config(SortingDirection::ASCENDING, SortingStrategy::COPY_SORT, GetParam()));

    auto result = sorter->apply_operation(single_channel_input(unsorted_values()));

    ASSERT_EQ(result.data.size(), 1U);
    EXPECT_EQ(channel_of(result, 0), sorted_values());
}

INSTANTIATE_TEST_SUITE_P(
    Algorithms,
    StandardSorterWiringAlgorithmTest,
    ::testing::Values(SortingAlgorithm::STANDARD, SortingAlgorithm::STABLE, SortingAlgorithm::HEAP));

// ============================================================================
// Strategy and recorded path
// ============================================================================

struct StrategyCase {
    SortingStrategy strategy;
    SortPath path;
    bool fully_sorted;
};

class StandardSorterWiringStrategyTest : public ::testing::TestWithParam<StrategyCase> {
};

TEST_P(StandardSorterWiringStrategyTest, RecordsPathAndKeepsEveryValue)
{
    const auto [strategy, path, fully_sorted] = GetParam();
    auto sorter = std::make_shared<ChannelSorter>(
        make_config(SortingDirection::ASCENDING, strategy));

    auto result = sorter->apply_operation(single_channel_input(unsorted_values()));

    ASSERT_EQ(result.data.size(), 1U);
    const auto values = channel_of(result, 0);

    auto as_multiset = values;
    std::ranges::sort(as_multiset);
    EXPECT_EQ(as_multiset, sorted_values());

    if (fully_sorted) {
        EXPECT_TRUE(std::ranges::is_sorted(values));
    }

    const auto* record = result.facets.get<SortRecord>();
    ASSERT_NE(record, nullptr);
    EXPECT_EQ(static_cast<SortPath>(record->path), path);
}

INSTANTIATE_TEST_SUITE_P(
    Strategies,
    StandardSorterWiringStrategyTest,
    ::testing::Values(
        StrategyCase { SortingStrategy::COPY_SORT, SortPath::COPY, true },
        StrategyCase { SortingStrategy::IN_PLACE, SortPath::IN_PLACE, true },
        StrategyCase { SortingStrategy::PARALLEL_SORT, SortPath::PARALLEL, true },
        StrategyCase { SortingStrategy::CHUNKED_SORT, SortPath::CHUNKED_MERGED, true },
        StrategyCase { SortingStrategy::PARTIAL_SORT, SortPath::PARTIAL, false }));

TEST(StandardSorterWiringChunkTest, SortsEachChunkAndMergesInOrder)
{
    auto sorter = std::make_shared<ChannelSorter>(
        make_config(SortingDirection::ASCENDING, SortingStrategy::CHUNKED_SORT, SortingAlgorithm::STANDARD, 4));

    auto result = sorter->apply_operation(single_channel_input(unsorted_values()));

    ASSERT_EQ(result.data.size(), 1U);
    const std::vector<double> expected { 1.0, 2.0, 5.0, 8.0, 3.0, 4.0, 7.0, 9.0, 0.0, 6.0 };
    EXPECT_EQ(channel_of(result, 0), expected);
}

// ============================================================================
// Result facets
// ============================================================================

TEST(StandardSorterWiringRecordTest, CarriesSettingsOnOutput)
{
    auto sorter = std::make_shared<ChannelSorter>(
        make_config(SortingDirection::DESCENDING, SortingStrategy::COPY_SORT, SortingAlgorithm::HEAP, 32));

    auto result = sorter->apply_operation(single_channel_input(unsorted_values()));

    const auto* record = result.facets.get<SortRecord>();
    ASSERT_NE(record, nullptr);
    EXPECT_EQ(record->config.direction, SortingDirection::DESCENDING);
    EXPECT_EQ(record->config.algorithm, SortingAlgorithm::HEAP);
    EXPECT_EQ(record->config.chunk_size, 32U);
    EXPECT_EQ(static_cast<SortPath>(record->path), SortPath::COPY);
}

TEST(StandardSorterWiringRecordTest, EmptyInputGivesEmptyOutput)
{
    auto sorter = std::make_shared<ChannelSorter>();
    ChannelDatum input { std::vector<Kakshya::DataVariant> {} };

    auto result = sorter->apply_operation(input);

    EXPECT_TRUE(result.data.empty());
}

// ============================================================================
// Other containers
// ============================================================================

TEST(StandardSorterWiringEigenTest, SortsEachColumnAscending)
{
    auto sorter = std::make_shared<StandardSorter<Eigen::MatrixXd>>();

    Eigen::MatrixXd test_matrix(2, 3);
    test_matrix << 3.0, 1.0, 2.0,
        6.0, 4.0, 5.0;

    Datum<Eigen::MatrixXd> input { test_matrix };
    auto result = sorter->apply_operation(input);

    for (int col = 0; col < result.data.cols(); ++col) {
        EXPECT_LE(result.data(0, col), result.data(1, col));
    }
}

} // namespace MayaFlux::Test
