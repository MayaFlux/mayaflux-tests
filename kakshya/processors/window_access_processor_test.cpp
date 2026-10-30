#include "gtest/gtest.h"

#include "../video_test_support.hpp"
#include "../window_test_support.hpp"

#include "MayaFlux/Kakshya/Processors/WindowAccessProcessor.hpp"

namespace MayaFlux::Test {
namespace {

    using Kakshya::DataVariant;
    using Kakshya::ProcessingState;
    using Kakshya::WindowAccessProcessor;
    using Kakshya::WindowContainer;
    using namespace WindowSupport;

    std::type_index element_type_of(const DataVariant& variant)
    {
        return std::visit(
            [](const auto& v) { return std::type_index(typeid(typename std::decay_t<decltype(v)>::value_type)); },
            variant);
    }

    std::vector<uint8_t> bytes_of(const DataVariant& variant)
    {
        return std::visit(
            [](const auto& v) {
                const auto* first = reinterpret_cast<const uint8_t*>(v.data());
                return std::vector<uint8_t>(first, first + v.size() * sizeof(typename std::decay_t<decltype(v)>::value_type));
            },
            variant);
    }

    class WindowAccessProcessorTest : public ::testing::Test {
    protected:
        void attach() { processor->on_attach(rig.container); }
        void process() { processor->process(rig.container); }

        WindowRig rig;
        std::shared_ptr<WindowAccessProcessor> processor { std::make_shared<WindowAccessProcessor>() };
    };

    TEST_F(WindowAccessProcessorTest, AttachingToAnotherContainerIsRefused)
    {
        const auto video = std::make_shared<Kakshya::VideoStreamContainer>(
            VideoSupport::k_width, VideoSupport::k_height, Kakshya::ImageFormat::RGBA8, VideoSupport::k_fps);

        EXPECT_THROW(processor->on_attach(video), std::invalid_argument);
    }

    TEST_F(WindowAccessProcessorTest, AttachingEnablesCaptureAndMarksTheContainerReady)
    {
        ASSERT_FALSE(rig.window->is_capture_enabled());

        attach();

        EXPECT_TRUE(rig.window->is_capture_enabled());
        EXPECT_TRUE(rig.container->is_ready_for_processing());
    }

    TEST_F(WindowAccessProcessorTest, AttachingCachesTheLiveSurfaceFormat)
    {
        rig.display.set_format(SurfaceFormat::R16G16B16A16_SFLOAT);

        attach();

        EXPECT_EQ(processor->get_surface_format(), SurfaceFormat::R16G16B16A16_SFLOAT);
    }

    TEST_F(WindowAccessProcessorTest, DetachingDisablesCaptureItEnabled)
    {
        attach();

        processor->on_detach(rig.container);

        EXPECT_FALSE(rig.window->is_capture_enabled());
    }

    TEST_F(WindowAccessProcessorTest, DetachingLeavesCaptureThatWasAlreadyOn)
    {
        rig.window->set_capture_enabled(true);
        attach();

        processor->on_detach(rig.container);

        EXPECT_TRUE(rig.window->is_capture_enabled());
    }

    TEST_F(WindowAccessProcessorTest, DetachingForgetsTheCachedState)
    {
        attach();
        rig.display.present(1);
        process();
        ASSERT_GT(processor->get_last_readback_bytes(), 0U);

        processor->on_detach(rig.container);

        EXPECT_EQ(processor->get_last_readback_bytes(), 0U);
        EXPECT_EQ(processor->get_surface_format(), SurfaceFormat::B8G8R8A8_SRGB);
    }

    TEST_F(WindowAccessProcessorTest, ProcessingCapturesTheLastFrame)
    {
        attach();
        const auto frame = rig.display.present(2);

        process();

        EXPECT_EQ(processed_pixels(*rig.container), frame);
        EXPECT_EQ(processor->get_last_readback_bytes(), frame.size());
        EXPECT_FALSE(processor->is_processing());
    }

    TEST_F(WindowAccessProcessorTest, ProcessingWalksThroughProcessingToProcessed)
    {
        attach();
        rig.display.present(2);
        std::vector<ProcessingState> seen;
        rig.container->register_state_change_callback(
            [&seen](const std::shared_ptr<Kakshya::SignalSourceContainer>&, ProcessingState state) { seen.push_back(state); });

        process();
        rig.container->unregister_state_change_callback();

        EXPECT_EQ(seen, (std::vector<ProcessingState> { ProcessingState::PROCESSING, ProcessingState::PROCESSED }));
    }

    TEST_F(WindowAccessProcessorTest, EachCaptureIsKeptInTheHistoryAndAdvancesTheHead)
    {
        attach();
        const auto first = rig.display.present(1);
        process();
        const auto second = rig.display.present(2);
        process();

        EXPECT_EQ(rig.container->get_write_head(), 2U);
        EXPECT_EQ(history_slot(*rig.container, 0), first);
        EXPECT_EQ(history_slot(*rig.container, 1), second);
    }

    TEST_F(WindowAccessProcessorTest, TheHistoryWrapsAndOverwritesTheOldest)
    {
        attach();
        std::vector<std::vector<uint8_t>> frames;
        for (uint64_t seed = 1; seed <= k_history + 2; ++seed) {
            frames.push_back(rig.display.present(seed));
            process();
        }

        EXPECT_EQ(rig.container->get_write_head(), 2U);
        EXPECT_EQ(history_slot(*rig.container, 0), frames[4]);
        EXPECT_EQ(history_slot(*rig.container, 1), frames[5]);
        EXPECT_EQ(history_slot(*rig.container, 2), frames[2]);
        EXPECT_EQ(history_slot(*rig.container, 3), frames[3]);
    }

    TEST_F(WindowAccessProcessorTest, WithoutASurfaceExtentNothingHappens)
    {
        attach();
        rig.display.resize(0, 0);
        std::vector<ProcessingState> seen;
        rig.container->register_state_change_callback(
            [&seen](const std::shared_ptr<Kakshya::SignalSourceContainer>&, ProcessingState state) { seen.push_back(state); });

        process();
        rig.container->unregister_state_change_callback();

        EXPECT_TRUE(seen.empty());
        EXPECT_EQ(rig.container->get_write_head(), 0U);
    }

    TEST_F(WindowAccessProcessorTest, WithoutACapturedFrameYetTheHistoryIsLeftAlone)
    {
        attach();
        rig.display.withdraw_frame();

        process();

        EXPECT_EQ(rig.container->get_write_head(), 0U);
        EXPECT_NE(rig.container->get_processing_state(), ProcessingState::PROCESSED);
    }

    TEST_F(WindowAccessProcessorTest, AnotherContainerIsNotProcessed)
    {
        attach();
        const auto video = std::make_shared<Kakshya::VideoStreamContainer>(
            VideoSupport::k_width, VideoSupport::k_height, Kakshya::ImageFormat::RGBA8, VideoSupport::k_fps);
        rig.display.present(1);

        EXPECT_NO_THROW(processor->process(video));

        EXPECT_TRUE(video->get_processed_data().empty());
    }

    TEST_F(WindowAccessProcessorTest, AResizedSurfaceReshapesTheContainer)
    {
        attach();
        rig.display.present(1);
        process();

        rig.window->set_size(6, 5);
        rig.display.resize(6, 5);
        rig.display.present(2);
        process();

        const auto& structure = std::as_const(*rig.container).get_structure();
        EXPECT_EQ(structure.get_width(), 6U);
        EXPECT_EQ(structure.get_height(), 5U);
        EXPECT_EQ(history_slot(*rig.container, 3).size(), size_t { 6 } * 5 * k_surface_channels);
        EXPECT_EQ(processed_pixels(*rig.container).size(), size_t { 6 } * 5 * k_surface_channels);
    }

    TEST_F(WindowAccessProcessorTest, TheFrameCapturedAtAResizeIsKept)
    {
        attach();
        rig.display.present(1);
        process();
        rig.window->set_size(6, 5);
        rig.display.resize(6, 5);
        const auto frame = rig.display.present(2);

        process();

        EXPECT_EQ(processed_pixels(*rig.container), frame);
        EXPECT_EQ(history_slot(*rig.container, 0), frame);
    }

    TEST_F(WindowAccessProcessorTest, TheContainerFollowsTheSwapchainWhenTheWindowStateLags)
    {
        attach();
        rig.display.present(1);
        process();
        rig.display.resize(6, 5);
        const auto frame = rig.display.present(2);

        process();

        const auto& structure = std::as_const(*rig.container).get_structure();
        EXPECT_EQ(structure.get_width(), 6U);
        EXPECT_EQ(structure.get_height(), 5U);
        EXPECT_EQ(history_slot(*rig.container, 0), frame);
        EXPECT_EQ(history_slot(*rig.container, 0).size(), size_t { 6 } * 5 * k_surface_channels);
    }

    TEST_F(WindowAccessProcessorTest, TheFloatViewFollowsEachNewCapture)
    {
        attach();
        const auto first = rig.display.present(1);
        process();
        const auto first_view = rig.container->processed_frame_as_float(0);
        ASSERT_EQ(first_view.size(), first.size());
        ASSERT_FLOAT_EQ(first_view[0], static_cast<float>(first[0]) / 255.0F);

        const auto second = rig.display.present(2);
        process();

        const auto second_view = rig.container->processed_frame_as_float(0);
        ASSERT_EQ(second_view.size(), second.size());
        EXPECT_FLOAT_EQ(second_view[0], static_cast<float>(second[0]) / 255.0F);
    }

    TEST(WindowAccessProcessorNoDisplayTest, WithoutADisplayServiceNothingIsCaptured)
    {
        const auto window = std::make_shared<ScriptedWindow>(k_surface_width, k_surface_height);
        const auto container = std::make_shared<WindowContainer>(window, k_history);
        WindowAccessProcessor processor;
        processor.on_attach(container);

        processor.process(container);

        EXPECT_EQ(container->get_write_head(), 0U);
        EXPECT_NE(container->get_processing_state(), ProcessingState::PROCESSED);
    }

    struct SurfaceCase {
        SurfaceFormat format;
        std::type_index element;
        const char* name;
    };

    class WindowAccessProcessorFormatTest : public ::testing::TestWithParam<SurfaceCase> { };

    INSTANTIATE_TEST_SUITE_P(Surfaces, WindowAccessProcessorFormatTest,
        ::testing::Values(
            SurfaceCase { SurfaceFormat::B8G8R8A8_SRGB, typeid(uint8_t), "Bgra8Srgb" },
            SurfaceCase { SurfaceFormat::R8G8B8A8_UNORM, typeid(uint8_t), "Rgba8Unorm" },
            SurfaceCase { SurfaceFormat::R16G16B16A16_SFLOAT, typeid(uint16_t), "Rgba16F" },
            SurfaceCase { SurfaceFormat::R32G32B32A32_SFLOAT, typeid(float), "Rgba32F" },
            SurfaceCase { SurfaceFormat::A2B10G10R10_UNORM, typeid(uint32_t), "A2b10g10r10" }),
        [](const ::testing::TestParamInfo<SurfaceCase>& info) { return std::string(info.param.name); });

    TEST_P(WindowAccessProcessorFormatTest, TheCaptureKeepsTheSurfacesNativeElementType)
    {
        const auto& c = GetParam();
        WindowRig rig(k_history, c.format);
        WindowAccessProcessor processor;
        processor.on_attach(rig.container);
        const auto frame = rig.display.present(3);

        processor.process(rig.container);

        const auto& processed = std::as_const(*rig.container).get_processed_data();
        ASSERT_FALSE(processed.empty());
        EXPECT_EQ(element_type_of(processed[0]), c.element);
        EXPECT_EQ(bytes_of(processed[0]), frame);
        EXPECT_EQ(processor.get_last_readback_bytes(), frame.size());
        EXPECT_EQ(processor.get_surface_format(), c.format);
    }

} // namespace
} // namespace MayaFlux::Test
