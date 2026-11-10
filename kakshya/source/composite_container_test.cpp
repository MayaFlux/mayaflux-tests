#include "gtest/gtest.h"

#include "MayaFlux/Kakshya/NDData/CompositeAccess.hpp"
#include "MayaFlux/Kakshya/NDData/CompositeInsertion.hpp"
#include "MayaFlux/Kakshya/Region/RegionSegment.hpp"
#include "MayaFlux/Kakshya/Source/CompositeContainer.hpp"

namespace MayaFlux::Test {
namespace {

    using Kakshya::CompositeArray;
    using Kakshya::CompositeContainer;
    using Kakshya::CompositeInsertion;
    using Kakshya::CompositeLayout;
    using Kakshya::ContainerDataStructure;
    using Kakshya::DataModality;
    using Kakshya::MemoryLayout;
    using Kakshya::Region;
    using Kakshya::RegionGroup;
    using Kakshya::RegionSegment;

    CompositeLayout scored_layout()
    {
        CompositeLayout layout;
        layout.add_field<std::string>("name");
        layout.add_field<double>("score");
        return layout;
    }

    CompositeLayout pair_layout()
    {
        CompositeLayout layout;
        layout.add_field<std::string>("name");
        layout.add_field<uint32_t>("rank");
        return layout;
    }

    Region span(uint64_t start, uint64_t end)
    {
        return Region(std::vector<uint64_t> { start }, std::vector<uint64_t> { end });
    }

    CompositeArray named_rows(const CompositeLayout& layout, const std::vector<std::string>& names)
    {
        CompositeArray array(layout);
        CompositeInsertion insertion(array);

        for (const auto& name : names) {
            const auto row = insertion.append();
            insertion.set_text(row, "name", name);
        }

        return array;
    }

    void append_scored_rows(CompositeContainer& container, size_t count)
    {
        auto insertion = container.insert();

        for (size_t i = 0; i < count; ++i) {
            const auto row = insertion.append();
            insertion.set_text(row, "name", "r" + std::to_string(i));
            if (i != 2)
                insertion.set(row, "score", static_cast<double>(i) * 1.5);
        }
    }

    CompositeContainer populated_container(size_t batch_size = 2)
    {
        CompositeContainer container(scored_layout(), batch_size);
        append_scored_rows(container, 6);
        return container;
    }

    std::string name_at(const CompositeArray& array, size_t index)
    {
        return std::string(array.at(index)->text("name").value_or(""));
    }

    class CompositeContainerTest : public ::testing::Test {
    };

    TEST_F(CompositeContainerTest, BatchSizeZeroIsNormalizedToOne)
    {
        CompositeContainer container(scored_layout(), 0);
        EXPECT_EQ(container.batch_size(), 1U);

        container.set_batch_size(0);
        EXPECT_EQ(container.batch_size(), 1U);

        container.set_batch_size(4);
        EXPECT_EQ(container.batch_size(), 4U);
    }

    TEST_F(CompositeContainerTest, NewContainerIsEmptyAndAtTheEnd)
    {
        CompositeContainer container(scored_layout(), 2);

        EXPECT_EQ(container.get_total_elements(), 0U);
        EXPECT_FALSE(container.has_data());
        EXPECT_TRUE(container.at_end());
        EXPECT_EQ(container.position(), 0U);
        EXPECT_EQ(container.get_raw_data(), nullptr);
        EXPECT_FALSE(container.materialize_next());
    }

    TEST_F(CompositeContainerTest, TakesOwnershipOfAnExistingArray)
    {
        CompositeContainer container(named_rows(scored_layout(), { "a", "b", "c" }), 2);

        EXPECT_EQ(container.get_total_elements(), 3U);
        EXPECT_EQ(container.get_data().size(), 3U);
        EXPECT_TRUE(container.has_data());
        EXPECT_NE(container.get_raw_data(), nullptr);
    }

    TEST_F(CompositeContainerTest, DimensionsDescribeTheElementAxis)
    {
        const auto container = populated_container();

        const auto dimensions = container.get_dimensions();
        ASSERT_EQ(dimensions.size(), 1U);
        EXPECT_EQ(dimensions[0].name, "elements");
        EXPECT_EQ(dimensions[0].size, 6U);
        EXPECT_EQ(dimensions[0].role, Kakshya::DataDimension::Role::CUSTOM);

        EXPECT_EQ(container.get_total_elements(), 6U);
        EXPECT_EQ(container.get_memory_layout(), MemoryLayout::ROW_MAJOR);
        EXPECT_EQ(container.get_frame_size(), 0U);
        EXPECT_EQ(container.get_num_frames(), 0U);
    }

    TEST_F(CompositeContainerTest, StructureCanBeReplaced)
    {
        CompositeContainer container(scored_layout(), 2);

        container.set_structure(ContainerDataStructure(DataModality::AUDIO_1D));

        EXPECT_EQ(container.get_structure().modality, DataModality::AUDIO_1D);
        EXPECT_EQ(container.get_structure().memory_layout, MemoryLayout::ROW_MAJOR);
    }

    TEST_F(CompositeContainerTest, MaterializeCopiesTheNextBatchWithItsText)
    {
        auto container = populated_container(2);

        ASSERT_TRUE(container.materialize_next());

        const auto& window = container.get_materialized_data();
        ASSERT_EQ(window.size(), 2U);
        EXPECT_EQ(name_at(window, 0), "r0");
        EXPECT_EQ(name_at(window, 1), "r1");
        EXPECT_EQ(container.position(), 2U);
    }

    TEST_F(CompositeContainerTest, MaterializeAdvancesThroughEveryBatch)
    {
        auto container = populated_container(2);

        ASSERT_TRUE(container.materialize_next());
        ASSERT_TRUE(container.materialize_next());

        EXPECT_EQ(name_at(container.get_materialized_data(), 0), "r2");
        EXPECT_EQ(container.position(), 4U);
        EXPECT_FALSE(container.at_end());
    }

    TEST_F(CompositeContainerTest, FinalWindowIsShortAndThenMaterializationStops)
    {
        auto container = populated_container(4);
        container.seek(4);

        ASSERT_TRUE(container.materialize_next());
        EXPECT_EQ(container.get_materialized_data().size(), 2U);
        EXPECT_EQ(name_at(container.get_materialized_data(), 0), "r4");
        EXPECT_EQ(container.position(), 6U);
        EXPECT_TRUE(container.at_end());

        EXPECT_FALSE(container.materialize_next());
        EXPECT_EQ(container.get_materialized_data().size(), 2U);
        EXPECT_EQ(name_at(container.get_materialized_data(), 0), "r4");
    }

    TEST_F(CompositeContainerTest, SeekClampsToTheEnd)
    {
        auto container = populated_container(2);

        container.seek(99);

        EXPECT_EQ(container.position(), 6U);
        EXPECT_TRUE(container.at_end());
        EXPECT_FALSE(container.materialize_next());
    }

    TEST_F(CompositeContainerTest, SeekSetsTheNextPosition)
    {
        auto container = populated_container(2);

        container.seek(3);
        ASSERT_TRUE(container.materialize_next());

        EXPECT_EQ(name_at(container.get_materialized_data(), 0), "r3");
    }

    TEST_F(CompositeContainerTest, SetBatchSizeChangesTheNextWindow)
    {
        auto container = populated_container(2);

        container.set_batch_size(3);
        ASSERT_TRUE(container.materialize_next());

        EXPECT_EQ(container.get_materialized_data().size(), 3U);
    }

    TEST_F(CompositeContainerTest, MaterializeFailsOnAnEmptyContainer)
    {
        CompositeContainer container(scored_layout(), 2);

        EXPECT_FALSE(container.materialize_next());
        EXPECT_EQ(container.get_materialized_data().size(), 0U);
    }

    TEST_F(CompositeContainerTest, MaterializedWindowIsIndependentOfLaterEdits)
    {
        auto container = populated_container(2);
        ASSERT_TRUE(container.materialize_next());

        container.set_region_data(span(0, 0), { named_rows(container.get_data().layout(), { "X" }) });

        EXPECT_EQ(name_at(container.get_data(), 0), "X");
        EXPECT_EQ(name_at(container.get_materialized_data(), 0), "r0");
    }

    TEST_F(CompositeContainerTest, SlicesBorrowConsecutiveRowsAndRejectBadRanges)
    {
        const auto container = populated_container();

        const auto middle = container.slice(1, 3);
        ASSERT_TRUE(middle);
        EXPECT_EQ(middle->size(), 3U);
        EXPECT_EQ(name_at(container.get_data(), middle->start()), "r1");

        const auto empty = container.slice(0, 0);
        ASSERT_TRUE(empty);
        EXPECT_EQ(empty->size(), 0U);

        EXPECT_FALSE(container.slice(5, 2));

        const auto region = container.slice(span(1, 2));
        ASSERT_TRUE(region);
        EXPECT_EQ(region->size(), 2U);

        EXPECT_FALSE(container.slice(span(4, 9)));
    }

    TEST_F(CompositeContainerTest, RegionDataIsAnOwningCopyOfTheRange)
    {
        const auto container = populated_container();

        const auto copies = container.get_region_data(span(1, 2));
        ASSERT_EQ(copies.size(), 1U);
        ASSERT_EQ(copies.front().size(), 2U);
        EXPECT_EQ(name_at(copies.front(), 0), "r1");
        EXPECT_EQ(name_at(copies.front(), 1), "r2");
    }

    TEST_F(CompositeContainerTest, InvalidRegionsCopyNothing)
    {
        const auto container = populated_container();

        EXPECT_TRUE(container.get_region_data(span(4, 9)).empty());

        const Region two_dimensional(std::vector<uint64_t> { 0, 0 }, std::vector<uint64_t> { 1, 1 });
        EXPECT_TRUE(container.get_region_data(two_dimensional).empty());
    }

    TEST_F(CompositeContainerTest, RegionCopiesDoNotSeeLaterEdits)
    {
        auto container = populated_container();

        const auto copies = container.get_region_data(span(0, 0));
        container.set_region_data(span(0, 0), { named_rows(container.get_data().layout(), { "X" }) });

        ASSERT_EQ(copies.size(), 1U);
        EXPECT_EQ(name_at(copies.front(), 0), "r0");
        EXPECT_EQ(name_at(container.get_data(), 0), "X");
    }

    TEST_F(CompositeContainerTest, RegionGroupsCopyEachValidRegionAndSkipInvalidOnes)
    {
        auto container = populated_container();
        container.add_region_group(RegionGroup("mixed", { span(0, 1), span(4, 9), span(3, 3) }));

        const auto group = container.get_region_group("mixed");
        EXPECT_EQ(group.regions.size(), 3U);

        const auto copies = container.get_region_group_data(group);
        ASSERT_EQ(copies.size(), 2U);
        EXPECT_EQ(copies[0].size(), 2U);
        EXPECT_EQ(name_at(copies[1], 0), "r3");
    }

    TEST_F(CompositeContainerTest, SegmentsCopyTheirSpanOfTheSourceRegion)
    {
        const auto container = populated_container();

        const RegionSegment segment(span(2, 4), std::vector<uint64_t> { 1 }, std::vector<uint64_t> { 2 });
        const auto copies = container.get_segments_data({ segment });

        ASSERT_EQ(copies.size(), 1U);
        ASSERT_EQ(copies.front().size(), 2U);
        EXPECT_EQ(name_at(copies.front(), 0), "r3");
        EXPECT_EQ(name_at(copies.front(), 1), "r4");
    }

    TEST_F(CompositeContainerTest, MalformedSegmentsAreSkipped)
    {
        const auto container = populated_container();

        const std::vector<RegionSegment> segments {
            RegionSegment(span(2, 4), std::vector<uint64_t> { 2 }, std::vector<uint64_t> { 2 }),
            RegionSegment(span(2, 4), std::vector<uint64_t> { 0 }, std::vector<uint64_t> { 0 }),
            RegionSegment(span(2, 4), std::vector<uint64_t> { 3 }, std::vector<uint64_t> { 1 }),
            RegionSegment(Region(std::vector<uint64_t> { 0, 0 }, std::vector<uint64_t> { 1, 1 }),
                std::vector<uint64_t> { 0, 0 }, std::vector<uint64_t> { 1, 1 }),
            RegionSegment(span(0, 1), std::vector<uint64_t> { 0 }, std::vector<uint64_t> { 1 })
        };

        const auto copies = container.get_segments_data(segments);

        ASSERT_EQ(copies.size(), 1U);
        EXPECT_EQ(name_at(copies.front(), 0), "r0");
    }

    TEST_F(CompositeContainerTest, SetRegionDataReplacesTheRangeAndKeepsTheRest)
    {
        auto container = populated_container();

        container.set_region_data(span(1, 2), { named_rows(container.get_data().layout(), { "a", "b" }) });

        const auto& data = container.get_data();
        ASSERT_EQ(data.size(), 6U);
        EXPECT_EQ(name_at(data, 0), "r0");
        EXPECT_EQ(name_at(data, 1), "a");
        EXPECT_EQ(name_at(data, 2), "b");
        EXPECT_EQ(name_at(data, 3), "r3");
        EXPECT_EQ(name_at(data, 5), "r5");
    }

    TEST_F(CompositeContainerTest, SetRegionDataAcceptsSeveralArraysThatFillTheRange)
    {
        auto container = populated_container();
        const auto layout = container.get_data().layout();

        std::vector<CompositeArray> replacement;
        replacement.push_back(named_rows(layout, { "a" }));
        replacement.push_back(named_rows(layout, { "b" }));
        container.set_region_data(span(1, 2), replacement);

        EXPECT_EQ(name_at(container.get_data(), 1), "a");
        EXPECT_EQ(name_at(container.get_data(), 2), "b");
        EXPECT_EQ(container.get_data().size(), 6U);
    }

    TEST_F(CompositeContainerTest, SetRegionDataReplacesTheOuterEdges)
    {
        auto container = populated_container();
        const auto layout = container.get_data().layout();

        container.set_region_data(span(0, 0), { named_rows(layout, { "first" }) });
        container.set_region_data(span(5, 5), { named_rows(layout, { "last" }) });

        const auto& data = container.get_data();
        EXPECT_EQ(name_at(data, 0), "first");
        EXPECT_EQ(name_at(data, 1), "r1");
        EXPECT_EQ(name_at(data, 4), "r4");
        EXPECT_EQ(name_at(data, 5), "last");
    }

    TEST_F(CompositeContainerTest, SetRegionDataRefusesACountMismatch)
    {
        auto container = populated_container();

        container.set_region_data(span(1, 2), { named_rows(container.get_data().layout(), { "a" }) });

        EXPECT_EQ(container.get_data().size(), 6U);
        EXPECT_EQ(name_at(container.get_data(), 1), "r1");
    }

    TEST_F(CompositeContainerTest, SetRegionDataRefusesAnInvalidRegion)
    {
        auto container = populated_container();

        container.set_region_data(span(4, 9), { named_rows(container.get_data().layout(), { "a", "b", "c" }) });

        EXPECT_EQ(container.get_data().size(), 6U);
        EXPECT_EQ(name_at(container.get_data(), 4), "r4");
    }

    TEST_F(CompositeContainerTest, SetRegionDataRefusesAnotherLayout)
    {
        auto container = populated_container();

        CompositeArray other(pair_layout());
        CompositeInsertion insertion(other);
        const auto row = insertion.append();
        insertion.set_text(row, "name", "x");

        container.set_region_data(span(1, 1), { other });

        EXPECT_EQ(container.get_data().size(), 6U);
        EXPECT_EQ(name_at(container.get_data(), 1), "r1");
    }

    TEST_F(CompositeContainerTest, RegionGroupsAreStoredByNameReplacedAndRemoved)
    {
        CompositeContainer container(scored_layout(), 2);

        container.add_region_group(RegionGroup("a", { span(0, 0) }));
        EXPECT_EQ(container.get_region_group("a").regions.size(), 1U);

        container.add_region_group(RegionGroup("a", { span(0, 0), span(1, 1) }));
        EXPECT_EQ(container.get_region_group("a").regions.size(), 2U);

        container.add_region_group(RegionGroup("b", { span(2, 2) }));
        EXPECT_EQ(container.get_all_region_groups().size(), 2U);

        container.remove_region_group("a");
        EXPECT_TRUE(container.get_region_group("a").regions.empty());
        EXPECT_EQ(container.get_all_region_groups().size(), 1U);

        container.remove_region_group("missing");
        EXPECT_EQ(container.get_all_region_groups().size(), 1U);
    }

    TEST_F(CompositeContainerTest, RegionsStayLoadedUntilCleared)
    {
        auto container = populated_container();

        EXPECT_TRUE(container.is_region_loaded(span(0, 5)));
        EXPECT_FALSE(container.is_region_loaded(span(0, 6)));

        container.load_region(span(0, 5));
        container.unload_region(span(0, 5));
        EXPECT_TRUE(container.is_region_loaded(span(0, 5)));
    }

    TEST_F(CompositeContainerTest, LinearIndicesAreTheElementIndex)
    {
        const auto container = populated_container();

        EXPECT_EQ(container.coordinates_to_linear_index({ 4 }), 4U);
        EXPECT_EQ(container.linear_index_to_coordinates(3), (std::vector<uint64_t> { 3 }));
        EXPECT_EQ(container.coordinates_to_linear_index({ 1, 2 }), 0U);
    }

    TEST_F(CompositeContainerTest, ClearRemovesElementsAndGroupsButKeepsTheLayout)
    {
        auto container = populated_container();
        container.add_region_group(RegionGroup("kept", { span(0, 0) }));
        ASSERT_TRUE(container.materialize_next());

        container.clear();

        EXPECT_EQ(container.get_total_elements(), 0U);
        EXPECT_FALSE(container.has_data());
        EXPECT_EQ(container.position(), 0U);
        EXPECT_EQ(container.get_materialized_data().size(), 0U);
        EXPECT_TRUE(container.get_region_group("kept").regions.empty());

        auto insertion = container.insert();
        const auto row = insertion.append();
        insertion.set_text(row, "name", "again");

        EXPECT_EQ(container.get_total_elements(), 1U);
        EXPECT_EQ(container.get_data().layout().fields().size(), 2U);
        EXPECT_EQ(name_at(container.get_data(), 0), "again");
    }

}
}
