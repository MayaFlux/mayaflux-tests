#include "file_contract_fixture.hpp"
#include "../support/test_metadata.hpp"

#include "MayaFlux/IO/Composite/DelimitedTextReader.hpp"
#include "MayaFlux/IO/Composite/DelimitedTextWriter.hpp"
#include "MayaFlux/IO/Composite/XmlReader.hpp"
#include "MayaFlux/IO/Composite/XmlWriter.hpp"
#include "MayaFlux/IO/StagedFileSet.hpp"
#include "MayaFlux/Kakshya/NDData/CompositeInsertion.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::writable_temp>() }))

namespace MayaFlux::Test {
namespace {

    using IO::StagedFileSet;

    StagedFileSet::WriteFn text_writer(const std::string& content)
    {
        return [content](const std::string& path) {
            {
                std::ofstream out(path, std::ios::binary);
                out << content;
            }
            return std::filesystem::exists(path);
        };
    }

    StagedFileSet::WriteFn failing_writer()
    {
        return [](const std::string&) { return false; };
    }

    StagedFileSet::WriteFn throwing_writer()
    {
        return [](const std::string&) -> bool { throw std::exception(); };
    }

    StagedFileSet::WriteFn silent_writer()
    {
        return [](const std::string&) { return true; };
    }

    std::string read_file(const std::filesystem::path& path)
    {
        std::ifstream in(path, std::ios::binary);
        return { std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
    }

    class StagedFileSetFixture : public FileContractFixture {
    protected:
        [[nodiscard]] std::string out_dir() const
        {
            return (root() / "out").string();
        }

        [[nodiscard]] std::filesystem::path out_path(const std::string& name) const
        {
            return root() / "out" / name;
        }

        [[nodiscard]] std::string path_of(const std::string& name) const
        {
            return (root() / name).string();
        }

        [[nodiscard]] bool staging_exists(const StagedFileSet& set) const
        {
            return std::filesystem::exists(set.staging_directory());
        }
    };

    TEST_F(StagedFileSetFixture, CommitPromotesEveryFileInWriteOrder)
    {
        StagedFileSet set(out_dir());
        std::string error;
        ASSERT_TRUE(set.write("a.txt", text_writer("alpha"), error)) << error;
        ASSERT_TRUE(set.write("b.txt", text_writer("beta"), error)) << error;

        ASSERT_TRUE(set.commit(error)) << error;
        EXPECT_EQ(read_file(out_path("a.txt")), "alpha");
        EXPECT_EQ(read_file(out_path("b.txt")), "beta");
        EXPECT_FALSE(staging_exists(set));
    }

    TEST_F(StagedFileSetFixture, WriterReceivesAPathInsideStagingNotTheTarget)
    {
        StagedFileSet set(out_dir());
        std::string seen;
        std::string error;
        ASSERT_TRUE(set.write("a.txt", [&](const std::string& path) {
            seen = path;
            return text_writer("alpha")(path);
        }, error)) << error;

        EXPECT_EQ(std::filesystem::path(seen).parent_path(), std::filesystem::path(set.staging_directory()));
        EXPECT_FALSE(std::filesystem::exists(out_path("a.txt")));
    }

    TEST_F(StagedFileSetFixture, NothingIsVisibleInTheTargetBeforeCommit)
    {
        StagedFileSet set(out_dir());
        std::string error;
        ASSERT_TRUE(set.write("a.txt", text_writer("alpha"), error)) << error;

        EXPECT_FALSE(std::filesystem::exists(out_path("a.txt")));
        EXPECT_TRUE(staging_exists(set));
    }

    TEST_F(StagedFileSetFixture, DirectoryAccessorReturnsTheResolvedTarget)
    {
        StagedFileSet set(out_dir());

        EXPECT_EQ(set.directory(), out_dir());
        EXPECT_EQ(std::filesystem::path(set.staging_directory()).parent_path(), std::filesystem::path(out_dir()));
    }

    TEST_F(StagedFileSetFixture, FailedWriteBlocksEveryPromotion)
    {
        StagedFileSet set(out_dir());
        std::string error;
        ASSERT_TRUE(set.write("a.txt", text_writer("alpha"), error)) << error;
        EXPECT_FALSE(set.write("b.txt", failing_writer(), error));
        EXPECT_FALSE(error.empty());

        EXPECT_FALSE(set.commit(error));
        EXPECT_FALSE(std::filesystem::exists(out_path("a.txt")));
        EXPECT_FALSE(std::filesystem::exists(out_path("b.txt")));
        EXPECT_FALSE(staging_exists(set));
    }

    TEST_F(StagedFileSetFixture, WriterThatThrowsIsReportedNotRethrown)
    {
        StagedFileSet set(out_dir());
        std::string error;

        EXPECT_FALSE(set.write("a.txt", throwing_writer(), error));
        EXPECT_NE(error.find("threw"), std::string::npos);
        EXPECT_FALSE(set.commit(error));
    }

    TEST_F(StagedFileSetFixture, WriterThatReportsSuccessWithoutAFileFails)
    {
        StagedFileSet set(out_dir());
        std::string error;

        EXPECT_FALSE(set.write("a.txt", silent_writer(), error));
        EXPECT_FALSE(set.commit(error));
        EXPECT_FALSE(std::filesystem::exists(out_path("a.txt")));
    }

    TEST_F(StagedFileSetFixture, InvalidNamesAreRefused)
    {
        const std::vector<std::string> names { "", ".", "..", "sub/a.txt", "sub\\a.txt" };
        for (const auto& name : names) {
            StagedFileSet set(out_dir());
            std::string error;
            EXPECT_FALSE(set.write(name, text_writer("x"), error)) << "name: " << name;
            EXPECT_FALSE(error.empty()) << "name: " << name;
        }
    }

    TEST_F(StagedFileSetFixture, RefusedNameLeavesTheSetUsable)
    {
        StagedFileSet set(out_dir());
        std::string error;

        EXPECT_FALSE(set.write("sub/a.txt", text_writer("x"), error));
        ASSERT_TRUE(set.write("a.txt", text_writer("alpha"), error)) << error;
        ASSERT_TRUE(set.commit(error)) << error;
        EXPECT_EQ(read_file(out_path("a.txt")), "alpha");
    }

    TEST_F(StagedFileSetFixture, DuplicateNameIsRefusedWithoutBlockingCommit)
    {
        StagedFileSet set(out_dir());
        std::string error;
        ASSERT_TRUE(set.write("a.txt", text_writer("alpha"), error)) << error;

        EXPECT_FALSE(set.write("a.txt", text_writer("again"), error));
        ASSERT_TRUE(set.commit(error)) << error;
        EXPECT_EQ(read_file(out_path("a.txt")), "alpha");
    }

    TEST_F(StagedFileSetFixture, EmptySetCannotBeCommitted)
    {
        StagedFileSet set(out_dir());
        std::string error;

        EXPECT_FALSE(set.commit(error));
        EXPECT_FALSE(error.empty());
    }

    TEST_F(StagedFileSetFixture, CommitIsOneShot)
    {
        StagedFileSet set(out_dir());
        std::string error;
        ASSERT_TRUE(set.write("a.txt", text_writer("alpha"), error)) << error;
        ASSERT_TRUE(set.commit(error)) << error;

        EXPECT_FALSE(set.commit(error));
        EXPECT_FALSE(set.write("b.txt", text_writer("beta"), error));
        EXPECT_FALSE(std::filesystem::exists(out_path("b.txt")));
    }

    TEST_F(StagedFileSetFixture, DestructionWithoutCommitLeavesTheTargetAlone)
    {
        {
            StagedFileSet set(out_dir());
            std::string error;
            ASSERT_TRUE(set.write("a.txt", text_writer("alpha"), error)) << error;
            ASSERT_TRUE(std::filesystem::exists(set.staging_directory()));
        }

        EXPECT_FALSE(std::filesystem::exists(out_path("a.txt")));
        EXPECT_FALSE(std::filesystem::exists(root() / "out" / ".staging"));
    }

    TEST_F(StagedFileSetFixture, StaleStagingFromAnEarlierRunIsRemoved)
    {
        write("out/.staging/old.txt", "stale");

        StagedFileSet set(out_dir());

        EXPECT_FALSE(std::filesystem::exists(root() / "out" / ".staging" / "old.txt"));
    }

    TEST_F(StagedFileSetFixture, CommitReplacesAnExistingFileOfTheSameName)
    {
        write("out/a.txt", "old");

        StagedFileSet set(out_dir());
        std::string error;
        ASSERT_TRUE(set.write("a.txt", text_writer("new"), error)) << error;
        ASSERT_TRUE(set.commit(error)) << error;

        EXPECT_EQ(read_file(out_path("a.txt")), "new");
    }

    TEST_F(StagedFileSetFixture, FilesOutsideTheSetSurvive)
    {
        write("out/keep.txt", "kept");

        StagedFileSet set(out_dir());
        std::string error;
        ASSERT_TRUE(set.write("a.txt", text_writer("alpha"), error)) << error;
        ASSERT_TRUE(set.commit(error)) << error;

        EXPECT_EQ(read_file(out_path("keep.txt")), "kept");
        EXPECT_EQ(read_file(out_path("a.txt")), "alpha");
    }

    Kakshya::CompositeLayout people_layout()
    {
        Kakshya::CompositeLayout layout;
        layout.add_field<std::string>("name");
        layout.add_field<uint32_t>("rank");
        return layout;
    }

    Kakshya::CompositeArray people_array()
    {
        Kakshya::CompositeArray array(people_layout());
        Kakshya::CompositeInsertion insertion(array);

        const auto ada = insertion.append();
        insertion.set_text(ada, "name", "Ada");
        insertion.set(ada, "rank", uint32_t { 1 });

        const auto nia = insertion.append();
        insertion.set_text(nia, "name", "Nia, Jr.");
        insertion.set(nia, "rank", uint32_t { 2 });

        return array;
    }

    TEST_F(StagedFileSetFixture, DelimitedRoundTripReadsTheCommittedFiles)
    {
        const auto people = people_array();

        StagedFileSet set(out_dir());
        std::string error;
        ASSERT_TRUE(set.write("people.csv", [&](const std::string& path) {
            return IO::DelimitedTextWriter().write(path, people);
        }, error)) << error;
        ASSERT_TRUE(set.write("manifest.txt", text_writer("rows=2\n"), error)) << error;
        ASSERT_TRUE(set.commit(error)) << error;

        const auto manifest = read_file(out_path("manifest.txt"));
        ASSERT_EQ(manifest, "rows=2\n");

        IO::DelimitedTextReader reader;
        ASSERT_TRUE(reader.set_layout(people_layout()));
        ASSERT_TRUE(reader.open(out_path("people.csv").string()));

        const auto rows = reader.read_composite();
        ASSERT_TRUE(rows);
        ASSERT_EQ(rows->size(), 2U);
        EXPECT_EQ(rows->at(0)->text("name"), std::string_view("Ada"));
        EXPECT_EQ(rows->at(0)->get<uint32_t>("rank"), 1U);
        EXPECT_EQ(rows->at(1)->text("name"), std::string_view("Nia, Jr."));
        EXPECT_EQ(rows->at(1)->get<uint32_t>("rank"), 2U);
    }

    TEST_F(StagedFileSetFixture, XmlRoundTripSurvivesReadWriteStageAndCommit)
    {
        write("source.xml", R"(<?xml version="1.0" encoding="UTF-8"?>
<forma version="1">
  <surface id="main">
    <element id="gain" kind="fader">label</element>
  </surface>
</forma>
)");

        IO::XmlReader source;
        ASSERT_TRUE(source.open(path_of("source.xml")));
        const auto original = source.read_composite();
        ASSERT_TRUE(original);

        StagedFileSet set(out_dir());
        std::string error;
        ASSERT_TRUE(set.write("copy.xml", [&](const std::string& path) {
            return IO::XmlWriter().write(path, *original);
        }, error)) << error;
        ASSERT_TRUE(set.commit(error)) << error;

        IO::XmlReader copy;
        ASSERT_TRUE(copy.open(out_path("copy.xml").string()));
        const auto reread = copy.read_composite();
        ASSERT_TRUE(reread);
        ASSERT_EQ(reread->size(), original->size());

        for (size_t i = 0; i < original->size(); ++i) {
            const auto before = original->at(i);
            const auto after = reread->at(i);
            EXPECT_EQ(after->text("kind"), before->text("kind")) << "row " << i;
            EXPECT_EQ(after->text("name"), before->text("name")) << "row " << i;
            EXPECT_EQ(after->text("value"), before->text("value")) << "row " << i;
            EXPECT_EQ(after->get<uint64_t>("parent"), before->get<uint64_t>("parent")) << "row " << i;
        }
    }

    TEST_F(StagedFileSetFixture, StagingThatCannotBeCreatedFailsTheWrite)
    {
        write("out", "a regular file is in the way");

        StagedFileSet set(out_dir());
        std::string error;

        EXPECT_FALSE(set.write("a.txt", text_writer("alpha"), error));
        EXPECT_FALSE(error.empty());
        EXPECT_FALSE(set.commit(error));
    }

    TEST_F(StagedFileSetFixture, RenameFailurePromotesEarlierFilesAndReportsTheRest)
    {
        std::filesystem::create_directories(root() / "out" / "b.txt");

        StagedFileSet set(out_dir());
        std::string error;
        ASSERT_TRUE(set.write("a.txt", text_writer("alpha"), error)) << error;
        ASSERT_TRUE(set.write("b.txt", text_writer("beta"), error)) << error;

        EXPECT_FALSE(set.commit(error));
        EXPECT_NE(error.find("b.txt"), std::string::npos);
        EXPECT_EQ(read_file(out_path("a.txt")), "alpha");
        EXPECT_FALSE(staging_exists(set));
    }

}
}
