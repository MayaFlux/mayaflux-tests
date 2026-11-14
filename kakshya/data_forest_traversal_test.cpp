#include "gtest/gtest.h"

#include "MayaFlux/Kakshya/DataForestTraversal.hpp"
#include "MayaFlux/Kakshya/NDData/CompositeAccess.hpp"
#include "MayaFlux/Kakshya/NDData/CompositeForest.hpp"
#include "MayaFlux/Kakshya/NDData/CompositeInsertion.hpp"
#include "MayaFlux/Kakshya/Source/CompositeContainer.hpp"

namespace MayaFlux::Test {
namespace {

    using Kakshya::Composite;
    using Kakshya::CompositeArray;
    using Kakshya::CompositeContainer;
    using Kakshya::CompositeForest;
    using Kakshya::CompositeInsertion;
    using Kakshya::CompositeLayout;
    using Kakshya::CompositeSlice;
    using Kakshya::DataFieldTraversal;
    using Kakshya::DataForestTraversal;
    using Kakshya::FieldArray;
    using Kakshya::ForestIndex;

    constexpr uint64_t none = ForestIndex::no_parent;

    static_assert(FieldArray<CompositeSlice>);

    CompositeLayout forest_layout()
    {
        CompositeLayout layout;
        layout.add_field<uint64_t>("parent");
        layout.add_field<std::string>("name");
        layout.add_field<uint32_t>("weight");
        return layout;
    }

    CompositeArray rows_with_parents(const std::vector<int>& parents)
    {
        CompositeArray rows(forest_layout());
        CompositeInsertion insert(rows);
        for (size_t i = 0; i < parents.size(); ++i) {
            const size_t row = insert.append();
            insert.set_text(row, "name", std::string(1, static_cast<char>('A' + i)));
            insert.set(row, "weight", static_cast<uint32_t>(i + 1));
            if (parents[i] >= 0)
                insert.set(row, "parent", static_cast<uint64_t>(parents[i]));
        }
        return rows;
    }

    CompositeArray sample_rows()
    {
        return rows_with_parents({ -1, 0, 0, 2, -1, 4 });
    }

    std::optional<uint64_t> parent_of(const Composite& row)
    {
        return row.get<uint64_t>("parent");
    }

    std::string name_of(const Composite& row)
    {
        return std::string(row.text("name").value_or(""));
    }

    std::vector<uint64_t> to_vector(std::span<const uint64_t> values)
    {
        return { values.begin(), values.end() };
    }

    using Rows = std::vector<uint64_t>;

    TEST(ForestIndexTest, BuildsStructureFromParents)
    {
        const std::vector<uint64_t> parents { none, 0, 0, 2, none, 4 };
        std::string error;
        const auto index = ForestIndex::build(parents, error);
        ASSERT_TRUE(index) << error;

        EXPECT_EQ(index->size(), 6U);
        EXPECT_FALSE(index->parent(0));
        EXPECT_EQ(index->parent(3), std::optional<uint64_t> { 2 });
        EXPECT_EQ(to_vector(index->children(0)), (Rows { 1, 2 }));
        EXPECT_EQ(to_vector(index->children(2)), (Rows { 3 }));
        EXPECT_TRUE(index->children(1).empty());
        EXPECT_EQ(to_vector(index->roots()), (Rows { 0, 4 }));

        const std::vector<size_t> sizes { 4, 1, 2, 1, 2, 1 };
        const std::vector<uint32_t> depths { 0, 1, 1, 2, 0, 1 };
        for (size_t row = 0; row < 6; ++row) {
            EXPECT_EQ(index->subtree_size(row), sizes[row]) << row;
            EXPECT_EQ(index->depth(row), depths[row]) << row;
        }

        EXPECT_TRUE(index->preorder());
        const auto range = index->subtree_range(2);
        ASSERT_TRUE(range);
        EXPECT_EQ(range->first, 2U);
        EXPECT_EQ(range->count, 2U);
    }

    TEST(ForestIndexTest, OutOfRangeRowsAreEmpty)
    {
        std::string error;
        const auto index = ForestIndex::build(std::vector<uint64_t> { none, 0 }, error);
        ASSERT_TRUE(index) << error;
        EXPECT_FALSE(index->parent(99));
        EXPECT_TRUE(index->children(99).empty());
        EXPECT_EQ(index->subtree_size(99), 0U);
        EXPECT_EQ(index->depth(99), 0U);
        EXPECT_FALSE(index->subtree_range(99));
    }

    TEST(ForestIndexTest, RejectsAParentThatDoesNotPrecedeItsChild)
    {
        std::string error;
        uint64_t error_row = 99;
        EXPECT_FALSE(ForestIndex::build(std::vector<uint64_t> { none, 2, 0 }, error, &error_row));
        EXPECT_EQ(error_row, 1U);
        EXPECT_NE(error.find("must precede"), std::string::npos);

        EXPECT_FALSE(ForestIndex::build(std::vector<uint64_t> { 0 }, error, &error_row));
        EXPECT_EQ(error_row, 0U);
    }

    TEST(ForestIndexTest, ReportsRowsWhoseSubtreesAreNotConsecutive)
    {
        std::string error;
        const auto index = ForestIndex::build(std::vector<uint64_t> { none, none, 0 }, error);
        ASSERT_TRUE(index) << error;

        EXPECT_FALSE(index->preorder());
        EXPECT_EQ(to_vector(index->roots()), (Rows { 0, 1 }));
        EXPECT_EQ(to_vector(index->children(0)), (Rows { 2 }));
        EXPECT_EQ(index->subtree_size(0), 2U);
        EXPECT_FALSE(index->subtree_range(0));
    }

    TEST(ForestIndexTest, EmptyInputIsAnEmptyPreorderForest)
    {
        std::string error;
        const auto index = ForestIndex::build(std::vector<uint64_t> {}, error);
        ASSERT_TRUE(index) << error;
        EXPECT_EQ(index->size(), 0U);
        EXPECT_TRUE(index->preorder());
        EXPECT_TRUE(index->roots().empty());
    }

    TEST(DataForestTraversalTest, VisitsRootsChildrenAncestorsAndDescendants)
    {
        const CompositeArray rows = sample_rows();
        std::string error;
        const auto index = Kakshya::build_forest_index(rows, parent_of, error);
        ASSERT_TRUE(index) << error;

        DataForestTraversal<CompositeArray> forest(rows, *index);
        EXPECT_TRUE(forest.matches());

        const auto collect = [&](auto walk) {
            std::vector<std::string> names;
            EXPECT_TRUE(walk([&](size_t, const Composite& row) { names.push_back(name_of(row)); }));
            return names;
        };
        using Names = std::vector<std::string>;

        EXPECT_EQ(collect([&](auto fn) { return forest.for_each_root(fn); }), (Names { "A", "E" }));
        EXPECT_EQ(collect([&](auto fn) { return forest.for_each_child(0, fn); }), (Names { "B", "C" }));
        EXPECT_EQ(collect([&](auto fn) { return forest.for_each_ancestor(3, fn); }), (Names { "C", "A" }));
        EXPECT_EQ(collect([&](auto fn) { return forest.for_each_descendant(0, fn); }), (Names { "B", "C", "D" }));
        EXPECT_EQ(collect([&](auto fn) { return forest.for_each_descendant(4, fn); }), (Names { "F" }));
        EXPECT_TRUE(collect([&](auto fn) { return forest.for_each_descendant(1, fn); }).empty());
    }

    TEST(DataForestTraversalTest, VisitorsPassTheRowIndex)
    {
        const CompositeArray rows = sample_rows();
        std::string error;
        const auto index = Kakshya::build_forest_index(rows, parent_of, error);
        ASSERT_TRUE(index) << error;

        DataForestTraversal<CompositeArray> forest(rows, *index);
        std::vector<size_t> visited;
        forest.for_each_descendant(0, [&](size_t row, const Composite&) { visited.push_back(row); });
        EXPECT_EQ(visited, (std::vector<size_t> { 1, 2, 3 }));
    }

    TEST(DataForestTraversalTest, AVisitorReturningFalseStopsTheWalk)
    {
        const CompositeArray rows = sample_rows();
        std::string error;
        const auto index = Kakshya::build_forest_index(rows, parent_of, error);
        ASSERT_TRUE(index) << error;

        DataForestTraversal<CompositeArray> forest(rows, *index);
        size_t visited = 0;
        EXPECT_FALSE(forest.for_each_descendant(0, [&](size_t, const Composite&) {
            ++visited;
            return false;
        }));
        EXPECT_EQ(visited, 1U);

        visited = 0;
        EXPECT_FALSE(forest.for_each_child(0, [&](size_t, const Composite&) {
            ++visited;
            return false;
        }));
        EXPECT_EQ(visited, 1U);
    }

    TEST(DataForestTraversalTest, SubtreeIsASliceThatAFieldTraversalCanWindow)
    {
        const CompositeArray rows = sample_rows();
        std::string error;
        const auto index = Kakshya::build_forest_index(rows, parent_of, error);
        ASSERT_TRUE(index) << error;

        DataForestTraversal<CompositeArray> forest(rows, *index);
        const auto branch = forest.subtree(2);
        ASSERT_TRUE(branch);
        ASSERT_EQ(branch->size(), 2U);
        EXPECT_EQ(name_of(*branch->at(0)), "C");

        DataFieldTraversal<CompositeSlice> window(*branch, 1);
        ASSERT_TRUE(window.current());
        EXPECT_EQ(window.current()->size(), 1U);
        window.advance();
        EXPECT_EQ(name_of(*window.current()->at(0)), "D");

        std::vector<std::string> names;
        window.for_each([&](const Composite& row) { names.push_back(name_of(row)); });
        EXPECT_EQ(names, (std::vector<std::string> { "C", "D" }));

        const auto weights = window.to_nddata<uint32_t>("weight");
        ASSERT_TRUE(weights);
        EXPECT_EQ(std::get<std::vector<uint32_t>>(weights->values), (std::vector<uint32_t> { 3, 4 }));
    }

    TEST(DataForestTraversalTest, SubtreeIsEmptyForABadRowOrANonPreorderForest)
    {
        const CompositeArray rows = sample_rows();
        std::string error;
        const auto index = Kakshya::build_forest_index(rows, parent_of, error);
        ASSERT_TRUE(index) << error;
        EXPECT_FALSE(DataForestTraversal<CompositeArray>(rows, *index).subtree(99));

        const CompositeArray scattered = rows_with_parents({ -1, -1, 0 });
        const auto scattered_index = Kakshya::build_forest_index(scattered, parent_of, error);
        ASSERT_TRUE(scattered_index) << error;
        EXPECT_FALSE(DataForestTraversal<CompositeArray>(scattered, *scattered_index).subtree(0));
    }

    TEST(DataForestTraversalTest, DescendantsFollowChildLinksWhenRowsAreNotPreorder)
    {
        const CompositeArray rows = rows_with_parents({ -1, -1, 0, 2 });
        std::string error;
        const auto index = Kakshya::build_forest_index(rows, parent_of, error);
        ASSERT_TRUE(index) << error;
        ASSERT_FALSE(index->preorder());

        DataForestTraversal<CompositeArray> forest(rows, *index);
        std::vector<std::string> names;
        forest.for_each_descendant(0, [&](size_t, const Composite& row) { names.push_back(name_of(row)); });
        EXPECT_EQ(names, (std::vector<std::string> { "C", "D" }));
    }

    TEST(DataForestTraversalTest, ReduceSubtreeFoldsTheRowAndItsDescendants)
    {
        const CompositeArray rows = sample_rows();
        std::string error;
        const auto index = Kakshya::build_forest_index(rows, parent_of, error);
        ASSERT_TRUE(index) << error;

        DataForestTraversal<CompositeArray> forest(rows, *index);
        const uint32_t total = forest.reduce_subtree(2, uint32_t { 0 },
            [](uint32_t sum, size_t, const Composite& row) {
                return sum + row.get<uint32_t>("weight").value_or(0);
            });
        EXPECT_EQ(total, 7U);
    }

    struct ParentSlice {
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

    struct ParentArray {
        std::vector<std::optional<int>> values;

        [[nodiscard]] size_t size() const noexcept { return values.size(); }

        [[nodiscard]] std::optional<int> at(size_t index) const noexcept
        {
            if (index >= values.size())
                return std::nullopt;
            return values[index];
        }

        [[nodiscard]] std::optional<ParentSlice> slice(size_t start, size_t count) const
        {
            if (start > values.size() || count > values.size() - start)
                return std::nullopt;
            return ParentSlice { .values = &values, .start = start, .count = count };
        }
    };

    static_assert(FieldArray<ParentArray>);

    std::optional<uint64_t> int_parent(int value)
    {
        if (value < 0)
            return std::nullopt;
        return static_cast<uint64_t>(value);
    }

    TEST(DataForestTraversalTest, WorksOverAnyFieldArray)
    {
        const ParentArray array { .values = { -1, 0, 0, 2 } };
        std::string error;
        const auto index = Kakshya::build_forest_index(array, int_parent, error);
        ASSERT_TRUE(index) << error;

        DataForestTraversal<ParentArray> forest(array, *index);
        std::vector<size_t> children;
        forest.for_each_child(0, [&](size_t row, int) { children.push_back(row); });
        EXPECT_EQ(children, (std::vector<size_t> { 1, 2 }));

        const auto branch = forest.subtree(2);
        ASSERT_TRUE(branch);
        EXPECT_EQ(branch->size(), 2U);
    }

    TEST(DataForestTraversalTest, BuildFailsForAnUnreadableRow)
    {
        const ParentArray array { .values = { -1, std::nullopt, 0 } };
        std::string error;
        uint64_t error_row = 99;
        EXPECT_FALSE(Kakshya::build_forest_index(array, int_parent, error, &error_row));
        EXPECT_EQ(error_row, 1U);
        EXPECT_EQ(error, "row is unreadable");
    }

    TEST(CompositeForestTest, IndexesACompositeByItsParentField)
    {
        const CompositeArray rows = sample_rows();
        std::string error;
        const auto index = Kakshya::index_composite_forest(rows, "parent", error);
        ASSERT_TRUE(index) << error;
        EXPECT_TRUE(index->preorder());
        EXPECT_EQ(to_vector(index->roots()), (Rows { 0, 4 }));
    }

    TEST(CompositeForestTest, RejectsAMissingOrMistypedParentField)
    {
        const CompositeArray rows = sample_rows();
        std::string error;
        EXPECT_FALSE(Kakshya::index_composite_forest(rows, "absent", error));
        EXPECT_NE(error.find("no uint64_t field"), std::string::npos);
        EXPECT_FALSE(Kakshya::index_composite_forest(rows, "name", error));
        EXPECT_NE(error.find("no uint64_t field"), std::string::npos);
    }

    TEST(CompositeForestTest, RejectsAParentThatFollowsItsChild)
    {
        CompositeArray rows(forest_layout());
        CompositeInsertion insert(rows);
        const size_t first = insert.append();
        const size_t second = insert.append();
        insert.set(first, "parent", uint64_t { 1 });
        insert.set(second, "weight", uint32_t { 1 });

        std::string error;
        uint64_t error_row = 99;
        EXPECT_FALSE(Kakshya::index_composite_forest(rows, "parent", error, &error_row));
        EXPECT_EQ(error_row, 0U);
    }

    TEST(CompositeForestTest, SubtreeRegionSelectsABranchInContainerTerms)
    {
        const CompositeArray rows = sample_rows();
        std::string error;
        const auto index = Kakshya::index_composite_forest(rows, "parent", error);
        ASSERT_TRUE(index) << error;

        const auto region = Kakshya::forest_subtree_region(*index, 2);
        ASSERT_TRUE(region);
        EXPECT_EQ(region->start_coordinates, (Rows { 2 }));
        EXPECT_EQ(region->end_coordinates, (Rows { 3 }));
        EXPECT_FALSE(Kakshya::forest_subtree_region(*index, 99));

        const auto selected = rows.slice(*region);
        ASSERT_TRUE(selected);
        EXPECT_EQ(selected->size(), 2U);
        EXPECT_EQ(name_of(*selected->at(1)), "D");
    }

    TEST(CompositeForestTest, ARegionCopyThroughTheContainerKeepsGlobalParentLinks)
    {
        CompositeContainer container(sample_rows());
        std::string error;
        const auto index = Kakshya::index_composite_forest(container.get_data(), "parent", error);
        ASSERT_TRUE(index) << error;

        const auto copies = container.get_region_data(*Kakshya::forest_subtree_region(*index, 2));
        ASSERT_EQ(copies.size(), 1U);
        ASSERT_EQ(copies.front().size(), 2U);
        EXPECT_EQ(copies.front().at(1)->get<uint64_t>("parent"), std::optional<uint64_t> { 2 });
        EXPECT_FALSE(Kakshya::index_composite_forest(copies.front(), "parent", error));
    }

    TEST(CompositeForestTest, CopySubtreeRebasesParentLinks)
    {
        const CompositeArray rows = sample_rows();
        std::string error;
        const auto index = Kakshya::index_composite_forest(rows, "parent", error);
        ASSERT_TRUE(index) << error;

        const auto whole = Kakshya::copy_forest_subtree(rows, *index, "parent", 0, error);
        ASSERT_TRUE(whole) << error;
        ASSERT_EQ(whole->size(), 4U);
        EXPECT_FALSE(whole->at(0)->has("parent"));
        EXPECT_EQ(whole->at(3)->get<uint64_t>("parent"), std::optional<uint64_t> { 2 });

        const auto branch = Kakshya::copy_forest_subtree(rows, *index, "parent", 2, error);
        ASSERT_TRUE(branch) << error;
        ASSERT_EQ(branch->size(), 2U);
        EXPECT_EQ(name_of(*branch->at(0)), "C");
        EXPECT_FALSE(branch->at(0)->has("parent"));
        EXPECT_EQ(branch->at(1)->get<uint64_t>("parent"), std::optional<uint64_t> { 0 });

        const auto offset = Kakshya::copy_forest_subtree(rows, *index, "parent", 4, error);
        ASSERT_TRUE(offset) << error;
        EXPECT_EQ(offset->at(1)->get<uint64_t>("parent"), std::optional<uint64_t> { 0 });

        const auto reindexed = Kakshya::index_composite_forest(*branch, "parent", error);
        ASSERT_TRUE(reindexed) << error;
        EXPECT_EQ(to_vector(reindexed->roots()), (Rows { 0 }));
        EXPECT_EQ(name_of(*rows.at(2)), "C");
    }

    TEST(CompositeForestTest, CopySubtreeRejectsBadInput)
    {
        const CompositeArray rows = sample_rows();
        std::string error;
        const auto index = Kakshya::index_composite_forest(rows, "parent", error);
        ASSERT_TRUE(index) << error;

        EXPECT_FALSE(Kakshya::copy_forest_subtree(rows, *index, "parent", 99, error));
        EXPECT_FALSE(Kakshya::copy_forest_subtree(rows, *index, "absent", 0, error));
        EXPECT_NE(error.find("no uint64_t field"), std::string::npos);

        const CompositeArray scattered = rows_with_parents({ -1, -1, 0 });
        const auto scattered_index = Kakshya::index_composite_forest(scattered, "parent", error);
        ASSERT_TRUE(scattered_index) << error;
        EXPECT_FALSE(Kakshya::copy_forest_subtree(scattered, *scattered_index, "parent", 0, error));
    }

    TEST(CompositeForestTest, BuildOwnsTheRowsAndTheirIndex)
    {
        std::string error;
        const auto forest = CompositeForest::build(sample_rows(), "parent", error);
        ASSERT_TRUE(forest) << error;
        EXPECT_EQ(forest->rows().size(), 6U);
        EXPECT_EQ(forest->index().size(), 6U);

        std::vector<std::string> names;
        forest->traversal().for_each_child(0, [&](size_t, const Composite& row) {
            names.push_back(name_of(row));
        });
        EXPECT_EQ(names, (std::vector<std::string> { "B", "C" }));

        const auto region = forest->subtree_region(4);
        ASSERT_TRUE(region);
        EXPECT_EQ(region->end_coordinates, (Rows { 5 }));

        const auto branch = forest->copy_subtree(4, error);
        ASSERT_TRUE(branch) << error;
        EXPECT_EQ(branch->size(), 2U);
    }

    TEST(CompositeForestTest, BuildFailsForAnUnindexableArray)
    {
        std::string error;
        EXPECT_FALSE(CompositeForest::build(sample_rows(), "absent", error));
        EXPECT_FALSE(error.empty());
    }

    TEST(CompositeSliceSlicingTest, SubSliceIsRelativeToTheSlice)
    {
        const CompositeArray rows = sample_rows();
        const auto outer = rows.slice(1, 4);
        ASSERT_TRUE(outer);

        const auto inner = outer->slice(1, 2);
        ASSERT_TRUE(inner);
        EXPECT_EQ(inner->size(), 2U);
        EXPECT_EQ(inner->start(), 2U);
        EXPECT_EQ(name_of(*inner->at(0)), "C");
        EXPECT_EQ(name_of(*inner->at(1)), "D");

        const auto empty = outer->slice(4, 0);
        ASSERT_TRUE(empty);
        EXPECT_EQ(empty->size(), 0U);
        EXPECT_FALSE(outer->slice(3, 2));
        EXPECT_FALSE(outer->slice(5, 0));
    }

    TEST(CompositeSliceSlicingTest, SubSliceDropsTheSourceRegion)
    {
        const CompositeArray rows = sample_rows();
        const auto outer = rows.slice(Kakshya::Region(std::vector<uint64_t> { 1 }, std::vector<uint64_t> { 4 }));
        ASSERT_TRUE(outer);
        ASSERT_NE(outer->region(), nullptr);

        const auto inner = outer->slice(0, 2);
        ASSERT_TRUE(inner);
        EXPECT_EQ(inner->region(), nullptr);
    }

    TEST(CompositeContainerForestTest, IndexesAndTraversesTheContainersParentLinks)
    {
        const CompositeContainer container(sample_rows());
        std::string error;
        const auto index = container.index_forest("parent", error);
        ASSERT_TRUE(index) << error;

        const auto forest = container.traverse_forest(*index);
        EXPECT_TRUE(forest.matches());

        std::vector<std::string> names;
        forest.for_each_child(0, [&](size_t, const Composite& row) { names.push_back(name_of(row)); });
        EXPECT_EQ(names, (std::vector<std::string> { "B", "C" }));

        const auto branch = forest.subtree(2);
        ASSERT_TRUE(branch);
        EXPECT_EQ(branch->size(), 2U);
    }

    TEST(CompositeContainerForestTest, IndexForestReportsABadParentField)
    {
        const CompositeContainer container(sample_rows());
        std::string error;
        EXPECT_FALSE(container.index_forest("absent", error));
        EXPECT_NE(error.find("no uint64_t field"), std::string::npos);
    }

    TEST(CompositeContainerForestTest, MaterializeSubtreeFillsTheWindowWithARebasedBranch)
    {
        CompositeContainer container(sample_rows());
        std::string error;
        const auto index = container.index_forest("parent", error);
        ASSERT_TRUE(index) << error;

        ASSERT_TRUE(container.materialize_subtree(*index, "parent", 2, error)) << error;
        const CompositeArray& window = container.get_materialized_data();
        ASSERT_EQ(window.size(), 2U);
        EXPECT_EQ(name_of(*window.at(0)), "C");
        EXPECT_EQ(name_of(*window.at(1)), "D");
        EXPECT_FALSE(window.at(0)->has("parent"));
        EXPECT_EQ(window.at(1)->get<uint64_t>("parent"), std::optional<uint64_t> { 0 });
        EXPECT_EQ(container.position(), 0U);
        EXPECT_TRUE(Kakshya::index_composite_forest(window, "parent", error));
    }

    TEST(CompositeContainerForestTest, AFailedMaterializeSubtreeKeepsThePriorWindow)
    {
        CompositeContainer container(sample_rows(), 1);
        ASSERT_TRUE(container.materialize_next());
        ASSERT_EQ(container.get_materialized_data().size(), 1U);

        std::string error;
        const auto index = container.index_forest("parent", error);
        ASSERT_TRUE(index) << error;
        EXPECT_FALSE(container.materialize_subtree(*index, "parent", 99, error));
        EXPECT_FALSE(error.empty());

        ASSERT_EQ(container.get_materialized_data().size(), 1U);
        EXPECT_EQ(name_of(*container.get_materialized_data().at(0)), "A");
        EXPECT_EQ(container.position(), 1U);
    }

}
}
