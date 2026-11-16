#include "gtest/gtest.h"
#include "../../support/test_metadata.hpp"

#include "../file_contract_fixture.hpp"
#include "ffmpeg_fixture.hpp"

#include "MayaFlux/IO/CodecContext/AudioEncodeContext.hpp"
#include "MayaFlux/IO/CodecContext/FFmpegDemuxContext.hpp"
#include "MayaFlux/IO/CodecContext/FFmpegMuxContext.hpp"
#include "MayaFlux/IO/CodecContext/VideoEncodeContext.hpp"

#include <span>
#include <vector>

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::writable_temp>() }))

namespace MayaFlux::Test {

namespace {

    using FFmpegFixture::write_mpeg4_clip;

    class FFmpegEncodeFileFixture : public FileContractFixture {
    protected:
        [[nodiscard]] std::string path_of(const std::string& name) const
        {
            return (root() / name).string();
        }

        [[nodiscard]] std::filesystem::path file(const std::string& name) const
        {
            return root() / name;
        }
    };

}

// =========================================================================
// FFmpegMuxContext
// =========================================================================

TEST(FFmpegMuxTest, ClosedMuxRejectsEveryOperation)
{
    IO::FFmpegMuxContext mux;

    EXPECT_FALSE(mux.is_open());
    EXPECT_FALSE(mux.is_header_written());
    EXPECT_EQ(mux.add_stream(), nullptr);
    EXPECT_FALSE(mux.write_header());
    EXPECT_FALSE(mux.write_packet(nullptr));
    EXPECT_FALSE(mux.last_error().empty());
}

TEST(FFmpegMuxTest, CloseOnClosedMuxIsRepeatable)
{
    IO::FFmpegMuxContext mux;

    mux.close();
    mux.close();

    EXPECT_FALSE(mux.is_open());
}

TEST_F(FFmpegEncodeFileFixture, OpenFailsForUnwritableDirectory)
{
    IO::FFmpegMuxContext mux;

    EXPECT_FALSE(mux.open(path_of("no_such_dir_xyz/out.mp4")));
    EXPECT_FALSE(mux.is_open());
    EXPECT_FALSE(mux.last_error().empty());
}

TEST_F(FFmpegEncodeFileFixture, OpenFailsForUnknownExplicitFormat)
{
    IO::FFmpegMuxContext mux;

    EXPECT_FALSE(mux.open(path_of("out.bin"), "no_such_muxer_xyz"));
    EXPECT_FALSE(mux.is_open());
}

TEST_F(FFmpegEncodeFileFixture, WriteHeaderTwiceIsRejected)
{
    IO::FFmpegMuxContext mux;
    ASSERT_TRUE(mux.open(path_of("tone.wav"))) << mux.last_error();

    AVStream* st = mux.add_stream();
    ASSERT_NE(st, nullptr);
    st->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
    st->codecpar->codec_id = AV_CODEC_ID_PCM_S16LE;
    st->codecpar->sample_rate = 8000;
    st->codecpar->ch_layout.nb_channels = 1;
    st->codecpar->ch_layout.order = AV_CHANNEL_ORDER_NATIVE;
    st->codecpar->ch_layout.u.mask = AV_CH_LAYOUT_MONO;
    st->codecpar->format = AV_SAMPLE_FMT_S16;
    st->time_base = { 1, 8000 };

    ASSERT_TRUE(mux.write_header()) << mux.last_error();
    EXPECT_TRUE(mux.is_header_written());

    EXPECT_FALSE(mux.write_header());
    EXPECT_NE(mux.last_error().find("more than once"), std::string::npos);
}

TEST_F(FFmpegEncodeFileFixture, WritePacketBeforeHeaderIsRejected)
{
    IO::FFmpegMuxContext mux;
    ASSERT_TRUE(mux.open(path_of("tone.wav"))) << mux.last_error();

    AVPacket* pkt = av_packet_alloc();
    ASSERT_NE(pkt, nullptr);
    EXPECT_FALSE(mux.write_packet(pkt));
    EXPECT_NE(mux.last_error().find("before header"), std::string::npos);
    av_packet_free(&pkt);
}

TEST_F(FFmpegEncodeFileFixture, CloseWithoutHeaderLeavesNoTrailerAndNoCrash)
{
    IO::FFmpegMuxContext mux;
    ASSERT_TRUE(mux.open(path_of("abandoned.wav"))) << mux.last_error();

    mux.close();
    mux.close();

    EXPECT_FALSE(mux.is_open());
    EXPECT_FALSE(mux.is_header_written());
}

// =========================================================================
// AudioEncodeContext
// =========================================================================

TEST(FFmpegAudioEncodeTest, OpenFailsOnUnopenedMux)
{
    IO::FFmpegMuxContext mux;
    IO::AudioEncodeContext enc;

    EXPECT_FALSE(enc.open(mux, 44100, 2, AV_CODEC_ID_NONE));
    EXPECT_FALSE(enc.is_valid());
    EXPECT_FALSE(enc.last_error().empty());
}

TEST(FFmpegAudioEncodeTest, EncodeAndDrainBeforeOpenFail)
{
    IO::FFmpegMuxContext mux;
    IO::AudioEncodeContext enc;
    const std::vector<double> samples(8, 0.0);

    EXPECT_FALSE(enc.encode_frames(samples, 4, mux));
    EXPECT_FALSE(enc.drain(mux));
}

TEST_F(FFmpegEncodeFileFixture, OpenWithDefaultCodecUsesContainerAudioCodec)
{
    IO::FFmpegMuxContext mux;
    ASSERT_TRUE(mux.open(path_of("tone.wav"))) << mux.last_error();

    IO::AudioEncodeContext enc;
    ASSERT_TRUE(enc.open(mux, 44100, 2, AV_CODEC_ID_NONE)) << enc.last_error();
    EXPECT_TRUE(enc.is_valid());
    EXPECT_EQ(enc.m_sample_rate, 44100U);
    EXPECT_EQ(enc.m_channels, 2U);
}

TEST_F(FFmpegEncodeFileFixture, EncodingZeroFramesIsANoOp)
{
    IO::FFmpegMuxContext mux;
    ASSERT_TRUE(mux.open(path_of("tone.wav"))) << mux.last_error();

    IO::AudioEncodeContext enc;
    ASSERT_TRUE(enc.open(mux, 8000, 1, AV_CODEC_ID_NONE)) << enc.last_error();
    ASSERT_TRUE(mux.write_header()) << mux.last_error();

    EXPECT_TRUE(enc.encode_frames(std::span<const double> {}, 0, mux)) << enc.last_error();
}

TEST_F(FFmpegEncodeFileFixture, StereoRoundTripKeepsEveryFrame)
{
    constexpr uint32_t k_rate = 44100;
    constexpr uint32_t k_frames = 44100;
    constexpr uint32_t k_chunk = 1000;

    {
        IO::FFmpegMuxContext mux;
        ASSERT_TRUE(mux.open(path_of("tone.wav"))) << mux.last_error();

        IO::AudioEncodeContext enc;
        ASSERT_TRUE(enc.open(mux, k_rate, 2, AV_CODEC_ID_NONE)) << enc.last_error();
        ASSERT_TRUE(mux.write_header()) << mux.last_error();

        std::vector<double> chunk(static_cast<size_t>(k_chunk) * 2U);
        uint32_t written = 0;
        while (written < k_frames) {
            const uint32_t n = std::min(k_chunk, k_frames - written);
            for (uint32_t f = 0; f < n; ++f) {
                chunk[static_cast<size_t>(f) * 2U] = 0.25;
                chunk[static_cast<size_t>(f) * 2U + 1U] = -0.25;
            }
            ASSERT_TRUE(enc.encode_frames(std::span<const double>(chunk.data(), static_cast<size_t>(n) * 2U), n, mux))
                << enc.last_error();
            written += n;
        }

        ASSERT_TRUE(enc.drain(mux)) << enc.last_error();
        enc.close();
        mux.close();
    }

    IO::FFmpegDemuxContext demux;
    ASSERT_TRUE(demux.open(path_of("tone.wav"))) << demux.last_error();
    EXPECT_NEAR(demux.duration_seconds(), 1.0, 0.01);

    const int audio = demux.find_best_stream(AVMEDIA_TYPE_AUDIO);
    ASSERT_EQ(audio, 0);
    EXPECT_EQ(demux.get_stream(audio)->codecpar->sample_rate, static_cast<int>(k_rate));
    EXPECT_EQ(demux.get_stream(audio)->codecpar->ch_layout.nb_channels, 2);
}

// =========================================================================
// VideoEncodeContext
// =========================================================================

TEST(FFmpegVideoEncodeTest, OpenFailsOnUnopenedMux)
{
    IO::FFmpegMuxContext mux;
    IO::VideoEncodeContext enc;

    EXPECT_FALSE(enc.open(mux, 64, 48, 24.0, AV_PIX_FMT_RGBA, AV_CODEC_ID_MPEG4));
    EXPECT_FALSE(enc.is_valid());
}

TEST(FFmpegVideoEncodeTest, EncodeAndDrainBeforeOpenFail)
{
    IO::FFmpegMuxContext mux;
    IO::VideoEncodeContext enc;
    std::vector<uint8_t> pixels(64 * 48 * 4, 0);

    EXPECT_FALSE(enc.encode_frame(pixels.data(), pixels.size(), 64, 48, mux));
    EXPECT_FALSE(enc.drain(mux));
}

TEST_F(FFmpegEncodeFileFixture, OpenRejectsZeroDimensionsAndZeroRate)
{
    IO::FFmpegMuxContext mux;
    ASSERT_TRUE(mux.open(path_of("clip.mp4"), "mp4")) << mux.last_error();

    IO::VideoEncodeContext zero_width;
    EXPECT_FALSE(zero_width.open(mux, 0, 48, 24.0, AV_PIX_FMT_RGBA, AV_CODEC_ID_MPEG4));

    IO::VideoEncodeContext zero_rate;
    EXPECT_FALSE(zero_rate.open(mux, 64, 48, 0.0, AV_PIX_FMT_RGBA, AV_CODEC_ID_MPEG4));
    EXPECT_NE(zero_rate.last_error().find("invalid dimensions"), std::string::npos);
}

TEST_F(FFmpegEncodeFileFixture, OddDimensionsOpenAndKeepSourceSize)
{
    IO::FFmpegMuxContext mux;
    ASSERT_TRUE(mux.open(path_of("odd.mp4"), "mp4")) << mux.last_error();

    IO::VideoEncodeContext enc;
    ASSERT_TRUE(enc.open(mux, 65, 49, 24.0, AV_PIX_FMT_RGBA, AV_CODEC_ID_MPEG4)) << enc.last_error();
    EXPECT_TRUE(enc.is_valid());
    EXPECT_EQ(enc.width(), 65U);
    EXPECT_EQ(enc.height(), 49U);
}

TEST_F(FFmpegEncodeFileFixture, EncodeRejectsZeroSourceDimensionsAndShortBuffers)
{
    IO::FFmpegMuxContext mux;
    ASSERT_TRUE(mux.open(path_of("clip.mp4"), "mp4")) << mux.last_error();

    IO::VideoEncodeContext enc;
    ASSERT_TRUE(enc.open(mux, 64, 48, 24.0, AV_PIX_FMT_RGBA, AV_CODEC_ID_MPEG4)) << enc.last_error();
    ASSERT_TRUE(mux.write_header()) << mux.last_error();

    std::vector<uint8_t> pixels(64 * 48 * 4, 0);

    EXPECT_FALSE(enc.encode_frame(pixels.data(), pixels.size(), 0, 48, mux));
    EXPECT_NE(enc.last_error().find("zero source dimensions"), std::string::npos);

    EXPECT_FALSE(enc.encode_frame(pixels.data(), pixels.size() - 1, 64, 48, mux));
    EXPECT_NE(enc.last_error().find("too small"), std::string::npos);
}

TEST_F(FFmpegEncodeFileFixture, ClipRoundTripExposesVideoStreamAndDuration)
{
    ASSERT_TRUE(write_mpeg4_clip(file("clip.mp4"), 64, 48, 12, 24.0));

    IO::FFmpegDemuxContext demux;
    ASSERT_TRUE(demux.open(path_of("clip.mp4"))) << demux.last_error();

    const int video = demux.find_best_stream(AVMEDIA_TYPE_VIDEO);
    ASSERT_GE(video, 0);

    AVStream* st = demux.get_stream(video);
    ASSERT_NE(st, nullptr);
    EXPECT_EQ(st->codecpar->width, 64);
    EXPECT_EQ(st->codecpar->height, 48);
    EXPECT_NEAR(demux.duration_seconds(), 0.5, 0.15);
}

TEST_F(FFmpegEncodeFileFixture, SourceSizeChangeMidStreamRebuildsScaler)
{
    IO::FFmpegMuxContext mux;
    ASSERT_TRUE(mux.open(path_of("resize.mp4"), "mp4")) << mux.last_error();

    IO::VideoEncodeContext enc;
    ASSERT_TRUE(enc.open(mux, 64, 48, 24.0, AV_PIX_FMT_RGBA, AV_CODEC_ID_MPEG4)) << enc.last_error();
    ASSERT_TRUE(mux.write_header()) << mux.last_error();

    std::vector<uint8_t> large(64 * 48 * 4, 128);
    std::vector<uint8_t> small(32 * 24 * 4, 64);

    EXPECT_TRUE(enc.encode_frame(large.data(), large.size(), 64, 48, mux)) << enc.last_error();
    EXPECT_TRUE(enc.encode_frame(small.data(), small.size(), 32, 24, mux)) << enc.last_error();
    EXPECT_TRUE(enc.encode_frame(large.data(), large.size(), 64, 48, mux)) << enc.last_error();
    EXPECT_TRUE(enc.drain(mux)) << enc.last_error();

    enc.close();
    mux.close();
}

}
