#include "gtest/gtest.h"
#include "../support/test_metadata.hpp"

#include "file_contract_fixture.hpp"
#include "ffmpeg/ffmpeg_fixture.hpp"

#include "MayaFlux/IO/Audio/SoundFileReader.hpp"
#include "MayaFlux/IO/Audio/SoundFileWriter.hpp"
#include "MayaFlux/IO/CodecContext/AudioStreamContext.hpp"
#include "MayaFlux/IO/CodecContext/FFmpegDemuxContext.hpp"

#include <span>

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::writable_temp>() }))

namespace MayaFlux::Test {

namespace {

    using FFmpegFixture::make_wav;
    using FFmpegFixture::write_bytes;

    class SoundFileFixture : public FileContractFixture {
    protected:
        [[nodiscard]] std::string path_of(const std::string& name) const
        {
            return (root() / name).string();
        }

        [[nodiscard]] std::filesystem::path file(const std::string& name) const
        {
            return root() / name;
        }

        static std::vector<double>& channel(std::vector<Kakshya::DataVariant>& data, size_t index)
        {
            return std::get<std::vector<double>>(data[index]);
        }
    };

}

// =========================================================================
// SoundFileReader: closed state
// =========================================================================

TEST(SoundFileReaderTest, ClosedReaderReportsNothing)
{
    IO::SoundFileReader reader;

    EXPECT_FALSE(reader.is_open());
    EXPECT_TRUE(reader.read_all().empty());
    EXPECT_EQ(reader.get_last_error(), "File not open");
    EXPECT_TRUE(reader.read_frames(10).empty());
    EXPECT_FALSE(reader.get_metadata().has_value());
    EXPECT_TRUE(reader.get_regions().empty());
    EXPECT_EQ(reader.get_read_position(), std::vector<uint64_t> { 0 });
    EXPECT_EQ(reader.get_dimension_sizes(), (std::vector<uint64_t> { 0, 0 }));
    EXPECT_EQ(reader.create_container(), nullptr);
}

TEST(SoundFileReaderTest, EmptyPositionSeekIsRejected)
{
    IO::SoundFileReader reader;

    EXPECT_FALSE(reader.seek({}));
    EXPECT_FALSE(reader.seek({ 0 }));
}

TEST(SoundFileReaderTest, LoadIntoContainerRejectsNullContainer)
{
    IO::SoundFileReader reader;

    EXPECT_FALSE(reader.load_into_container(nullptr));
    EXPECT_FALSE(reader.get_last_error().empty());
}

TEST(SoundFileReaderTest, SupportedExtensionsIncludeWav)
{
    IO::SoundFileReader reader;
    const auto exts = reader.get_supported_extensions();

    EXPECT_NE(std::find(exts.begin(), exts.end(), "wav"), exts.end());
}

// =========================================================================
// SoundFileReader: opening and probing
// =========================================================================

TEST_F(SoundFileFixture, OpenNonexistentFileFailsWithError)
{
    IO::SoundFileReader reader;

    EXPECT_FALSE(reader.open(path_of("missing.wav")));
    EXPECT_FALSE(reader.is_open());
    EXPECT_FALSE(reader.get_last_error().empty());
}

TEST_F(SoundFileFixture, CanReadRejectsNonAudioContent)
{
    write_bytes(file("notes.wav"), FFmpegFixture::noise_bytes(2048));

    IO::SoundFileReader reader;
    EXPECT_FALSE(reader.can_read(path_of("notes.wav")));
}

TEST_F(SoundFileFixture, CanReadAcceptsWav)
{
    write_bytes(file("tone.wav"), make_wav(1, 8000, 800));

    IO::SoundFileReader reader;
    EXPECT_TRUE(reader.can_read(path_of("tone.wav")));
}

TEST_F(SoundFileFixture, CloseThenReopenIsClean)
{
    write_bytes(file("tone.wav"), make_wav(1, 8000, 800));

    IO::SoundFileReader reader;
    ASSERT_TRUE(reader.open(path_of("tone.wav"))) << reader.get_last_error();
    reader.close();
    reader.close();
    EXPECT_FALSE(reader.is_open());

    ASSERT_TRUE(reader.open(path_of("tone.wav"))) << reader.get_last_error();
    EXPECT_TRUE(reader.is_open());
}

// =========================================================================
// SoundFileReader: reading
// =========================================================================

TEST_F(SoundFileFixture, ReadAllReturnsEveryInterleavedSample)
{
    write_bytes(file("stereo.wav"), make_wav(2, 44100, 4410));

    IO::SoundFileReader reader;
    ASSERT_TRUE(reader.open(path_of("stereo.wav"))) << reader.get_last_error();

    auto data = reader.read_all();
    ASSERT_EQ(data.size(), 1U);
    EXPECT_EQ(channel(data, 0).size(), 4410U * 2U);
}

TEST_F(SoundFileFixture, DeinterleavedReadSplitsChannels)
{
    write_bytes(file("stereo.wav"), make_wav(2, 44100, 4410));

    IO::SoundFileReader reader;
    reader.set_audio_options(IO::AudioReadOptions::DEINTERLEAVE);
    ASSERT_TRUE(reader.open(path_of("stereo.wav"))) << reader.get_last_error();

    auto data = reader.read_all();
    ASSERT_EQ(data.size(), 2U);
    EXPECT_EQ(channel(data, 0).size(), 4410U);
    EXPECT_EQ(channel(data, 1).size(), 4410U);
}

TEST_F(SoundFileFixture, ReadFramesAdvancesPositionAndHonoursOffset)
{
    write_bytes(file("mono.wav"), make_wav(1, 8000, 8000));

    IO::SoundFileReader reader;
    ASSERT_TRUE(reader.open(path_of("mono.wav"))) << reader.get_last_error();

    auto first = reader.read_frames(100, 0);
    ASSERT_EQ(first.size(), 1U);
    EXPECT_EQ(channel(first, 0).size(), 100U);
    EXPECT_EQ(reader.get_read_position(), std::vector<uint64_t> { 100 });

    auto from_offset = reader.read_frames(50, 1000);
    ASSERT_EQ(from_offset.size(), 1U);
    EXPECT_EQ(channel(from_offset, 0).size(), 50U);
    EXPECT_EQ(reader.get_read_position(), std::vector<uint64_t> { 1050 });
}

TEST_F(SoundFileFixture, SeekPastEndClampsToTotalFrames)
{
    write_bytes(file("mono.wav"), make_wav(1, 8000, 8000));

    IO::SoundFileReader reader;
    ASSERT_TRUE(reader.open(path_of("mono.wav"))) << reader.get_last_error();

    EXPECT_TRUE(reader.seek({ 999999 })) << reader.get_last_error();
    EXPECT_EQ(reader.get_read_position(), std::vector<uint64_t> { 8000 });
}

TEST_F(SoundFileFixture, ReadAllAfterPartialReadStillReturnsWholeFile)
{
    write_bytes(file("mono.wav"), make_wav(1, 8000, 8000));

    IO::SoundFileReader reader;
    ASSERT_TRUE(reader.open(path_of("mono.wav"))) << reader.get_last_error();

    (void)reader.read_frames(1000, 0);
    auto data = reader.read_all();

    ASSERT_EQ(data.size(), 1U);
    EXPECT_EQ(channel(data, 0).size(), 8000U);
}

TEST_F(SoundFileFixture, ReadAllAtTargetRateReturnsOneSecondOfOutput)
{
    write_bytes(file("tone.wav"), make_wav(1, 44100, 44100));

    IO::SoundFileReader reader;
    reader.set_target_sample_rate(48000);
    ASSERT_TRUE(reader.open(path_of("tone.wav"))) << reader.get_last_error();

    auto data = reader.read_all();
    ASSERT_EQ(data.size(), 1U);
    EXPECT_NEAR(static_cast<double>(channel(data, 0).size()), 48000.0, 50.0);
}

// =========================================================================
// SoundFileReader: bounded loading
// =========================================================================

TEST_F(SoundFileFixture, LoadBoundedRejectsFileOverLimitWithoutTruncate)
{
    write_bytes(file("long.wav"), make_wav(1, 8000, 8000));

    IO::SoundFileReader reader;
    EXPECT_EQ(reader.load_bounded(path_of("long.wav"), 4000, false), nullptr);
    EXPECT_NE(reader.get_last_error().find("exceeds limit"), std::string::npos);
    EXPECT_FALSE(reader.is_open());
}

TEST_F(SoundFileFixture, LoadBoundedTruncatesWhenAllowed)
{
    write_bytes(file("long.wav"), make_wav(1, 8000, 8000));

    IO::SoundFileReader reader;
    EXPECT_NE(reader.load_bounded(path_of("long.wav"), 4000, true), nullptr) << reader.get_last_error();
    EXPECT_FALSE(reader.is_open());
}

TEST_F(SoundFileFixture, LoadBoundedDefaultLimitIsFiveSeconds)
{
    write_bytes(file("six.wav"), make_wav(1, 8000, 48000));
    write_bytes(file("four.wav"), make_wav(1, 8000, 32000));

    IO::SoundFileReader reader;
    EXPECT_EQ(reader.load_bounded(path_of("six.wav")), nullptr);
    EXPECT_NE(reader.load_bounded(path_of("four.wav")), nullptr) << reader.get_last_error();
}

TEST_F(SoundFileFixture, LoadBoundedRejectsUnsupportedFile)
{
    write_bytes(file("notes.wav"), FFmpegFixture::noise_bytes(2048));

    IO::SoundFileReader reader;
    EXPECT_EQ(reader.load_bounded(path_of("notes.wav")), nullptr);
    EXPECT_NE(reader.get_last_error().find("unsupported"), std::string::npos);
}

// =========================================================================
// SoundFileReader: metadata and containers
// =========================================================================

TEST_F(SoundFileFixture, MetadataReportsFormatAndFileSize)
{
    const std::string wav = make_wav(1, 8000, 800);
    write_bytes(file("tone.wav"), wav);

    IO::SoundFileReader reader;
    ASSERT_TRUE(reader.open(path_of("tone.wav"), IO::FileReadOptions::EXTRACT_METADATA)) << reader.get_last_error();

    auto meta = reader.get_metadata();
    ASSERT_TRUE(meta.has_value());
    EXPECT_EQ(meta->format, "wav");
    EXPECT_EQ(meta->file_size, wav.size());
}

TEST_F(SoundFileFixture, OpenFromDemuxRejectsClosedDemux)
{
    IO::SoundFileReader reader;

    EXPECT_FALSE(reader.open_from_demux(nullptr, nullptr, "none.wav"));
    EXPECT_EQ(reader.get_last_error(), "open_from_demux: demux context is null or not open");
}

TEST_F(SoundFileFixture, OpenFromDemuxWithMissingPathDoesNotThrow)
{
    write_bytes(file("tone.wav"), make_wav(1, 8000, 800));

    auto demux = std::make_shared<IO::FFmpegDemuxContext>();
    ASSERT_TRUE(demux->open(path_of("tone.wav"))) << demux->last_error();

    auto audio = std::make_shared<IO::AudioStreamContext>();
    ASSERT_TRUE(audio->open(*demux)) << audio->last_error();

    IO::SoundFileReader reader;
    EXPECT_NO_THROW(reader.open_from_demux(demux, audio, path_of("gone.wav"), IO::FileReadOptions::EXTRACT_METADATA));
}

TEST_F(SoundFileFixture, CreateContainerRequiresOpenFile)
{
    IO::SoundFileReader reader;
    EXPECT_EQ(reader.create_container(), nullptr);
    EXPECT_EQ(reader.get_last_error(), "File not open");
}

TEST_F(SoundFileFixture, LoadIntoContainerPopulatesSoundContainer)
{
    write_bytes(file("tone.wav"), make_wav(1, 8000, 800));

    IO::SoundFileReader reader;
    ASSERT_TRUE(reader.open(path_of("tone.wav"))) << reader.get_last_error();

    auto container = reader.create_container();
    ASSERT_NE(container, nullptr);
    EXPECT_TRUE(reader.load_into_container(container)) << reader.get_last_error();
}

// =========================================================================
// SoundFileWriter
// =========================================================================

TEST(SoundFileWriterTest, ClosedWriterIsNotOpenAndIgnoresWrites)
{
    IO::SoundFileWriter writer;
    const std::vector<double> samples(8, 0.0);

    EXPECT_FALSE(writer.is_open());
    writer.write(std::span<const double>(samples), 4);
    EXPECT_FALSE(writer.is_open());
}

TEST_F(SoundFileFixture, OpenUnwritablePathFailsWithError)
{
    IO::SoundFileWriter writer;

    EXPECT_FALSE(writer.open(path_of("no_such_dir_xyz/out.wav"), 1, 8000, AV_CODEC_ID_NONE));
    EXPECT_FALSE(writer.is_open());
    EXPECT_FALSE(writer.last_error().empty());
}

TEST_F(SoundFileFixture, OpenWhileOpenIsRejected)
{
    IO::SoundFileWriter writer;
    ASSERT_TRUE(writer.open(path_of("a.wav"), 1, 8000, AV_CODEC_ID_NONE)) << writer.last_error();

    EXPECT_FALSE(writer.open(path_of("b.wav"), 1, 8000, AV_CODEC_ID_NONE));
    EXPECT_EQ(writer.last_error(), "open() called while already open");

    EXPECT_TRUE(writer.close().get());
}

TEST_F(SoundFileFixture, InterleavedWriteRoundTripsThroughReader)
{
    constexpr uint32_t k_frames = 4410;
    {
        IO::SoundFileWriter writer;
        ASSERT_TRUE(writer.open(path_of("out.wav"), 2, 44100, AV_CODEC_ID_NONE)) << writer.last_error();

        std::vector<double> interleaved(static_cast<size_t>(k_frames) * 2U);
        for (uint32_t f = 0; f < k_frames; ++f) {
            interleaved[static_cast<size_t>(f) * 2U] = 0.25;
            interleaved[static_cast<size_t>(f) * 2U + 1U] = -0.25;
        }
        writer.write(std::span<const double>(interleaved), k_frames);
        EXPECT_TRUE(writer.close().get()) << writer.last_error();
    }

    IO::SoundFileReader reader;
    ASSERT_TRUE(reader.open(path_of("out.wav"))) << reader.get_last_error();

    auto data = reader.read_all();
    ASSERT_EQ(data.size(), 1U);
    const auto& samples = channel(data, 0);
    ASSERT_EQ(samples.size(), static_cast<size_t>(k_frames) * 2U);
    EXPECT_NEAR(samples[0], 0.25, 1e-3);
    EXPECT_NEAR(samples[1], -0.25, 1e-3);
}

TEST_F(SoundFileFixture, PlanarWriteRoundTripsThroughReader)
{
    constexpr uint32_t k_frames = 1000;
    {
        IO::SoundFileWriter writer;
        ASSERT_TRUE(writer.open(path_of("planar.wav"), 2, 8000, AV_CODEC_ID_NONE)) << writer.last_error();

        std::vector<Kakshya::DataVariant> planar;
        planar.emplace_back(std::vector<double>(k_frames, 0.5));
        planar.emplace_back(std::vector<double>(k_frames, -0.5));
        writer.write(planar);
        EXPECT_TRUE(writer.close().get()) << writer.last_error();
    }

    IO::SoundFileReader reader;
    ASSERT_TRUE(reader.open(path_of("planar.wav"))) << reader.get_last_error();

    auto data = reader.read_all();
    ASSERT_EQ(data.size(), 1U);
    EXPECT_EQ(channel(data, 0).size(), static_cast<size_t>(k_frames) * 2U);
}

TEST_F(SoundFileFixture, ZeroFrameWriteLeavesEmptyFile)
{
    {
        IO::SoundFileWriter writer;
        ASSERT_TRUE(writer.open(path_of("empty.wav"), 1, 8000, AV_CODEC_ID_NONE)) << writer.last_error();
        writer.write(std::span<const double> {}, 0);
        EXPECT_TRUE(writer.close().get()) << writer.last_error();
    }

    IO::SoundFileReader reader;
    ASSERT_TRUE(reader.open(path_of("empty.wav"))) << reader.get_last_error();
    EXPECT_EQ(reader.get_dimension_sizes()[0], 0U);
}

TEST_F(SoundFileFixture, CloseTwiceReturnsSameResult)
{
    IO::SoundFileWriter writer;
    ASSERT_TRUE(writer.open(path_of("twice.wav"), 1, 8000, AV_CODEC_ID_NONE)) << writer.last_error();

    auto first = writer.close();
    auto second = writer.close();
    EXPECT_TRUE(first.get());
    EXPECT_TRUE(second.get());
}

TEST_F(SoundFileFixture, WriteAfterCloseIsIgnored)
{
    IO::SoundFileWriter writer;
    ASSERT_TRUE(writer.open(path_of("after.wav"), 1, 8000, AV_CODEC_ID_NONE)) << writer.last_error();
    EXPECT_TRUE(writer.close().get());

    const std::vector<double> samples(8, 0.1);
    EXPECT_NO_THROW(writer.write(std::span<const double>(samples), 8));
    EXPECT_FALSE(writer.is_open());
}

TEST_F(SoundFileFixture, DestructorFinalisesOpenFile)
{
    {
        IO::SoundFileWriter writer;
        ASSERT_TRUE(writer.open(path_of("scoped.wav"), 1, 8000, AV_CODEC_ID_NONE)) << writer.last_error();

        const std::vector<double> samples(800, 0.1);
        writer.write(std::span<const double>(samples), 800);
    }

    IO::SoundFileReader reader;
    ASSERT_TRUE(reader.open(path_of("scoped.wav"))) << reader.get_last_error();
    EXPECT_EQ(reader.get_dimension_sizes()[0], 800U);
}

TEST_F(SoundFileFixture, OpenWithZeroChannelsFails)
{
    IO::SoundFileWriter writer;

    EXPECT_FALSE(writer.open(path_of("zero.wav"), 0, 8000, AV_CODEC_ID_NONE));
    EXPECT_FALSE(writer.is_open());
}

}
