#include "gtest/gtest.h"
#include "../support/test_metadata.hpp"

#include "MayaFlux/IO/Camera/CameraSource.hpp"
#include "MayaFlux/IO/Camera/FFmpegCameraReader.hpp"
#include "MayaFlux/Kakshya/Source/CameraContainer.hpp"

extern "C" {
#include <libavdevice/avdevice.h>
#include <libavformat/avformat.h>
}

#include <algorithm>
#include <chrono>
#include <thread>

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {

namespace {

    /**
     * @brief Synthetic camera: ffmpeg's lavfi test pattern exposed as a device.
     *
     * The reader opens it through the same path as a real camera, so no
     * hardware is needed. Returns false when this ffmpeg build has no lavfi.
     */
    bool lavfi_available()
    {
        avdevice_register_all();
        return av_find_input_format("lavfi") != nullptr;
    }

    IO::CameraConfig synthetic_config(uint32_t width, uint32_t height, double fps)
    {
        IO::CameraConfig config;
        config.device_name = "testsrc=size=" + std::to_string(width) + "x" + std::to_string(height)
            + ":rate=" + std::to_string(static_cast<int>(fps));
        config.format_override = "lavfi";
        config.target_width = width;
        config.target_height = height;
        config.target_fps = fps;
        return config;
    }

    bool has_nonzero_byte(std::span<const uint8_t> bytes)
    {
        return std::any_of(bytes.begin(), bytes.end(), [](uint8_t b) { return b != 0; });
    }

}

// =========================================================================
// Configuration and presets (no device)
// =========================================================================

TEST(FFmpegCameraTest, DefaultConfigAsksFor1080pAt30AndDeviceFormat)
{
    IO::CameraConfig config;

    EXPECT_EQ(config.target_width, 1920U);
    EXPECT_EQ(config.target_height, 1080U);
    EXPECT_EQ(config.target_fps, 30.0);
    EXPECT_EQ(config.pixel_format, -1);
    EXPECT_TRUE(config.format_override.empty());
    EXPECT_TRUE(config.input_format.empty());
}

TEST(FFmpegCameraTest, PresetModesStartWithDeviceDefaultAndFlagHighEndModes)
{
    IO::FFmpegCameraReader reader;
    const auto modes = reader.preset_modes();

    ASSERT_EQ(modes.size(), 6U);
    EXPECT_EQ(modes.front().label, "Device default");
    EXPECT_EQ(modes.front().width, 0U);
    EXPECT_EQ(modes.front().height, 0U);

    std::vector<std::string> high_end;
    for (const auto& mode : modes) {
        if (mode.high_end)
            high_end.push_back(mode.label);
    }
    EXPECT_EQ(high_end.size(), 2U);
}

TEST(FFmpegCameraTest, ApplyModeWritesSizeAndRateIntoConfig)
{
    IO::FFmpegCameraReader reader;
    const auto modes = reader.preset_modes();
    IO::CameraConfig config;

    modes.at(2).apply(config);

    EXPECT_EQ(config.target_width, 1280U);
    EXPECT_EQ(config.target_height, 720U);
    EXPECT_EQ(config.target_fps, 30.0);
}

// =========================================================================
// Closed reader and failure paths (no device)
// =========================================================================

TEST(FFmpegCameraTest, ClosedReaderIsInert)
{
    IO::FFmpegCameraReader reader;

    EXPECT_FALSE(reader.is_open());
    EXPECT_EQ(reader.width(), 0U);
    EXPECT_EQ(reader.height(), 0U);
    EXPECT_EQ(reader.frame_rate(), 0.0);
    EXPECT_EQ(reader.create_container(), nullptr);
    EXPECT_EQ(reader.last_error(), "Cannot create container: reader not open");
    EXPECT_FALSE(reader.pull_frame(nullptr));

    reader.close();
    reader.close();
    EXPECT_FALSE(reader.is_open());
}

TEST(FFmpegCameraTest, OpenWithEmptyDeviceNameFails)
{
    avdevice_register_all();
    IO::FFmpegCameraReader reader;
    IO::CameraConfig config;
    config.format_override = "lavfi";
    config.device_name.clear();

    EXPECT_FALSE(reader.open(config));
    EXPECT_FALSE(reader.is_open());
    EXPECT_FALSE(reader.last_error().empty());
}

TEST(FFmpegCameraTest, OpenWithUnknownFormatFailsAndNamesIt)
{
    IO::FFmpegCameraReader reader;
    IO::CameraConfig config;
    config.device_name = "anything";
    config.format_override = "no_such_camera_format_xyz";

    EXPECT_FALSE(reader.open(config));
    EXPECT_FALSE(reader.is_open());
    EXPECT_NE(reader.last_error().find("no_such_camera_format_xyz"), std::string::npos);
}

TEST(FFmpegCameraTest, SetContainerThenCloseStopsTheDecodeThread)
{
    IO::FFmpegCameraReader reader;
    auto container = std::make_shared<Kakshya::CameraContainer>(
        64U, 48U, Kakshya::ImageFormat::RGBA8, 30.0);

    reader.set_container(container);
    reader.pull_frame_all();
    reader.close();

    EXPECT_FALSE(reader.is_open());
    EXPECT_FALSE(reader.pull_frame(container));
}

TEST(FFmpegCameraTest, EnumerateConfigsIsSortedAndNamesEveryDevice)
{
    IO::FFmpegCameraReader reader;
    const auto configs = reader.enumerate_configs();

    EXPECT_TRUE(std::is_sorted(configs.begin(), configs.end(),
        [](const IO::CameraConfig& a, const IO::CameraConfig& b) {
            return a.device_name < b.device_name;
        }));
    for (const auto& config : configs)
        EXPECT_FALSE(config.device_name.empty());
}

// =========================================================================
// Live path through the synthetic lavfi camera
// =========================================================================

TEST(FFmpegCameraLiveTest, OpenReportsNegotiatedDimensionsAndRate)
{
    if (!lavfi_available())
        GTEST_SKIP() << "ffmpeg built without lavfi";

    IO::FFmpegCameraReader reader;
    ASSERT_TRUE(reader.open(synthetic_config(64, 48, 24.0))) << reader.last_error();

    EXPECT_TRUE(reader.is_open());
    EXPECT_EQ(reader.width(), 64U);
    EXPECT_EQ(reader.height(), 48U);
    EXPECT_NEAR(reader.frame_rate(), 24.0, 0.5);
}

TEST(FFmpegCameraLiveTest, PullFrameFillsContainerWithPixels)
{
    if (!lavfi_available())
        GTEST_SKIP() << "ffmpeg built without lavfi";

    IO::FFmpegCameraReader reader;
    ASSERT_TRUE(reader.open(synthetic_config(64, 48, 24.0))) << reader.last_error();

    auto container = reader.create_container();
    ASSERT_NE(container, nullptr);
    ASSERT_TRUE(reader.pull_frame(container)) << reader.last_error();

    const auto pixels = container->get_frame_pixels(0);
    EXPECT_EQ(pixels.size(), static_cast<size_t>(64) * 48 * 4);
    EXPECT_TRUE(has_nonzero_byte(pixels));
}

TEST(FFmpegCameraLiveTest, CreateContainerAfterCloseIsNull)
{
    if (!lavfi_available())
        GTEST_SKIP() << "ffmpeg built without lavfi";

    IO::FFmpegCameraReader reader;
    ASSERT_TRUE(reader.open(synthetic_config(64, 48, 24.0))) << reader.last_error();
    reader.close();

    EXPECT_EQ(reader.create_container(), nullptr);
    EXPECT_FALSE(reader.is_open());
}

TEST(FFmpegCameraLiveTest, ReopenAtAnotherSizeChangesDimensions)
{
    if (!lavfi_available())
        GTEST_SKIP() << "ffmpeg built without lavfi";

    IO::FFmpegCameraReader reader;
    ASSERT_TRUE(reader.open(synthetic_config(64, 48, 24.0))) << reader.last_error();
    ASSERT_TRUE(reader.open(synthetic_config(32, 24, 24.0))) << reader.last_error();

    EXPECT_EQ(reader.width(), 32U);
    EXPECT_EQ(reader.height(), 24U);
}

TEST(FFmpegCameraLiveTest, FailedOpenAfterGoodOpenLeavesReaderClosed)
{
    if (!lavfi_available())
        GTEST_SKIP() << "ffmpeg built without lavfi";

    IO::FFmpegCameraReader reader;
    ASSERT_TRUE(reader.open(synthetic_config(64, 48, 24.0))) << reader.last_error();

    IO::CameraConfig bad;
    bad.device_name = "anything";
    bad.format_override = "no_such_camera_format_xyz";
    EXPECT_FALSE(reader.open(bad));
    EXPECT_FALSE(reader.is_open());
    EXPECT_EQ(reader.width(), 0U);
}

TEST(FFmpegCameraLiveTest, DecodeThreadDeliversRequestedFrame)
{
    if (!lavfi_available())
        GTEST_SKIP() << "ffmpeg built without lavfi";

    IO::FFmpegCameraReader reader;
    reader.setup_io_service(7);
    ASSERT_TRUE(reader.open(synthetic_config(64, 48, 24.0))) << reader.last_error();

    auto container = reader.create_container();
    ASSERT_NE(container, nullptr);
    reader.set_container(container);
    reader.pull_frame_all();

    bool delivered = false;
    for (int i = 0; i < 200 && !delivered; ++i) {
        delivered = has_nonzero_byte(container->get_frame_pixels(0));
        if (!delivered)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    reader.close();
    EXPECT_TRUE(delivered);
}

}
