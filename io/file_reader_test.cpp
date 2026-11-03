#include "file_contract_fixture.hpp"

#include "MayaFlux/IO/CodecContext/AudioStreamContext.hpp"
#include "MayaFlux/IO/CodecContext/FFmpegDemuxContext.hpp"
#include "MayaFlux/IO/CodecContext/VideoStreamContext.hpp"
#include "MayaFlux/IO/FileReader.hpp"

namespace MayaFlux::Test {
namespace {

    struct ReaderPathCase {
        const char* input;
        const char* expected;
        bool exists;
    };

    class FileReaderPathTest
        : public FileContractFixture
        , public ::testing::WithParamInterface<ReaderPathCase> {
    };

    TEST_P(FileReaderPathTest, ResolvesLocalAndMissingPaths)
    {
        const auto& param = GetParam();
        if (param.exists)
            write(param.expected);

        EXPECT_EQ(IO::FileReader::resolve_path(param.input), param.expected);
    }

    INSTANTIATE_TEST_SUITE_P(
        Paths,
        FileReaderPathTest,
        ::testing::Values(
            ReaderPathCase { "sample.wav", "sample.wav", true },
            ReaderPathCase { "nested/sample.wav", "nested/sample.wav", true },
            ReaderPathCase { "missing\\sample.wav", "missing/sample.wav", false }));

    TEST_F(FileContractFixture, AbsoluteReaderPathIsUnchanged)
    {
        const auto path = root() / "sample.wav";
        write("sample.wav");

        EXPECT_EQ(IO::FileReader::resolve_path(path.string()), path.string());
    }

    TEST(FileMetadataTest, TagsStayWholeAndInFileOrder)
    {
        IO::FileMetadata metadata;
        EXPECT_EQ(metadata.get<IO::ContainerTags>(), nullptr);

        IO::ContainerTags tags;
        tags.entries = { { "title", "take 1" }, { "artist", "mf" } };
        metadata.set(tags);

        const auto* stored = metadata.get<IO::ContainerTags>();
        ASSERT_NE(stored, nullptr);
        EXPECT_EQ(stored->entries, tags.entries);
    }

    TEST(FileMetadataTest, WholeInfoStructIsStoredAndReplacedByType)
    {
        IO::FileMetadata metadata;
        EXPECT_EQ(metadata.get<IO::ContainerInfo>(), nullptr);

        metadata.set(IO::ContainerInfo { .duration_seconds = 2.5, .bit_rate = 128000 });
        metadata.set(IO::ContainerInfo { .duration_seconds = std::nullopt, .bit_rate = 64000 });

        const auto* container = metadata.get<IO::ContainerInfo>();
        ASSERT_NE(container, nullptr);
        EXPECT_FALSE(container->duration_seconds.has_value());
        EXPECT_EQ(container->bit_rate, 64000);
    }

    TEST(FileMetadataTest, IdenticallyNamedFieldsOfDifferentInfoStructsStayDistinct)
    {
        IO::FileMetadata metadata;
        metadata.set(IO::ContainerInfo { .bit_rate = 256000 });
        metadata.set(IO::AudioStreamInfo { .sample_rate = 48000, .channels = 2, .bit_rate = 192000 });

        ASSERT_NE(metadata.get<IO::ContainerInfo>(), nullptr);
        ASSERT_NE(metadata.get<IO::AudioStreamInfo>(), nullptr);
        EXPECT_EQ(metadata.get<IO::ContainerInfo>()->bit_rate, 256000);
        EXPECT_EQ(metadata.get<IO::AudioStreamInfo>()->bit_rate, 192000);
    }

    TEST(FileMetadataTest, UnknownVideoFieldsStayAbsentInsideTheStruct)
    {
        IO::FileMetadata metadata;
        metadata.set(IO::VideoStreamInfo { .codec = "h264", .width = 640, .height = 480, .frame_rate = 25.0 });

        const auto* video = metadata.get<IO::VideoStreamInfo>();
        ASSERT_NE(video, nullptr);
        EXPECT_EQ(video->width, 640U);
        EXPECT_FALSE(video->pixel_format.has_value());
        EXPECT_FALSE(video->sar_num.has_value());
    }

    struct FileRegionCase {
        std::vector<uint64_t> start;
        std::vector<uint64_t> end;
        const char* name;
    };

    class FileRegionConversionTest : public ::testing::TestWithParam<FileRegionCase> { };

    TEST_P(FileRegionConversionTest, PreservesCoordinatesAndLabel)
    {
        const auto& param = GetParam();
        IO::FileRegion file_region;
        file_region.name = param.name;
        file_region.start_coordinates = param.start;
        file_region.end_coordinates = param.end;

        const auto region = file_region.to_region();

        EXPECT_EQ(region.start_coordinates, param.start);
        EXPECT_EQ(region.end_coordinates, param.end);
        ASSERT_TRUE(region.label.has_value());
        EXPECT_EQ(*region.label, param.name);
    }

    INSTANTIATE_TEST_SUITE_P(
        Dimensions,
        FileRegionConversionTest,
        ::testing::Values(
            FileRegionCase { { 7 }, { 7 }, "cue" },
            FileRegionCase { { 7 }, { 15 }, "chapter" },
            FileRegionCase { { 2, 3 }, { 5, 8 }, "crop" }));

    TEST(FileRegionTest, ChapterInfoReachesRegionOnEveryCoordinateShape)
    {
        const std::vector<std::pair<std::vector<uint64_t>, std::vector<uint64_t>>> shapes {
            { { 7 }, { 7 } },
            { { 7 }, { 15 } },
            { { 2, 3 }, { 5, 8 } },
        };

        for (const auto& [start, end] : shapes) {
            IO::FileRegion file_region;
            file_region.type = "chapter";
            file_region.start_coordinates = start;
            file_region.end_coordinates = end;
            file_region.set(IO::ChapterInfo { .chapter_index = 4 });

            const auto region = file_region.to_region();
            const auto* chapter = region.get<IO::ChapterInfo>();
            ASSERT_NE(chapter, nullptr);
            EXPECT_EQ(chapter->chapter_index, 4U);
        }
    }

    TEST(FileRegionTest, KeyframeInfoReachesRegionAndAbsentInfoStaysAbsent)
    {
        IO::FileRegion keyframe;
        keyframe.type = "keyframe";
        keyframe.start_coordinates = { 50 };
        keyframe.end_coordinates = { 50 };
        keyframe.set(IO::KeyframeInfo { .pts = 90000, .timestamp_seconds = 1.0, .keyframe_index = 2 });

        const auto keyframe_region = keyframe.to_region();
        const auto* info = keyframe_region.get<IO::KeyframeInfo>();
        ASSERT_NE(info, nullptr);
        EXPECT_EQ(info->pts, 90000);
        EXPECT_EQ(info->keyframe_index, 2);
        EXPECT_EQ(keyframe_region.get<IO::ChapterInfo>(), nullptr);

        IO::FileRegion cue;
        cue.type = "cue";
        cue.start_coordinates = { 10 };
        cue.end_coordinates = { 10 };
        const auto cue_region = cue.to_region();
        EXPECT_EQ(cue_region.get<IO::KeyframeInfo>(), nullptr);
        EXPECT_EQ(cue_region.get<IO::ChapterInfo>(), nullptr);
    }

}
}
