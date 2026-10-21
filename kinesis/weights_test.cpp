#include "../test_config.h"

#include "MayaFlux/Kinesis/Stochastic/Weights.hpp"

#include <array>
#include <string>
#include <utility>
#include <vector>

namespace MayaFlux::Test {

using Kinesis::Stochastic::Algorithm;
using Kinesis::Stochastic::Stochastic;
using Kinesis::Stochastic::Weights;

namespace {

    struct NormalizeCase {
        const char* name;
        std::vector<double> row;
        std::vector<double> expected;
    };

    struct EdgeCase {
        const char* name;
        double weight;
        bool symmetric;
        bool normalize;
        std::array<std::array<double, 3>, 3> expected;
    };

    struct PickCase {
        const char* name;
        std::vector<double> weights;
    };

    std::vector<double> row_of(const Weights& table, size_t row)
    {
        const auto view = table.row(row);
        return { view.begin(), view.end() };
    }

class WeightsShapeTest : public ::testing::TestWithParam<std::pair<size_t, size_t>> { };

TEST_P(WeightsShapeTest, ConstructionSizesTheTableAndFillsEveryCell)
{
    const auto [rows, cols] = GetParam();
    const Weights table(rows, cols, 0.25);

    EXPECT_EQ(table.rows(), rows);
    EXPECT_EQ(table.cols(), cols);
    for (size_t r = 0; r < rows; ++r) {
        for (size_t c = 0; c < cols; ++c) {
            EXPECT_DOUBLE_EQ(table.get(r, c), 0.25) << r << "," << c;
        }
    }
}

INSTANTIATE_TEST_SUITE_P(
    Dimensions,
    WeightsShapeTest,
    ::testing::Values(
        std::pair<size_t, size_t> { 1, 1 },
        std::pair<size_t, size_t> { 3, 3 },
        std::pair<size_t, size_t> { 2, 5 },
        std::pair<size_t, size_t> { 5, 2 }));

TEST(WeightsTest, ADefaultTableIsEmpty)
{
    const Weights table;

    EXPECT_EQ(table.rows(), 0U);
    EXPECT_EQ(table.cols(), 0U);
}

TEST(WeightsTest, CellsAreWrittenAndReadThroughTheProxy)
{
    Weights table(2, 3);

    table(1, 2) = 0.75;
    const double read = table(1, 2);

    EXPECT_DOUBLE_EQ(read, 0.75);
    EXPECT_DOUBLE_EQ(table.get(1, 2), 0.75);
    EXPECT_DOUBLE_EQ(table.get(1, 1), 0.0);
    EXPECT_DOUBLE_EQ(table.get(0, 2), 0.0);
}

TEST(WeightsTest, AssigningOneCellFromAnotherCopiesItsValue)
{
    Weights table(2, 2);
    table(0, 0) = 0.4;

    table(1, 1) = table(0, 0);

    EXPECT_DOUBLE_EQ(table.get(1, 1), 0.4);
    EXPECT_DOUBLE_EQ(table.get(0, 0), 0.4);
}

TEST(WeightsTest, RowViewExposesTheRowInColumnOrder)
{
    Weights table(2, 3);
    table(1, 0) = 1.0;
    table(1, 1) = 2.0;
    table(1, 2) = 3.0;

    EXPECT_EQ(row_of(table, 1), (std::vector<double> { 1.0, 2.0, 3.0 }));
    EXPECT_EQ(row_of(table, 0), (std::vector<double> { 0.0, 0.0, 0.0 }));
}

TEST(WeightsTest, NestedListsAssignEveryCell)
{
    Weights table(2, 2);

    table = { { 0.1, 0.9 }, { 0.6, 0.4 } };

    EXPECT_EQ(row_of(table, 0), (std::vector<double> { 0.1, 0.9 }));
    EXPECT_EQ(row_of(table, 1), (std::vector<double> { 0.6, 0.4 }));
}

TEST(WeightsTest, NestedListsOfTheWrongShapeAreRejected)
{
    Weights table(2, 2);

    EXPECT_THROW((table = { { 1.0, 2.0 } }), std::invalid_argument);
    EXPECT_THROW((table = { { 1.0, 2.0 }, { 3.0 } }), std::invalid_argument);
}

TEST(WeightsTest, AssignReplacesTheDimensionsAndResetsTheCells)
{
    Weights table(2, 2, 1.0);

    table.assign(3, 4, 0.5);

    EXPECT_EQ(table.rows(), 3U);
    EXPECT_EQ(table.cols(), 4U);
    EXPECT_DOUBLE_EQ(table.get(2, 3), 0.5);
    EXPECT_DOUBLE_EQ(table.get(0, 0), 0.5);
}

TEST(WeightsTest, FillSetsEveryCell)
{
    Weights table(2, 3, 1.0);

    table.fill(-2.0);

    for (size_t r = 0; r < 2; ++r) {
        for (size_t c = 0; c < 3; ++c) {
            EXPECT_DOUBLE_EQ(table.get(r, c), -2.0);
        }
    }
}

TEST(WeightsTest, ScaleRowMultipliesOnlyThatRow)
{
    Weights table(2, 2, 2.0);

    table.scale_row(0, 0.5);

    EXPECT_EQ(row_of(table, 0), (std::vector<double> { 1.0, 1.0 }));
    EXPECT_EQ(row_of(table, 1), (std::vector<double> { 2.0, 2.0 }));
}

class WeightsNormalizeTest : public ::testing::TestWithParam<NormalizeCase> { };

TEST_P(WeightsNormalizeTest, NormalizeRowScalesToUnitSumUnlessTheSumIsZero)
{
    const auto& config = GetParam();
    Weights table(2, config.row.size(), 1.0);
    for (size_t c = 0; c < config.row.size(); ++c) {
        table(0, c) = config.row[c];
    }

    table.normalize_row(0);

    const auto actual = row_of(table, 0);
    ASSERT_EQ(actual.size(), config.expected.size());
    for (size_t c = 0; c < actual.size(); ++c) {
        EXPECT_NEAR(actual[c], config.expected[c], 1e-12) << "column " << c;
    }
    for (size_t c = 0; c < config.row.size(); ++c) {
        EXPECT_DOUBLE_EQ(table.get(1, c), 1.0);
    }
}

INSTANTIATE_TEST_SUITE_P(
    Rows,
    WeightsNormalizeTest,
    ::testing::Values(
        NormalizeCase { "Uniform", { 1.0, 1.0, 1.0, 1.0 }, { 0.25, 0.25, 0.25, 0.25 } },
        NormalizeCase { "Skewed", { 1.0, 3.0 }, { 0.25, 0.75 } },
        NormalizeCase { "NegativeSum", { -1.0, -3.0 }, { 0.25, 0.75 } },
        NormalizeCase { "ZeroRow", { 0.0, 0.0, 0.0 }, { 0.0, 0.0, 0.0 } },
        NormalizeCase { "CancellingRow", { 1.0, -1.0 }, { 1.0, -1.0 } }),
    [](const ::testing::TestParamInfo<NormalizeCase>& info) { return std::string(info.param.name); });

TEST(WeightsTest, UniformExcludingSelfZeroesTheDiagonalAndNormalizesEveryRow)
{
    Weights table(4, 4);

    table.uniform_excluding_self();

    for (size_t r = 0; r < 4; ++r) {
        EXPECT_DOUBLE_EQ(table.get(r, r), 0.0) << "row " << r;
        for (size_t c = 0; c < 4; ++c) {
            if (c != r) {
                EXPECT_NEAR(table.get(r, c), 1.0 / 3.0, 1e-12) << r << "," << c;
            }
        }
    }
}

TEST(WeightsTest, UniformExcludingSelfFillsRowsPastTheColumnsUniformly)
{
    Weights table(3, 2);

    table.uniform_excluding_self();

    EXPECT_EQ(row_of(table, 0), (std::vector<double> { 0.0, 1.0 }));
    EXPECT_EQ(row_of(table, 1), (std::vector<double> { 1.0, 0.0 }));
    EXPECT_EQ(row_of(table, 2), (std::vector<double> { 0.5, 0.5 }));
}

class WeightsEdgeTest : public ::testing::TestWithParam<EdgeCase> { };

TEST_P(WeightsEdgeTest, EdgesFollowTheDirectionWeightAndNormalizationOptions)
{
    const auto& config = GetParam();
    Weights table(3, 3);
    const std::array<std::pair<size_t, size_t>, 2> edges { { { 0, 1 }, { 1, 2 } } };

    table.apply_edges(edges, config.weight, config.symmetric, config.normalize);

    for (size_t r = 0; r < 3; ++r) {
        for (size_t c = 0; c < 3; ++c) {
            EXPECT_NEAR(table.get(r, c), config.expected[r][c], 1e-12) << r << "," << c;
        }
    }
}

INSTANTIATE_TEST_SUITE_P(
    Options,
    WeightsEdgeTest,
    ::testing::Values(
        EdgeCase { "SymmetricNormalized", 1.0, true, true,
            { { { 0.0, 1.0, 0.0 }, { 0.5, 0.0, 0.5 }, { 0.0, 1.0, 0.0 } } } },
        EdgeCase { "SymmetricRaw", 2.0, true, false,
            { { { 0.0, 2.0, 0.0 }, { 2.0, 0.0, 2.0 }, { 0.0, 2.0, 0.0 } } } },
        EdgeCase { "DirectedRaw", 1.0, false, false,
            { { { 0.0, 1.0, 0.0 }, { 0.0, 0.0, 1.0 }, { 0.0, 0.0, 0.0 } } } },
        EdgeCase { "DirectedNormalized", 3.0, false, true,
            { { { 0.0, 1.0, 0.0 }, { 0.0, 0.0, 1.0 }, { 0.0, 0.0, 0.0 } } } }),
    [](const ::testing::TestParamInfo<EdgeCase>& info) { return std::string(info.param.name); });

TEST(WeightsEdgeExtraTest, EdgesOutsideTheTableAreSkipped)
{
    Weights table(3, 3);
    const std::array<std::pair<size_t, size_t>, 3> edges { { { 0, 5 }, { 7, 0 }, { 1, 2 } } };

    table.apply_edges(edges, 1.0, true, false);

    for (size_t r = 0; r < 3; ++r) {
        for (size_t c = 0; c < 3; ++c) {
            const bool written = (r == 1 && c == 2) || (r == 2 && c == 1);
            EXPECT_DOUBLE_EQ(table.get(r, c), written ? 1.0 : 0.0) << r << "," << c;
        }
    }
}

class WeightsEmptyPickTest : public ::testing::TestWithParam<PickCase> { };

TEST_P(WeightsEmptyPickTest, NoPositiveWeightReturnsTheLength)
{
    Stochastic generator(Algorithm::UNIFORM);
    generator.seed(7);

    EXPECT_EQ(Weights::weighted_pick(GetParam().weights, generator), GetParam().weights.size());
}

INSTANTIATE_TEST_SUITE_P(
    Rows,
    WeightsEmptyPickTest,
    ::testing::Values(
        PickCase { "Empty", {} },
        PickCase { "AllZero", { 0.0, 0.0, 0.0 } },
        PickCase { "AllNegative", { -1.0, -2.0 } },
        PickCase { "ZeroAndNegative", { 0.0, -4.0, 0.0 } }),
    [](const ::testing::TestParamInfo<PickCase>& info) { return std::string(info.param.name); });

class WeightsSinglePickTest : public ::testing::TestWithParam<std::pair<PickCase, size_t>> { };

TEST_P(WeightsSinglePickTest, OnlyPositiveSupportIsEverPicked)
{
    const auto& [config, expected] = GetParam();
    Stochastic generator(Algorithm::UNIFORM);
    generator.seed(11);

    for (size_t draw = 0; draw < 200; ++draw) {
        ASSERT_EQ(Weights::weighted_pick(config.weights, generator), expected) << "draw " << draw;
    }
}

INSTANTIATE_TEST_SUITE_P(
    Rows,
    WeightsSinglePickTest,
    ::testing::Values(
        std::pair { PickCase { "Interior", { 0.0, 0.0, 5.0, 0.0 } }, size_t { 2 } },
        std::pair { PickCase { "First", { 3.0, 0.0, 0.0 } }, size_t { 0 } },
        std::pair { PickCase { "Last", { 0.0, 0.0, 0.1 } }, size_t { 2 } },
        std::pair { PickCase { "NegativesIgnored", { -3.0, 0.0, 2.0 } }, size_t { 2 } }),
    [](const ::testing::TestParamInfo<std::pair<PickCase, size_t>>& info) { return std::string(info.param.first.name); });

TEST(WeightsPickTest, DrawsFollowTheRelativeWeights)
{
    Stochastic generator(Algorithm::UNIFORM);
    generator.seed(2024);
    const std::vector<double> weights { 1.0, 3.0, 0.0 };
    std::array<size_t, 3> counts {};
    constexpr size_t draws = 20000;

    for (size_t i = 0; i < draws; ++i) {
        const size_t pick = Weights::weighted_pick(weights, generator);
        ASSERT_LT(pick, counts.size());
        ++counts[pick];
    }

    EXPECT_EQ(counts[2], 0U);
    EXPECT_NEAR(static_cast<double>(counts[0]) / draws, 0.25, 0.02);
    EXPECT_NEAR(static_cast<double>(counts[1]) / draws, 0.75, 0.02);
}

TEST(WeightsPickTest, RowPickReadsOnlyThatRow)
{
    Weights table(2, 3);
    table = { { 0.0, 1.0, 0.0 }, { 0.0, 0.0, 1.0 } };
    Stochastic generator(Algorithm::UNIFORM);
    generator.seed(5);

    for (size_t draw = 0; draw < 100; ++draw) {
        ASSERT_EQ(table.weighted_pick(0, generator), 1U);
        ASSERT_EQ(table.weighted_pick(1, generator), 2U);
    }
}

TEST(WeightsPickTest, AnAllZeroRowReturnsTheColumnCount)
{
    const Weights table(2, 4);
    Stochastic generator(Algorithm::UNIFORM);
    generator.seed(9);

    EXPECT_EQ(table.weighted_pick(1, generator), 4U);
}

}

}
