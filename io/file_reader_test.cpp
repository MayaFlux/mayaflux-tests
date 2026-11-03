#include "file_contract_fixture.hpp"

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

    TEST(FileMetadataTest, ReturnsPresentValuesAndReportsAbsentOrMismatchedValues)
    {
        IO::FileMetadata metadata;
        metadata.attributes["sample_rate"] = uint32_t { 48000 };

        EXPECT_EQ(metadata.get_attribute<uint32_t>("sample_rate"), 48000U);
        EXPECT_FALSE(metadata.get_attribute<uint32_t>("channels").has_value());
        EXPECT_FALSE(metadata.get_attribute<std::string>("sample_rate").has_value());
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

    TEST(FileRegionTest, MultidimensionalConversionCopiesAttributes)
    {
        IO::FileRegion file_region;
        file_region.start_coordinates = { 2, 3 };
        file_region.end_coordinates = { 5, 8 };
        file_region.attributes["quality"] = 0.75;

        const auto region = file_region.to_region();

        ASSERT_TRUE(region.get_attribute<double>("quality").has_value());
        EXPECT_DOUBLE_EQ(*region.get_attribute<double>("quality"), 0.75);
    }

}
}
