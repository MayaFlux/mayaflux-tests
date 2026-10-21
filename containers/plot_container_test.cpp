#include "../test_config.h"

#include "MayaFlux/Kakshya/NDData/DataAccess.hpp"
#include "MayaFlux/Kakshya/Processors/PlotProcessor.hpp"
#include "MayaFlux/Kakshya/Region/Region.hpp"
#include "MayaFlux/Kakshya/Region/RegionGroup.hpp"
#include "MayaFlux/Kakshya/Source/PlotContainer.hpp"

namespace MayaFlux::Test {

using Kakshya::DataVariant;
using Kakshya::PlotContainer;
using Kakshya::PlotProcessor;
using Kakshya::ProcessingState;
using Kakshya::Region;
using Role = Kakshya::DataDimension::Role;

namespace {

    std::shared_ptr<PlotContainer> make_container(std::initializer_list<uint64_t> sizes = {})
    {
        auto container = std::make_shared<PlotContainer>();
        for (const uint64_t size : sizes)
            container->add_series("s", size, Role::SPATIAL_Y);
        return container;
    }

    std::vector<double> values(const std::shared_ptr<PlotContainer>& container, uint32_t index)
    {
        auto frame = container->get_frame(index).as<double>();
        return { frame.begin(), frame.end() };
    }

    std::vector<double> processed(const std::shared_ptr<PlotContainer>& container, size_t index)
    {
        return std::get<std::vector<double>>(container->get_processed_data()[index]);
    }

    std::vector<double> slice_of(const std::vector<DataVariant>& data)
    {
        return data.empty() ? std::vector<double> {} : std::get<std::vector<double>>(data[0]);
    }

    Region span_of(uint64_t series, uint64_t first, uint64_t last)
    {
        return Region(std::vector<uint64_t> { series, first }, std::vector<uint64_t> { series, last });
    }

    void fill(const std::shared_ptr<PlotContainer>& container, uint32_t index, std::vector<double> data)
    {
        container->write_series(index, data);
    }

}

// =============================================================================
// Series bookkeeping
// =============================================================================

TEST(PlotContainerTest, ANewContainerIsEmptyAndNotReady)
{
    auto container = make_container();

    EXPECT_EQ(container->series_count(), 0U);
    EXPECT_FALSE(container->has_data());
    EXPECT_EQ(container->get_total_elements(), 0U);
    EXPECT_EQ(container->get_num_frames(), 0U);
    EXPECT_EQ(container->get_raw_data(), nullptr);
    EXPECT_TRUE(container->get_dimensions().empty());
    EXPECT_FALSE(container->is_ready_for_processing());
    EXPECT_EQ(container->get_processing_state(), ProcessingState::IDLE);
}

TEST(PlotContainerTest, AddSeriesAssignsSequentialIndicesAndRecordsMetadata)
{
    auto container = make_container();

    const uint32_t a = container->add_series("alpha", 8, Role::SPATIAL_X);
    const uint32_t b = container->add_series("beta", 3, Role::SPATIAL_Y);

    EXPECT_EQ(a, 0U);
    EXPECT_EQ(b, 1U);
    EXPECT_EQ(container->series_count(), 2U);
    EXPECT_EQ(container->series_name(0), "alpha");
    EXPECT_EQ(container->series_name(1), "beta");
    EXPECT_EQ(container->series_size(0), 8U);
    EXPECT_EQ(container->series_size(1), 3U);
    EXPECT_EQ(container->series_role(0), Role::SPATIAL_X);
    EXPECT_EQ(container->series_role(1), Role::SPATIAL_Y);
    EXPECT_TRUE(container->has_data());
    EXPECT_EQ(container->get_total_elements(), 11U);
    EXPECT_EQ(container->get_num_frames(), 2U);
}

TEST(PlotContainerTest, AddedSeriesStartAsZerosInBothTheDataAndTheProcessedCopy)
{
    auto container = make_container({ 4 });

    EXPECT_EQ(values(container, 0), (std::vector<double> { 0.0, 0.0, 0.0, 0.0 }));
    EXPECT_EQ(processed(container, 0), (std::vector<double> { 0.0, 0.0, 0.0, 0.0 }));
}

TEST(PlotContainerTest, DimensionsMirrorTheSeries)
{
    auto container = make_container();
    container->add_series("x", 5, Role::SPATIAL_X);
    container->add_series("y", 7, Role::FREQUENCY);

    const auto dims = container->get_dimensions();

    ASSERT_EQ(dims.size(), 2U);
    EXPECT_EQ(dims[0].name, "x");
    EXPECT_EQ(dims[0].size, 5U);
    EXPECT_EQ(dims[0].role, Role::SPATIAL_X);
    EXPECT_EQ(dims[1].name, "y");
    EXPECT_EQ(dims[1].size, 7U);
    EXPECT_EQ(dims[1].role, Role::FREQUENCY);
}

TEST(PlotContainerTest, AccessorsGiveNeutralValuesForUnknownIndices)
{
    auto container = make_container({ 2 });

    EXPECT_TRUE(container->series_name(9).empty());
    EXPECT_EQ(container->series_size(9), 0U);
    EXPECT_EQ(container->series_role(9), Role::CUSTOM);
}

TEST(PlotContainerTest, ReportsAFixedRowMajorOneDimensionalShape)
{
    auto container = make_container({ 3 });

    EXPECT_EQ(container->get_memory_layout(), Kakshya::MemoryLayout::ROW_MAJOR);
    EXPECT_EQ(container->get_frame_size(), 1U);
    EXPECT_EQ(container->value_element_type(), std::type_index(typeid(double)));
}

TEST(PlotContainerTest, RawDataPointsAtTheFirstSeries)
{
    auto container = make_container({ 3, 2 });
    fill(container, 0, { 4.0, 5.0, 6.0 });

    const auto* raw = static_cast<const double*>(container->get_raw_data());

    ASSERT_NE(raw, nullptr);
    EXPECT_DOUBLE_EQ(raw[0], 4.0);
    EXPECT_DOUBLE_EQ(raw[2], 6.0);
}

TEST(PlotContainerTest, ChannelAccessCoversEverySeries)
{
    auto container = make_container({ 3, 2, 4 });

    EXPECT_EQ(container->all_channel_data().size(), 3U);
}

TEST(PlotContainerTest, CoordinateMappingUsesTheSampleIndexWithinASeries)
{
    auto container = make_container({ 4 });

    EXPECT_EQ(container->coordinates_to_linear_index({ 2, 7 }), 7U);
    EXPECT_EQ(container->coordinates_to_linear_index({ 3 }), 0U);
    EXPECT_EQ(container->linear_index_to_coordinates(5), (std::vector<uint64_t> { 0, 5 }));
}

// =============================================================================
// Writing
// =============================================================================

TEST(PlotContainerTest, WriteSeriesCopiesAtMostTheSeriesSize)
{
    auto container = make_container({ 3 });

    container->write_series(0, std::vector<double> { 1.0, 2.0, 3.0, 4.0, 5.0 });

    EXPECT_EQ(values(container, 0), (std::vector<double> { 1.0, 2.0, 3.0 }));
}

TEST(PlotContainerTest, AShorterWriteLeavesTheTailUntouched)
{
    auto container = make_container({ 4 });
    fill(container, 0, { 9.0, 9.0, 9.0, 9.0 });

    container->write_series(0, std::vector<double> { 1.0, 2.0 });

    EXPECT_EQ(values(container, 0), (std::vector<double> { 1.0, 2.0, 9.0, 9.0 }));
}

TEST(PlotContainerTest, WriteSampleSetsOneValue)
{
    auto container = make_container({ 3 });

    container->write_sample(0, 1, 7.5);

    EXPECT_EQ(values(container, 0), (std::vector<double> { 0.0, 7.5, 0.0 }));
}

TEST(PlotContainerTest, WritesToUnknownSeriesOrSamplesChangeNothing)
{
    auto container = make_container({ 2 });
    fill(container, 0, { 1.0, 2.0 });

    container->write_series(5, std::vector<double> { 9.0, 9.0 });
    container->write_sample(5, 0, 9.0);
    container->write_sample(0, 2, 9.0);

    EXPECT_EQ(values(container, 0), (std::vector<double> { 1.0, 2.0 }));
}

TEST(PlotContainerTest, ResizeSeriesGrowsWithZerosAndShrinksToThePrefix)
{
    auto container = make_container({ 3 });
    fill(container, 0, { 1.0, 2.0, 3.0 });

    container->resize_series(0, 5);
    EXPECT_EQ(values(container, 0), (std::vector<double> { 1.0, 2.0, 3.0, 0.0, 0.0 }));
    EXPECT_EQ(container->series_size(0), 5U);
    EXPECT_EQ(container->get_dimensions()[0].size, 5U);
    EXPECT_EQ(processed(container, 0).size(), 5U);

    container->resize_series(0, 2);
    EXPECT_EQ(values(container, 0), (std::vector<double> { 1.0, 2.0 }));
    EXPECT_EQ(container->get_dimensions()[0].size, 2U);
    EXPECT_EQ(processed(container, 0).size(), 2U);
}

TEST(PlotContainerTest, ResizingAnUnknownSeriesChangesNothing)
{
    auto container = make_container({ 3 });

    container->resize_series(4, 10);

    EXPECT_EQ(container->series_count(), 1U);
    EXPECT_EQ(container->series_size(0), 3U);
}

TEST(PlotContainerTest, ClearDropsEverySeriesAndReturnsToIdle)
{
    auto container = make_container({ 3, 3 });
    container->set_raw(0, { 1.0, 2.0, 3.0 });
    ASSERT_EQ(container->get_processing_state(), ProcessingState::READY);

    container->clear();

    EXPECT_EQ(container->series_count(), 0U);
    EXPECT_FALSE(container->has_data());
    EXPECT_TRUE(container->get_processed_data().empty());
    EXPECT_EQ(container->get_processing_state(), ProcessingState::IDLE);
}

// =============================================================================
// Region access
// =============================================================================

TEST(PlotContainerTest, RegionReadSlicesASeriesInclusively)
{
    auto container = make_container({ 10 });
    fill(container, 0, { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9 });

    EXPECT_EQ(slice_of(container->get_region_data(span_of(0, 2, 5))), (std::vector<double> { 2, 3, 4, 5 }));
    EXPECT_EQ(slice_of(container->get_region_data(span_of(0, 7, 7))), (std::vector<double> { 7 }));
}

TEST(PlotContainerTest, RegionReadClampsTheEndAndDefaultsToTheWholeSeries)
{
    auto container = make_container({ 4 });
    fill(container, 0, { 1, 2, 3, 4 });

    EXPECT_EQ(slice_of(container->get_region_data(span_of(0, 2, 99))), (std::vector<double> { 3, 4 }));
    const Region whole(std::vector<uint64_t> { 0 }, std::vector<uint64_t> { 0 });
    EXPECT_EQ(slice_of(container->get_region_data(whole)), (std::vector<double> { 1, 2, 3, 4 }));
}

TEST(PlotContainerTest, RegionReadOfNothingUsableIsEmpty)
{
    auto container = make_container({ 4, 0 });
    fill(container, 0, { 1, 2, 3, 4 });

    EXPECT_TRUE(container->get_region_data(Region {}).empty());
    EXPECT_TRUE(container->get_region_data(span_of(7, 0, 1)).empty());
    EXPECT_TRUE(container->get_region_data(span_of(0, 3, 1)).empty());
    EXPECT_TRUE(container->get_region_data(span_of(1, 0, 1)).empty());
}

TEST(PlotContainerTest, RegionWriteReplacesASliceAndTruncatesTheSource)
{
    auto container = make_container({ 6 });

    container->set_region_data(span_of(0, 1, 3), { DataVariant(std::vector<double> { 9, 8, 7, 6, 5 }) });

    EXPECT_EQ(values(container, 0), (std::vector<double> { 0, 9, 8, 7, 0, 0 }));
}

TEST(PlotContainerTest, RegionWriteClampsTheEndToTheSeries)
{
    auto container = make_container({ 4 });

    container->set_region_data(span_of(0, 2, 99), { DataVariant(std::vector<double> { 1, 2, 3, 4 }) });

    EXPECT_EQ(values(container, 0), (std::vector<double> { 0, 0, 1, 2 }));
}

TEST(PlotContainerTest, RegionWriteIgnoresUnusableInput)
{
    auto container = make_container({ 3 });
    fill(container, 0, { 1, 2, 3 });

    container->set_region_data(span_of(0, 0, 2), {});
    container->set_region_data(span_of(0, 0, 2), { DataVariant(std::vector<double> {}) });
    container->set_region_data(span_of(0, 0, 2), { DataVariant(std::vector<float> { 9.F, 9.F }) });
    container->set_region_data(span_of(5, 0, 2), { DataVariant(std::vector<double> { 9, 9 }) });
    container->set_region_data(Region {}, { DataVariant(std::vector<double> { 9, 9 }) });
    container->set_region_data(span_of(0, 3, 1), { DataVariant(std::vector<double> { 9, 9 }) });

    EXPECT_EQ(values(container, 0), (std::vector<double> { 1, 2, 3 }));
}

TEST(PlotContainerTest, RegionWriteToAnEmptySeriesDoesNothing)
{
    auto container = make_container({ 0 });

    container->set_region_data(span_of(0, 0, 3), { DataVariant(std::vector<double> { 1, 2, 3, 4 }) });

    EXPECT_EQ(container->series_size(0), 0U);
}

TEST(PlotContainerTest, RegionGroupDataConcatenatesEachRegionsSlice)
{
    auto container = make_container({ 4, 3 });
    fill(container, 0, { 1, 2, 3, 4 });
    fill(container, 1, { 5, 6, 7 });
    Kakshya::RegionGroup group;
    group.name = "pair";
    group.regions = { span_of(0, 0, 1), span_of(1, 1, 2) };

    const auto data = container->get_region_group_data(group);

    ASSERT_EQ(data.size(), 2U);
    EXPECT_EQ(std::get<std::vector<double>>(data[0]), (std::vector<double> { 1, 2 }));
    EXPECT_EQ(std::get<std::vector<double>>(data[1]), (std::vector<double> { 6, 7 }));
}

TEST(PlotContainerTest, RegionGroupsAreStoredRetrievedAndRemovedByName)
{
    auto container = make_container({ 4 });
    Kakshya::RegionGroup group;
    group.name = "peaks";
    group.regions = { span_of(0, 0, 1), span_of(0, 2, 3) };

    container->add_region_group(group);

    EXPECT_EQ(container->get_region_group("peaks").regions.size(), 2U);
    EXPECT_EQ(container->get_all_region_groups().size(), 1U);
    EXPECT_TRUE(container->get_region_group("missing").name.empty());

    container->remove_region_group("peaks");
    EXPECT_TRUE(container->get_all_region_groups().empty());
}

// =============================================================================
// Processing state
// =============================================================================

TEST(PlotContainerTest, BindingASourceMakesTheContainerReady)
{
    auto container = make_container({ 2 });
    EXPECT_FALSE(container->is_ready_for_processing());

    container->bind(0, [](std::vector<double>&) { });

    EXPECT_EQ(container->get_processing_state(), ProcessingState::READY);
    EXPECT_TRUE(container->is_ready_for_processing());
}

TEST(PlotContainerTest, ProcessDefaultMovesReadyToProcessedAndStaysProcessable)
{
    auto container = make_container({ 2 });
    int runs = 0;
    container->bind(0, [&runs](std::vector<double>&) { ++runs; });

    container->process_default();
    EXPECT_EQ(container->get_processing_state(), ProcessingState::PROCESSED);
    EXPECT_TRUE(container->is_ready_for_processing());

    container->process_default();
    EXPECT_EQ(runs, 2);
}

TEST(PlotContainerTest, ProcessDefaultDoesNothingWithoutAProcessorOrReadiness)
{
    auto no_processor = make_container({ 2 });
    no_processor->process_default();
    EXPECT_EQ(no_processor->get_processing_state(), ProcessingState::IDLE);

    auto not_ready = make_container({ 2 });
    int runs = 0;
    not_ready->bind(0, [&runs](std::vector<double>&) { ++runs; });
    not_ready->mark_ready_for_processing(false);
    not_ready->process_default();
    EXPECT_EQ(runs, 0);
    EXPECT_EQ(not_ready->get_processing_state(), ProcessingState::IDLE);
}

TEST(PlotContainerTest, ProcessingFillsTheDataAndTheProcessedCopy)
{
    auto container = make_container({ 3 });
    container->bind(0, [](std::vector<double>& out) { out = { 1.0, 2.0, 3.0 }; });

    container->process_default();

    EXPECT_EQ(values(container, 0), (std::vector<double> { 1.0, 2.0, 3.0 }));
    EXPECT_EQ(processed(container, 0), (std::vector<double> { 1.0, 2.0, 3.0 }));
}

TEST(PlotContainerTest, StateCallbackSeesEachTransitionOnce)
{
    auto container = make_container({ 2 });
    std::vector<ProcessingState> seen;
    container->register_state_change_callback(
        [&seen](const std::shared_ptr<Kakshya::SignalSourceContainer>&, ProcessingState state) { seen.push_back(state); });

    container->bind(0, [](std::vector<double>&) { });
    container->process_default();
    container->update_processing_state(ProcessingState::PROCESSED);

    EXPECT_EQ(seen, (std::vector<ProcessingState> { ProcessingState::READY, ProcessingState::PROCESSING, ProcessingState::PROCESSED }));
}

TEST(PlotContainerTest, AnUnregisteredStateCallbackIsNotCalled)
{
    auto container = make_container({ 2 });
    int calls = 0;
    container->register_state_change_callback(
        [&calls](const std::shared_ptr<Kakshya::SignalSourceContainer>&, ProcessingState) { ++calls; });
    container->unregister_state_change_callback();

    container->bind(0, [](std::vector<double>&) { });
    container->process_default();

    EXPECT_EQ(calls, 0);
}

// =============================================================================
// Binding
// =============================================================================

TEST(PlotContainerTest, BindingCreatesTheDefaultProcessorOnce)
{
    auto container = make_container({ 2, 2 });
    EXPECT_EQ(container->get_default_processor(), nullptr);

    container->bind(0, [](std::vector<double>&) { });
    const auto first = container->get_default_processor();
    container->bind(1, [](std::vector<double>&) { });

    ASSERT_NE(first, nullptr);
    EXPECT_EQ(container->get_default_processor(), first);
}

TEST(PlotContainerTest, BindingAnOutOfRangeSeriesIsRejectedBeforeAnyProcessorIsMade)
{
    auto container = make_container({ 2 });

    container->bind(5, [](std::vector<double>&) { });
    container->set_raw(9, { 1.0 });

    EXPECT_EQ(container->get_default_processor(), nullptr);
    EXPECT_EQ(container->get_processing_state(), ProcessingState::IDLE);
}

TEST(PlotContainerTest, SetRawLoadsDataOnTheNextProcess)
{
    auto container = make_container({ 3 });

    container->set_raw(0, { 4.0, 5.0, 6.0 });
    container->process_default();

    EXPECT_EQ(values(container, 0), (std::vector<double> { 4.0, 5.0, 6.0 }));
}

TEST(PlotContainerTest, UnbindStopsAProcessorFromTouchingTheSeries)
{
    auto container = make_container({ 2 });
    int runs = 0;
    container->bind(0, [&runs](std::vector<double>&) { ++runs; });
    container->process_default();

    container->unbind(0);
    container->process_default();

    EXPECT_EQ(runs, 1);
}

TEST(PlotContainerTest, UnbindBeforeAnyBindingIsHarmless)
{
    auto container = make_container({ 2 });

    container->unbind(0);

    EXPECT_EQ(container->get_default_processor(), nullptr);
}

TEST(PlotContainerTest, ReplacingTheDefaultProcessorDetachesTheOldOne)
{
    auto container = make_container({ 2 });
    container->bind(0, [](std::vector<double>&) { });
    const auto old_processor = std::static_pointer_cast<PlotProcessor>(container->get_default_processor());
    ASSERT_TRUE(old_processor->has_binding(0));

    container->set_default_processor(std::make_shared<PlotProcessor>());

    EXPECT_FALSE(old_processor->has_binding(0));
    EXPECT_NE(container->get_default_processor(), old_processor);
}

}
