#include "gtest/gtest.h"
#include "../../support/test_metadata.hpp"

#include "../file_contract_fixture.hpp"
#include "ffmpeg_fixture.hpp"

#include "MayaFlux/IO/CodecContext/FFmpegDemuxContext.hpp"

extern "C" {
#include <libavformat/avformat.h>
}

#include <atomic>
#include <limits>
#include <thread>

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::writable_temp>() }))

namespace MayaFlux::Test {

using FFmpegFixture::make_wav;
using FFmpegFixture::noise_bytes;

namespace {

    class FFmpegDemuxFileFixture : public FileContractFixture {
    protected:
        [[nodiscard]] std::string path_of(const std::string& name) const
        {
            return (root() / name).string();
        }

        void write_binary(const std::string& name, const std::string& bytes) const
        {
            std::ofstream f(root() / name, std::ios::binary);
            f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        }
    };

}

// =========================================================================
// Closed context: no file, no state
// =========================================================================

TEST(FFmpegDemuxTest, DefaultConstructedContextIsClosed)
{
    IO::FFmpegDemuxContext demux;

    EXPECT_FALSE(demux.is_open());
    EXPECT_EQ(demux.format_context, nullptr);
    EXPECT_EQ(demux.stream_count(), 0U);
    EXPECT_TRUE(demux.last_error().empty());
}

TEST(FFmpegDemuxTest, CloseWithoutOpenIsHarmlessAndRepeatable)
{
    IO::FFmpegDemuxContext demux;

    demux.close();
    demux.close();

    EXPECT_FALSE(demux.is_open());
    EXPECT_TRUE(demux.last_error().empty());
}

TEST(FFmpegDemuxTest, StreamQueriesOnClosedContextReturnEmptyResults)
{
    IO::FFmpegDemuxContext demux;

    EXPECT_EQ(demux.find_best_stream(AVMEDIA_TYPE_AUDIO), -1);
    EXPECT_EQ(demux.find_best_stream(AVMEDIA_TYPE_VIDEO), -1);
    EXPECT_EQ(demux.get_stream(0), nullptr);
    EXPECT_EQ(demux.get_stream(-1), nullptr);
    EXPECT_TRUE(demux.extract_chapter_regions().empty());
    EXPECT_EQ(demux.duration_seconds(), 0.0);
}

TEST(FFmpegDemuxTest, FindBestStreamOnClosedContextLeavesCodecNull)
{
    IO::FFmpegDemuxContext demux;
    const void* codec = &demux;

    EXPECT_EQ(demux.find_best_stream(AVMEDIA_TYPE_AUDIO, &codec), -1);
    EXPECT_EQ(codec, nullptr);
}

TEST(FFmpegDemuxTest, SeekOnClosedContextFailsWithoutTouchingState)
{
    IO::FFmpegDemuxContext demux;

    EXPECT_FALSE(demux.seek(0, 0));
    EXPECT_FALSE(demux.seek(-1, 0));
    EXPECT_FALSE(demux.is_open());
}

TEST(FFmpegDemuxTest, ExtractMetadataOnClosedContextLeavesFormatEmpty)
{
    IO::FFmpegDemuxContext demux;
    IO::FileMetadata meta;

    demux.extract_container_metadata(meta);

    EXPECT_TRUE(meta.format.empty());
}

TEST(FFmpegDemuxTest, OpenDeviceWithUnknownFormatFailsAndReportsFormatName)
{
    IO::FFmpegDemuxContext demux;

    EXPECT_FALSE(demux.open_device("anydevice", "no_such_input_format_xyz"));
    EXPECT_FALSE(demux.is_open());
    EXPECT_NE(demux.last_error().find("no_such_input_format_xyz"), std::string::npos);
}

TEST(FFmpegDemuxTest, InitFfmpegIsIdempotent)
{
    IO::FFmpegDemuxContext::init_ffmpeg();
    IO::FFmpegDemuxContext::init_ffmpeg();

    IO::FFmpegDemuxContext demux;
    EXPECT_FALSE(demux.is_open());
}

// =========================================================================
// Malformed and missing input
// =========================================================================

TEST_F(FFmpegDemuxFileFixture, OpenNonexistentFileFailsAndNamesThePath)
{
    IO::FFmpegDemuxContext demux;

    EXPECT_FALSE(demux.open(path_of("missing.wav")));
    EXPECT_FALSE(demux.is_open());
    EXPECT_NE(demux.last_error().find("missing.wav"), std::string::npos);
}

TEST_F(FFmpegDemuxFileFixture, OpenDirectoryFails)
{
    std::filesystem::create_directories(root() / "not_a_file.wav");

    IO::FFmpegDemuxContext demux;
    EXPECT_FALSE(demux.open(path_of("not_a_file.wav")));
    EXPECT_FALSE(demux.is_open());
}

TEST_F(FFmpegDemuxFileFixture, OpenEmptyPathFails)
{
    IO::FFmpegDemuxContext demux;

    EXPECT_FALSE(demux.open(""));
    EXPECT_FALSE(demux.is_open());
    EXPECT_FALSE(demux.last_error().empty());
}

TEST_F(FFmpegDemuxFileFixture, OpenZeroByteFileFails)
{
    write_binary("empty.wav", "");

    IO::FFmpegDemuxContext demux;
    EXPECT_FALSE(demux.open(path_of("empty.wav")));
    EXPECT_FALSE(demux.is_open());
}

TEST_F(FFmpegDemuxFileFixture, OpenRandomBytesFails)
{
    write_binary("noise.bin", noise_bytes(4096));

    IO::FFmpegDemuxContext demux;
    EXPECT_FALSE(demux.open(path_of("noise.bin")));
    EXPECT_FALSE(demux.is_open());
    EXPECT_FALSE(demux.last_error().empty());
}

TEST_F(FFmpegDemuxFileFixture, OpenRiffHeaderWithoutFormatChunkFails)
{
    write_binary("header_only.wav", std::string("RIFF\x24\0\0\0WAVE", 12));

    IO::FFmpegDemuxContext demux;
    EXPECT_FALSE(demux.open(path_of("header_only.wav")));
    EXPECT_FALSE(demux.is_open());
}

TEST_F(FFmpegDemuxFileFixture, TruncatedDataChunkOpensWithShorterDuration)
{
    const std::string full = make_wav(1, 8000, 8000);
    write_binary("cut.wav", full.substr(0, full.size() / 2));

    IO::FFmpegDemuxContext demux;
    ASSERT_TRUE(demux.open(path_of("cut.wav"))) << demux.last_error();
    EXPECT_EQ(demux.stream_count(), 1U);
    EXPECT_LE(demux.duration_seconds(), 1.0 + 0.01);
}

TEST_F(FFmpegDemuxFileFixture, EmbeddedNulInPathIsNotTruncatedToAnotherFile)
{
    write_binary("clip.wav", make_wav(1, 8000, 800));

    const std::string hostile = path_of("clip.wav") + std::string("\0missing", 8);

    IO::FFmpegDemuxContext demux;
    EXPECT_FALSE(demux.open(hostile));
    EXPECT_FALSE(demux.is_open());
}

// =========================================================================
// Lifecycle under failure and reuse
// =========================================================================

TEST_F(FFmpegDemuxFileFixture, ReopenAfterFailureLeavesNothingOpen)
{
    write_binary("good.wav", make_wav(1, 8000, 800));
    write_binary("noise.bin", noise_bytes(2048));

    IO::FFmpegDemuxContext demux;
    ASSERT_TRUE(demux.open(path_of("good.wav"))) << demux.last_error();
    ASSERT_TRUE(demux.is_open());

    EXPECT_FALSE(demux.open(path_of("noise.bin")));
    EXPECT_FALSE(demux.is_open());
    EXPECT_EQ(demux.stream_count(), 0U);
    EXPECT_EQ(demux.get_stream(0), nullptr);
    EXPECT_EQ(demux.duration_seconds(), 0.0);
}

TEST_F(FFmpegDemuxFileFixture, FailureThenSuccessRecovers)
{
    write_binary("good.wav", make_wav(2, 22050, 2205));

    IO::FFmpegDemuxContext demux;
    EXPECT_FALSE(demux.open(path_of("missing.wav")));
    ASSERT_TRUE(demux.open(path_of("good.wav"))) << demux.last_error();
    EXPECT_EQ(demux.stream_count(), 1U);
    EXPECT_TRUE(demux.last_error().empty());
}

TEST_F(FFmpegDemuxFileFixture, ReopenReplacesThePreviousFile)
{
    write_binary("short.wav", make_wav(1, 8000, 8000));
    write_binary("long.wav", make_wav(1, 8000, 32000));

    IO::FFmpegDemuxContext demux;
    ASSERT_TRUE(demux.open(path_of("short.wav"))) << demux.last_error();
    EXPECT_NEAR(demux.duration_seconds(), 1.0, 0.01);

    ASSERT_TRUE(demux.open(path_of("long.wav"))) << demux.last_error();
    EXPECT_NEAR(demux.duration_seconds(), 4.0, 0.01);
}

TEST_F(FFmpegDemuxFileFixture, CloseThenDoubleCloseThenReopenIsClean)
{
    write_binary("mono.wav", make_wav(1, 8000, 800));

    IO::FFmpegDemuxContext demux;
    ASSERT_TRUE(demux.open(path_of("mono.wav"))) << demux.last_error();

    demux.close();
    demux.close();
    EXPECT_FALSE(demux.is_open());
    EXPECT_EQ(demux.stream_count(), 0U);

    ASSERT_TRUE(demux.open(path_of("mono.wav"))) << demux.last_error();
    EXPECT_EQ(demux.stream_count(), 1U);
}

TEST_F(FFmpegDemuxFileFixture, DestructorWithOpenFileReleasesCleanly)
{
    write_binary("mono.wav", make_wav(1, 8000, 800));

    for (int i = 0; i < 16; ++i) {
        auto demux = std::make_unique<IO::FFmpegDemuxContext>();
        ASSERT_TRUE(demux->open(path_of("mono.wav"))) << demux->last_error();
    }
}

// =========================================================================
// Stream discovery on a valid file
// =========================================================================

TEST_F(FFmpegDemuxFileFixture, GetStreamRejectsNegativeAndOutOfRangeIndices)
{
    write_binary("mono.wav", make_wav(1, 8000, 800));

    IO::FFmpegDemuxContext demux;
    ASSERT_TRUE(demux.open(path_of("mono.wav"))) << demux.last_error();

    EXPECT_NE(demux.get_stream(0), nullptr);
    EXPECT_EQ(demux.get_stream(-1), nullptr);
    EXPECT_EQ(demux.get_stream(1), nullptr);
    EXPECT_EQ(demux.get_stream(std::numeric_limits<int>::max()), nullptr);
    EXPECT_EQ(demux.get_stream(std::numeric_limits<int>::min()), nullptr);
}

TEST_F(FFmpegDemuxFileFixture, AudioStreamReportsSourceRateAndChannels)
{
    write_binary("stereo.wav", make_wav(2, 44100, 4410));

    IO::FFmpegDemuxContext demux;
    ASSERT_TRUE(demux.open(path_of("stereo.wav"))) << demux.last_error();

    const int audio = demux.find_best_stream(AVMEDIA_TYPE_AUDIO);
    ASSERT_EQ(audio, 0);

    AVStream* stream = demux.get_stream(audio);
    ASSERT_NE(stream, nullptr);
    EXPECT_EQ(stream->codecpar->sample_rate, 44100);
    EXPECT_EQ(stream->codecpar->ch_layout.nb_channels, 2);
    EXPECT_EQ(stream->codecpar->codec_type, AVMEDIA_TYPE_AUDIO);
}

TEST_F(FFmpegDemuxFileFixture, VideoSearchOnAudioOnlyFileReturnsNoStreamAndNullCodec)
{
    write_binary("audio_only.wav", make_wav(1, 8000, 800));

    IO::FFmpegDemuxContext demux;
    ASSERT_TRUE(demux.open(path_of("audio_only.wav"))) << demux.last_error();

    const void* codec = nullptr;
    EXPECT_LT(demux.find_best_stream(AVMEDIA_TYPE_VIDEO, &codec), 0);
    EXPECT_EQ(codec, nullptr);
}

TEST_F(FFmpegDemuxFileFixture, DurationMatchesFrameCountOverRate)
{
    write_binary("half.wav", make_wav(1, 16000, 8000));

    IO::FFmpegDemuxContext demux;
    ASSERT_TRUE(demux.open(path_of("half.wav"))) << demux.last_error();

    EXPECT_NEAR(demux.duration_seconds(), 0.5, 0.01);
}

TEST_F(FFmpegDemuxFileFixture, PlainWavHasNoChaptersAndReportsWavFormat)
{
    write_binary("plain.wav", make_wav(1, 8000, 800));

    IO::FFmpegDemuxContext demux;
    ASSERT_TRUE(demux.open(path_of("plain.wav"))) << demux.last_error();

    EXPECT_TRUE(demux.extract_chapter_regions().empty());

    IO::FileMetadata meta;
    demux.extract_container_metadata(meta);
    EXPECT_EQ(meta.format, "wav");
}

// =========================================================================
// Seeking on a valid file
// =========================================================================

TEST_F(FFmpegDemuxFileFixture, SeekToStreamOutsideRangeFails)
{
    write_binary("mono.wav", make_wav(1, 8000, 8000));

    IO::FFmpegDemuxContext demux;
    ASSERT_TRUE(demux.open(path_of("mono.wav"))) << demux.last_error();

    EXPECT_FALSE(demux.seek(7, 0));
    EXPECT_FALSE(demux.last_error().empty());
    EXPECT_TRUE(demux.is_open());
}

TEST_F(FFmpegDemuxFileFixture, SeekToStartSucceeds)
{
    write_binary("mono.wav", make_wav(1, 8000, 8000));

    IO::FFmpegDemuxContext demux;
    ASSERT_TRUE(demux.open(path_of("mono.wav"))) << demux.last_error();

    EXPECT_TRUE(demux.seek(0, 0)) << demux.last_error();
}

TEST_F(FFmpegDemuxFileFixture, SeekPastEndAndNegativeTimestampDoNotBreakTheContext)
{
    write_binary("mono.wav", make_wav(1, 8000, 8000));

    IO::FFmpegDemuxContext demux;
    ASSERT_TRUE(demux.open(path_of("mono.wav"))) << demux.last_error();

    (void)demux.seek(0, std::numeric_limits<int64_t>::max());
    (void)demux.seek(0, -1);
    (void)demux.seek(0, std::numeric_limits<int64_t>::min());

    EXPECT_TRUE(demux.is_open());
    EXPECT_NE(demux.get_stream(0), nullptr);
    EXPECT_TRUE(demux.seek(0, 0)) << demux.last_error();
}

// =========================================================================
// Concurrency: one context per thread
// =========================================================================

TEST_F(FFmpegDemuxFileFixture, ConcurrentOpenCloseOnSeparateContextsAllSucceed)
{
    write_binary("mono.wav", make_wav(1, 8000, 800));
    const std::string path = path_of("mono.wav");

    constexpr int k_threads = 6;
    constexpr int k_iterations = 10;
    std::atomic<int> failures { 0 };

    std::vector<std::thread> workers;
    workers.reserve(k_threads);
    for (int t = 0; t < k_threads; ++t) {
        workers.emplace_back([&path, &failures] {
            for (int i = 0; i < k_iterations; ++i) {
                IO::FFmpegDemuxContext demux;
                if (!demux.open(path) || demux.stream_count() != 1U)
                    failures.fetch_add(1);
                demux.close();
            }
        });
    }
    for (auto& w : workers)
        w.join();

    EXPECT_EQ(failures.load(), 0);
}

}
