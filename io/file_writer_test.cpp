#include "file_contract_fixture.hpp"

#include "MayaFlux/IO/FileWriter.hpp"

namespace MayaFlux::Test {
namespace {

    struct WriterPathCase {
        const char* input;
        const char* expected;
    };

    class FileWriterPathTest
        : public FileContractFixture
        , public ::testing::WithParamInterface<WriterPathCase> {
    };

    TEST_P(FileWriterPathTest, BareNamesUseTheSuppliedOutputDirectory)
    {
        const auto& param = GetParam();
        const auto output = root() / "output";

        EXPECT_EQ(IO::resolve_write_path(param.input, output.string()),
            (output / param.expected).string());
    }

    INSTANTIATE_TEST_SUITE_P(
        Names,
        FileWriterPathTest,
        ::testing::Values(
            WriterPathCase { "notes.txt", "notes.txt" },
            WriterPathCase { "image.png", "image.png" }));

    TEST_F(FileContractFixture, AbsoluteWriterPathIsUnchanged)
    {
        const auto path = root() / "output" / "notes.txt";

        EXPECT_EQ(IO::resolve_write_path(path.string()), path.string());
    }

    struct SequenceCase {
        const char* pattern;
        uint64_t frame;
        const char* expected;
    };

    class FileWriterSequenceTest : public FileContractFixture,
                                   public ::testing::WithParamInterface<SequenceCase> { };

    TEST_P(FileWriterSequenceTest, FormatsTheFrameInTheOutputName)
    {
        const auto& param = GetParam();
        const auto resolved = IO::resolve_sequence_path(param.pattern, param.frame);

        EXPECT_EQ(std::filesystem::path(resolved).filename(), param.expected);
    }

    INSTANTIATE_TEST_SUITE_P(
        Frames,
        FileWriterSequenceTest,
        ::testing::Values(
            SequenceCase { "smoke.{:04}.vdb", 7, "smoke.0007.vdb" },
            SequenceCase { "smoke.{:04}.vdb", 42, "smoke.0042.vdb" },
            SequenceCase { "frame.{}.png", 12, "frame.12.png" }));

    TEST_F(FileContractFixture, SequencePathRejectsMultipleReplacementFields)
    {
        EXPECT_THROW(IO::resolve_sequence_path("frame.{}.{}.png", 7), std::format_error);
    }

}
}
