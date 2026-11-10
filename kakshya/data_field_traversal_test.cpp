#include "gtest/gtest.h"

#include "MayaFlux/Kakshya/DataFieldTraversal.hpp"
#include "MayaFlux/Kakshya/NDData/CompositeAccess.hpp"
#include "MayaFlux/Kakshya/NDData/CompositeInsertion.hpp"
#include "MayaFlux/Kakshya/Source/CompositeContainer.hpp"

namespace MayaFlux::Test {
namespace {

    using Kakshya::Composite;
    using Kakshya::CompositeArray;
    using Kakshya::CompositeContainer;
    using Kakshya::CompositeInsertion;
    using Kakshya::CompositeLayout;
    using Kakshya::DataFieldTraversal;
    using Kakshya::FieldArray;

    using Traversal = DataFieldTraversal<CompositeArray>;

    struct SparseSlice {
        const std::vector<std::optional<int>>* values;
        size_t start;
        size_t count;

        [[nodiscard]] size_t size() const noexcept { return count; }

        [[nodiscard]] std::optional<int> at(size_t index) const noexcept
        {
            if (index >= count)
                return std::nullopt;
            return (*values)[start + index];
        }
    };

    struct SparseArray {
        std::vector<std::optional<int>> values;

        [[nodiscard]] size_t size() const noexcept { return values.size(); }

        [[nodiscard]] std::optional<int> at(size_t index) const noexcept
        {
            if (index >= values.size())
                return std::nullopt;
            return values[index];
        }

        [[nodiscard]] std::optional<SparseSlice> slice(size_t start, size_t count) const
        {
            if (start > values.size() || count > values.size() - start)
                return std::nullopt;
            return SparseSlice { .values = &values, .start = start, .count = count };
        }
    };

    static_assert(FieldArray<CompositeArray>);
    static_assert(FieldArray<SparseArray>);

    CompositeLayout scored_layout()
    {
        CompositeLayout layout;
        layout.add_field<std::string>("name");
        layout.add_field<double>("score");
        return layout;
    }

    CompositeArray scored_array(size_t rows)
    {
        CompositeArray array(scored_layout());
        CompositeInsertion insertion(array);

        for (size_t i = 0; i < rows; ++i) {
            const auto row = insertion.append();
            insertion.set_text(row, "name", "r" + std::to_string(i));
            if (i != 2)
                insertion.set(row, "score", static_cast<double>(i) * 1.5);
        }

        return array;
    }

    std::string name_of(const Composite& element)
    {
        return std::string(element.text("name").value_or(""));
    }

    class DataFieldTraversalTest : public ::testing::Test {
    protected:
        CompositeArray array = scored_array(5);
    };

    TEST_F(DataFieldTraversalTest, BatchSizeDefaultsToOne)
    {
        const Traversal traversal(array);

        EXPECT_EQ(traversal.batch_size(), 1U);
    }

    TEST_F(DataFieldTraversalTest, ZeroBatchSizeIsNormalizedToOne)
    {
        const Traversal traversal(array, 0);

        EXPECT_EQ(traversal.batch_size(), 1U);
    }

    TEST_F(DataFieldTraversalTest, SetBatchSizeNormalizesZeroToOne)
    {
        Traversal traversal(array, 4);
        traversal.set_batch_size(0);

        EXPECT_EQ(traversal.batch_size(), 1U);

        traversal.set_batch_size(3);
        EXPECT_EQ(traversal.batch_size(), 3U);
    }

    TEST_F(DataFieldTraversalTest, StartsAtTheFirstElement)
    {
        const Traversal traversal(array, 2);

        EXPECT_EQ(traversal.position(), 0U);
        EXPECT_EQ(traversal.remaining(), 5U);
        EXPECT_FALSE(traversal.at_end());
    }

    TEST_F(DataFieldTraversalTest, CurrentWindowHoldsBatchSizeElements)
    {
        const Traversal traversal(array, 2);

        const auto window = traversal.current();
        ASSERT_TRUE(window);
        EXPECT_EQ(window->size(), 2U);
        EXPECT_EQ(name_of(*window->at(0)), "r0");
        EXPECT_EQ(name_of(*window->at(1)), "r1");
    }

    TEST_F(DataFieldTraversalTest, FinalWindowIsShortened)
    {
        Traversal traversal(array, 3);
        traversal.seek(4);

        const auto window = traversal.current();
        ASSERT_TRUE(window);
        EXPECT_EQ(window->size(), 1U);
        EXPECT_EQ(name_of(*window->at(0)), "r4");
    }

    TEST_F(DataFieldTraversalTest, AdvanceWithoutArgumentMovesByBatchSize)
    {
        Traversal traversal(array, 2);

        traversal.advance();
        EXPECT_EQ(traversal.position(), 2U);

        traversal.advance();
        EXPECT_EQ(traversal.position(), 4U);
    }

    TEST_F(DataFieldTraversalTest, AdvanceWithExplicitCountMovesByThatCount)
    {
        Traversal traversal(array, 2);

        traversal.advance(1);
        EXPECT_EQ(traversal.position(), 1U);
        EXPECT_EQ(traversal.batch_size(), 2U);
    }

    TEST_F(DataFieldTraversalTest, AdvanceNeverPassesTheEnd)
    {
        Traversal traversal(array, 2);

        traversal.advance(100);

        EXPECT_EQ(traversal.position(), 5U);
        EXPECT_EQ(traversal.remaining(), 0U);
        EXPECT_TRUE(traversal.at_end());
    }

    TEST_F(DataFieldTraversalTest, SeekSetsThePosition)
    {
        Traversal traversal(array, 2);

        traversal.seek(3);

        EXPECT_EQ(traversal.position(), 3U);
        EXPECT_EQ(traversal.remaining(), 2U);
        EXPECT_FALSE(traversal.at_end());
    }

    TEST_F(DataFieldTraversalTest, SeekClampsToTheEnd)
    {
        Traversal traversal(array, 2);

        traversal.seek(99);

        EXPECT_EQ(traversal.position(), 5U);
        EXPECT_TRUE(traversal.at_end());
    }

    TEST_F(DataFieldTraversalTest, WindowAtTheEndIsEmpty)
    {
        Traversal traversal(array, 2);
        traversal.seek(5);

        const auto window = traversal.current();
        ASSERT_TRUE(window);
        EXPECT_EQ(window->size(), 0U);
    }

    TEST_F(DataFieldTraversalTest, WindowsDoNotMoveThePosition)
    {
        const Traversal traversal(array, 2);

        traversal.for_each_in_window([](const Composite&) { });
        EXPECT_TRUE(traversal.window_to_nddata<double>("score"));

        EXPECT_EQ(traversal.position(), 0U);
    }

    TEST_F(DataFieldTraversalTest, ForEachVisitsEveryElementInOrder)
    {
        const Traversal traversal(array, 2);
        std::vector<std::string> names;

        traversal.for_each([&](const Composite& element) {
            names.push_back(name_of(element));
        });

        EXPECT_EQ(names, (std::vector<std::string> { "r0", "r1", "r2", "r3", "r4" }));
    }

    TEST_F(DataFieldTraversalTest, ForEachInWindowVisitsOnlyTheCurrentWindow)
    {
        Traversal traversal(array, 2);
        traversal.seek(1);
        std::vector<std::string> names;

        traversal.for_each_in_window([&](const Composite& element) {
            names.push_back(name_of(element));
        });

        EXPECT_EQ(names, (std::vector<std::string> { "r1", "r2" }));
    }

    TEST_F(DataFieldTraversalTest, ForEachInWindowAtTheEndVisitsNothing)
    {
        Traversal traversal(array, 2);
        traversal.seek(5);
        size_t visits = 0;

        traversal.for_each_in_window([&](const Composite&) { ++visits; });

        EXPECT_EQ(visits, 0U);
    }

    TEST_F(DataFieldTraversalTest, ReduceFoldsEveryElementRegardlessOfPosition)
    {
        Traversal traversal(array, 2);
        traversal.seek(3);

        const double total = traversal.reduce(0.0, [](double sum, const Composite& element) {
            return sum + element.get<double>("score").value_or(0.0);
        });

        EXPECT_EQ(total, 12.0);
    }

    TEST_F(DataFieldTraversalTest, ReduceReturnsTheInitialValueForAnEmptyArray)
    {
        const CompositeArray empty(scored_layout());
        const Traversal traversal(empty, 2);

        const auto count = traversal.reduce(size_t { 7 }, [](size_t sum, const Composite&) {
            return sum + 1;
        });

        EXPECT_EQ(count, 7U);
    }

    TEST_F(DataFieldTraversalTest, ProjectsTheWholeArrayAsValuesAndPresence)
    {
        const Traversal traversal(array, 2);

        const auto projection = traversal.to_nddata<double>("score");
        ASSERT_TRUE(projection);

        EXPECT_EQ(std::get<std::vector<double>>(projection->values),
            (std::vector<double> { 0.0, 1.5, 0.0, 4.5, 6.0 }));
        EXPECT_EQ(std::get<std::vector<uint8_t>>(projection->presence),
            (std::vector<uint8_t> { 1, 1, 0, 1, 1 }));
    }

    TEST_F(DataFieldTraversalTest, ProjectsOnlyTheCurrentWindow)
    {
        Traversal traversal(array, 2);
        traversal.seek(1);

        const auto projection = traversal.window_to_nddata<double>("score");
        ASSERT_TRUE(projection);

        EXPECT_EQ(std::get<std::vector<double>>(projection->values),
            (std::vector<double> { 1.5, 0.0 }));
        EXPECT_EQ(std::get<std::vector<uint8_t>>(projection->presence),
            (std::vector<uint8_t> { 1, 0 }));
    }

    TEST_F(DataFieldTraversalTest, WindowProjectionAtTheEndHasNoValues)
    {
        Traversal traversal(array, 2);
        traversal.seek(5);

        const auto projection = traversal.window_to_nddata<double>("score");
        ASSERT_TRUE(projection);
        EXPECT_TRUE(std::get<std::vector<double>>(projection->values).empty());
    }

    TEST_F(DataFieldTraversalTest, ProjectionRefusesAnotherFieldType)
    {
        const Traversal traversal(array, 2);

        EXPECT_FALSE(traversal.to_nddata<uint32_t>("score"));
    }

    TEST_F(DataFieldTraversalTest, ProjectionRefusesAnUnknownField)
    {
        const Traversal traversal(array, 2);

        EXPECT_FALSE(traversal.to_nddata<double>("missing"));
    }

    TEST_F(DataFieldTraversalTest, IndependentTraversalsKeepTheirOwnPositions)
    {
        Traversal first(array, 2);
        Traversal second(array, 3);

        first.advance();
        second.seek(4);

        EXPECT_EQ(first.position(), 2U);
        EXPECT_EQ(second.position(), 4U);
        EXPECT_EQ(first.batch_size(), 2U);
        EXPECT_EQ(second.batch_size(), 3U);
    }

    TEST_F(DataFieldTraversalTest, SeesElementsAppendedAfterConstruction)
    {
        const Traversal traversal(array, 2);

        CompositeInsertion insertion(array);
        EXPECT_EQ(insertion.append(), 5U);

        EXPECT_EQ(traversal.remaining(), 6U);
    }

    TEST_F(DataFieldTraversalTest, EmptyArrayIsAtTheEndImmediately)
    {
        const CompositeArray empty(scored_layout());
        const Traversal traversal(empty, 2);

        EXPECT_TRUE(traversal.at_end());
        EXPECT_EQ(traversal.remaining(), 0U);

        const auto window = traversal.current();
        ASSERT_TRUE(window);
        EXPECT_EQ(window->size(), 0U);
    }

    TEST_F(DataFieldTraversalTest, ContainerTraverseBorrowsItsArray)
    {
        CompositeContainer container(scored_layout(), 2);
        auto insertion = container.insert();
        for (size_t i = 0; i < 3; ++i) {
            const auto row = insertion.append();
            insertion.set_text(row, "name", "c" + std::to_string(i));
        }

        auto traversal = container.traverse(2);
        EXPECT_EQ(traversal.remaining(), 3U);

        traversal.advance();
        std::vector<std::string> names;
        traversal.for_each_in_window([&](const Composite& element) {
            names.push_back(name_of(element));
        });

        EXPECT_EQ(names, (std::vector<std::string> { "c2" }));
    }

    TEST(SparseTraversalTest, SkipsElementsThatAreNotAvailable)
    {
        const SparseArray sparse { .values = { 1, std::nullopt, 3, std::nullopt, 5 } };
        const DataFieldTraversal<SparseArray> traversal(sparse, 2);
        std::vector<int> visited;

        traversal.for_each([&](int value) { visited.push_back(value); });

        EXPECT_EQ(visited, (std::vector<int> { 1, 3, 5 }));
    }

    TEST(SparseTraversalTest, WindowSkipsElementsThatAreNotAvailable)
    {
        const SparseArray sparse { .values = { std::nullopt, 2, std::nullopt, 4 } };
        DataFieldTraversal<SparseArray> traversal(sparse, 2);
        traversal.advance();
        std::vector<int> visited;

        traversal.for_each_in_window([&](int value) { visited.push_back(value); });

        EXPECT_EQ(visited, (std::vector<int> { 4 }));
    }

    TEST(SparseTraversalTest, ReduceSkipsElementsThatAreNotAvailable)
    {
        const SparseArray sparse { .values = { 1, std::nullopt, 3, std::nullopt, 5 } };
        const DataFieldTraversal<SparseArray> traversal(sparse, 1);

        const int total = traversal.reduce(0, [](int sum, int value) { return sum + value; });

        EXPECT_EQ(total, 9);
    }

}
}
