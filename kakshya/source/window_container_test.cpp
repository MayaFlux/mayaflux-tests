#include "gtest/gtest.h"

#include "../window_test_support.hpp"

#include "MayaFlux/Kakshya/DataProcessingChain.hpp"
#include "MayaFlux/Kakshya/Processors/WindowAccessProcessor.hpp"
#include "MayaFlux/Kakshya/Source/WindowContainer.hpp"

namespace MayaFlux::Test {
namespace {

    using Kakshya::DataDimension;
    using Kakshya::ImageFormat;
    using Kakshya::ProcessingState;
    using Kakshya::Region;
    using Kakshya::RegionGroup;
    using Kakshya::WindowAccessProcessor;
    using Kakshya::WindowContainer;
    using namespace WindowSupport;

    constexpr size_t k_surface_bytes = size_t { k_surface_width } * k_surface_height * k_surface_channels;

    TEST(WindowContainerTest, RequiresAWindow)
    {
        EXPECT_THROW(WindowContainer(nullptr), std::invalid_argument);
    }

    TEST(WindowContainerTest, DefaultsToSixtyHistorySlots)
    {
        const auto window = std::make_shared<ScriptedWindow>(k_surface_width, k_surface_height);

        const WindowContainer container(window);

        EXPECT_EQ(container.get_frame_capacity(), 60U);
    }

    class WindowContainerRigTest : public ::testing::Test {
    protected:
        std::vector<uint8_t> capture(uint64_t seed)
        {
            const auto frame = rig.display.present(seed);
            rig.container->process_default();
            return frame;
        }

        WindowRig rig;
    };

    TEST_F(WindowContainerRigTest, ReportsItsWindowAndHistoryCapacity)
    {
        EXPECT_EQ(rig.container->get_window(), rig.window);
        EXPECT_EQ(rig.container->get_frame_capacity(), k_history);
        EXPECT_EQ(rig.container->get_write_head(), 0U);
    }

    TEST_F(WindowContainerRigTest, TakesTheSurfaceSizeFromTheWindow)
    {
        const auto& structure = std::as_const(*rig.container).get_structure();

        EXPECT_EQ(structure.get_width(), k_surface_width);
        EXPECT_EQ(structure.get_height(), k_surface_height);
        EXPECT_EQ(structure.get_channel_count(), k_surface_channels);
    }

    TEST_F(WindowContainerRigTest, EveryHistorySlotHoldsOneBlackSurface)
    {
        for (uint32_t i = 0; i < k_history; ++i) {
            ASSERT_NE(rig.container->mutable_frame_ptr(i), nullptr) << i;
            EXPECT_EQ(history_slot(*rig.container, i), std::vector<uint8_t>(k_surface_bytes, 0)) << i;
        }
    }

    TEST_F(WindowContainerRigTest, SlotsBeyondTheHistoryHaveNoPointer)
    {
        EXPECT_EQ(rig.container->mutable_frame_ptr(k_history), nullptr);
        EXPECT_EQ(rig.container->mutable_frame_ptr(1000), nullptr);
    }

    TEST_F(WindowContainerRigTest, WritingASlotIsVisibleInTheHistory)
    {
        const auto bytes = surface_bytes(k_surface_width, k_surface_height, SurfaceFormat::R8G8B8A8_UNORM, 3);

        std::memcpy(rig.container->mutable_frame_ptr(2), bytes.data(), bytes.size());

        EXPECT_EQ(history_slot(*rig.container, 2), bytes);
        EXPECT_EQ(history_slot(*rig.container, 1), std::vector<uint8_t>(k_surface_bytes, 0));
    }

    TEST_F(WindowContainerRigTest, TheWriteHeadAdvancesAndWraps)
    {
        for (uint32_t i = 1; i < k_history; ++i) {
            rig.container->advance_write_head();
            EXPECT_EQ(rig.container->get_write_head(), i);
        }

        rig.container->advance_write_head();

        EXPECT_EQ(rig.container->get_write_head(), 0U);
    }

    TEST_F(WindowContainerRigTest, StartsIdleAndNotReady)
    {
        EXPECT_EQ(rig.container->get_processing_state(), ProcessingState::IDLE);
        EXPECT_FALSE(rig.container->is_ready_for_processing());
    }

    TEST_F(WindowContainerRigTest, TheReadyFlagIsSetAndCleared)
    {
        rig.container->mark_ready_for_processing(true);
        EXPECT_TRUE(rig.container->is_ready_for_processing());

        rig.container->mark_ready_for_processing(false);
        EXPECT_FALSE(rig.container->is_ready_for_processing());
    }

    TEST_F(WindowContainerRigTest, StateChangesReachTheCallbackOnlyWhenTheStateChanges)
    {
        std::vector<ProcessingState> seen;
        rig.container->register_state_change_callback(
            [&seen](const std::shared_ptr<Kakshya::SignalSourceContainer>&, ProcessingState state) { seen.push_back(state); });

        rig.container->update_processing_state(ProcessingState::READY);
        rig.container->update_processing_state(ProcessingState::READY);
        rig.container->update_processing_state(ProcessingState::PROCESSED);
        rig.container->unregister_state_change_callback();
        rig.container->update_processing_state(ProcessingState::IDLE);

        EXPECT_EQ(seen, (std::vector<ProcessingState> { ProcessingState::READY, ProcessingState::PROCESSED }));
    }

    TEST_F(WindowContainerRigTest, RegionGroupsAreStoredByNameAndRemovable)
    {
        RegionGroup group("hits");
        group.add_region(Region(std::vector<uint64_t> { 0, 0 }, std::vector<uint64_t> { 1, 1 }));

        rig.container->add_region_group(group);

        EXPECT_EQ(rig.container->get_region_group("hits").regions.size(), 1U);
        EXPECT_EQ(rig.container->get_all_region_groups().size(), 1U);

        rig.container->remove_region_group("hits");

        EXPECT_TRUE(rig.container->get_all_region_groups().empty());
        EXPECT_TRUE(rig.container->get_region_group("hits").regions.empty());
    }

    TEST_F(WindowContainerRigTest, RegionsAreAlwaysLoaded)
    {
        const Region region(std::vector<uint64_t> { 0, 0 }, std::vector<uint64_t> { 1, 1 });

        EXPECT_TRUE(rig.container->is_region_loaded(region));
        EXPECT_NO_THROW(rig.container->load_region(region));
        EXPECT_NO_THROW(rig.container->unload_region(region));
        EXPECT_TRUE(rig.container->is_region_loaded(region));
    }

    TEST_F(WindowContainerRigTest, ReadersAreCountedAndReleased)
    {
        EXPECT_FALSE(rig.container->has_active_readers());

        const uint32_t first = rig.container->register_dimension_reader(0);
        const uint32_t second = rig.container->register_dimension_reader(0);

        EXPECT_NE(first, second);
        EXPECT_TRUE(rig.container->has_active_readers());

        rig.container->unregister_dimension_reader(0);
        EXPECT_TRUE(rig.container->has_active_readers());
        rig.container->unregister_dimension_reader(0);
        EXPECT_FALSE(rig.container->has_active_readers());
    }

    TEST_F(WindowContainerRigTest, UnregisteringWithNoReadersDoesNotUnderflow)
    {
        rig.container->unregister_dimension_reader(0);

        EXPECT_FALSE(rig.container->has_active_readers());
        EXPECT_TRUE(rig.container->all_dimensions_consumed());
    }

    TEST_F(WindowContainerRigTest, ConsumptionCompletesOnceEveryReaderHasMarked)
    {
        const uint32_t first = rig.container->register_dimension_reader(0);
        const uint32_t second = rig.container->register_dimension_reader(0);

        EXPECT_FALSE(rig.container->all_dimensions_consumed());
        rig.container->mark_dimension_consumed(0, first);
        EXPECT_FALSE(rig.container->all_dimensions_consumed());
        rig.container->mark_dimension_consumed(0, second);
        EXPECT_TRUE(rig.container->all_dimensions_consumed());
    }

    TEST_F(WindowContainerRigTest, TheProcessingChainIsCreatedOnceAndReplaceable)
    {
        const auto chain = rig.container->get_processing_chain();

        ASSERT_NE(chain, nullptr);
        EXPECT_EQ(rig.container->get_processing_chain(), chain);

        const auto other = std::make_shared<Kakshya::DataProcessingChain>();
        rig.container->set_processing_chain(other);
        EXPECT_EQ(rig.container->get_processing_chain(), other);
    }

    TEST_F(WindowContainerRigTest, TheDefaultProcessorIsAWindowAccessProcessorThatEnablesCapture)
    {
        ASSERT_FALSE(rig.window->is_capture_enabled());
        EXPECT_EQ(rig.container->get_default_processor(), nullptr);

        rig.container->create_default_processor();

        EXPECT_NE(std::dynamic_pointer_cast<WindowAccessProcessor>(rig.container->get_default_processor()), nullptr);
        EXPECT_TRUE(rig.window->is_capture_enabled());
        EXPECT_TRUE(rig.container->is_ready_for_processing());
    }

    TEST_F(WindowContainerRigTest, ReplacingTheDefaultProcessorDetachesTheOldOne)
    {
        rig.container->create_default_processor();
        ASSERT_TRUE(rig.window->is_capture_enabled());

        rig.container->set_default_processor(nullptr);

        EXPECT_EQ(rig.container->get_default_processor(), nullptr);
        EXPECT_FALSE(rig.window->is_capture_enabled());
    }

    TEST_F(WindowContainerRigTest, SettingAProcessorAttachesIt)
    {
        const auto processor = std::make_shared<WindowAccessProcessor>();

        rig.container->set_default_processor(processor);

        EXPECT_EQ(rig.container->get_default_processor(), processor);
        EXPECT_EQ(processor->get_surface_format(), SurfaceFormat::R8G8B8A8_UNORM);
        EXPECT_TRUE(rig.window->is_capture_enabled());
    }

    TEST_F(WindowContainerRigTest, ProcessingWithoutADefaultDoesNothing)
    {
        EXPECT_NO_THROW(rig.container->process_default());

        EXPECT_EQ(rig.container->get_processing_state(), ProcessingState::IDLE);
        EXPECT_EQ(rig.container->get_write_head(), 0U);
    }

    TEST_F(WindowContainerRigTest, ProcessingTheDefaultCapturesTheSurface)
    {
        rig.container->create_default_processor();
        const auto frame = rig.display.present(5);

        rig.container->process_default();

        EXPECT_EQ(processed_pixels(*rig.container), frame);
        EXPECT_EQ(rig.container->get_processing_state(), ProcessingState::PROCESSED);
    }

    TEST_F(WindowContainerRigTest, ClearingDropsTheCaptureKeepsTheHistoryAndGoesIdle)
    {
        rig.container->create_default_processor();
        capture(5);

        rig.container->clear();

        EXPECT_FALSE(rig.container->has_data());
        EXPECT_TRUE(processed_pixels(*rig.container).empty());
        EXPECT_EQ(rig.container->get_processing_state(), ProcessingState::IDLE);
        EXPECT_EQ(rig.container->get_num_frames(), 1U);
    }

    TEST_F(WindowContainerRigTest, DimensionsDescribeOneSurface)
    {
        const auto dims = rig.container->get_dimensions();

        ASSERT_EQ(dims.size(), 3U);
        EXPECT_EQ(dims[0].role, DataDimension::Role::SPATIAL_Y);
        EXPECT_EQ(dims[1].role, DataDimension::Role::SPATIAL_X);
        EXPECT_EQ(dims[2].role, DataDimension::Role::CHANNEL);
        EXPECT_EQ(dims[0].size, k_surface_height);
        EXPECT_EQ(dims[1].size, k_surface_width);
        EXPECT_EQ(dims[2].size, k_surface_channels);
        EXPECT_EQ(rig.container->get_total_elements(), k_surface_bytes);
        EXPECT_EQ(rig.container->get_frame_size(), k_surface_bytes);
    }

    TEST_F(WindowContainerRigTest, HasNoDataUntilTheFirstCapture)
    {
        RegionGroup group("all");
        group.add_region(Region(std::vector<uint64_t> { 0, 0 }, std::vector<uint64_t> { 2, 3 }));
        rig.container->add_region_group(group);
        rig.container->create_default_processor();

        EXPECT_FALSE(rig.container->has_data());
        EXPECT_EQ(rig.container->get_raw_data(), nullptr);
        EXPECT_TRUE(rig.container->get_region_data(Region(std::vector<uint64_t> { 0, 0 }, std::vector<uint64_t> { 2, 3 })).empty());

        capture(1);

        EXPECT_TRUE(rig.container->has_data());
        EXPECT_NE(rig.container->get_raw_data(), nullptr);
    }

    TEST_F(WindowContainerRigTest, FramesHeldCountCapturesUpToTheCapacity)
    {
        rig.container->create_default_processor();
        EXPECT_EQ(rig.container->get_num_frames(), 0U);

        capture(1);
        capture(2);
        EXPECT_EQ(rig.container->get_num_frames(), 2U);

        for (uint64_t seed = 3; seed <= 8; ++seed) {
            capture(seed);
        }
        EXPECT_EQ(rig.container->get_num_frames(), k_history);
    }

    TEST_F(WindowContainerRigTest, FramesAreAddressedOldestFirst)
    {
        rig.container->create_default_processor();
        std::vector<std::vector<uint8_t>> frames;
        for (uint64_t seed = 1; seed <= 6; ++seed) {
            frames.push_back(capture(seed));
        }

        for (uint64_t i = 0; i < k_history; ++i) {
            const auto view = rig.container->get_frame(i).as<uint8_t>();
            EXPECT_EQ(std::vector<uint8_t>(view.begin(), view.end()), frames[i + 2]) << i;
        }
        EXPECT_TRUE(rig.container->get_frame(k_history).as<uint8_t>().empty());
    }

    TEST_F(WindowContainerRigTest, FramesCopyOutOldestFirstAndPadWithZeros)
    {
        rig.container->create_default_processor();
        std::vector<std::vector<uint8_t>> frames;
        for (uint64_t seed = 1; seed <= 6; ++seed) {
            frames.push_back(capture(seed));
        }

        std::vector<uint8_t> middle(2 * k_surface_bytes);
        rig.container->copy_frames(middle, 1, 2);
        std::vector<uint8_t> expected = frames[3];
        expected.insert(expected.end(), frames[4].begin(), frames[4].end());
        EXPECT_EQ(middle, expected);

        std::vector<uint8_t> tail(2 * k_surface_bytes);
        rig.container->copy_frames(tail, k_history - 1, 2);
        expected = frames[5];
        expected.resize(2 * k_surface_bytes, 0);
        EXPECT_EQ(tail, expected);
    }

    TEST_F(WindowContainerRigTest, CopyingFramesAsAnotherElementTypeIsRefused)
    {
        std::vector<double> out(4);

        EXPECT_THROW(rig.container->copy_frames(out, 0, 1), std::runtime_error);
    }

    TEST_F(WindowContainerRigTest, ValuesAreReadAsYXChannel)
    {
        rig.container->create_default_processor();
        EXPECT_EQ(rig.container->read_value<uint8_t>({ 0, 0, 0 }, 77), 77);

        const auto frame = capture(1);

        EXPECT_EQ(rig.container->read_value<uint8_t>({ 2, 3, 1 }, 77), frame[(2 * k_surface_width + 3) * k_surface_channels + 1]);
        EXPECT_EQ(rig.container->read_value<uint8_t>({ 0, 0, 0 }, 77), frame[0]);
    }

    TEST_F(WindowContainerRigTest, ValuesOutsideTheSurfaceOrOfAnotherTypeAreIgnored)
    {
        rig.container->create_default_processor();
        capture(1);

        EXPECT_EQ(rig.container->read_value<uint8_t>({ k_surface_height, 0, 0 }, 77), 77);
        EXPECT_EQ(rig.container->read_value<uint8_t>({ 0, k_surface_width, 0 }, 77), 77);
        EXPECT_EQ(rig.container->read_value<uint8_t>({ 0, 0, k_surface_channels }, 77), 77);
        EXPECT_EQ(rig.container->read_value<uint8_t>({ 0, 0 }, 77), 77);
        EXPECT_EQ(rig.container->read_value<float>({ 0, 0, 0 }, -1.0F), -1.0F);
    }

    TEST_F(WindowContainerRigTest, ARegionCropsTheCaptureByYAndX)
    {
        rig.container->create_default_processor();
        const auto frame = capture(1);
        RegionGroup group("crop");
        group.add_region(Region(std::vector<uint64_t> { 1, 1 }, std::vector<uint64_t> { 2, 2 }));
        rig.container->add_region_group(group);

        const auto data = rig.container->get_region_data(Region(std::vector<uint64_t> { 0, 0 }, std::vector<uint64_t> { 2, 3 }));

        std::vector<uint8_t> expected;
        for (uint32_t y = 1; y <= 2; ++y) {
            for (uint32_t x = 1; x <= 2; ++x) {
                for (uint32_t c = 0; c < k_surface_channels; ++c) {
                    expected.push_back(frame[(y * k_surface_width + x) * k_surface_channels + c]);
                }
            }
        }
        ASSERT_EQ(data.size(), 1U);
        EXPECT_EQ(raw_bytes(data[0]), expected);
    }

    TEST_F(WindowContainerRigTest, ARegionWithAChannelRangeCropsThatChannel)
    {
        rig.container->create_default_processor();
        const auto frame = capture(1);
        RegionGroup group("channel");
        group.add_region(Region(std::vector<uint64_t> { 1, 1, 2 }, std::vector<uint64_t> { 2, 2, 2 }));
        rig.container->add_region_group(group);

        const auto data = rig.container->get_region_data(Region(std::vector<uint64_t> { 0, 0 }, std::vector<uint64_t> { 2, 3 }));

        std::vector<uint8_t> expected;
        for (uint32_t y = 1; y <= 2; ++y) {
            for (uint32_t x = 1; x <= 2; ++x) {
                expected.push_back(frame[(y * k_surface_width + x) * k_surface_channels + 2]);
            }
        }
        ASSERT_EQ(data.size(), 1U);
        EXPECT_EQ(raw_bytes(data[0]), expected);
    }

    TEST_F(WindowContainerRigTest, ARegionOutsideTheQueryIsNotReturned)
    {
        rig.container->create_default_processor();
        capture(1);
        RegionGroup group("far");
        group.add_region(Region(std::vector<uint64_t> { 2, 2 }, std::vector<uint64_t> { 2, 3 }));
        rig.container->add_region_group(group);

        const auto data = rig.container->get_region_data(Region(std::vector<uint64_t> { 0, 0 }, std::vector<uint64_t> { 1, 1 }));

        EXPECT_TRUE(data.empty());
    }

    TEST_F(WindowContainerRigTest, PublishingAMatchingCaptureKeepsItInTheHistory)
    {
        const auto frame = surface_bytes(k_surface_width, k_surface_height, SurfaceFormat::B8G8R8A8_SRGB, 4);

        EXPECT_TRUE(rig.container->publish_capture(frame));

        EXPECT_EQ(rig.container->get_write_head(), 1U);
        EXPECT_EQ(history_slot(*rig.container, 0), frame);
        EXPECT_EQ(processed_pixels(*rig.container), frame);
    }

    TEST_F(WindowContainerRigTest, PublishingACaptureThatDoesNotFitStillUpdatesTheProcessedData)
    {
        const std::vector<uint8_t> wrong_size(10, 1);

        EXPECT_FALSE(rig.container->publish_capture(wrong_size));
        EXPECT_FALSE(rig.container->publish_capture(std::vector<float>(k_surface_bytes, 0.5F)));

        EXPECT_EQ(rig.container->get_write_head(), 0U);
        EXPECT_EQ(rig.container->get_num_frames(), 0U);
        EXPECT_EQ(rig.container->processed_frame_as_float(0).size(), k_surface_bytes);
    }

    TEST_F(WindowContainerRigTest, ReshapingChangesTheElementTypeAndRestartsTheHistory)
    {
        rig.container->publish_capture(surface_bytes(k_surface_width, k_surface_height, SurfaceFormat::B8G8R8A8_SRGB, 1));

        rig.container->handle_surface_resize(6, 5, SurfaceFormat::R32G32B32A32_SFLOAT);

        const auto& structure = std::as_const(*rig.container).get_structure();
        EXPECT_EQ(structure.get_width(), 6U);
        EXPECT_EQ(structure.get_height(), 5U);
        EXPECT_EQ(structure.get_channel_count(), 4U);
        EXPECT_EQ(rig.container->get_surface_format(), SurfaceFormat::R32G32B32A32_SFLOAT);
        EXPECT_EQ(rig.container->value_element_type(), std::type_index(typeid(float)));
        EXPECT_EQ(rig.container->get_write_head(), 0U);
        EXPECT_EQ(rig.container->get_num_frames(), 0U);
        EXPECT_FALSE(rig.container->has_data());
        EXPECT_EQ(history_slot(*rig.container, 0).size(), size_t { 6 } * 5 * 4 * sizeof(float));
    }

    TEST_F(WindowContainerRigTest, AttachingAProcessorAdoptsTheSwapchainFormat)
    {
        rig.display.set_format(SurfaceFormat::R16G16B16A16_SFLOAT);
        ASSERT_EQ(rig.container->get_surface_format(), SurfaceFormat::B8G8R8A8_SRGB);

        rig.container->create_default_processor();

        EXPECT_EQ(rig.container->get_surface_format(), SurfaceFormat::R16G16B16A16_SFLOAT);
        EXPECT_EQ(rig.container->value_element_type(), std::type_index(typeid(uint16_t)));
        EXPECT_EQ(std::as_const(*rig.container).get_structure().get_channel_count(), 4U);
    }

    TEST(WindowContainerPackedTest, APackedSurfaceIsOneWordPerPixel)
    {
        WindowRig rig(k_history, SurfaceFormat::A2B10G10R10_UNORM);
        rig.container->create_default_processor();
        rig.display.present(1);
        rig.container->process_default();

        EXPECT_EQ(rig.container->value_element_type(), std::type_index(typeid(uint32_t)));
        EXPECT_EQ(std::as_const(*rig.container).get_structure().get_channel_count(), 1U);
        EXPECT_EQ(rig.container->get_total_elements(), size_t { k_surface_width } * k_surface_height);
        EXPECT_EQ(rig.container->get_num_frames(), 1U);
    }

    TEST(WindowContainerPackedTest, ThePackedFloatViewUnpacksToFourChannels)
    {
        WindowRig rig(k_history, SurfaceFormat::A2B10G10R10_UNORM);
        rig.container->create_default_processor();
        const auto frame = rig.display.present(2);
        rig.container->process_default();

        const auto view = rig.container->processed_frame_as_float(0);

        ASSERT_EQ(view.size(), size_t { k_surface_width } * k_surface_height * 4);
        uint32_t word = 0;
        std::memcpy(&word, frame.data(), sizeof(word));
        EXPECT_FLOAT_EQ(view[0], static_cast<float>(word & 0x3FFU) / 1023.0F);
        EXPECT_FLOAT_EQ(view[1], static_cast<float>((word >> 10U) & 0x3FFU) / 1023.0F);
        EXPECT_FLOAT_EQ(view[2], static_cast<float>((word >> 20U) & 0x3FFU) / 1023.0F);
        EXPECT_FLOAT_EQ(view[3], static_cast<float>((word >> 30U) & 0x3U) / 3.0F);
    }

    TEST(WindowContainerFloatTest, AFloatSurfaceIsViewedWithoutConversion)
    {
        WindowRig rig(k_history, SurfaceFormat::R32G32B32A32_SFLOAT);
        rig.container->create_default_processor();
        const auto frame = rig.display.present(3);
        rig.container->process_default();

        const auto view = rig.container->processed_frame_as_float(0);

        ASSERT_EQ(view.size() * sizeof(float), frame.size());
        EXPECT_EQ(std::memcmp(view.data(), frame.data(), frame.size()), 0);
    }

    TEST(WindowContainerTest, AZeroCapacityHistoryHoldsOneSlot)
    {
        const auto window = std::make_shared<ScriptedWindow>(k_surface_width, k_surface_height);

        const WindowContainer container(window, 0);

        EXPECT_EQ(container.get_frame_capacity(), 1U);
    }

    TEST_F(WindowContainerRigTest, AnEightBitCaptureNormalisesToFloat)
    {
        rig.container->get_processed_data()[0] = std::vector<uint8_t> { 0, 51, 255 };

        const auto values = rig.container->processed_frame_as_float(0);

        ASSERT_EQ(values.size(), 3U);
        EXPECT_FLOAT_EQ(values[0], 0.0F);
        EXPECT_FLOAT_EQ(values[1], 0.2F);
        EXPECT_FLOAT_EQ(values[2], 1.0F);
    }

    TEST_F(WindowContainerRigTest, TheFloatViewOfAMissingSlotIsEmpty)
    {
        EXPECT_TRUE(rig.container->processed_frame_as_float(1).empty());
    }

    TEST_F(WindowContainerRigTest, InvalidatingTheFloatCacheRecomputesFromTheCapture)
    {
        rig.container->get_processed_data()[0] = std::vector<uint8_t> { 0, 255 };
        ASSERT_EQ(rig.container->processed_frame_as_float(0).size(), 2U);

        rig.container->get_processed_data()[0] = std::vector<uint8_t> { 255, 0 };
        rig.container->invalidate_float_frame_cache(0);

        const auto values = rig.container->processed_frame_as_float(0);
        ASSERT_EQ(values.size(), 2U);
        EXPECT_FLOAT_EQ(values[0], 1.0F);
        EXPECT_FLOAT_EQ(values[1], 0.0F);
    }

    TEST_F(WindowContainerRigTest, ElementValuesAreBytes)
    {
        EXPECT_EQ(rig.container->value_element_type(), std::type_index(typeid(uint8_t)));
        EXPECT_EQ(rig.container->get_memory_layout(), Kakshya::MemoryLayout::ROW_MAJOR);
    }

    struct FormatCase {
        SurfaceFormat surface;
        ImageFormat image;
        const char* name;
    };

    class WindowContainerFormatTest : public ::testing::TestWithParam<FormatCase> { };

    INSTANTIATE_TEST_SUITE_P(Surfaces, WindowContainerFormatTest,
        ::testing::Values(
            FormatCase { SurfaceFormat::B8G8R8A8_SRGB, ImageFormat::BGRA8_SRGB, "Bgra8Srgb" },
            FormatCase { SurfaceFormat::B8G8R8A8_UNORM, ImageFormat::BGRA8, "Bgra8Unorm" },
            FormatCase { SurfaceFormat::R8G8B8A8_SRGB, ImageFormat::RGBA8_SRGB, "Rgba8Srgb" },
            FormatCase { SurfaceFormat::R8G8B8A8_UNORM, ImageFormat::RGBA8, "Rgba8Unorm" },
            FormatCase { SurfaceFormat::R16G16B16A16_SFLOAT, ImageFormat::RGBA16F, "Rgba16F" },
            FormatCase { SurfaceFormat::R32G32B32A32_SFLOAT, ImageFormat::RGBA32F, "Rgba32F" }),
        [](const ::testing::TestParamInfo<FormatCase>& info) { return std::string(info.param.name); });

    TEST_P(WindowContainerFormatTest, TheImageFormatFollowsTheLiveSwapchain)
    {
        WindowRig rig(k_history, GetParam().surface);

        EXPECT_EQ(rig.container->get_image_format(), GetParam().image);
    }

    TEST(WindowContainerTest, WithoutADisplayTheImageFormatFallsBackToBgraSrgb)
    {
        const auto window = std::make_shared<ScriptedWindow>(k_surface_width, k_surface_height);
        const WindowContainer container(window, k_history);

        EXPECT_EQ(container.get_image_format(), ImageFormat::BGRA8_SRGB);
    }

} // namespace
} // namespace MayaFlux::Test
