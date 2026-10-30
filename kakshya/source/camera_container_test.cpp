#include "gtest/gtest.h"

#include "../video_test_support.hpp"

#include "MayaFlux/Kakshya/Processors/FrameAccessProcessor.hpp"
#include "MayaFlux/Kakshya/Source/CameraContainer.hpp"

namespace MayaFlux::Test {
namespace {

    using Kakshya::CameraContainer;
    using Kakshya::FrameAccessProcessor;
    using Kakshya::ImageFormat;
    using Kakshya::ProcessingState;
    using namespace VideoSupport;

    constexpr uint64_t k_reader = 7;

    std::shared_ptr<CameraContainer> make_camera(ImageFormat format = ImageFormat::RGBA8)
    {
        return std::make_shared<CameraContainer>(k_width, k_height, format, k_fps);
    }

    bool write_frame(CameraContainer& camera, uint64_t seed)
    {
        auto* slot = camera.mutable_frame_ptr();
        if (!slot) {
            return false;
        }
        const auto bytes = frame_bytes(seed, camera.get_frame_byte_size());
        std::memcpy(slot, bytes.data(), bytes.size());
        return true;
    }

    struct CameraFormat {
        ImageFormat format;
        size_t bytes_per_pixel;
        std::type_index element;
        const char* name;
    };

    class CameraContainerFormatTest : public ::testing::TestWithParam<CameraFormat> { };

    INSTANTIATE_TEST_SUITE_P(Formats, CameraContainerFormatTest,
        ::testing::Values(
            CameraFormat { ImageFormat::R8, 1, typeid(uint8_t), "R8" },
            CameraFormat { ImageFormat::RGB8, 3, typeid(uint8_t), "RGB8" },
            CameraFormat { ImageFormat::RGBA8, 4, typeid(uint8_t), "RGBA8" },
            CameraFormat { ImageFormat::RGBA16, 8, typeid(uint16_t), "RGBA16" },
            CameraFormat { ImageFormat::RGBA32F, 16, typeid(float), "RGBA32F" },
            CameraFormat { ImageFormat::DEPTH32F, 4, typeid(float), "DEPTH32F" }),
        [](const ::testing::TestParamInfo<CameraFormat>& info) { return std::string(info.param.name); });

    TEST_P(CameraContainerFormatTest, HoldsOneBlackFrameOfTheFormatsSize)
    {
        const auto& c = GetParam();

        const auto camera = make_camera(c.format);

        const size_t frame_size = size_t { k_width } * k_height * c.bytes_per_pixel;
        EXPECT_EQ(camera->get_frame_byte_size(), frame_size);
        EXPECT_EQ(camera->value_element_type(), c.element);
        EXPECT_EQ(pixels_of(*camera, 0), std::vector<uint8_t>(frame_size, 0));
        EXPECT_NE(camera->mutable_frame_ptr(), nullptr);
    }

    TEST_P(CameraContainerFormatTest, AWrittenFrameIsReadBackWhole)
    {
        const auto camera = make_camera(GetParam().format);

        ASSERT_TRUE(write_frame(*camera, 3));

        EXPECT_EQ(pixels_of(*camera, 0), frame_bytes(3, camera->get_frame_byte_size()));
    }

    TEST(CameraContainerTest, AlwaysHoldsExactlyOneFrame)
    {
        const auto camera = make_camera();

        EXPECT_EQ(camera->get_num_frames(), 1U);
        EXPECT_EQ(camera->get_total_elements(), k_rgba8_frame_bytes);
        EXPECT_EQ(camera->get_dimensions().at(0).size, 1U);
        EXPECT_TRUE(camera->has_data());
        EXPECT_TRUE(camera->is_ready());
        EXPECT_FALSE(camera->is_ring_mode());
        EXPECT_TRUE(camera->get_frame_pixels(1).empty());
    }

    TEST(CameraContainerTest, DefaultsToThirtyFramesPerSecond)
    {
        const CameraContainer camera(k_width, k_height, ImageFormat::RGBA8);

        EXPECT_DOUBLE_EQ(camera.get_frame_rate(), 30.0);
    }

    TEST(CameraContainerTest, TakesItsGeometryFromTheConstructor)
    {
        const auto camera = make_camera();

        EXPECT_EQ(camera->get_width(), k_width);
        EXPECT_EQ(camera->get_height(), k_height);
        EXPECT_EQ(camera->get_format(), ImageFormat::RGBA8);
        EXPECT_DOUBLE_EQ(camera->get_frame_rate(), k_fps);
        EXPECT_EQ(camera->get_processing_state(), ProcessingState::IDLE);
    }

    TEST(CameraContainerTest, PackedDepthFormatsAreRefused)
    {
        EXPECT_THROW(CameraContainer(k_width, k_height, ImageFormat::DEPTH24, k_fps), std::invalid_argument);
    }

    TEST(CameraContainerTest, NeverLoopsEvenWhenAskedTo)
    {
        const auto camera = make_camera();

        camera->set_looping(true);

        EXPECT_FALSE(camera->is_looping());
        EXPECT_FALSE(camera->is_at_end());
    }

    TEST(CameraContainerTest, TheFrameViewShowsTheLatestWrite)
    {
        const auto camera = make_camera();
        write_frame(*camera, 2);

        const auto values = camera->get_frame(0).as<uint8_t>();

        EXPECT_EQ(std::vector<uint8_t>(values.begin(), values.end()), frame_bytes(2));
    }

    TEST(CameraContainerTest, MarkingReadyAfterAWriteMakesTheContainerReady)
    {
        const auto camera = make_camera();
        write_frame(*camera, 1);

        camera->mark_ready_for_processing(true);

        EXPECT_EQ(camera->get_processing_state(), ProcessingState::READY);
        EXPECT_TRUE(camera->is_ready_for_processing());
    }

    TEST(CameraContainerTest, TheDefaultProcessorReadsWithoutAdvancing)
    {
        const auto camera = make_camera();

        camera->create_default_processor();

        const auto processor = std::dynamic_pointer_cast<FrameAccessProcessor>(camera->get_default_processor());
        ASSERT_NE(processor, nullptr);
        EXPECT_FALSE(processor->get_auto_advance());
        EXPECT_EQ(processor->get_frame_byte_size(), k_rgba8_frame_bytes);
        EXPECT_EQ(camera->get_processing_state(), ProcessingState::READY);
    }

    TEST(CameraContainerTest, ProcessingCopiesTheLiveFrame)
    {
        const auto camera = make_camera();
        camera->create_default_processor();
        write_frame(*camera, 4);

        camera->process_default();

        EXPECT_EQ(processed_bytes(*camera), frame_bytes(4));
        EXPECT_EQ(camera->get_processing_state(), ProcessingState::PROCESSED);
    }

    TEST(CameraContainerTest, ProcessingRepeatsTheSameFrameUntilANewOneArrives)
    {
        const auto camera = make_camera();
        camera->create_default_processor();
        write_frame(*camera, 4);
        camera->process_default();
        camera->process_default();
        EXPECT_EQ(processed_bytes(*camera), frame_bytes(4));

        write_frame(*camera, 5);
        camera->process_default();

        EXPECT_EQ(processed_bytes(*camera), frame_bytes(5));
        EXPECT_EQ(camera->get_read_position(), std::vector<uint64_t> { 0 });
    }

    TEST(CameraContainerTest, ProcessingBeforeAnyFrameArrivesGivesBlack)
    {
        const auto camera = make_camera();
        camera->create_default_processor();

        camera->process_default();

        EXPECT_EQ(processed_bytes(*camera), std::vector<uint8_t>(k_rgba8_frame_bytes, 0));
    }

    TEST(CameraContainerTest, ClearingKeepsTheSingleFrameSlot)
    {
        const auto camera = make_camera();
        write_frame(*camera, 6);

        camera->clear();

        EXPECT_EQ(camera->get_num_frames(), 1U);
        ASSERT_NE(camera->mutable_frame_ptr(), nullptr);
        EXPECT_EQ(pixels_of(*camera, 0), std::vector<uint8_t>(k_rgba8_frame_bytes, 0));
    }

    class CameraContainerIoTest : public ::testing::Test {
    protected:
        void SetUp() override
        {
            camera = make_camera();
            camera->create_default_processor();
        }

        RecordingIOService io;
        std::shared_ptr<CameraContainer> camera;
    };

    TEST_F(CameraContainerIoTest, EachProcessingCycleRequestsTheNextFrame)
    {
        camera->setup_io(k_reader);

        camera->process_default();
        camera->process_default();

        EXPECT_EQ(io.frames, (std::vector<uint64_t> { k_reader, k_reader }));
    }

    TEST_F(CameraContainerIoTest, NothingIsRequestedBeforeTheIOIsWired)
    {
        camera->process_default();

        EXPECT_TRUE(io.frames.empty());
    }

    TEST_F(CameraContainerIoTest, TheReaderIdTravelsWithTheRequest)
    {
        camera->setup_io(41);

        camera->process_default();

        EXPECT_EQ(io.frames, std::vector<uint64_t> { 41 });
    }

    TEST_F(CameraContainerIoTest, WiringAgainReplacesTheReaderId)
    {
        camera->setup_io(1);
        camera->setup_io(2);

        camera->process_default();

        EXPECT_EQ(io.frames, std::vector<uint64_t> { 2 });
    }

    TEST_F(CameraContainerIoTest, AServiceWithoutAFrameRequestIsTolerated)
    {
        io.service().request_frame = nullptr;
        camera->setup_io(k_reader);

        EXPECT_NO_THROW(camera->process_default());
    }

    TEST_F(CameraContainerIoTest, ARequestIsMadeEvenBeforeTheFirstFrameHasArrived)
    {
        camera->mark_ready_for_processing(false);
        camera->setup_io(k_reader);

        camera->process_default();

        EXPECT_EQ(io.frames, std::vector<uint64_t> { k_reader });
    }

    TEST_F(CameraContainerIoTest, ACameraNeverAsksForADecode)
    {
        camera->setup_io(k_reader);

        camera->process_default();
        camera->update_read_position_for_channel(0, 0);

        EXPECT_TRUE(io.decodes.empty());
    }

    TEST(CameraContainerNoServiceTest, WiringWithoutAServiceIsTolerated)
    {
        const auto camera = make_camera();
        camera->create_default_processor();

        EXPECT_NO_THROW(camera->setup_io(k_reader));
        EXPECT_NO_THROW(camera->process_default());
    }

} // namespace
} // namespace MayaFlux::Test
