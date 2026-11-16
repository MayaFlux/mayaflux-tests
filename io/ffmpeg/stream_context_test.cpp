#include "gtest/gtest.h"
#include "../../support/test_metadata.hpp"

#include "../file_contract_fixture.hpp"
#include "ffmpeg_fixture.hpp"

#include "MayaFlux/IO/CodecContext/AudioStreamContext.hpp"
#include "MayaFlux/IO/CodecContext/FFmpegDemuxContext.hpp"
#include "MayaFlux/IO/CodecContext/VideoStreamContext.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::writable_temp>() }))

namespace MayaFlux::Test {

namespace {

    using FFmpegFixture::make_wav;
    using FFmpegFixture::write_bytes;
    using FFmpegFixture::write_mpeg4_clip;

    class FFmpegStreamFileFixture : public FileContractFixture {
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
// AudioStreamContext
// =========================================================================

TEST(FFmpegAudioStreamTest, ClosedContextIsInvalidAndEmpty)
{
    IO::AudioStreamContext audio;

    EXPECT_FALSE(audio.is_valid());
    EXPECT_EQ(audio.codec_context, nullptr);
    EXPECT_EQ(audio.swr_context, nullptr);
    EXPECT_EQ(audio.stream_index, -1);
    EXPECT_EQ(audio.total_frames, 0U);
    EXPECT_EQ(audio.sample_rate, 0U);
    EXPECT_EQ(audio.channels, 0U);
}

TEST(FFmpegAudioStreamTest, CloseOnClosedContextIsRepeatable)
{
    IO::AudioStreamContext audio;

    audio.close();
    audio.close();

    EXPECT_FALSE(audio.is_valid());
    EXPECT_TRUE(audio.last_error().empty());
}

TEST(FFmpegAudioStreamTest, FlushAndDrainOnClosedContextAreSafe)
{
    IO::AudioStreamContext audio;

    audio.flush_codec();
    audio.drain_resampler_init();

    EXPECT_FALSE(audio.is_valid());
}

TEST(FFmpegAudioStreamTest, CueRegionsOnClosedContextAreEmpty)
{
    IO::AudioStreamContext audio;
    IO::FFmpegDemuxContext demux;

    EXPECT_TRUE(audio.extract_cue_regions(demux).empty());
}

TEST_F(FFmpegStreamFileFixture, OpenFailsWhenDemuxIsNotOpen)
{
    IO::FFmpegDemuxContext demux;
    IO::AudioStreamContext audio;

    EXPECT_FALSE(audio.open(demux));
    EXPECT_FALSE(audio.is_valid());
    EXPECT_EQ(audio.last_error(), "Demux context is not open");
}

TEST_F(FFmpegStreamFileFixture, OpenWavExposesRateChannelsAndFrames)
{
    write_bytes(file("stereo.wav"), make_wav(2, 44100, 4410));

    IO::FFmpegDemuxContext demux;
    ASSERT_TRUE(demux.open(path_of("stereo.wav"))) << demux.last_error();

    IO::AudioStreamContext audio;
    ASSERT_TRUE(audio.open(demux)) << audio.last_error();

    EXPECT_TRUE(audio.is_valid());
    EXPECT_EQ(audio.stream_index, 0);
    EXPECT_EQ(audio.sample_rate, 44100U);
    EXPECT_EQ(audio.channels, 2U);
    EXPECT_NEAR(static_cast<double>(audio.total_frames), 4410.0, 1.0);
}

TEST_F(FFmpegStreamFileFixture, OpenPlanarWithTargetRateStaysValid)
{
    write_bytes(file("mono.wav"), make_wav(1, 48000, 4800));

    IO::FFmpegDemuxContext demux;
    ASSERT_TRUE(demux.open(path_of("mono.wav"))) << demux.last_error();

    IO::AudioStreamContext audio;
    ASSERT_TRUE(audio.open(demux, true, 22050)) << audio.last_error();
    EXPECT_TRUE(audio.is_valid());
    EXPECT_EQ(audio.sample_rate, 48000U);
}

TEST_F(FFmpegStreamFileFixture, CloseResetsEveryPublicField)
{
    write_bytes(file("mono.wav"), make_wav(1, 8000, 800));

    IO::FFmpegDemuxContext demux;
    ASSERT_TRUE(demux.open(path_of("mono.wav"))) << demux.last_error();

    IO::AudioStreamContext audio;
    ASSERT_TRUE(audio.open(demux)) << audio.last_error();

    audio.close();

    EXPECT_FALSE(audio.is_valid());
    EXPECT_EQ(audio.codec_context, nullptr);
    EXPECT_EQ(audio.swr_context, nullptr);
    EXPECT_EQ(audio.stream_index, -1);
    EXPECT_EQ(audio.total_frames, 0U);
    EXPECT_EQ(audio.sample_rate, 0U);
    EXPECT_EQ(audio.channels, 0U);
}

TEST_F(FFmpegStreamFileFixture, FailedOpenAfterValidOpenLeavesContextInvalid)
{
    write_bytes(file("mono.wav"), make_wav(1, 8000, 800));

    IO::FFmpegDemuxContext demux;
    ASSERT_TRUE(demux.open(path_of("mono.wav"))) << demux.last_error();

    IO::AudioStreamContext audio;
    ASSERT_TRUE(audio.open(demux)) << audio.last_error();

    IO::FFmpegDemuxContext closed;
    EXPECT_FALSE(audio.open(closed));
    EXPECT_FALSE(audio.is_valid());
    EXPECT_EQ(audio.codec_context, nullptr);
}

TEST_F(FFmpegStreamFileFixture, ReopenAfterCloseAcceptsAnotherFile)
{
    write_bytes(file("mono.wav"), make_wav(1, 8000, 800));
    write_bytes(file("stereo.wav"), make_wav(2, 44100, 441));

    IO::FFmpegDemuxContext first;
    ASSERT_TRUE(first.open(path_of("mono.wav"))) << first.last_error();
    IO::AudioStreamContext audio;
    ASSERT_TRUE(audio.open(first)) << audio.last_error();
    EXPECT_EQ(audio.channels, 1U);

    audio.close();

    IO::FFmpegDemuxContext second;
    ASSERT_TRUE(second.open(path_of("stereo.wav"))) << second.last_error();
    ASSERT_TRUE(audio.open(second)) << audio.last_error();
    EXPECT_EQ(audio.channels, 2U);
    EXPECT_EQ(audio.sample_rate, 44100U);
}

TEST_F(FFmpegStreamFileFixture, FlushAndDrainOnValidContextKeepItValid)
{
    write_bytes(file("mono.wav"), make_wav(1, 8000, 800));

    IO::FFmpegDemuxContext demux;
    ASSERT_TRUE(demux.open(path_of("mono.wav"))) << demux.last_error();

    IO::AudioStreamContext audio;
    ASSERT_TRUE(audio.open(demux)) << audio.last_error();

    audio.flush_codec();
    audio.drain_resampler_init();
    audio.flush_codec();

    EXPECT_TRUE(audio.is_valid());
}

TEST_F(FFmpegStreamFileFixture, PlainWavHasNoCueRegions)
{
    write_bytes(file("mono.wav"), make_wav(1, 8000, 800));

    IO::FFmpegDemuxContext demux;
    ASSERT_TRUE(demux.open(path_of("mono.wav"))) << demux.last_error();

    IO::AudioStreamContext audio;
    ASSERT_TRUE(audio.open(demux)) << audio.last_error();

    EXPECT_TRUE(audio.extract_cue_regions(demux).empty());
}

// =========================================================================
// VideoStreamContext
// =========================================================================

TEST(FFmpegVideoStreamTest, ClosedContextIsInvalidAndEmpty)
{
    IO::VideoStreamContext video;

    EXPECT_FALSE(video.is_valid());
    EXPECT_FALSE(video.is_codec_valid());
    EXPECT_EQ(video.codec_context, nullptr);
    EXPECT_EQ(video.sws_context, nullptr);
    EXPECT_EQ(video.stream_index, -1);
    EXPECT_EQ(video.width, 0U);
    EXPECT_EQ(video.height, 0U);
    EXPECT_EQ(video.out_linesize, 0);
}

TEST(FFmpegVideoStreamTest, KeyframeRegionsOnClosedContextAreEmpty)
{
    IO::VideoStreamContext video;
    IO::FFmpegDemuxContext demux;

    EXPECT_TRUE(video.extract_keyframe_regions(demux).empty());
}

TEST(FFmpegVideoStreamTest, RebuildScalerRejectsNullFrame)
{
    IO::VideoStreamContext video;

    EXPECT_FALSE(video.rebuild_scaler_from_frame(nullptr));
    EXPECT_FALSE(video.is_valid());
}

TEST_F(FFmpegStreamFileFixture, OpenFailsOnAudioOnlyFile)
{
    write_bytes(file("mono.wav"), make_wav(1, 8000, 800));

    IO::FFmpegDemuxContext demux;
    ASSERT_TRUE(demux.open(path_of("mono.wav"))) << demux.last_error();

    IO::VideoStreamContext video;
    EXPECT_FALSE(video.open(demux));
    EXPECT_FALSE(video.is_valid());
    EXPECT_EQ(video.last_error(), "No video stream found");
}

TEST_F(FFmpegStreamFileFixture, VideoOpenFailsWhenDemuxIsNotOpen)
{
    IO::FFmpegDemuxContext demux;
    IO::VideoStreamContext video;

    EXPECT_FALSE(video.open(demux));
    EXPECT_EQ(video.last_error(), "Demux context is not open");
}

TEST_F(FFmpegStreamFileFixture, OpenClipExposesDimensions)
{
    ASSERT_TRUE(write_mpeg4_clip(file("clip.mp4"), 64, 48, 12, 24.0));

    IO::FFmpegDemuxContext demux;
    ASSERT_TRUE(demux.open(path_of("clip.mp4"))) << demux.last_error();

    IO::VideoStreamContext video;
    ASSERT_TRUE(video.open(demux)) << video.last_error();

    EXPECT_TRUE(video.is_valid());
    EXPECT_EQ(video.width, 64U);
    EXPECT_EQ(video.height, 48U);
    EXPECT_EQ(video.out_width, 64U);
    EXPECT_EQ(video.out_height, 48U);
}

TEST_F(FFmpegStreamFileFixture, ShortClipFrameRateIsStreamAverageNotEncoderRate)
{
    ASSERT_TRUE(write_mpeg4_clip(file("clip.mp4"), 64, 48, 12, 24.0));

    IO::FFmpegDemuxContext demux;
    ASSERT_TRUE(demux.open(path_of("clip.mp4"))) << demux.last_error();

    IO::VideoStreamContext video;
    ASSERT_TRUE(video.open(demux)) << video.last_error();

    EXPECT_NEAR(video.frame_rate, 288.0 / 11.0, 1e-6);
}

TEST_F(FFmpegStreamFileFixture, LongClipFrameRateIsWithinOneFrameBiasOfEncoderRate)
{
    ASSERT_TRUE(write_mpeg4_clip(file("long.mp4"), 64, 48, 240, 24.0));

    IO::FFmpegDemuxContext demux;
    ASSERT_TRUE(demux.open(path_of("long.mp4"))) << demux.last_error();

    IO::VideoStreamContext video;
    ASSERT_TRUE(video.open(demux)) << video.last_error();

    EXPECT_NEAR(video.frame_rate, 24.0, 0.15);
}

TEST_F(FFmpegStreamFileFixture, OpenWithTargetSizeScalesOutputAndAlignsStride)
{
    ASSERT_TRUE(write_mpeg4_clip(file("clip.mp4"), 64, 48, 4, 24.0));

    IO::FFmpegDemuxContext demux;
    ASSERT_TRUE(demux.open(path_of("clip.mp4"))) << demux.last_error();

    IO::VideoStreamContext video;
    ASSERT_TRUE(video.open(demux, 32, 24, AV_PIX_FMT_RGBA)) << video.last_error();

    EXPECT_EQ(video.out_width, 32U);
    EXPECT_EQ(video.out_height, 24U);
    EXPECT_EQ(video.out_bytes_per_pixel, 4U);
    EXPECT_GE(video.out_linesize, 32 * 4);
    EXPECT_EQ(video.out_linesize % 32, 0);
}

TEST_F(FFmpegStreamFileFixture, OpenWithPlanarTargetFormatIsRejected)
{
    ASSERT_TRUE(write_mpeg4_clip(file("clip.mp4"), 64, 48, 4, 24.0));

    IO::FFmpegDemuxContext demux;
    ASSERT_TRUE(demux.open(path_of("clip.mp4"))) << demux.last_error();

    IO::VideoStreamContext video;
    EXPECT_FALSE(video.open(demux, 0, 0, AV_PIX_FMT_YUV420P));
    EXPECT_FALSE(video.is_valid());
    EXPECT_NE(video.last_error().find("multi-planar"), std::string::npos);
}

TEST_F(FFmpegStreamFileFixture, OpenWithUnknownTargetFormatIsRejected)
{
    ASSERT_TRUE(write_mpeg4_clip(file("clip.mp4"), 64, 48, 4, 24.0));

    IO::FFmpegDemuxContext demux;
    ASSERT_TRUE(demux.open(path_of("clip.mp4"))) << demux.last_error();

    IO::VideoStreamContext video;
    EXPECT_FALSE(video.open(demux, 0, 0, 9999));
    EXPECT_FALSE(video.is_valid());
}

TEST_F(FFmpegStreamFileFixture, OpenDeviceOnFileWithoutVideoFails)
{
    write_bytes(file("mono.wav"), make_wav(1, 8000, 800));

    IO::FFmpegDemuxContext demux;
    ASSERT_TRUE(demux.open(path_of("mono.wav"))) << demux.last_error();

    IO::VideoStreamContext video;
    EXPECT_FALSE(video.open_device(demux));
    EXPECT_FALSE(video.is_codec_valid());
}

TEST_F(FFmpegStreamFileFixture, ExtractKeyframeRegionsStartsAtFrameZeroAndLeavesDemuxUsable)
{
    ASSERT_TRUE(write_mpeg4_clip(file("clip.mp4"), 64, 48, 12, 24.0));

    IO::FFmpegDemuxContext demux;
    ASSERT_TRUE(demux.open(path_of("clip.mp4"))) << demux.last_error();

    IO::VideoStreamContext video;
    ASSERT_TRUE(video.open(demux)) << video.last_error();

    const auto regions = video.extract_keyframe_regions(demux);
    ASSERT_FALSE(regions.empty());
    EXPECT_EQ(regions.front().type, "keyframe");
    EXPECT_EQ(regions.front().start_coordinates.front(), 0U);

    EXPECT_TRUE(demux.seek(video.stream_index, 0)) << demux.last_error();
}

TEST_F(FFmpegStreamFileFixture, CloseThenReopenClipIsClean)
{
    ASSERT_TRUE(write_mpeg4_clip(file("clip.mp4"), 64, 48, 4, 24.0));

    IO::FFmpegDemuxContext demux;
    ASSERT_TRUE(demux.open(path_of("clip.mp4"))) << demux.last_error();

    IO::VideoStreamContext video;
    ASSERT_TRUE(video.open(demux)) << video.last_error();
    video.close();
    video.close();
    EXPECT_FALSE(video.is_valid());

    ASSERT_TRUE(video.open(demux)) << video.last_error();
    EXPECT_TRUE(video.is_valid());
}

TEST_F(FFmpegStreamFileFixture, FlushCodecOnValidContextKeepsItValid)
{
    ASSERT_TRUE(write_mpeg4_clip(file("clip.mp4"), 64, 48, 4, 24.0));

    IO::FFmpegDemuxContext demux;
    ASSERT_TRUE(demux.open(path_of("clip.mp4"))) << demux.last_error();

    IO::VideoStreamContext video;
    ASSERT_TRUE(video.open(demux)) << video.last_error();

    video.flush_codec();
    EXPECT_TRUE(video.is_valid());
}

}
