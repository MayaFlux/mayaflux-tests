#include "file_contract_fixture.hpp"
#include "../support/test_metadata.hpp"

#include "MayaFlux/IO/Composite/DelimitedTextReader.hpp"
#include "MayaFlux/IO/Composite/DelimitedTextWriter.hpp"
#include "MayaFlux/Kakshya/NDData/CompositeAccess.hpp"
#include "MayaFlux/Kakshya/NDData/CompositeInsertion.hpp"
#include "MayaFlux/Kakshya/Source/CompositeContainer.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::writable_temp>() }))

namespace MayaFlux::Test {
namespace {

    using IO::DelimitedTextInfo;
    using IO::DelimitedTextReader;
    using IO::DelimitedTextWriter;
    using IO::FileRegion;
    using Kakshya::CompositeArray;
    using Kakshya::CompositeInsertion;
    using Kakshya::CompositeLayout;

    bool contains(const std::string& text, std::string_view needle)
    {
        return text.find(needle) != std::string::npos;
    }

    CompositeLayout people_layout()
    {
        CompositeLayout layout;
        layout.add_field<std::string>("name");
        layout.add_field<uint32_t>("rank");
        layout.add_field<double>("score");
        layout.add_field<std::string>("note");
        return layout;
    }

    CompositeLayout scored_layout()
    {
        CompositeLayout layout;
        layout.add_field<std::string>("name");
        layout.add_field<uint32_t>("rank");
        layout.add_field<double>("score");
        return layout;
    }

    CompositeLayout pair_layout()
    {
        CompositeLayout layout;
        layout.add_field<std::string>("name");
        layout.add_field<uint32_t>("rank");
        return layout;
    }

    CompositeLayout single_layout()
    {
        CompositeLayout layout;
        layout.add_field<std::string>("name");
        return layout;
    }

    CompositeLayout numeric_layout()
    {
        CompositeLayout layout;
        layout.add_field<bool>("flag");
        layout.add_field<signed char>("small");
        layout.add_field<uint64_t>("big");
        layout.add_field<int64_t>("wide");
        layout.add_field<float>("ratio");
        layout.add_field<double>("exact");
        return layout;
    }

    CompositeArray people_array()
    {
        CompositeArray array(people_layout());
        CompositeInsertion insertion(array);

        const auto ada = insertion.append();
        insertion.set_text(ada, "name", "Ada");
        insertion.set(ada, "rank", uint32_t { 1 });
        insertion.set(ada, "score", 0.95);
        insertion.set_text(ada, "note", "");

        const auto jo = insertion.append();
        insertion.set_text(jo, "name", "Jo");
        insertion.set(jo, "rank", uint32_t { 2 });

        const auto nia = insertion.append();
        insertion.set_text(nia, "name", "Nia, Jr.");
        insertion.set(nia, "rank", uint32_t { 3 });
        insertion.set(nia, "score", 0.74);
        insertion.set_text(nia, "note", "He said \"hello\"\nagain");

        return array;
    }

    CompositeArray pair_array(const std::vector<std::pair<std::string, uint32_t>>& rows)
    {
        CompositeArray array(pair_layout());
        CompositeInsertion insertion(array);

        for (const auto& [name, rank] : rows) {
            const auto row = insertion.append();
            insertion.set_text(row, "name", name);
            insertion.set(row, "rank", rank);
        }

        return array;
    }

    CompositeArray numeric_array()
    {
        CompositeArray array(numeric_layout());
        CompositeInsertion insertion(array);

        const auto row = insertion.append();
        insertion.set(row, "flag", true);
        insertion.set(row, "small", static_cast<signed char>(-7));
        insertion.set(row, "big", std::numeric_limits<uint64_t>::max());
        insertion.set(row, "wide", std::numeric_limits<int64_t>::min());
        insertion.set(row, "ratio", 0.1F);
        insertion.set(row, "exact", 0.1);

        return array;
    }

    class DelimitedTextFixture : public FileContractFixture {
    protected:
        [[nodiscard]] std::string path(const std::string& name) const
        {
            return (root() / name).string();
        }

        [[nodiscard]] std::string contents(const std::string& name) const
        {
            std::ifstream file(path(name), std::ios::binary);
            return { std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
        }
    };

    class DelimitedTextRoundTripTest
        : public DelimitedTextFixture
        , public ::testing::WithParamInterface<std::string> {
    };

    TEST_P(DelimitedTextRoundTripTest, PresenceValuesAndTextSurviveAFileTrip)
    {
        const auto file = "people" + GetParam();

        DelimitedTextWriter writer;
        ASSERT_TRUE(writer.write(path(file), people_array()));

        DelimitedTextReader reader;
        ASSERT_TRUE(reader.set_layout(people_layout()));
        ASSERT_TRUE(reader.open(path(file)));

        const auto rows = reader.read_composite();
        ASSERT_TRUE(rows);
        ASSERT_EQ(rows->size(), 3U);

        const auto ada = rows->at(0);
        const auto jo = rows->at(1);
        const auto nia = rows->at(2);

        EXPECT_EQ(ada->text("name"), std::string_view("Ada"));
        EXPECT_EQ(ada->get<uint32_t>("rank"), 1U);
        EXPECT_EQ(ada->get<double>("score"), 0.95);
        EXPECT_EQ(ada->text("note"), std::string_view(""));

        EXPECT_EQ(jo->text("name"), std::string_view("Jo"));
        EXPECT_FALSE(jo->has("score"));
        EXPECT_FALSE(jo->has("note"));

        EXPECT_EQ(nia->text("name"), std::string_view("Nia, Jr."));
        EXPECT_EQ(nia->get<double>("score"), 0.74);
        EXPECT_EQ(nia->text("note"), std::string_view("He said \"hello\"\nagain"));
    }

    INSTANTIATE_TEST_SUITE_P(
        Delimiters,
        DelimitedTextRoundTripTest,
        ::testing::Values(std::string(".csv"), std::string(".tsv")));

    TEST_F(DelimitedTextFixture, WritesHeaderAndOneLinePerElement)
    {
        DelimitedTextWriter writer;
        ASSERT_TRUE(writer.write(path("pairs.csv"), pair_array({ { "Ada", 1 }, { "Jo", 2 } })));

        EXPECT_EQ(contents("pairs.csv"), "name,rank\nAda,1\nJo,2\n");
    }

    TEST_F(DelimitedTextFixture, TsvUsesTabsFromItsExtension)
    {
        DelimitedTextWriter writer;
        ASSERT_TRUE(writer.write(path("pairs.tsv"), pair_array({ { "Ada", 1 } })));

        EXPECT_EQ(contents("pairs.tsv"), "name\trank\nAda\t1\n");
    }

    TEST_F(DelimitedTextFixture, DelimiterOverrideReplacesTheExtensionChoice)
    {
        DelimitedTextWriter writer;
        ASSERT_TRUE(writer.set_delimiter(';'));
        ASSERT_TRUE(writer.write(path("pairs.csv"), pair_array({ { "Ada", 1 } })));

        EXPECT_EQ(contents("pairs.csv"), "name;rank\nAda;1\n");
    }

    TEST_F(DelimitedTextFixture, HeaderCanBeOmitted)
    {
        DelimitedTextWriter writer;
        ASSERT_TRUE(writer.set_write_header(false));
        ASSERT_TRUE(writer.write(path("pairs.csv"), pair_array({ { "Ada", 1 } })));

        EXPECT_EQ(contents("pairs.csv"), "Ada,1\n");
    }

    TEST_F(DelimitedTextFixture, SettersRefuseWhileAFileIsOpen)
    {
        DelimitedTextWriter writer;
        ASSERT_TRUE(writer.open(path("open.csv"), pair_layout()));

        EXPECT_FALSE(writer.set_write_header(false));
        EXPECT_FALSE(writer.set_delimiter(';'));
        EXPECT_TRUE(writer.close());
    }

    TEST_F(DelimitedTextFixture, MissingFieldsAreEmptyCells)
    {
        CompositeArray array(pair_layout());
        CompositeInsertion insertion(array);
        const auto row = insertion.append();
        insertion.set(row, "rank", uint32_t { 1 });

        DelimitedTextWriter writer;
        ASSERT_TRUE(writer.write(path("sparse.csv"), array));

        EXPECT_EQ(contents("sparse.csv"), "name,rank\n,1\n");
    }

    TEST_F(DelimitedTextFixture, AllAbsentMultiColumnRowIsWrittenAsDelimiters)
    {
        CompositeArray array(pair_layout());
        CompositeInsertion insertion(array);
        EXPECT_EQ(insertion.append(), 0U);

        DelimitedTextWriter writer;
        ASSERT_TRUE(writer.write(path("empty_row.csv"), array));

        EXPECT_EQ(contents("empty_row.csv"), "name,rank\n,\n");
    }

    TEST_F(DelimitedTextFixture, PresentEmptyTextIsQuoted)
    {
        CompositeArray array(pair_layout());
        CompositeInsertion insertion(array);
        const auto row = insertion.append();
        insertion.set_text(row, "name", "");
        insertion.set(row, "rank", uint32_t { 1 });

        DelimitedTextWriter writer;
        ASSERT_TRUE(writer.write(path("quoted.csv"), array));

        EXPECT_EQ(contents("quoted.csv"), "name,rank\n\"\",1\n");
    }

    TEST_F(DelimitedTextFixture, QuotesDelimitersQuotesAndNewlines)
    {
        CompositeArray array(pair_layout());
        CompositeInsertion insertion(array);
        const auto row = insertion.append();
        insertion.set_text(row, "name", "Nia, \"Jr.\"\nx");
        insertion.set(row, "rank", uint32_t { 3 });

        DelimitedTextWriter writer;
        ASSERT_TRUE(writer.write(path("escaped.csv"), array));

        EXPECT_EQ(contents("escaped.csv"), "name,rank\n\"Nia, \"\"Jr.\"\"\nx\",3\n");
    }

    TEST_F(DelimitedTextFixture, WritesTheExpectedBytesForMixedRows)
    {
        DelimitedTextWriter writer;
        ASSERT_TRUE(writer.write(path("people.csv"), people_array()));

        EXPECT_EQ(contents("people.csv"),
            "name,rank,score,note\n"
            "Ada,1,0.95,\"\"\n"
            "Jo,2,,\n"
            "\"Nia, Jr.\",3,0.74,\"He said \"\"hello\"\"\nagain\"\n");
    }

    TEST_F(DelimitedTextFixture, NumbersUseTheShortestRoundTripForm)
    {
        DelimitedTextWriter writer;
        ASSERT_TRUE(writer.write(path("numbers.csv"), numeric_array()));

        EXPECT_EQ(contents("numbers.csv"),
            "flag,small,big,wide,ratio,exact\n"
            "true,-7,18446744073709551615,-9223372036854775808,0.1,0.1\n");
    }

    TEST_F(DelimitedTextFixture, NonFiniteDoubleFailsTheWrite)
    {
        CompositeLayout layout;
        layout.add_field<double>("score");

        CompositeArray array(std::move(layout));
        CompositeInsertion insertion(array);
        const auto row = insertion.append();
        insertion.set(row, "score", std::numeric_limits<double>::quiet_NaN());

        DelimitedTextWriter writer;
        EXPECT_FALSE(writer.write(path("nan.csv"), array));
        EXPECT_FALSE(writer.get_last_error().empty());
    }

    TEST_F(DelimitedTextFixture, DynamicFieldsAreRefusedAtOpen)
    {
        CompositeLayout layout;
        layout.add_dynamic_field("cell");

        DelimitedTextWriter writer;
        EXPECT_FALSE(writer.open(path("dynamic.csv"), layout));
        EXPECT_TRUE(contains(writer.get_last_error(), "Unsupported"));
    }

    TEST_F(DelimitedTextFixture, BlobFieldsAreRefusedAtOpen)
    {
        CompositeLayout layout;
        layout.add_field<std::vector<uint8_t>>("blob");

        DelimitedTextWriter writer;
        EXPECT_FALSE(writer.open(path("blob.csv"), layout));
        EXPECT_TRUE(contains(writer.get_last_error(), "Unsupported"));
    }

    TEST_F(DelimitedTextFixture, OpenRefusesOtherExtensions)
    {
        DelimitedTextWriter writer;

        EXPECT_FALSE(writer.can_write(path("data.txt")));
        EXPECT_FALSE(writer.open(path("data.txt"), pair_layout()));
    }

    TEST_F(DelimitedTextFixture, OpenRefusesAnotherFileWhileOneIsOpen)
    {
        DelimitedTextWriter writer;
        ASSERT_TRUE(writer.open(path("first.csv"), pair_layout()));

        EXPECT_FALSE(writer.open(path("second.csv"), pair_layout()));
        EXPECT_TRUE(writer.close());
    }

    TEST_F(DelimitedTextFixture, OpenFailsWhenTheDirectoryDoesNotExist)
    {
        DelimitedTextWriter writer;

        EXPECT_FALSE(writer.open(path("missing/dir/out.csv"), pair_layout()));
        EXPECT_FALSE(writer.is_open());
    }

    TEST_F(DelimitedTextFixture, WriteRowsAppendsAcrossSlices)
    {
        const auto array = pair_array({ { "Ada", 1 }, { "Jo", 2 }, { "Nia", 3 } });

        DelimitedTextWriter writer;
        ASSERT_TRUE(writer.open(path("slices.csv"), array.layout()));
        ASSERT_TRUE(writer.write_rows(*array.slice(0, 2)));
        ASSERT_TRUE(writer.write_rows(*array.slice(2, 1)));
        EXPECT_EQ(writer.rows_written(), 3U);
        EXPECT_TRUE(writer.close());

        EXPECT_EQ(contents("slices.csv"), "name,rank\nAda,1\nJo,2\nNia,3\n");
    }

    TEST_F(DelimitedTextFixture, WriteRowsRefusesAnotherLayout)
    {
        const auto pairs = pair_array({ { "Ada", 1 } });
        const auto people = people_array();

        DelimitedTextWriter writer;
        ASSERT_TRUE(writer.open(path("mixed.csv"), pairs.layout()));
        EXPECT_FALSE(writer.write_rows(*people.slice(0, 1)));
        EXPECT_TRUE(contains(writer.get_last_error(), "differs"));
        EXPECT_TRUE(writer.close());
    }

    TEST_F(DelimitedTextFixture, WriteRowsBeforeOpenFails)
    {
        const auto array = pair_array({ { "Ada", 1 } });

        DelimitedTextWriter writer;
        EXPECT_FALSE(writer.write_rows(*array.slice(0, 1)));
        EXPECT_TRUE(contains(writer.get_last_error(), "No delimited text output is open"));
    }

    TEST_F(DelimitedTextFixture, EmptyArrayWritesOnlyTheHeader)
    {
        DelimitedTextWriter writer;
        ASSERT_TRUE(writer.write(path("empty.csv"), pair_array({})));

        EXPECT_EQ(contents("empty.csv"), "name,rank\n");
    }

    TEST_F(DelimitedTextFixture, SingleColumnRowWithNoValueIsRefused)
    {
        CompositeArray array(single_layout());
        CompositeInsertion insertion(array);
        EXPECT_EQ(insertion.append(), 0U);

        DelimitedTextWriter writer;
        EXPECT_FALSE(writer.write(path("single.csv"), array));
        EXPECT_FALSE(writer.get_last_error().empty());
    }

    TEST_F(DelimitedTextFixture, ReadsWithoutALayoutAsTextColumns)
    {
        write("untyped.csv", "a,b\n1,2\n");

        DelimitedTextReader reader;
        ASSERT_TRUE(reader.open(path("untyped.csv")));

        const auto layout = reader.get_layout();
        ASSERT_TRUE(layout);
        ASSERT_EQ(layout->fields().size(), 2U);
        EXPECT_EQ(layout->fields()[0].name, "a");
        EXPECT_EQ(layout->fields()[1].name, "b");
        EXPECT_EQ(layout->fields()[0].type, typeid(std::string));

        const auto rows = reader.read_composite();
        ASSERT_TRUE(rows);
        EXPECT_EQ(rows->at(0)->text("a"), std::string_view("1"));
        EXPECT_EQ(rows->at(0)->text("b"), std::string_view("2"));
    }

    TEST_F(DelimitedTextFixture, MetadataNamesTheFormatAndFields)
    {
        write("meta.csv", "a,b\n1,2\n");

        DelimitedTextReader reader;
        ASSERT_TRUE(reader.open(path("meta.csv")));

        const auto metadata = reader.get_metadata();
        ASSERT_TRUE(metadata);
        EXPECT_EQ(metadata->format, "csv");

        const auto* info = metadata->get<DelimitedTextInfo>();
        ASSERT_NE(info, nullptr);
        EXPECT_EQ(info->fields, (std::vector<std::string> { "a", "b" }));
    }

    TEST_F(DelimitedTextFixture, HeaderlessInputNeedsALayout)
    {
        write("headerless.csv", "Ada,1\n");

        DelimitedTextReader reader;
        ASSERT_TRUE(reader.set_has_header(false));

        EXPECT_FALSE(reader.open(path("headerless.csv")));
        EXPECT_TRUE(contains(reader.get_last_error(), "Headerless input requires"));
    }

    TEST_F(DelimitedTextFixture, HeaderlessInputUsesTheSuppliedLayout)
    {
        write("headerless.csv", "Ada,1\nJo,2\n");

        DelimitedTextReader reader;
        ASSERT_TRUE(reader.set_has_header(false));
        ASSERT_TRUE(reader.set_layout(pair_layout()));
        ASSERT_TRUE(reader.open(path("headerless.csv")));

        const auto rows = reader.read_composite();
        ASSERT_TRUE(rows);
        ASSERT_EQ(rows->size(), 2U);
        EXPECT_EQ(rows->at(0)->text("name"), std::string_view("Ada"));
        EXPECT_EQ(rows->at(1)->get<uint32_t>("rank"), 2U);
    }

    TEST_F(DelimitedTextFixture, HeaderNamesMustMatchTheSuppliedLayout)
    {
        write("names.csv", "name,rank\nAda,1\n");

        CompositeLayout layout;
        layout.add_field<std::string>("name");
        layout.add_field<double>("score");

        DelimitedTextReader reader;
        ASSERT_TRUE(reader.set_layout(std::move(layout)));

        EXPECT_FALSE(reader.open(path("names.csv")));
        EXPECT_TRUE(contains(reader.get_last_error(), "mismatched"));
    }

    TEST_F(DelimitedTextFixture, HeaderCountMustMatchTheSuppliedLayout)
    {
        write("count.csv", "name,rank,score\nAda,1,0.5\n");

        DelimitedTextReader reader;
        ASSERT_TRUE(reader.set_layout(pair_layout()));

        EXPECT_FALSE(reader.open(path("count.csv")));
        EXPECT_TRUE(contains(reader.get_last_error(), "Header does not match"));
    }

    TEST_F(DelimitedTextFixture, UnsupportedSuppliedFieldIsRefused)
    {
        write("blob.csv", "blob\nx\n");

        CompositeLayout layout;
        layout.add_field<std::vector<uint8_t>>("blob");

        DelimitedTextReader reader;
        ASSERT_TRUE(reader.set_layout(std::move(layout)));

        EXPECT_FALSE(reader.open(path("blob.csv")));
        EXPECT_TRUE(contains(reader.get_last_error(), "Unsupported"));
    }

    TEST_F(DelimitedTextFixture, ReaderOpenRefusesOtherExtensions)
    {
        write("data.txt", "a\n1\n");

        DelimitedTextReader reader;
        EXPECT_FALSE(reader.can_read(path("data.txt")));
        EXPECT_FALSE(reader.open(path("data.txt")));
    }

    TEST_F(DelimitedTextFixture, MissingFileFailsToOpen)
    {
        DelimitedTextReader reader;

        EXPECT_FALSE(reader.open(path("nothing.csv")));
        EXPECT_FALSE(reader.is_open());
    }

    TEST_F(DelimitedTextFixture, EmptyFileHasNoHeader)
    {
        write("empty.csv", "");

        DelimitedTextReader reader;
        EXPECT_FALSE(reader.open(path("empty.csv")));
        EXPECT_TRUE(contains(reader.get_last_error(), "no header"));
    }

    TEST_F(DelimitedTextFixture, BlankOnlyFileHasNoHeader)
    {
        write("blank.csv", "\n\n\n");

        DelimitedTextReader reader;
        EXPECT_FALSE(reader.open(path("blank.csv")));
        EXPECT_TRUE(contains(reader.get_last_error(), "no header"));
    }

    TEST_F(DelimitedTextFixture, HeaderOnlyFileHasNoRows)
    {
        write("header.csv", "name,rank\n");

        DelimitedTextReader reader;
        ASSERT_TRUE(reader.set_layout(pair_layout()));
        ASSERT_TRUE(reader.open(path("header.csv")));

        const auto rows = reader.read_composite();
        ASSERT_TRUE(rows);
        EXPECT_TRUE(rows->empty());
    }

    TEST_F(DelimitedTextFixture, ByteOrderMarkIsStrippedFromTheHeader)
    {
        write("bom.csv", "\xEF\xBB\xBFname,rank\nAda,1\n");

        DelimitedTextReader reader;
        ASSERT_TRUE(reader.set_layout(pair_layout()));
        ASSERT_TRUE(reader.open(path("bom.csv")));

        const auto rows = reader.read_composite();
        ASSERT_TRUE(rows);
        EXPECT_EQ(rows->at(0)->text("name"), std::string_view("Ada"));
    }

    TEST_F(DelimitedTextFixture, CrLfLineEndingsAreAccepted)
    {
        write("crlf.csv", "name,rank\r\nAda,1\r\nJo,2\r\n");

        DelimitedTextReader reader;
        ASSERT_TRUE(reader.set_layout(pair_layout()));
        ASSERT_TRUE(reader.open(path("crlf.csv")));

        const auto rows = reader.read_composite();
        ASSERT_TRUE(rows);
        ASSERT_EQ(rows->size(), 2U);
        EXPECT_EQ(rows->at(1)->get<uint32_t>("rank"), 2U);
    }

    TEST_F(DelimitedTextFixture, QuotedEmptyIsPresentAndUnquotedEmptyIsAbsent)
    {
        write("empty_cells.csv", "name,score\n,\n\"\",\"\"\n");

        CompositeLayout layout;
        layout.add_field<std::string>("name");
        layout.add_field<double>("score");

        DelimitedTextReader reader;
        ASSERT_TRUE(reader.set_layout(std::move(layout)));
        ASSERT_TRUE(reader.open(path("empty_cells.csv")));

        const auto rows = reader.read_composite();
        ASSERT_TRUE(rows);
        ASSERT_EQ(rows->size(), 2U);

        EXPECT_FALSE(rows->at(0)->has("name"));
        EXPECT_FALSE(rows->at(0)->has("score"));

        EXPECT_EQ(rows->at(1)->text("name"), std::string_view(""));
        EXPECT_FALSE(rows->at(1)->has("score"));
    }

    TEST_F(DelimitedTextFixture, MissingTrailingCellsAreAbsent)
    {
        write("short.csv", "name,rank\nAda\n");

        DelimitedTextReader reader;
        ASSERT_TRUE(reader.set_layout(pair_layout()));
        ASSERT_TRUE(reader.open(path("short.csv")));

        const auto rows = reader.read_composite();
        ASSERT_TRUE(rows);
        EXPECT_EQ(rows->at(0)->text("name"), std::string_view("Ada"));
        EXPECT_FALSE(rows->at(0)->has("rank"));
    }

    TEST_F(DelimitedTextFixture, NumericValuesReadBackExactly)
    {
        DelimitedTextWriter writer;
        ASSERT_TRUE(writer.write(path("numbers.csv"), numeric_array()));

        DelimitedTextReader reader;
        ASSERT_TRUE(reader.set_layout(numeric_layout()));
        ASSERT_TRUE(reader.open(path("numbers.csv")));

        const auto rows = reader.read_composite();
        ASSERT_TRUE(rows);
        ASSERT_EQ(rows->size(), 1U);

        const auto row = rows->at(0);
        EXPECT_EQ(row->get<bool>("flag"), true);
        EXPECT_EQ(row->get<signed char>("small"), static_cast<signed char>(-7));
        EXPECT_EQ(row->get<uint64_t>("big"), std::numeric_limits<uint64_t>::max());
        EXPECT_EQ(row->get<int64_t>("wide"), std::numeric_limits<int64_t>::min());
        EXPECT_EQ(row->get<float>("ratio"), 0.1F);
        EXPECT_EQ(row->get<double>("exact"), 0.1);
    }

    struct BadCell {
        const char* rank;
        const char* score;
    };

    class DelimitedTextBadCellTest
        : public DelimitedTextFixture
        , public ::testing::WithParamInterface<BadCell> {
    };

    TEST_P(DelimitedTextBadCellTest, UnparseableNumbersFailTheRead)
    {
        const auto& cell = GetParam();
        write("bad.csv", std::string("name,rank,score\nAda,") + cell.rank + "," + cell.score + "\n");

        DelimitedTextReader reader;
        ASSERT_TRUE(reader.set_layout(scored_layout()));
        ASSERT_TRUE(reader.open(path("bad.csv")));

        EXPECT_FALSE(reader.read_composite());
        EXPECT_TRUE(contains(reader.get_last_error(), "Invalid value in field"));
    }

    INSTANTIATE_TEST_SUITE_P(
        Numbers,
        DelimitedTextBadCellTest,
        ::testing::Values(
            BadCell { "abc", "0.5" },
            BadCell { "1.5", "0.5" },
            BadCell { "-1", "0.5" },
            BadCell { "4294967296", "0.5" },
            BadCell { "1", "nan" },
            BadCell { "1", "inf" },
            BadCell { "1", "1e400" },
            BadCell { "1", "0.5x" }));

    class DelimitedTextMalformedTest
        : public DelimitedTextFixture
        , public ::testing::WithParamInterface<std::string> {
    };

    TEST_P(DelimitedTextMalformedTest, MalformedQuotingFailsTheRead)
    {
        write("malformed.csv", "name,rank\n" + GetParam());

        DelimitedTextReader reader;
        ASSERT_TRUE(reader.set_layout(pair_layout()));
        ASSERT_TRUE(reader.open(path("malformed.csv")));

        EXPECT_FALSE(reader.read_composite());
        EXPECT_FALSE(reader.get_last_error().empty());
    }

    INSTANTIATE_TEST_SUITE_P(
        Quoting,
        DelimitedTextMalformedTest,
        ::testing::Values(
            std::string("\"Ada,1\n"),
            std::string("\"Ada\"x,1\n"),
            std::string("Ad\"a,1\n")));

    TEST_F(DelimitedTextFixture, TooManyFieldsFailTheRead)
    {
        write("wide.csv", "name,rank\nAda,1,extra\n");

        DelimitedTextReader reader;
        ASSERT_TRUE(reader.set_layout(pair_layout()));
        ASSERT_TRUE(reader.open(path("wide.csv")));

        EXPECT_FALSE(reader.read_composite());
        EXPECT_TRUE(contains(reader.get_last_error(), "more fields"));
    }

    TEST_F(DelimitedTextFixture, ReadNextReturnsBoundedWindows)
    {
        write("rows.csv", "name\nr0\nr1\nr2\nr3\nr4\n");

        DelimitedTextReader reader;
        ASSERT_TRUE(reader.set_layout(single_layout()));
        ASSERT_TRUE(reader.open(path("rows.csv")));

        const auto first = reader.read_next(2);
        ASSERT_TRUE(first);
        EXPECT_EQ(first->size(), 2U);
        EXPECT_EQ(reader.get_read_position(), (std::vector<uint64_t> { 2 }));

        const auto second = reader.read_next(2);
        ASSERT_TRUE(second);
        EXPECT_EQ(second->at(0)->text("name"), std::string_view("r2"));
        EXPECT_EQ(reader.get_read_position(), (std::vector<uint64_t> { 4 }));

        const auto third = reader.read_next(2);
        ASSERT_TRUE(third);
        EXPECT_EQ(third->size(), 1U);
        EXPECT_EQ(third->at(0)->text("name"), std::string_view("r4"));

        const auto fourth = reader.read_next(2);
        ASSERT_TRUE(fourth);
        EXPECT_TRUE(fourth->empty());
    }

    TEST_F(DelimitedTextFixture, ReadNextZeroReturnsEmptyWithoutAdvancing)
    {
        write("rows.csv", "name\nr0\n");

        DelimitedTextReader reader;
        ASSERT_TRUE(reader.set_layout(single_layout()));
        ASSERT_TRUE(reader.open(path("rows.csv")));

        const auto none = reader.read_next(0);
        ASSERT_TRUE(none);
        EXPECT_TRUE(none->empty());
        EXPECT_EQ(reader.get_read_position(), (std::vector<uint64_t> { 0 }));
    }

    TEST_F(DelimitedTextFixture, ReadCompositeRegionSelectsInclusiveRows)
    {
        write("rows.csv", "name\nr0\nr1\nr2\nr3\nr4\n");

        DelimitedTextReader reader;
        ASSERT_TRUE(reader.set_layout(single_layout()));
        ASSERT_TRUE(reader.open(path("rows.csv")));

        FileRegion region;
        region.start_coordinates = { 1 };
        region.end_coordinates = { 2 };

        const auto selected = reader.read_composite_region(region);
        ASSERT_TRUE(selected);
        ASSERT_EQ(selected->size(), 2U);
        EXPECT_EQ(selected->at(0)->text("name"), std::string_view("r1"));
        EXPECT_EQ(selected->at(1)->text("name"), std::string_view("r2"));
    }

    TEST_F(DelimitedTextFixture, ReadCompositeRegionRefusesInvalidAndOverlongRegions)
    {
        write("rows.csv", "name\nr0\nr1\nr2\nr3\nr4\n");

        DelimitedTextReader reader;
        ASSERT_TRUE(reader.set_layout(single_layout()));
        ASSERT_TRUE(reader.open(path("rows.csv")));

        FileRegion overlong;
        overlong.start_coordinates = { 3 };
        overlong.end_coordinates = { 5 };
        EXPECT_FALSE(reader.read_composite_region(overlong));
        EXPECT_FALSE(reader.get_last_error().empty());

        FileRegion inverted;
        inverted.start_coordinates = { 2 };
        inverted.end_coordinates = { 1 };
        EXPECT_FALSE(reader.read_composite_region(inverted));
        EXPECT_FALSE(reader.get_last_error().empty());
    }

    TEST_F(DelimitedTextFixture, SeekReplaysFromTheRequestedRecord)
    {
        write("rows.csv", "name\nr0\nr1\nr2\n");

        DelimitedTextReader reader;
        ASSERT_TRUE(reader.set_layout(single_layout()));
        ASSERT_TRUE(reader.open(path("rows.csv")));

        ASSERT_TRUE(reader.read_next(3));
        ASSERT_TRUE(reader.seek({ 1 }));

        const auto replayed = reader.read_next(1);
        ASSERT_TRUE(replayed);
        EXPECT_EQ(replayed->at(0)->text("name"), std::string_view("r1"));

        ASSERT_TRUE(reader.seek({ 0 }));
        const auto rewound = reader.read_next(1);
        ASSERT_TRUE(rewound);
        EXPECT_EQ(rewound->at(0)->text("name"), std::string_view("r0"));
    }

    TEST_F(DelimitedTextFixture, SeekPastTheEndLeavesNothingToRead)
    {
        write("rows.csv", "name\nr0\nr1\n");

        DelimitedTextReader reader;
        ASSERT_TRUE(reader.set_layout(single_layout()));
        ASSERT_TRUE(reader.open(path("rows.csv")));

        EXPECT_FALSE(reader.seek({ 99 }));

        const auto rest = reader.read_next(5);
        ASSERT_TRUE(rest);
        EXPECT_TRUE(rest->empty());
    }

    TEST_F(DelimitedTextFixture, SeekRequiresOneCoordinate)
    {
        write("rows.csv", "name\nr0\n");

        DelimitedTextReader reader;
        ASSERT_TRUE(reader.set_layout(single_layout()));
        ASSERT_TRUE(reader.open(path("rows.csv")));

        EXPECT_FALSE(reader.seek({ 0, 0 }));
    }

    TEST_F(DelimitedTextFixture, CreateCompositeContainerHoldsEveryRow)
    {
        write("rows.csv", "name\nr0\nr1\nr2\nr3\nr4\n");

        DelimitedTextReader reader;
        ASSERT_TRUE(reader.set_layout(single_layout()));
        ASSERT_TRUE(reader.open(path("rows.csv")));

        const auto container = reader.create_composite_container(2);
        ASSERT_TRUE(container);
        EXPECT_EQ(container->get_total_elements(), 5U);
        EXPECT_EQ(container->get_data().at(4)->text("name"), std::string_view("r4"));
    }

    TEST_F(DelimitedTextFixture, LayoutIsAvailableOnlyWhileOpen)
    {
        write("rows.csv", "name\nr0\n");

        DelimitedTextReader reader;
        EXPECT_FALSE(reader.get_layout());

        ASSERT_TRUE(reader.set_layout(single_layout()));
        ASSERT_TRUE(reader.open(path("rows.csv")));
        ASSERT_TRUE(reader.get_layout());

        reader.close();
        EXPECT_FALSE(reader.get_layout());
        EXPECT_FALSE(reader.read_next(1));
        EXPECT_TRUE(contains(reader.get_last_error(), "No delimited text file is open"));
    }

    TEST_F(DelimitedTextFixture, ReaderSettersRefuseWhileAFileIsOpen)
    {
        write("rows.csv", "name\nr0\n");

        DelimitedTextReader reader;
        ASSERT_TRUE(reader.set_layout(single_layout()));
        ASSERT_TRUE(reader.open(path("rows.csv")));

        EXPECT_FALSE(reader.set_layout(pair_layout()));
        EXPECT_FALSE(reader.set_has_header(false));
        EXPECT_FALSE(reader.set_delimiter(';'));
    }

    TEST_F(DelimitedTextFixture, DelimiterRejectsReservedCharacters)
    {
        DelimitedTextReader reader;

        EXPECT_FALSE(reader.set_delimiter('\0'));
        EXPECT_FALSE(reader.set_delimiter('"'));
        EXPECT_FALSE(reader.set_delimiter('\n'));
        EXPECT_FALSE(reader.set_delimiter('\r'));
        EXPECT_TRUE(reader.set_delimiter(';'));
    }

    TEST_F(DelimitedTextFixture, DelimiterOverrideReadsSemicolonFiles)
    {
        write("semi.csv", "name;rank\nAda;1\n");

        DelimitedTextReader reader;
        ASSERT_TRUE(reader.set_delimiter(';'));
        ASSERT_TRUE(reader.set_layout(pair_layout()));
        ASSERT_TRUE(reader.open(path("semi.csv")));

        const auto rows = reader.read_composite();
        ASSERT_TRUE(rows);
        EXPECT_EQ(rows->at(0)->get<uint32_t>("rank"), 1U);
    }

    TEST_F(DelimitedTextFixture, BlankLinesAreSkipped)
    {
        write("blanks.csv", "name,rank\n\nAda,1\n\n\nJo,2\n\n");

        DelimitedTextReader reader;
        ASSERT_TRUE(reader.set_layout(pair_layout()));
        ASSERT_TRUE(reader.open(path("blanks.csv")));

        const auto rows = reader.read_composite();
        ASSERT_TRUE(rows);
        ASSERT_EQ(rows->size(), 2U);
        EXPECT_EQ(rows->at(0)->text("name"), std::string_view("Ada"));
        EXPECT_EQ(rows->at(1)->text("name"), std::string_view("Jo"));
    }

    TEST_F(DelimitedTextFixture, BlankLinesBeforeTheHeaderAreSkipped)
    {
        write("leading.csv", "\n\nname,rank\nAda,1\n");

        DelimitedTextReader reader;
        ASSERT_TRUE(reader.set_layout(pair_layout()));
        ASSERT_TRUE(reader.open(path("leading.csv")));

        const auto rows = reader.read_composite();
        ASSERT_TRUE(rows);
        ASSERT_EQ(rows->size(), 1U);
        EXPECT_EQ(rows->at(0)->text("name"), std::string_view("Ada"));
    }

    TEST_F(DelimitedTextFixture, BlankLinesAreNotCountedAsRecords)
    {
        write("positions.csv", "name,rank\nAda,1\n\nJo,2\n\n");

        DelimitedTextReader reader;
        ASSERT_TRUE(reader.set_layout(pair_layout()));
        ASSERT_TRUE(reader.open(path("positions.csv")));

        const auto ada = reader.read_next(1);
        ASSERT_TRUE(ada);
        EXPECT_EQ(ada->at(0)->text("name"), std::string_view("Ada"));
        EXPECT_EQ(reader.get_read_position(), (std::vector<uint64_t> { 1 }));

        const auto jo = reader.read_next(1);
        ASSERT_TRUE(jo);
        EXPECT_EQ(jo->at(0)->text("name"), std::string_view("Jo"));
        EXPECT_EQ(reader.get_read_position(), (std::vector<uint64_t> { 2 }));

        ASSERT_TRUE(reader.seek({ 1 }));
        const auto replayed = reader.read_next(1);
        ASSERT_TRUE(replayed);
        EXPECT_EQ(replayed->at(0)->text("name"), std::string_view("Jo"));
    }

    TEST_F(DelimitedTextFixture, CarriageReturnOnlyBlankLinesAreSkipped)
    {
        write("crlf_blank.csv", "name,rank\r\n\r\nAda,1\r\n\r\n");

        DelimitedTextReader reader;
        ASSERT_TRUE(reader.set_layout(pair_layout()));
        ASSERT_TRUE(reader.open(path("crlf_blank.csv")));

        const auto rows = reader.read_composite();
        ASSERT_TRUE(rows);
        ASSERT_EQ(rows->size(), 1U);
    }

    TEST_F(DelimitedTextFixture, QuotedEmptySingleCellIsNotABlankLine)
    {
        write("quoted_single.csv", "name\n\"\"\nAda\n");

        DelimitedTextReader reader;
        ASSERT_TRUE(reader.set_layout(single_layout()));
        ASSERT_TRUE(reader.open(path("quoted_single.csv")));

        const auto rows = reader.read_composite();
        ASSERT_TRUE(rows);
        ASSERT_EQ(rows->size(), 2U);
        EXPECT_EQ(rows->at(0)->text("name"), std::string_view(""));
    }

    TEST_F(DelimitedTextFixture, AllAbsentMultiColumnRowsRoundTrip)
    {
        write("absent_rows.csv", "name,rank\n,\nAda,1\n");

        DelimitedTextReader reader;
        ASSERT_TRUE(reader.set_layout(pair_layout()));
        ASSERT_TRUE(reader.open(path("absent_rows.csv")));

        const auto rows = reader.read_composite();
        ASSERT_TRUE(rows);
        ASSERT_EQ(rows->size(), 2U);
        EXPECT_FALSE(rows->at(0)->has("name"));
        EXPECT_FALSE(rows->at(0)->has("rank"));
        EXPECT_EQ(rows->at(1)->text("name"), std::string_view("Ada"));
    }

}
}
