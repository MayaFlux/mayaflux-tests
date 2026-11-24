#include "gtest/gtest.h"
#include "../support/test_metadata.hpp"

#include "io_manager_fixture.hpp"

#include "MayaFlux/Buffers/Textures/TextureBuffer.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::writable_temp>() }))

namespace MayaFlux::Test {

namespace {

    class IOManagerVideoLoadTest : public IOManagerFixture { };
    class IOManagerVideoLifetimeTest : public IOManagerFixture { };
    class IOManagerVideoWriterTest : public IOManagerFixture { };
    class IOManagerCameraTest : public IOManagerFixture { };
    class IOManagerImageLoadTest : public IOManagerFixture { };
    class IOManagerImageSaveTest : public IOManagerFixture { };

    constexpr uint32_t image_width = 4;
    constexpr uint32_t image_height = 3;

    std::string make_ppm()
    {
        std::string bytes = "P6\n4 3\n255\n";
        for (uint32_t y = 0; y < image_height; ++y) {
            for (uint32_t x = 0; x < image_width; ++x) {
                bytes.push_back(static_cast<char>(20 + x * 17 + y * 3));
                bytes.push_back(static_cast<char>(60 + x * 5 + y * 19));
                bytes.push_back(static_cast<char>(100 + x * 11 + y * 7));
            }
        }
        return bytes;
    }

    Kakshya::ImageData make_rgba(uint8_t seed = 0)
    {
        std::vector<uint8_t> pixels;
        pixels.reserve(static_cast<size_t>(image_width) * image_height * 4U);
        for (uint32_t y = 0; y < image_height; ++y) {
            for (uint32_t x = 0; x < image_width; ++x) {
                pixels.push_back(static_cast<uint8_t>(seed + x * 40U));
                pixels.push_back(static_cast<uint8_t>(seed + y * 70U));
                pixels.push_back(static_cast<uint8_t>(seed + 9U));
                pixels.push_back(255);
            }
        }
        Kakshya::ImageData data;
        data.pixels = std::move(pixels);
        data.width = image_width;
        data.height = image_height;
        data.channels = 4;
        data.format = Kakshya::ImageFormat::RGBA8;
        return data;
    }

}

// =========================================================================
// Video loading
// =========================================================================

TEST_F(IOManagerVideoLoadTest, MissingFileGivesNullPairAndRegistersNothing)
{
    const auto [video, audio] = manager->load_video(path_of("missing.mp4"), small_video_config());

    EXPECT_EQ(video, nullptr);
    EXPECT_EQ(audio, nullptr);
    EXPECT_TRUE(manager->get_video_reader_ids().empty());
}

TEST_F(IOManagerVideoLoadTest, SingleArgumentFormGivesNullForAMissingFile)
{
    EXPECT_EQ(manager->load_video(path_of("missing.mp4")), nullptr);
    EXPECT_TRUE(manager->get_video_reader_ids().empty());
}

TEST_F(IOManagerVideoLoadTest, EmptyPathGivesNullPair)
{
    const auto [video, audio] = manager->load_video("", small_video_config());

    EXPECT_EQ(video, nullptr);
    EXPECT_EQ(audio, nullptr);
    EXPECT_TRUE(manager->get_video_reader_ids().empty());
}

TEST_F(IOManagerVideoLoadTest, NonVideoBytesUnderAVideoExtensionGiveNull)
{
    FFmpegFixture::write_bytes(root() / "noise.mp4", FFmpegFixture::noise_bytes(8192));

    const auto [video, audio] = manager->load_video(path_of("noise.mp4"), small_video_config());

    EXPECT_EQ(video, nullptr);
    EXPECT_EQ(audio, nullptr);
    EXPECT_TRUE(manager->get_video_reader_ids().empty());
}

TEST_F(IOManagerVideoLoadTest, EmptyFileGivesNull)
{
    FFmpegFixture::write_bytes(root() / "empty.mp4", "");

    const auto [video, audio] = manager->load_video(path_of("empty.mp4"), small_video_config());

    EXPECT_EQ(video, nullptr);
    EXPECT_TRUE(manager->get_video_reader_ids().empty());
}

TEST_F(IOManagerVideoLoadTest, AnAudioOnlyFileIsNotAVideo)
{
    write_wav("tone.wav");

    const auto [video, audio] = manager->load_video(path_of("tone.wav"), small_video_config());

    EXPECT_EQ(video, nullptr);
    EXPECT_TRUE(manager->get_video_reader_ids().empty());
}

TEST_F(IOManagerVideoLoadTest, ATruncatedClipNeverCrashesAndLeavesNoReaderIfItFails)
{
    ASSERT_TRUE(write_clip("clip.mp4"));
    const auto bytes = [&] {
        std::ifstream input(root() / "clip.mp4", std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    }();
    ASSERT_GT(bytes.size(), 64U);
    FFmpegFixture::write_bytes(root() / "cut.mp4", bytes.substr(0, bytes.size() / 4));

    const auto [video, audio] = manager->load_video(path_of("cut.mp4"), small_video_config());

    if (!video) {
        EXPECT_TRUE(manager->get_video_reader_ids().empty());
    } else {
        EXPECT_EQ(manager->get_video_reader_ids().size(), 1U);
    }
}

TEST_F(IOManagerVideoLoadTest, ValidClipLoadsAndRegistersOneReader)
{
    ASSERT_TRUE(write_clip("clip.mp4"));

    const auto [video, audio] = manager->load_video(path_of("clip.mp4"), small_video_config());

    ASSERT_NE(video, nullptr);
    EXPECT_EQ(audio, nullptr);
    EXPECT_EQ(manager->get_video_reader_ids().size(), 1U);
}

TEST_F(IOManagerVideoLoadTest, TheRegisteredReaderCarriesItsIdAndIsOpen)
{
    ASSERT_TRUE(write_clip("clip.mp4"));
    const auto [video, audio] = manager->load_video(path_of("clip.mp4"), small_video_config());
    ASSERT_NE(video, nullptr);

    const auto ids = manager->get_video_reader_ids();
    ASSERT_EQ(ids.size(), 1U);
    const auto reader = manager->get_video_reader(ids[0]);
    ASSERT_NE(reader, nullptr);
    EXPECT_EQ(reader->get_reader_id(), ids[0]);
    EXPECT_TRUE(reader->is_open());
}

TEST_F(IOManagerVideoLoadTest, ContainerGetsItsFrameProcessor)
{
    ASSERT_TRUE(write_clip("clip.mp4"));

    const auto [video, audio] = manager->load_video(path_of("clip.mp4"), small_video_config());

    ASSERT_NE(video, nullptr);
    EXPECT_NE(video->get_default_processor(), nullptr);
}

TEST_F(IOManagerVideoLoadTest, SingleArgumentFormLoadsTheSameClip)
{
    ASSERT_TRUE(write_clip("clip.mp4", 3));

    const auto video = manager->load_video(path_of("clip.mp4"));

    ASSERT_NE(video, nullptr);
    EXPECT_EQ(manager->get_video_reader_ids().size(), 1U);
}

TEST_F(IOManagerVideoLoadTest, EachLoadGetsItsOwnReaderAndId)
{
    ASSERT_TRUE(write_clip("clip.mp4"));

    const auto first = manager->load_video(path_of("clip.mp4"), small_video_config());
    const auto second = manager->load_video(path_of("clip.mp4"), small_video_config());

    ASSERT_NE(first.first, nullptr);
    ASSERT_NE(second.first, nullptr);
    EXPECT_NE(first.first, second.first);
    const auto ids = manager->get_video_reader_ids();
    EXPECT_EQ(ids.size(), 2U);
    EXPECT_NE(ids[0], ids[1]);
}

TEST_F(IOManagerVideoLoadTest, AFailedLoadBetweenSuccessesLeavesTheIdsAlone)
{
    ASSERT_TRUE(write_clip("clip.mp4"));
    const auto first = manager->load_video(path_of("clip.mp4"), small_video_config());
    ASSERT_NE(first.first, nullptr);

    const auto failed = manager->load_video(path_of("missing.mp4"), small_video_config());

    EXPECT_EQ(failed.first, nullptr);
    EXPECT_EQ(manager->get_video_reader_ids().size(), 1U);
}

TEST_F(IOManagerVideoLoadTest, RequestingAudioFromASilentClipStillGivesTheVideo)
{
    ASSERT_TRUE(write_clip("clip.mp4"));
    auto config = small_video_config();
    config.video_options = IO::VideoReadOptions::EXTRACT_AUDIO;

    const auto [video, audio] = manager->load_video(path_of("clip.mp4"), config);

    ASSERT_NE(video, nullptr);
    EXPECT_EQ(audio, nullptr);
    EXPECT_EQ(manager->get_extracted_audio(video), nullptr);
}

TEST_F(IOManagerVideoLoadTest, ExtractedAudioIsKeyedByItsVideoContainer)
{
    ASSERT_TRUE(write_clip("clip.mp4"));
    const auto loaded = manager->load_video(path_of("clip.mp4"), small_video_config());
    ASSERT_NE(loaded.first, nullptr);

    EXPECT_EQ(manager->get_extracted_audio(loaded.first), nullptr);
    EXPECT_EQ(manager->get_extracted_audio(nullptr), nullptr);
}

TEST_F(IOManagerVideoLoadTest, VideoBufferOfAnUnhookedContainerIsNull)
{
    ASSERT_TRUE(write_clip("clip.mp4"));
    const auto loaded = manager->load_video(path_of("clip.mp4"), small_video_config());
    ASSERT_NE(loaded.first, nullptr);

    EXPECT_EQ(manager->get_video_buffer(loaded.first), nullptr);
}

// =========================================================================
// Video reader lifetime
// =========================================================================

TEST_F(IOManagerVideoLifetimeTest, ReleasingTheReaderLeavesTheContainerUsable)
{
    ASSERT_TRUE(write_clip("clip.mp4"));
    const auto loaded = manager->load_video(path_of("clip.mp4"), small_video_config());
    ASSERT_NE(loaded.first, nullptr);
    const auto id = manager->get_video_reader_ids().at(0);

    manager->release_video_reader(id);

    EXPECT_TRUE(manager->get_video_reader_ids().empty());
    EXPECT_NE(loaded.first->get_default_processor(), nullptr);
}

TEST_F(IOManagerVideoLifetimeTest, DecodeRequestsForALoadedReaderAreAccepted)
{
    ASSERT_TRUE(write_clip("clip.mp4"));
    const auto loaded = manager->load_video(path_of("clip.mp4"), small_video_config());
    ASSERT_NE(loaded.first, nullptr);
    const auto id = manager->get_video_reader_ids().at(0);

    for (int i = 0; i < 10; ++i) {
        EXPECT_NO_THROW(io_service()->request_decode(id));
    }
}

TEST_F(IOManagerVideoLifetimeTest, StopKeepsALoadedReaderAlive)
{
    ASSERT_TRUE(write_clip("clip.mp4"));
    const auto loaded = manager->load_video(path_of("clip.mp4"), small_video_config());
    ASSERT_NE(loaded.first, nullptr);

    manager->stop();

    EXPECT_EQ(manager->get_video_reader_ids().size(), 1U);
}

TEST_F(IOManagerVideoLifetimeTest, ShutdownWithALoadedReaderReturnsAndReleasesIt)
{
    ASSERT_TRUE(write_clip("clip.mp4"));
    const auto loaded = manager->load_video(path_of("clip.mp4"), small_video_config());
    ASSERT_NE(loaded.first, nullptr);

    manager->shutdown();

    EXPECT_TRUE(manager->get_video_reader_ids().empty());
}

TEST_F(IOManagerVideoLifetimeTest, DestructionWithALoadedReaderJoinsItsThread)
{
    ASSERT_TRUE(write_clip("clip.mp4"));
    const auto loaded = manager->load_video(path_of("clip.mp4"), small_video_config());
    ASSERT_NE(loaded.first, nullptr);

    EXPECT_NO_THROW(manager.reset());
}

TEST_F(IOManagerVideoLifetimeTest, ReleasingTheLastReferenceToAReaderWhileItIsDecodingIsSafe)
{
    ASSERT_TRUE(write_clip("clip.mp4", 30));
    const auto loaded = manager->load_video(path_of("clip.mp4"), small_video_config());
    ASSERT_NE(loaded.first, nullptr);
    const auto id = manager->get_video_reader_ids().at(0);

    std::atomic<bool> stop { false };
    {
        std::jthread requester([&] {
            while (!stop.load(std::memory_order_acquire)) {
                io_service()->request_decode(id);
            }
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        manager->release_video_reader(id);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        stop.store(true, std::memory_order_release);
    }

    EXPECT_TRUE(manager->get_video_reader_ids().empty());
}

TEST_F(IOManagerVideoLifetimeTest, ManyLoadAndReleaseCyclesLeaveNothingBehind)
{
    ASSERT_TRUE(write_clip("clip.mp4", 3));

    for (int i = 0; i < 6; ++i) {
        const auto loaded = manager->load_video(path_of("clip.mp4"), small_video_config());
        ASSERT_NE(loaded.first, nullptr) << i;
        for (const auto id : manager->get_video_reader_ids()) {
            manager->release_video_reader(id);
        }
    }

    EXPECT_TRUE(manager->get_video_reader_ids().empty());
}

// =========================================================================
// Video writers
// =========================================================================

TEST_F(IOManagerVideoWriterTest, ValidPathGivesAWriter)
{
    const auto writer = manager->create_writer(
        path_of("out.mp4"), 64, 48, 30.0, AV_PIX_FMT_RGBA, AV_CODEC_ID_MPEG4);

    ASSERT_NE(writer, nullptr);
    writer->close().wait();
}

TEST_F(IOManagerVideoWriterTest, UnwritableDirectoryGivesNull)
{
    const auto writer = manager->create_writer(
        path_of("no_such_dir/out.mp4"), 64, 48, 30.0, AV_PIX_FMT_RGBA, AV_CODEC_ID_MPEG4);

    EXPECT_EQ(writer, nullptr);
}

TEST_F(IOManagerVideoWriterTest, UnknownExtensionGivesNull)
{
    const auto writer = manager->create_writer(
        path_of("out.notaformat"), 64, 48, 30.0, AV_PIX_FMT_RGBA);

    EXPECT_EQ(writer, nullptr);
}

TEST_F(IOManagerVideoWriterTest, NoCaptureIsActiveBeforeAnyIsStarted)
{
    EXPECT_TRUE(manager->get_video_capture_ids().empty());
    EXPECT_TRUE(manager->get_video_writers().empty());
}

// =========================================================================
// Cameras
// =========================================================================

TEST_F(IOManagerCameraTest, OpeningAMissingDeviceGivesNullAndRegistersNothing)
{
    IO::CameraConfig config;
    config.device_name = "/nonexistent/mayaflux_no_such_camera";
    config.target_width = 64;
    config.target_height = 48;

    EXPECT_EQ(manager->open_camera(config), nullptr);
    EXPECT_TRUE(manager->get_camera_reader_ids().empty());
}

TEST_F(IOManagerCameraTest, OpeningAnEmptyDeviceNameNeverCrashes)
{
    IO::CameraConfig config;

    EXPECT_NO_THROW({
        const auto container = manager->open_camera(config);
        if (!container) {
            EXPECT_TRUE(manager->get_camera_reader_ids().empty());
        }
    });
}

TEST_F(IOManagerCameraTest, AHostedSourceIsRetrievableByItsId)
{
    auto source = std::make_shared<CountingCameraSource>();

    const auto id = manager->register_camera_source(source);

    EXPECT_EQ(manager->get_camera_reader(id), source);
    EXPECT_EQ(manager->get_camera_reader_ids(), std::vector<uint64_t> { id });
}

TEST_F(IOManagerCameraTest, CameraBufferOfAnUnhookedContainerIsNull)
{
    EXPECT_EQ(manager->get_camera_buffer(nullptr), nullptr);
}

// =========================================================================
// Images: loading
// =========================================================================

TEST_F(IOManagerImageLoadTest, MissingFileGivesNullAndRetainsNothing)
{
    EXPECT_EQ(manager->load_image(path_of("missing.png")), nullptr);
    EXPECT_TRUE(manager->get_image_readers().empty());
}

TEST_F(IOManagerImageLoadTest, EmptyPathGivesNull)
{
    EXPECT_EQ(manager->load_image(""), nullptr);
    EXPECT_TRUE(manager->get_image_readers().empty());
}

TEST_F(IOManagerImageLoadTest, DirectoryPathGivesNull)
{
    EXPECT_EQ(manager->load_image(root().string()), nullptr);
    EXPECT_TRUE(manager->get_image_readers().empty());
}

TEST_F(IOManagerImageLoadTest, NonImageBytesGiveNull)
{
    FFmpegFixture::write_bytes(root() / "noise.png", FFmpegFixture::noise_bytes(2048));

    EXPECT_EQ(manager->load_image(path_of("noise.png")), nullptr);
    EXPECT_TRUE(manager->get_image_readers().empty());
}

TEST_F(IOManagerImageLoadTest, EmptyFileGivesNull)
{
    FFmpegFixture::write_bytes(root() / "empty.png", "");

    EXPECT_EQ(manager->load_image(path_of("empty.png")), nullptr);
    EXPECT_TRUE(manager->get_image_readers().empty());
}

TEST_F(IOManagerImageLoadTest, ValidImageLoadsAtItsOwnSizeAndRetainsItsReader)
{
    FFmpegFixture::write_bytes(root() / "image.pnm", make_ppm());

    const auto texture = manager->load_image(path_of("image.pnm"));

    ASSERT_NE(texture, nullptr);
    EXPECT_EQ(texture->get_width(), image_width);
    EXPECT_EQ(texture->get_height(), image_height);
    EXPECT_EQ(manager->get_image_readers().size(), 1U);
}

TEST_F(IOManagerImageLoadTest, EveryChannelRequestLoadsTheSameExtent)
{
    FFmpegFixture::write_bytes(root() / "image.pnm", make_ppm());

    for (const int channels : { 0, 1, 4 }) {
        const auto texture = manager->load_image(path_of("image.pnm"), channels);
        ASSERT_NE(texture, nullptr) << channels;
        EXPECT_EQ(texture->get_width(), image_width) << channels;
        EXPECT_EQ(texture->get_height(), image_height) << channels;
    }
    EXPECT_EQ(manager->get_image_readers().size(), 3U);
}

TEST_F(IOManagerImageLoadTest, AnExplicitThreeChannelRequestIsRefusedAndRetainsNothing)
{
    FFmpegFixture::write_bytes(root() / "image.pnm", make_ppm());

    EXPECT_EQ(manager->load_image(path_of("image.pnm"), 3), nullptr);
    EXPECT_TRUE(manager->get_image_readers().empty());
}

TEST_F(IOManagerImageLoadTest, AThreeChannelSourceIsPromotedWhenTheLayoutIsLeftToTheReader)
{
    FFmpegFixture::write_bytes(root() / "image.pnm", make_ppm());

    const auto texture = manager->load_image(path_of("image.pnm"), 0);

    ASSERT_NE(texture, nullptr);
    EXPECT_EQ(texture->get_width(), image_width);
    EXPECT_EQ(texture->get_height(), image_height);
}

TEST_F(IOManagerImageLoadTest, ARegionCropsToItsExtent)
{
    FFmpegFixture::write_bytes(root() / "image.pnm", make_ppm());

    const auto texture = manager->load_image(path_of("image.pnm"), 4, Kakshya::Region::image_rect(1, 1, 2, 2));

    ASSERT_NE(texture, nullptr);
    EXPECT_EQ(texture->get_width(), 2U);
    EXPECT_EQ(texture->get_height(), 2U);
}

TEST_F(IOManagerImageLoadTest, ASinglePixelRegionLoadsOnePixel)
{
    FFmpegFixture::write_bytes(root() / "image.pnm", make_ppm());

    const auto texture = manager->load_image(path_of("image.pnm"), 4, Kakshya::Region::image_rect(3, 2, 3, 2));

    ASSERT_NE(texture, nullptr);
    EXPECT_EQ(texture->get_width(), 1U);
    EXPECT_EQ(texture->get_height(), 1U);
}

TEST_F(IOManagerImageLoadTest, AFailedLoadBetweenSuccessesRetainsNothingExtra)
{
    FFmpegFixture::write_bytes(root() / "image.pnm", make_ppm());
    ASSERT_NE(manager->load_image(path_of("image.pnm")), nullptr);

    EXPECT_EQ(manager->load_image(path_of("missing.png")), nullptr);

    EXPECT_EQ(manager->get_image_readers().size(), 1U);
}

TEST_F(IOManagerImageLoadTest, ShutdownReleasesRetainedImageReaders)
{
    FFmpegFixture::write_bytes(root() / "image.pnm", make_ppm());
    ASSERT_NE(manager->load_image(path_of("image.pnm")), nullptr);

    manager->shutdown();

    EXPECT_TRUE(manager->get_image_readers().empty());
}

// =========================================================================
// Images: saving
// =========================================================================

TEST_F(IOManagerImageSaveTest, SavedPngReadsBackPixelForPixel)
{
    const auto original = make_rgba();
    ASSERT_TRUE(manager->save_image(original, path_of("saved.png")));
    manager->wait_for_pending_saves();

    ASSERT_TRUE(exists("saved.png"));
    const auto back = IO::ImageReader::load(path_of("saved.png"), 4);
    ASSERT_TRUE(back.has_value());
    EXPECT_EQ(back->width, image_width);
    EXPECT_EQ(back->height, image_height);
    ASSERT_NE(back->as_uint8(), nullptr);
    EXPECT_EQ(*back->as_uint8(), *original.as_uint8());
}

TEST_F(IOManagerImageSaveTest, SavedImageLoadsThroughTheManager)
{
    ASSERT_TRUE(manager->save_image(make_rgba(), path_of("saved.png")));
    manager->wait_for_pending_saves();

    const auto texture = manager->load_image(path_of("saved.png"));

    ASSERT_NE(texture, nullptr);
    EXPECT_EQ(texture->get_width(), image_width);
    EXPECT_EQ(texture->get_height(), image_height);
}

TEST_F(IOManagerImageSaveTest, SaveReturnsBeforeTheWriteAndTheFileAppearsOnWait)
{
    ASSERT_TRUE(manager->save_image(make_rgba(), path_of("later.png")));
    manager->wait_for_pending_saves();

    EXPECT_TRUE(exists("later.png"));
}

TEST_F(IOManagerImageSaveTest, AnUnknownExtensionReportsQueuedButWritesNothing)
{
    EXPECT_TRUE(manager->save_image(make_rgba(), path_of("saved.notaformat")));
    EXPECT_NO_THROW(manager->wait_for_pending_saves());

    EXPECT_FALSE(exists("saved.notaformat"));
}

TEST_F(IOManagerImageSaveTest, AMissingDirectoryReportsQueuedButWritesNothing)
{
    EXPECT_TRUE(manager->save_image(make_rgba(), path_of("no_such_dir/saved.png")));
    EXPECT_NO_THROW(manager->wait_for_pending_saves());

    EXPECT_FALSE(std::filesystem::exists(root() / "no_such_dir"));
}

TEST_F(IOManagerImageSaveTest, AnEmptyImageDoesNotThrowAndDoesNotPoisonLaterSaves)
{
    Kakshya::ImageData empty;
    empty.format = Kakshya::ImageFormat::RGBA8;
    empty.pixels = std::vector<uint8_t> {};

    EXPECT_NO_THROW({
        static_cast<void>(manager->save_image(std::move(empty), path_of("empty.png")));
        manager->wait_for_pending_saves();
    });

    ASSERT_TRUE(manager->save_image(make_rgba(), path_of("after.png")));
    manager->wait_for_pending_saves();
    EXPECT_TRUE(exists("after.png"));
}

TEST_F(IOManagerImageSaveTest, MismatchedSizeAndPixelCountDoesNotThrowAndDoesNotPoisonLaterSaves)
{
    auto bad = make_rgba();
    bad.width = 100;
    bad.height = 100;

    EXPECT_NO_THROW({
        static_cast<void>(manager->save_image(std::move(bad), path_of("bad.png")));
        manager->wait_for_pending_saves();
    });

    ASSERT_TRUE(manager->save_image(make_rgba(), path_of("after.png")));
    manager->wait_for_pending_saves();
    EXPECT_TRUE(exists("after.png"));
}

TEST_F(IOManagerImageSaveTest, TheSameBytesSavedToTwoFormatsBothLand)
{
    ASSERT_TRUE(manager->save_image(make_rgba(), path_of("one.png")));
    ASSERT_TRUE(manager->save_image(make_rgba(), path_of("two.bmp")));
    manager->wait_for_pending_saves();

    EXPECT_GT(size_of("one.png"), 0U);
    EXPECT_GT(size_of("two.bmp"), 0U);
}

TEST_F(IOManagerImageSaveTest, ASaveQueuedBeforeShutdownCompletesBeforeItReturns)
{
    ASSERT_TRUE(manager->save_image(make_rgba(), path_of("queued.png")));

    manager->shutdown();

    EXPECT_TRUE(exists("queued.png"));
    EXPECT_GT(size_of("queued.png"), 0U);
}

TEST_F(IOManagerImageSaveTest, ASaveQueuedBeforeDestructionCompletesBeforeItReturns)
{
    ASSERT_TRUE(manager->save_image(make_rgba(), path_of("queued.png")));

    manager.reset();

    EXPECT_TRUE(exists("queued.png"));
    EXPECT_GT(size_of("queued.png"), 0U);
}

}
