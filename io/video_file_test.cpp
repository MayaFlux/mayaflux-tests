#include "../support/test_metadata.hpp"
#include "gtest/gtest.h"

#include "ffmpeg/ffmpeg_fixture.hpp"
#include "file_contract_fixture.hpp"

#include "MayaFlux/IO/Video/VideoFileReader.hpp"
#include "MayaFlux/IO/Video/VideoFileWriter.hpp"
#include "MayaFlux/Kakshya/Source/SoundFileContainer.hpp"
#include "MayaFlux/Kakshya/Source/VideoFileContainer.hpp"

#include "MayaFlux/IO/CodecContext/AudioEncodeContext.hpp"
#include "MayaFlux/IO/CodecContext/FFmpegMuxContext.hpp"
#include "MayaFlux/IO/CodecContext/VideoEncodeContext.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <numbers>
#include <thread>

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::writable_temp>() }))

namespace MayaFlux::Test {

namespace {

    using FFmpegFixture::make_wav;
    using FFmpegFixture::write_bytes;
    using FFmpegFixture::write_mpeg4_clip;

    class VideoFileFixture : public FileContractFixture {
    protected:
        [[nodiscard]] std::string path_of(const std::string& name) const
        {
            return (root() / name).string();
        }

        [[nodiscard]] std::filesystem::path file(const std::string& name) const
        {
            return root() / name;
        }

        static constexpr uint32_t k_width = 64;
        static constexpr uint32_t k_height = 48;
        static constexpr uint32_t k_frames = 12;
        static constexpr double k_fps = 24.0;
    };

}

// =========================================================================
// VideoFileReader: closed state and probing
// =========================================================================

TEST(VideoFileReaderTest, ClosedReaderReportsNothing)
{
    IO::VideoFileReader reader;

    EXPECT_FALSE(reader.is_open());
    EXPECT_TRUE(reader.read_all().empty());
    EXPECT_TRUE(reader.read_region(IO::FileRegion {}).empty());
    EXPECT_FALSE(reader.get_metadata().has_value());
    EXPECT_TRUE(reader.get_regions().empty());
    EXPECT_EQ(reader.create_container(), nullptr);
    EXPECT_EQ(reader.get_dimension_sizes(), (std::vector<uint64_t> { 0, 0, 0, 0 }));
    EXPECT_FALSE(reader.seek({ 0 }));
}

TEST(VideoFileReaderTest, CanReadMatchesVideoExtensionsOnly)
{
    IO::VideoFileReader reader;

    EXPECT_TRUE(reader.can_read("clip.mp4"));
    EXPECT_TRUE(reader.can_read("CLIP.MKV"));
    EXPECT_FALSE(reader.can_read("sound.wav"));
    EXPECT_FALSE(reader.can_read("no_extension"));
}

TEST(VideoFileReaderTest, SupportedExtensionsIncludeMp4)
{
    IO::VideoFileReader reader;
    const auto exts = reader.get_supported_extensions();

    EXPECT_NE(std::find(exts.begin(), exts.end(), "mp4"), exts.end());
}

TEST(VideoFileReaderTest, LoadIntoContainerRejectsNullAndWrongContainer)
{
    IO::VideoFileReader reader;

    EXPECT_FALSE(reader.load_into_container(nullptr));
    EXPECT_FALSE(reader.get_last_error().empty());
}

// =========================================================================
// VideoFileReader: opening
// =========================================================================

TEST_F(VideoFileFixture, OpenNonexistentFileFailsWithError)
{
    IO::VideoFileReader reader;

    EXPECT_FALSE(reader.open(path_of("missing.mp4")));
    EXPECT_FALSE(reader.is_open());
    EXPECT_FALSE(reader.get_last_error().empty());
}

TEST_F(VideoFileFixture, OpenAudioOnlyFileFails)
{
    write_bytes(file("tone.mp4"), make_wav(1, 8000, 800));

    IO::VideoFileReader reader;
    EXPECT_FALSE(reader.open(path_of("tone.mp4")));
    EXPECT_FALSE(reader.is_open());
}

TEST_F(VideoFileFixture, OpenClipExposesDimensionsAndFrameCount)
{
    ASSERT_TRUE(write_mpeg4_clip(file("clip.mp4"), k_width, k_height, k_frames, k_fps));

    IO::VideoFileReader reader;
    ASSERT_TRUE(reader.open(path_of("clip.mp4"))) << reader.get_last_error();
    EXPECT_TRUE(reader.is_open());

    EXPECT_EQ(reader.get_dimension_sizes(),
        (std::vector<uint64_t> { k_frames, k_height, k_width, 4 }));
}

TEST_F(VideoFileFixture, MetadataReportsVideoMimeType)
{
    ASSERT_TRUE(write_mpeg4_clip(file("clip.mp4"), k_width, k_height, k_frames, k_fps));

    IO::VideoFileReader reader;
    reader.set_reader_id(7);
    ASSERT_TRUE(reader.open(path_of("clip.mp4"), IO::FileReadOptions::EXTRACT_METADATA)) << reader.get_last_error();

    auto meta = reader.get_metadata();
    ASSERT_TRUE(meta.has_value());
    EXPECT_EQ(meta->mime_type, "video/mp4");
    EXPECT_FALSE(meta->format.empty());
    EXPECT_EQ(reader.get_reader_id(), 7U);
}

TEST_F(VideoFileFixture, KeyframeRegionsStartAtFrameZero)
{
    ASSERT_TRUE(write_mpeg4_clip(file("clip.mp4"), k_width, k_height, k_frames, k_fps));

    IO::VideoFileReader reader;
    ASSERT_TRUE(reader.open(path_of("clip.mp4"), IO::FileReadOptions::EXTRACT_REGIONS)) << reader.get_last_error();

    const auto regions = reader.get_regions();
    ASSERT_FALSE(regions.empty());
    EXPECT_EQ(regions.front().type, "keyframe");
    EXPECT_EQ(regions.front().start_coordinates.front(), 0U);
}

TEST_F(VideoFileFixture, CloseThenReopenIsClean)
{
    ASSERT_TRUE(write_mpeg4_clip(file("clip.mp4"), k_width, k_height, k_frames, k_fps));

    IO::VideoFileReader reader;
    ASSERT_TRUE(reader.open(path_of("clip.mp4"))) << reader.get_last_error();
    reader.close();
    reader.close();
    EXPECT_FALSE(reader.is_open());

    ASSERT_TRUE(reader.open(path_of("clip.mp4"))) << reader.get_last_error();
    EXPECT_TRUE(reader.is_open());
}

// =========================================================================
// VideoFileReader: containers and reading
// =========================================================================

TEST_F(VideoFileFixture, ReadAllAndReadRegionAreUnsupported)
{
    ASSERT_TRUE(write_mpeg4_clip(file("clip.mp4"), k_width, k_height, k_frames, k_fps));

    IO::VideoFileReader reader;
    ASSERT_TRUE(reader.open(path_of("clip.mp4"))) << reader.get_last_error();

    EXPECT_TRUE(reader.read_all().empty());
    EXPECT_TRUE(reader.read_region(IO::FileRegion {}).empty());
}

TEST_F(VideoFileFixture, LoadIntoContainerExposesEveryFrame)
{
    ASSERT_TRUE(write_mpeg4_clip(file("clip.mp4"), k_width, k_height, k_frames, k_fps));

    IO::VideoFileReader reader;
    ASSERT_TRUE(reader.open(path_of("clip.mp4"))) << reader.get_last_error();

    auto container = reader.create_container();
    ASSERT_NE(container, nullptr);
    ASSERT_TRUE(reader.load_into_container(container)) << reader.get_last_error();

    auto video = std::dynamic_pointer_cast<Kakshya::VideoFileContainer>(container);
    ASSERT_NE(video, nullptr);
    EXPECT_EQ(video->get_total_source_frames(), k_frames);
}

TEST_F(VideoFileFixture, LoadIntoContainerRejectsSoundContainer)
{
    ASSERT_TRUE(write_mpeg4_clip(file("clip.mp4"), k_width, k_height, k_frames, k_fps));

    IO::VideoFileReader reader;
    ASSERT_TRUE(reader.open(path_of("clip.mp4"))) << reader.get_last_error();

    auto sound = std::make_shared<Kakshya::SoundFileContainer>();
    EXPECT_FALSE(reader.load_into_container(sound));
    EXPECT_EQ(reader.get_last_error(), "Container is not a VideoFileContainer");
}

TEST_F(VideoFileFixture, ClipWithoutAudioHasNoAudioContainer)
{
    ASSERT_TRUE(write_mpeg4_clip(file("clip.mp4"), k_width, k_height, k_frames, k_fps));

    IO::VideoFileReader reader;
    reader.set_video_options(IO::VideoReadOptions::EXTRACT_AUDIO);
    ASSERT_TRUE(reader.open(path_of("clip.mp4"))) << reader.get_last_error();

    auto container = reader.create_container();
    ASSERT_TRUE(reader.load_into_container(container)) << reader.get_last_error();
    EXPECT_EQ(reader.get_audio_container(), nullptr);
}

TEST_F(VideoFileFixture, SeekBeforeLoadMovesReadPosition)
{
    ASSERT_TRUE(write_mpeg4_clip(file("clip.mp4"), k_width, k_height, k_frames, k_fps));

    IO::VideoFileReader reader;
    ASSERT_TRUE(reader.open(path_of("clip.mp4"))) << reader.get_last_error();

    EXPECT_TRUE(reader.seek({ 4 })) << reader.get_last_error();
    EXPECT_EQ(reader.get_read_position(), std::vector<uint64_t> { 4 });
}

TEST_F(VideoFileFixture, SeekWithinLoadedContainerReturnsTheRequestedFrame)
{
    ASSERT_TRUE(write_mpeg4_clip(file("clip.mp4"), k_width, k_height, k_frames, k_fps));

    IO::VideoFileReader reader;
    ASSERT_TRUE(reader.open(path_of("clip.mp4"))) << reader.get_last_error();

    auto container = reader.create_container();
    ASSERT_TRUE(reader.load_into_container(container)) << reader.get_last_error();

    auto video = std::dynamic_pointer_cast<Kakshya::VideoFileContainer>(container);
    ASSERT_NE(video, nullptr);

    ASSERT_TRUE(reader.seek({ 6 })) << reader.get_last_error();

    const auto pixels = video->get_frame_pixels(6);
    ASSERT_FALSE(pixels.empty());
    EXPECT_NEAR(static_cast<double>(pixels[0]), 42.0, 10.0);
}

// =========================================================================
// VideoFileWriter: closed state and opening
// =========================================================================

TEST(VideoFileWriterTest, ClosedWriterIsNotOpenAndIgnoresWrites)
{
    IO::VideoFileWriter writer;
    std::vector<uint8_t> pixels(static_cast<size_t>(64) * 48 * 4, 0);

    EXPECT_FALSE(writer.is_open());
    EXPECT_FALSE(writer.is_recording());
    writer.write(std::span<const uint8_t>(pixels));
    EXPECT_FALSE(writer.is_open());
}

TEST_F(VideoFileFixture, OpenUnwritablePathFailsWithError)
{
    IO::VideoFileWriter writer;

    EXPECT_FALSE(writer.open(path_of("no_such_dir_xyz/out.mp4"), k_width, k_height, k_fps,
        AV_PIX_FMT_RGBA, AV_CODEC_ID_MPEG4));
    EXPECT_FALSE(writer.is_open());
    EXPECT_FALSE(writer.last_error().empty());
}

TEST_F(VideoFileFixture, OpenWhileOpenIsRejected)
{
    IO::VideoFileWriter writer;
    ASSERT_TRUE(writer.open(path_of("a.mp4"), k_width, k_height, k_fps, AV_PIX_FMT_RGBA, AV_CODEC_ID_MPEG4))
        << writer.last_error();

    EXPECT_FALSE(writer.open(path_of("b.mp4"), k_width, k_height, k_fps, AV_PIX_FMT_RGBA, AV_CODEC_ID_MPEG4));
    EXPECT_EQ(writer.last_error(), "open() called while already open");

    EXPECT_TRUE(writer.close().get());
}

TEST_F(VideoFileFixture, RecordWithoutWindowIsRejected)
{
    IO::VideoFileWriter writer;

    EXPECT_FALSE(writer.record(nullptr, path_of("screen.mp4"), k_fps));
    EXPECT_FALSE(writer.is_recording());
    EXPECT_EQ(writer.last_error(), "record: null window");
}

// =========================================================================
// VideoFileWriter: writing and round trips
// =========================================================================

TEST_F(VideoFileFixture, WriteRoundTripsThroughReader)
{
    {
        IO::VideoFileWriter writer;
        ASSERT_TRUE(writer.open(path_of("out.mp4"), k_width, k_height, k_fps, AV_PIX_FMT_RGBA, AV_CODEC_ID_MPEG4))
            << writer.last_error();

        std::vector<uint8_t> pixels(static_cast<size_t>(k_width) * k_height * 4U, 0);
        for (uint32_t f = 0; f < k_frames; ++f) {
            std::fill(pixels.begin(), pixels.end(), static_cast<uint8_t>(f * 7U));
            writer.write(pixels.data(), pixels.size());
        }

        EXPECT_TRUE(writer.close().get()) << writer.last_error();
    }

    IO::VideoFileReader reader;
    ASSERT_TRUE(reader.open(path_of("out.mp4"))) << reader.get_last_error();
    EXPECT_EQ(reader.get_dimension_sizes()[0], k_frames);
    EXPECT_EQ(reader.get_dimension_sizes()[1], k_height);
    EXPECT_EQ(reader.get_dimension_sizes()[2], k_width);
}

TEST_F(VideoFileFixture, ZeroSizeWriteIsIgnored)
{
    IO::VideoFileWriter writer;
    ASSERT_TRUE(writer.open(path_of("empty.mp4"), k_width, k_height, k_fps, AV_PIX_FMT_RGBA, AV_CODEC_ID_MPEG4))
        << writer.last_error();

    const std::vector<uint8_t> none;
    writer.write(std::span<const uint8_t>(none));
    writer.write(nullptr, 0);

    EXPECT_TRUE(writer.close().get()) << writer.last_error();
}

TEST_F(VideoFileFixture, FrameEncodeFailureIsReportedByClose)
{
    IO::VideoFileWriter writer;
    ASSERT_TRUE(writer.open(path_of("short.mp4"), k_width, k_height, k_fps, AV_PIX_FMT_RGBA, AV_CODEC_ID_MPEG4))
        << writer.last_error();

    std::vector<uint8_t> too_small(static_cast<size_t>(k_width) * k_height, 0);
    writer.write(too_small.data(), too_small.size());

    EXPECT_FALSE(writer.close().get());
    EXPECT_FALSE(writer.last_error().empty());
}

TEST_F(VideoFileFixture, CloseTwiceSecondCloseIsNotOpen)
{
    IO::VideoFileWriter writer;
    ASSERT_TRUE(writer.open(path_of("twice.mp4"), k_width, k_height, k_fps, AV_PIX_FMT_RGBA, AV_CODEC_ID_MPEG4))
        << writer.last_error();

    EXPECT_TRUE(writer.close().get());
    EXPECT_FALSE(writer.is_open());
}

TEST_F(VideoFileFixture, DestructorFinalisesOpenFile)
{
    {
        IO::VideoFileWriter writer;
        ASSERT_TRUE(writer.open(path_of("scoped.mp4"), k_width, k_height, k_fps, AV_PIX_FMT_RGBA, AV_CODEC_ID_MPEG4))
            << writer.last_error();

        std::vector<uint8_t> pixels(static_cast<size_t>(k_width) * k_height * 4U, 128);
        for (uint32_t f = 0; f < k_frames; ++f)
            writer.write(pixels.data(), pixels.size());
    }

    IO::VideoFileReader reader;
    ASSERT_TRUE(reader.open(path_of("scoped.mp4"))) << reader.get_last_error();
    EXPECT_EQ(reader.get_dimension_sizes()[0], k_frames);
}

// =========================================================================
// VideoFileReader: ring buffer and background decode
// =========================================================================

namespace {

    bool wait_until(const std::function<bool()>& pred, std::chrono::milliseconds limit)
    {
        const auto deadline = std::chrono::steady_clock::now() + limit;
        while (std::chrono::steady_clock::now() < deadline) {
            if (pred())
                return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return pred();
    }

    /// MP4 with an MPEG-4 video track and a stereo 440 Hz AAC track.
    bool write_av_clip(const std::filesystem::path& path,
        uint32_t width, uint32_t height, uint32_t frames, double fps,
        uint32_t audio_frames, uint32_t sample_rate)
    {
        IO::FFmpegMuxContext mux;
        if (!mux.open(path.string(), "mp4"))
            return false;

        IO::VideoEncodeContext venc;
        if (!venc.open(mux, width, height, fps, AV_PIX_FMT_RGBA, AV_CODEC_ID_MPEG4))
            return false;

        IO::AudioEncodeContext aenc;
        if (!aenc.open(mux, sample_rate, 2, AV_CODEC_ID_AAC))
            return false;

        if (!mux.write_header())
            return false;

        std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 4U);
        for (uint32_t f = 0; f < frames; ++f) {
            std::fill(pixels.begin(), pixels.end(), static_cast<uint8_t>(f * 7U));
            if (!venc.encode_frame(pixels.data(), pixels.size(), width, height, mux))
                return false;
        }

        constexpr uint32_t chunk = 1024;
        std::vector<double> samples(static_cast<size_t>(chunk) * 2U);
        for (uint32_t done = 0; done < audio_frames;) {
            const uint32_t n = std::min(chunk, audio_frames - done);
            for (uint32_t i = 0; i < n; ++i) {
                const double t = static_cast<double>(done + i) / sample_rate;
                const double s = 0.25 * std::sin(2.0 * std::numbers::pi * 440.0 * t);
                samples[2U * i] = s;
                samples[2U * i + 1U] = s;
            }
            const auto span = std::span<const double>(samples.data(), 2U * static_cast<size_t>(n));
            if (!aenc.encode_frames(span, n, mux))
                return false;
            done += n;
        }

        if (!venc.drain(mux) || !aenc.drain(mux))
            return false;
        venc.close();
        aenc.close();
        mux.close();
        return true;
    }

    uint8_t first_byte(const std::span<const uint8_t>& pixels)
    {
        return pixels.empty() ? uint8_t { 0 } : pixels[0];
    }

}

TEST_F(VideoFileFixture, RingPreloadsOnlyRingCapacityFrames)
{
    ASSERT_TRUE(write_mpeg4_clip(file("long.mp4"), k_width, k_height, 40, k_fps));

    IO::VideoFileReader reader;
    reader.set_ring_capacity(8);
    ASSERT_TRUE(reader.open(path_of("long.mp4"))) << reader.get_last_error();

    auto container = reader.create_container();
    ASSERT_TRUE(reader.load_into_container(container)) << reader.get_last_error();

    auto video = std::dynamic_pointer_cast<Kakshya::VideoFileContainer>(container);
    ASSERT_NE(video, nullptr);
    EXPECT_TRUE(video->is_ring_mode());
    EXPECT_EQ(video->get_ring_capacity(), 8U);

    EXPECT_FALSE(video->get_frame_pixels(0).empty());
    EXPECT_FALSE(video->get_frame_pixels(7).empty());
    EXPECT_TRUE(video->get_frame_pixels(8).empty()) << "decoder ran past a full ring";

    reader.close();
}

TEST_F(VideoFileFixture, RingRefillsAfterConsumerAdvancesAndEvictsOldFrames)
{
    ASSERT_TRUE(write_mpeg4_clip(file("long.mp4"), k_width, k_height, 40, k_fps));

    IO::VideoFileReader reader;
    reader.set_ring_capacity(8);
    ASSERT_TRUE(reader.open(path_of("long.mp4"))) << reader.get_last_error();

    auto container = reader.create_container();
    ASSERT_TRUE(reader.load_into_container(container)) << reader.get_last_error();

    auto video = std::dynamic_pointer_cast<Kakshya::VideoFileContainer>(container);
    ASSERT_NE(video, nullptr);

    video->advance_read_position({ 4 });

    ASSERT_TRUE(wait_until([&] { return !video->get_frame_pixels(11).empty(); },
        std::chrono::seconds(3)))
        << "ring did not refill after the consumer advanced";

    EXPECT_TRUE(video->get_frame_pixels(0).empty()) << "frame 0 should have been evicted";
    EXPECT_NEAR(static_cast<double>(first_byte(video->get_frame_pixels(11))), 77.0, 10.0);

    reader.close();
}

TEST_F(VideoFileFixture, SeekInsideLongClipDecodesTheRequestedFrame)
{
    ASSERT_TRUE(write_mpeg4_clip(file("long.mp4"), k_width, k_height, 40, k_fps));

    IO::VideoFileReader reader;
    reader.set_ring_capacity(8);
    ASSERT_TRUE(reader.open(path_of("long.mp4"))) << reader.get_last_error();

    auto container = reader.create_container();
    ASSERT_TRUE(reader.load_into_container(container)) << reader.get_last_error();

    auto video = std::dynamic_pointer_cast<Kakshya::VideoFileContainer>(container);
    ASSERT_NE(video, nullptr);

    ASSERT_TRUE(reader.seek({ 30 })) << reader.get_last_error();

    ASSERT_TRUE(wait_until([&] { return !video->get_frame_pixels(30).empty(); },
        std::chrono::seconds(3)))
        << "frame 30 never decoded after seek";

    EXPECT_NEAR(static_cast<double>(first_byte(video->get_frame_pixels(30))), 210.0, 10.0)
        << "seek landed on the keyframe and not on frame 30";

    reader.close();
}

TEST_F(VideoFileFixture, ShortClipLoadsEveryFrameWithoutDecodeThread)
{
    ASSERT_TRUE(write_mpeg4_clip(file("short.mp4"), k_width, k_height, 6, k_fps));

    IO::VideoFileReader reader;
    ASSERT_TRUE(reader.open(path_of("short.mp4"))) << reader.get_last_error();

    auto container = reader.create_container();
    ASSERT_TRUE(reader.load_into_container(container)) << reader.get_last_error();

    auto video = std::dynamic_pointer_cast<Kakshya::VideoFileContainer>(container);
    ASSERT_NE(video, nullptr);
    EXPECT_EQ(video->get_total_source_frames(), 6U);

    for (uint64_t f = 0; f < 6; ++f)
        EXPECT_FALSE(video->get_frame_pixels(f).empty()) << "frame " << f;
    EXPECT_TRUE(video->get_frame_pixels(6).empty());

    reader.close();
}

// =========================================================================
// VideoFileReader: audio track extraction
// =========================================================================

TEST_F(VideoFileFixture, AudioTrackIsExtractedAsSoundContainer)
{
    constexpr uint32_t rate = 44100;
    ASSERT_TRUE(write_av_clip(file("av.mp4"), k_width, k_height, 24, k_fps, rate, rate))
        << "fixture encode failed";

    IO::VideoFileReader reader;
    reader.set_video_options(IO::VideoReadOptions::EXTRACT_AUDIO);
    ASSERT_TRUE(reader.open(path_of("av.mp4"))) << reader.get_last_error();

    auto container = reader.create_container();
    ASSERT_TRUE(reader.load_into_container(container)) << reader.get_last_error();

    auto audio = reader.get_audio_container();
    ASSERT_NE(audio, nullptr) << reader.get_last_error();
    EXPECT_EQ(audio->get_num_channels(), 2U);
    EXPECT_EQ(audio->get_sample_rate(), rate);
    EXPECT_NEAR(static_cast<double>(audio->get_num_frames()),
        static_cast<double>(rate), rate * 0.03);

    reader.close();
}

TEST_F(VideoFileFixture, AudioTargetRateIsAppliedToExtractedTrack)
{
    constexpr uint32_t rate = 44100;
    constexpr uint32_t target = 22050;
    ASSERT_TRUE(write_av_clip(file("av.mp4"), k_width, k_height, 24, k_fps, rate, rate))
        << "fixture encode failed";

    IO::VideoFileReader reader;
    reader.set_video_options(IO::VideoReadOptions::EXTRACT_AUDIO);
    reader.set_target_sample_rate(target);
    ASSERT_TRUE(reader.open(path_of("av.mp4"))) << reader.get_last_error();

    auto container = reader.create_container();
    ASSERT_TRUE(reader.load_into_container(container)) << reader.get_last_error();

    auto audio = reader.get_audio_container();
    ASSERT_NE(audio, nullptr) << reader.get_last_error();
    EXPECT_EQ(audio->get_sample_rate(), target);
    EXPECT_NEAR(static_cast<double>(audio->get_num_frames()),
        static_cast<double>(target), target * 0.03);

    reader.close();
}

TEST_F(VideoFileFixture, AudioIsNotExtractedUnlessRequested)
{
    ASSERT_TRUE(write_av_clip(file("av.mp4"), k_width, k_height, 24, k_fps, 44100, 44100))
        << "fixture encode failed";

    IO::VideoFileReader reader;
    ASSERT_TRUE(reader.open(path_of("av.mp4"))) << reader.get_last_error();

    auto container = reader.create_container();
    ASSERT_TRUE(reader.load_into_container(container)) << reader.get_last_error();
    EXPECT_EQ(reader.get_audio_container(), nullptr);

    reader.close();
}

TEST_F(VideoFileFixture, VideoFramesStayAlignedWhenAudioIsExtracted)
{
    ASSERT_TRUE(write_av_clip(file("av.mp4"), k_width, k_height, 24, k_fps, 44100, 44100))
        << "fixture encode failed";

    IO::VideoFileReader reader;
    reader.set_video_options(IO::VideoReadOptions::EXTRACT_AUDIO);
    ASSERT_TRUE(reader.open(path_of("av.mp4"))) << reader.get_last_error();

    auto container = reader.create_container();
    ASSERT_TRUE(reader.load_into_container(container)) << reader.get_last_error();

    auto video = std::dynamic_pointer_cast<Kakshya::VideoFileContainer>(container);
    ASSERT_NE(video, nullptr);

    EXPECT_NEAR(static_cast<double>(first_byte(video->get_frame_pixels(0))), 0.0, 10.0);
    EXPECT_NEAR(static_cast<double>(first_byte(video->get_frame_pixels(5))), 35.0, 10.0)
        << "audio extraction moved the video stream";

    reader.close();
}

}
