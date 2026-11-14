#include "file_contract_fixture.hpp"
#include "../support/test_metadata.hpp"

#include "MayaFlux/IO/Composite/XmlReader.hpp"
#include "MayaFlux/IO/Composite/XmlWriter.hpp"
#include "MayaFlux/Kakshya/NDData/CompositeAccess.hpp"
#include "MayaFlux/Kakshya/NDData/CompositeInsertion.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::writable_temp>() }))

namespace MayaFlux::Test {
namespace {

    using IO::XmlReader;
    using IO::XmlWriter;
    using Kakshya::CompositeArray;
    using Kakshya::CompositeInsertion;

    class XmlFixture : public FileContractFixture {
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

    TEST_F(XmlFixture, ReaderExposesPreorderNodesAndAttributes)
    {
        write("nodes.xml",
            "<?xml version=\"1.0\"?><root id=\"main\">left<child/>right"
            "<![CDATA[<raw>]]><!--note--><?app mode?></root>");

        XmlReader reader;
        ASSERT_TRUE(reader.open(path("nodes.xml")));

        const auto layout = reader.get_layout();
        ASSERT_TRUE(layout);
        EXPECT_TRUE(layout->same_structure(IO::xml_composite_layout()));
        EXPECT_GT(layout->stride_bytes(), 0U);
        EXPECT_FALSE(reader.supports_streaming());

        const auto rows = reader.read_composite();
        ASSERT_TRUE(rows);
        ASSERT_EQ(rows->size(), 10U);

        EXPECT_EQ(rows->at(0)->text("kind"), std::string_view("declaration"));
        EXPECT_FALSE(rows->at(0)->has("parent"));
        EXPECT_EQ(rows->at(1)->text("name"), std::string_view("version"));
        EXPECT_EQ(rows->at(1)->get<uint64_t>("parent"), 0U);

        EXPECT_EQ(rows->at(2)->text("name"), std::string_view("root"));
        EXPECT_EQ(rows->at(3)->text("value"), std::string_view("main"));
        EXPECT_EQ(rows->at(3)->get<uint64_t>("parent"), 2U);

        EXPECT_EQ(rows->at(4)->text("value"), std::string_view("left"));
        EXPECT_EQ(rows->at(5)->text("name"), std::string_view("child"));
        EXPECT_EQ(rows->at(5)->get<uint64_t>("parent"), 2U);
        EXPECT_EQ(rows->at(6)->text("value"), std::string_view("right"));

        EXPECT_EQ(rows->at(7)->text("kind"), std::string_view("cdata"));
        EXPECT_EQ(rows->at(7)->text("value"), std::string_view("<raw>"));
        EXPECT_EQ(rows->at(8)->text("kind"), std::string_view("comment"));
        EXPECT_EQ(rows->at(8)->text("value"), std::string_view("note"));

        EXPECT_EQ(rows->at(9)->text("kind"), std::string_view("pi"));
        EXPECT_EQ(rows->at(9)->text("name"), std::string_view("app"));
        EXPECT_EQ(rows->at(9)->text("value"), std::string_view("mode"));
    }

    TEST_F(XmlFixture, ElementFieldsExposeAttributesAndRepeatedChildren)
    {
        write("fields.xml",
            "<?xml version=\"1.0\"?><root mode=\"fast\">"
            "<vertex position=\"0 1 2\"/><group><vertex position=\"9 9 9\"/></group>"
            "<vertex position=\"3 4 5\"/></root>");

        XmlReader reader;
        ASSERT_TRUE(reader.open(path("fields.xml")));
        const auto root = reader.root_element();
        ASSERT_TRUE(root);
        EXPECT_EQ(root->name(), "root");
        EXPECT_EQ(root->attribute("mode"), std::string_view("fast"));
        EXPECT_FALSE(root->attribute("position"));

        std::vector<std::string> fields;
        std::vector<uint64_t> vertex_rows;
        root->for_each_field([&](const IO::XmlFieldView& field) {
            fields.emplace_back(field.name);
            if (field.kind == IO::XmlFieldKind::Attribute) {
                EXPECT_EQ(field.name, "mode");
                EXPECT_EQ(field.value, "fast");
                EXPECT_FALSE(field.element);
            }
            if (field.kind == IO::XmlFieldKind::Element && field.name == "vertex") {
                ASSERT_TRUE(field.element);
                EXPECT_EQ(field.element->row(), field.row);
                vertex_rows.push_back(field.row);
            }
        });

        EXPECT_EQ(fields, (std::vector<std::string> { "mode", "vertex", "group", "vertex" }));
        ASSERT_EQ(vertex_rows.size(), 2U);
        EXPECT_EQ(reader.element_at(vertex_rows[0])->attribute("position"),
            std::string_view("0 1 2"));
        EXPECT_EQ(reader.element_at(vertex_rows[1])->attribute("position"),
            std::string_view("3 4 5"));

        size_t visited = 0;
        EXPECT_FALSE(root->for_each_field([&](const IO::XmlFieldView&) {
            ++visited;
            return false;
        }));
        EXPECT_EQ(visited, 1U);
    }

    TEST_F(XmlFixture, ElementFieldsSkipMixedContentAndKeepRawRowsAvailable)
    {
        write("mixed_fields.xml",
            "<!--before--><root empty=\"\">left<child/>right<![CDATA[raw]]><!--note--></root>");

        XmlReader reader;
        ASSERT_TRUE(reader.open(path("mixed_fields.xml")));
        const auto root = reader.root_element();
        ASSERT_TRUE(root);
        EXPECT_EQ(root->attribute("empty"), std::string_view(""));
        EXPECT_FALSE(reader.element_at(0));
        EXPECT_FALSE(reader.element_at(999));

        std::vector<IO::XmlFieldKind> kinds;
        root->for_each_field([&](const IO::XmlFieldView& field) {
            kinds.push_back(field.kind);
        });
        EXPECT_EQ(kinds, (std::vector<IO::XmlFieldKind> {
            IO::XmlFieldKind::Attribute, IO::XmlFieldKind::Element }));

        const auto rows = reader.read_composite();
        ASSERT_TRUE(rows);
        EXPECT_EQ(rows->at(0)->text("kind"), std::string_view("comment"));
        EXPECT_EQ(reader.root_element()->row(), root->row());

        reader.close();
        EXPECT_FALSE(reader.root_element());
        EXPECT_FALSE(reader.element_at(root->row()));
    }

    TEST_F(XmlFixture, ReaderKeepsEmptyValuesDistinctFromAbsentFields)
    {
        write("empty.xml", "<root empty=\"\"><child/></root>");

        XmlReader reader;
        ASSERT_TRUE(reader.open(path("empty.xml")));

        const auto rows = reader.read_composite();
        ASSERT_TRUE(rows);
        ASSERT_EQ(rows->size(), 3U);

        EXPECT_FALSE(rows->at(0)->has("value"));
        EXPECT_EQ(rows->at(1)->text("value"), std::string_view(""));
        EXPECT_FALSE(rows->at(2)->has("value"));
    }

    TEST_F(XmlFixture, BoundedReadsRetainGlobalParentIndices)
    {
        write("batch.xml", "<root id=\"x\"><child/></root>");

        XmlReader reader;
        ASSERT_TRUE(reader.open(path("batch.xml")));
        EXPECT_EQ(reader.get_dimension_sizes(), (std::vector<uint64_t> { 3 }));

        const auto first = reader.read_next(2);
        ASSERT_TRUE(first);
        ASSERT_EQ(first->size(), 2U);
        EXPECT_EQ(first->at(1)->get<uint64_t>("parent"), 0U);
        EXPECT_EQ(reader.get_read_position(), (std::vector<uint64_t> { 2 }));

        const auto second = reader.read_next(2);
        ASSERT_TRUE(second);
        ASSERT_EQ(second->size(), 1U);
        EXPECT_EQ(second->at(0)->text("name"), std::string_view("child"));
        EXPECT_EQ(second->at(0)->get<uint64_t>("parent"), 0U);

        const auto end = reader.read_next(1);
        ASSERT_TRUE(end);
        EXPECT_TRUE(end->empty());

        ASSERT_TRUE(reader.seek({ 1 }));
        const auto replayed = reader.read_next(1);
        ASSERT_TRUE(replayed);
        EXPECT_EQ(replayed->at(0)->text("name"), std::string_view("id"));
    }

    TEST_F(XmlFixture, ReaderPreservesPrologAndNestedParentIndices)
    {
        write("prolog.xml",
            "<!--before--><!DOCTYPE root><root><group><leaf>A&amp;B</leaf>"
            "</group></root><!--after-->");

        XmlReader reader;
        ASSERT_TRUE(reader.open(path("prolog.xml")));

        const auto rows = reader.read_composite();
        ASSERT_TRUE(rows);
        ASSERT_EQ(rows->size(), 7U);

        EXPECT_EQ(rows->at(0)->text("kind"), std::string_view("comment"));
        EXPECT_FALSE(rows->at(0)->has("parent"));
        EXPECT_EQ(rows->at(1)->text("kind"), std::string_view("doctype"));
        EXPECT_EQ(rows->at(1)->text("value"), std::string_view("root"));
        EXPECT_FALSE(rows->at(1)->has("parent"));

        EXPECT_EQ(rows->at(2)->text("name"), std::string_view("root"));
        EXPECT_EQ(rows->at(3)->get<uint64_t>("parent"), 2U);
        EXPECT_EQ(rows->at(4)->get<uint64_t>("parent"), 3U);
        EXPECT_EQ(rows->at(5)->get<uint64_t>("parent"), 4U);
        EXPECT_EQ(rows->at(5)->text("value"), std::string_view("A&B"));

        EXPECT_EQ(rows->at(6)->text("kind"), std::string_view("comment"));
        EXPECT_FALSE(rows->at(6)->has("parent"));
    }

    TEST_F(XmlFixture, ReaderRejectsMissingAndRootlessFiles)
    {
        XmlReader reader;
        EXPECT_TRUE(reader.can_read(path("data.XML")));
        EXPECT_FALSE(reader.can_read(path("data.txt")));

        EXPECT_FALSE(reader.open(path("missing.xml")));
        EXPECT_FALSE(reader.is_open());
        EXPECT_EQ(reader.get_last_error(), "Cannot open XML file");

        write("rootless.xml", "<?xml version=\"1.0\"?><!--only a comment-->");
        EXPECT_FALSE(reader.open(path("rootless.xml")));
        EXPECT_FALSE(reader.is_open());
        EXPECT_NE(reader.get_last_error().find("XML parse failed"), std::string::npos);
    }

    TEST_F(XmlFixture, ReaderEnforcesSeekBoundsAndResetsOnClose)
    {
        write("seek.xml", "<root><child/></root>");

        XmlReader reader;
        EXPECT_FALSE(reader.read_next(1));
        EXPECT_FALSE(reader.seek({ 0 }));
        ASSERT_TRUE(reader.open(path("seek.xml")));

        const auto empty = reader.read_next(0);
        ASSERT_TRUE(empty);
        EXPECT_TRUE(empty->empty());
        EXPECT_EQ(reader.get_read_position(), (std::vector<uint64_t> { 0 }));

        EXPECT_FALSE(reader.seek({}));
        EXPECT_FALSE(reader.seek({ 0, 1 }));
        EXPECT_FALSE(reader.seek({ 3 }));
        EXPECT_EQ(reader.get_read_position(), (std::vector<uint64_t> { 0 }));

        ASSERT_TRUE(reader.seek({ 2 }));
        const auto end = reader.read_composite();
        ASSERT_TRUE(end);
        EXPECT_TRUE(end->empty());

        reader.close();
        EXPECT_FALSE(reader.is_open());
        EXPECT_FALSE(reader.get_layout());
        EXPECT_FALSE(reader.get_metadata());
        EXPECT_TRUE(reader.get_dimension_sizes().empty());
        EXPECT_FALSE(reader.read_composite());
    }

    TEST_F(XmlFixture, WriterPreservesMixedContentAcrossBatches)
    {
        write("source.xml", "<root a=\"\">before<child/>after<![CDATA[<raw>]]></root>");

        XmlReader source;
        ASSERT_TRUE(source.open(path("source.xml")));

        const auto rows = source.read_composite();
        ASSERT_TRUE(rows);

        XmlWriter writer;
        ASSERT_TRUE(writer.open(path("copy.xml"), rows->layout()));

        const auto first = rows->slice(0, 3);
        const auto rest = rows->slice(3, rows->size() - 3);
        ASSERT_TRUE(first);
        ASSERT_TRUE(rest);
        ASSERT_TRUE(writer.write_rows(*first));
        ASSERT_TRUE(writer.write_rows(*rest));
        ASSERT_TRUE(writer.close());

        XmlReader copy;
        ASSERT_TRUE(copy.open(path("copy.xml")));

        const auto copied = copy.read_composite();
        ASSERT_TRUE(copied);
        ASSERT_EQ(copied->size(), rows->size());

        for (size_t i = 0; i < rows->size(); ++i) {
            const auto expected = rows->at(i);
            const auto actual = copied->at(i);
            ASSERT_TRUE(expected);
            ASSERT_TRUE(actual);

            EXPECT_EQ(actual->get<uint64_t>("parent"), expected->get<uint64_t>("parent"));
            EXPECT_EQ(actual->text("kind"), expected->text("kind"));
            EXPECT_EQ(actual->text("name"), expected->text("name"));
            EXPECT_EQ(actual->text("value"), expected->text("value"));
        }
    }

    TEST_F(XmlFixture, FullDocumentSurvivesReadWriteRead)
    {
        write("full_source.xml",
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            "<!DOCTYPE scene><!--before-->"
            "<scene title=\"A &amp; B\"><group id=\"g\">"
            "<item>alpha &lt; beta</item><![CDATA[<raw>&]]><?step ready?>"
            "</group><empty flag=\"\"/></scene><!--after-->");

        XmlReader source;
        ASSERT_TRUE(source.open(path("full_source.xml")));

        const auto original = source.read_composite();
        ASSERT_TRUE(original);
        ASSERT_EQ(original->size(), 16U);

        XmlWriter writer;
        ASSERT_TRUE(writer.write(path("full_copy.xml"), *original));

        XmlReader copy;
        ASSERT_TRUE(copy.open(path("full_copy.xml")));

        const auto roundtrip = copy.read_composite();
        ASSERT_TRUE(roundtrip);
        ASSERT_EQ(roundtrip->size(), original->size());

        for (size_t i = 0; i < original->size(); ++i) {
            const auto expected = original->at(i);
            const auto actual = roundtrip->at(i);
            ASSERT_TRUE(expected);
            ASSERT_TRUE(actual);

            EXPECT_EQ(actual->get<uint64_t>("parent"), expected->get<uint64_t>("parent"));
            EXPECT_EQ(actual->text("kind"), expected->text("kind"));
            EXPECT_EQ(actual->text("name"), expected->text("name"));
            EXPECT_EQ(actual->text("value"), expected->text("value"));
        }
    }

    TEST_F(XmlFixture, ManuallyConstructedRowsWriteReadableXml)
    {
        CompositeArray rows(IO::xml_composite_layout());
        CompositeInsertion insertion(rows);

        const auto root_row = insertion.append();
        ASSERT_TRUE(insertion.set_text(root_row, "kind", "element"));
        ASSERT_TRUE(insertion.set_text(root_row, "name", "settings"));

        const auto attribute = insertion.append();
        ASSERT_TRUE(insertion.set(attribute, "parent", uint64_t { 0 }));
        ASSERT_TRUE(insertion.set_text(attribute, "kind", "attribute"));
        ASSERT_TRUE(insertion.set_text(attribute, "name", "mode"));
        ASSERT_TRUE(insertion.set_text(attribute, "value", "live"));

        const auto child = insertion.append();
        ASSERT_TRUE(insertion.set(child, "parent", uint64_t { 0 }));
        ASSERT_TRUE(insertion.set_text(child, "kind", "element"));
        ASSERT_TRUE(insertion.set_text(child, "name", "label"));

        const auto value = insertion.append();
        ASSERT_TRUE(insertion.set(value, "parent", uint64_t { 2 }));
        ASSERT_TRUE(insertion.set_text(value, "kind", "text"));
        ASSERT_TRUE(insertion.set_text(value, "value", "A & B"));

        XmlWriter writer;
        ASSERT_TRUE(writer.write(path("settings.xml"), rows));
        EXPECT_NE(contents("settings.xml").find("A &amp; B"), std::string::npos);

        XmlReader reader;
        ASSERT_TRUE(reader.open(path("settings.xml")));

        const auto written = reader.read_composite();
        ASSERT_TRUE(written);
        ASSERT_EQ(written->size(), 4U);
        EXPECT_EQ(written->at(0)->text("name"), std::string_view("settings"));
        EXPECT_EQ(written->at(1)->text("value"), std::string_view("live"));
        EXPECT_EQ(written->at(2)->get<uint64_t>("parent"), 0U);
        EXPECT_EQ(written->at(3)->get<uint64_t>("parent"), 2U);
        EXPECT_EQ(written->at(3)->text("value"), std::string_view("A & B"));
    }

    TEST_F(XmlFixture, WriterEscapesTextAndNormalizesDeclarationEncoding)
    {
        write("encoded.xml",
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
            "<root a=\"a&amp;b\">x&lt;y</root>");

        XmlReader reader;
        ASSERT_TRUE(reader.open(path("encoded.xml")));

        auto rows = reader.read_composite();
        ASSERT_TRUE(rows);

        ASSERT_TRUE(rows->set_text(2, "value", "UTF-16"));

        XmlWriter writer;
        ASSERT_TRUE(writer.write(path("encoded_copy.xml"), *rows));

        const auto output = contents("encoded_copy.xml");
        EXPECT_NE(output.find("encoding=\"UTF-8\""), std::string::npos);
        EXPECT_NE(output.find("a&amp;b"), std::string::npos);
        EXPECT_NE(output.find("x&lt;y"), std::string::npos);

        XmlReader copy;
        ASSERT_TRUE(copy.open(path("encoded_copy.xml")));

        const auto written = copy.read_composite();
        ASSERT_TRUE(written);
        ASSERT_EQ(written->size(), 6U);
        EXPECT_EQ(written->at(2)->text("value"), std::string_view("UTF-8"));
        EXPECT_EQ(written->at(4)->text("value"), std::string_view("a&b"));
        EXPECT_EQ(written->at(5)->text("value"), std::string_view("x<y"));
    }

    TEST_F(XmlFixture, InvalidXmlFailsDuringOpen)
    {
        write("broken.xml", "<root><child></root>");

        XmlReader reader;
        EXPECT_FALSE(reader.open(path("broken.xml")));
        EXPECT_FALSE(reader.is_open());
        EXPECT_NE(reader.get_last_error().find("XML parse failed"), std::string::npos);
    }

    TEST_F(XmlFixture, WriterRejectsInvalidParentIndex)
    {
        CompositeArray rows(IO::xml_composite_layout());
        CompositeInsertion insertion(rows);

        const auto child = insertion.append();
        ASSERT_TRUE(insertion.set_text(child, "kind", "element"));
        ASSERT_TRUE(insertion.set_text(child, "name", "child"));
        ASSERT_TRUE(insertion.set(child, "parent", uint64_t { 99 }));

        XmlWriter writer;
        ASSERT_TRUE(writer.open(path("invalid_parent.xml"), rows.layout()));

        const auto slice = rows.slice(0, rows.size());
        ASSERT_TRUE(slice);
        EXPECT_FALSE(writer.write_rows(*slice));
        EXPECT_NE(writer.get_last_error().find("parent index"), std::string::npos);
        EXPECT_FALSE(writer.close());
    }

    TEST_F(XmlFixture, WriterRejectsDuplicateAttributes)
    {
        CompositeArray rows(IO::xml_composite_layout());
        CompositeInsertion insertion(rows);

        const auto root_row = insertion.append();
        ASSERT_TRUE(insertion.set_text(root_row, "kind", "element"));
        ASSERT_TRUE(insertion.set_text(root_row, "name", "root"));

        const auto first = insertion.append();
        ASSERT_TRUE(insertion.set(first, "parent", uint64_t { 0 }));
        ASSERT_TRUE(insertion.set_text(first, "kind", "attribute"));
        ASSERT_TRUE(insertion.set_text(first, "name", "id"));
        ASSERT_TRUE(insertion.set_text(first, "value", "a"));

        const auto second = insertion.append();
        ASSERT_TRUE(insertion.set(second, "parent", uint64_t { 0 }));
        ASSERT_TRUE(insertion.set_text(second, "kind", "attribute"));
        ASSERT_TRUE(insertion.set_text(second, "name", "id"));
        ASSERT_TRUE(insertion.set_text(second, "value", "b"));

        XmlWriter writer;
        ASSERT_TRUE(writer.open(path("duplicate.xml"), rows.layout()));

        const auto slice = rows.slice(0, rows.size());
        ASSERT_TRUE(slice);
        EXPECT_FALSE(writer.write_rows(*slice));
        EXPECT_NE(writer.get_last_error().find("unique"), std::string::npos);
        EXPECT_FALSE(writer.close());
    }

    TEST_F(XmlFixture, WriterRequiresTheXmlNodeLayout)
    {
        Kakshya::CompositeLayout other;
        ASSERT_TRUE(other.add_field<std::string>("name"));

        XmlWriter writer;
        EXPECT_FALSE(writer.open(path("wrong_layout.xml"), other));
        EXPECT_FALSE(writer.is_open());
    }

    TEST_F(XmlFixture, WriterRequiresOneRootButAllowsAnotherWriteAfterFailedFlush)
    {
        CompositeArray rows(IO::xml_composite_layout());
        CompositeInsertion insertion(rows);

        const auto root_row = insertion.append();
        ASSERT_TRUE(insertion.set_text(root_row, "kind", "element"));
        ASSERT_TRUE(insertion.set_text(root_row, "name", "root"));

        XmlWriter writer;
        ASSERT_TRUE(writer.open(path("delayed_root.xml"), rows.layout()));
        EXPECT_FALSE(writer.flush());
        EXPECT_NE(writer.get_last_error().find("one root"), std::string::npos);

        const auto slice = rows.slice(0, rows.size());
        ASSERT_TRUE(slice);
        ASSERT_TRUE(writer.write_rows(*slice));
        EXPECT_TRUE(writer.close());

        XmlReader reader;
        ASSERT_TRUE(reader.open(path("delayed_root.xml")));
        const auto written = reader.read_composite();
        ASSERT_TRUE(written);
        ASSERT_EQ(written->size(), 1U);
        EXPECT_EQ(written->at(0)->text("name"), std::string_view("root"));
    }

    TEST_F(XmlFixture, FlushWritesReadableXmlAndCloseWritesLaterRows)
    {
        CompositeArray rows(IO::xml_composite_layout());
        CompositeInsertion insertion(rows);

        const auto root_row = insertion.append();
        ASSERT_TRUE(insertion.set_text(root_row, "kind", "element"));
        ASSERT_TRUE(insertion.set_text(root_row, "name", "root"));

        const auto child = insertion.append();
        ASSERT_TRUE(insertion.set(child, "parent", uint64_t { 0 }));
        ASSERT_TRUE(insertion.set_text(child, "kind", "element"));
        ASSERT_TRUE(insertion.set_text(child, "name", "child"));

        XmlWriter writer;
        ASSERT_TRUE(writer.open(path("flushed.xml"), rows.layout()));

        const auto first = rows.slice(0, 1);
        ASSERT_TRUE(first);
        ASSERT_TRUE(writer.write_rows(*first));
        ASSERT_TRUE(writer.flush());

        XmlReader after_flush;
        ASSERT_TRUE(after_flush.open(path("flushed.xml")));
        const auto initial = after_flush.read_composite();
        ASSERT_TRUE(initial);
        ASSERT_EQ(initial->size(), 1U);

        const auto second = rows.slice(1, 1);
        ASSERT_TRUE(second);
        ASSERT_TRUE(writer.write_rows(*second));
        ASSERT_TRUE(writer.close());

        XmlReader after_close;
        ASSERT_TRUE(after_close.open(path("flushed.xml")));
        const auto complete = after_close.read_composite();
        ASSERT_TRUE(complete);
        ASSERT_EQ(complete->size(), 2U);
        EXPECT_EQ(complete->at(1)->text("name"), std::string_view("child"));
        EXPECT_EQ(complete->at(1)->get<uint64_t>("parent"), 0U);
    }

    TEST_F(XmlFixture, WriterRejectsAttributeAfterElementContent)
    {
        CompositeArray rows(IO::xml_composite_layout());
        CompositeInsertion insertion(rows);

        const auto root_row = insertion.append();
        ASSERT_TRUE(insertion.set_text(root_row, "kind", "element"));
        ASSERT_TRUE(insertion.set_text(root_row, "name", "root"));

        const auto child = insertion.append();
        ASSERT_TRUE(insertion.set(child, "parent", uint64_t { 0 }));
        ASSERT_TRUE(insertion.set_text(child, "kind", "element"));
        ASSERT_TRUE(insertion.set_text(child, "name", "child"));

        const auto attribute = insertion.append();
        ASSERT_TRUE(insertion.set(attribute, "parent", uint64_t { 0 }));
        ASSERT_TRUE(insertion.set_text(attribute, "kind", "attribute"));
        ASSERT_TRUE(insertion.set_text(attribute, "name", "late"));
        ASSERT_TRUE(insertion.set_text(attribute, "value", "x"));

        XmlWriter writer;
        ASSERT_TRUE(writer.open(path("late_attribute.xml"), rows.layout()));

        const auto slice = rows.slice(0, rows.size());
        ASSERT_TRUE(slice);
        EXPECT_FALSE(writer.write_rows(*slice));
        EXPECT_NE(writer.get_last_error().find("precede its content"), std::string::npos);
        EXPECT_FALSE(writer.close());
    }

    TEST_F(XmlFixture, WriterRejectsASecondRoot)
    {
        CompositeArray rows(IO::xml_composite_layout());
        CompositeInsertion insertion(rows);

        const auto first = insertion.append();
        ASSERT_TRUE(insertion.set_text(first, "kind", "element"));
        ASSERT_TRUE(insertion.set_text(first, "name", "first"));

        const auto second = insertion.append();
        ASSERT_TRUE(insertion.set_text(second, "kind", "element"));
        ASSERT_TRUE(insertion.set_text(second, "name", "second"));

        XmlWriter writer;
        ASSERT_TRUE(writer.open(path("two_roots.xml"), rows.layout()));

        const auto slice = rows.slice(0, rows.size());
        ASSERT_TRUE(slice);
        EXPECT_FALSE(writer.write_rows(*slice));
        EXPECT_NE(writer.get_last_error().find("one root element"), std::string::npos);
        EXPECT_FALSE(writer.close());
    }

    TEST_F(XmlFixture, WriterRejectsAParentThatIsNoLongerOpen)
    {
        CompositeArray rows(IO::xml_composite_layout());
        CompositeInsertion insertion(rows);

        const auto root_row = insertion.append();
        ASSERT_TRUE(insertion.set_text(root_row, "kind", "element"));
        ASSERT_TRUE(insertion.set_text(root_row, "name", "root"));

        const auto first = insertion.append();
        ASSERT_TRUE(insertion.set(first, "parent", uint64_t { 0 }));
        ASSERT_TRUE(insertion.set_text(first, "kind", "element"));
        ASSERT_TRUE(insertion.set_text(first, "name", "first"));

        const auto second = insertion.append();
        ASSERT_TRUE(insertion.set(second, "parent", uint64_t { 0 }));
        ASSERT_TRUE(insertion.set_text(second, "kind", "element"));
        ASSERT_TRUE(insertion.set_text(second, "name", "second"));

        const auto late_child = insertion.append();
        ASSERT_TRUE(insertion.set(late_child, "parent", uint64_t { 1 }));
        ASSERT_TRUE(insertion.set_text(late_child, "kind", "element"));
        ASSERT_TRUE(insertion.set_text(late_child, "name", "late"));

        XmlWriter writer;
        ASSERT_TRUE(writer.open(path("closed_parent.xml"), rows.layout()));

        const auto slice = rows.slice(0, rows.size());
        ASSERT_TRUE(slice);
        EXPECT_FALSE(writer.write_rows(*slice));
        EXPECT_NE(writer.get_last_error().find("open ancestor"), std::string::npos);
        EXPECT_FALSE(writer.close());
    }

    TEST_F(XmlFixture, WriterRejectsMissingKindAndUnsupportedKind)
    {
        CompositeArray missing(IO::xml_composite_layout());
        CompositeInsertion missing_insertion(missing);
        const auto missing_row = missing_insertion.append();
        ASSERT_EQ(missing_row, 0U);

        XmlWriter writer;
        ASSERT_TRUE(writer.open(path("missing_kind.xml"), missing.layout()));

        const auto missing_slice = missing.slice(0, missing.size());
        ASSERT_TRUE(missing_slice);
        EXPECT_FALSE(writer.write_rows(*missing_slice));
        EXPECT_NE(writer.get_last_error().find("missing its kind"), std::string::npos);
        EXPECT_FALSE(writer.close());

        CompositeArray unsupported(IO::xml_composite_layout());
        CompositeInsertion unsupported_insertion(unsupported);
        const auto row = unsupported_insertion.append();
        ASSERT_TRUE(unsupported_insertion.set_text(row, "kind", "unknown"));

        ASSERT_TRUE(writer.open(path("unknown_kind.xml"), unsupported.layout()));

        const auto unsupported_slice = unsupported.slice(0, unsupported.size());
        ASSERT_TRUE(unsupported_slice);
        EXPECT_FALSE(writer.write_rows(*unsupported_slice));
        EXPECT_NE(writer.get_last_error().find("Unsupported XML node kind"), std::string::npos);
        EXPECT_FALSE(writer.close());
    }

    TEST_F(XmlFixture, WriterRejectsInvalidNamesAndControlCharacters)
    {
        CompositeArray invalid_name(IO::xml_composite_layout());
        CompositeInsertion name_insertion(invalid_name);
        const auto named = name_insertion.append();
        ASSERT_TRUE(name_insertion.set_text(named, "kind", "element"));
        ASSERT_TRUE(name_insertion.set_text(named, "name", "bad name"));

        XmlWriter writer;
        ASSERT_TRUE(writer.open(path("bad_name.xml"), invalid_name.layout()));

        const auto name_slice = invalid_name.slice(0, invalid_name.size());
        ASSERT_TRUE(name_slice);
        EXPECT_FALSE(writer.write_rows(*name_slice));
        EXPECT_NE(writer.get_last_error().find("invalid name or value"), std::string::npos);
        EXPECT_FALSE(writer.close());

        CompositeArray invalid_value(IO::xml_composite_layout());
        CompositeInsertion value_insertion(invalid_value);
        const auto root_row = value_insertion.append();
        ASSERT_TRUE(value_insertion.set_text(root_row, "kind", "element"));
        ASSERT_TRUE(value_insertion.set_text(root_row, "name", "root"));

        const auto text_row = value_insertion.append();
        ASSERT_TRUE(value_insertion.set(text_row, "parent", uint64_t { 0 }));
        ASSERT_TRUE(value_insertion.set_text(text_row, "kind", "text"));
        ASSERT_TRUE(value_insertion.set_text(text_row, "value", std::string_view("\x01", 1)));

        ASSERT_TRUE(writer.open(path("bad_value.xml"), invalid_value.layout()));

        const auto value_slice = invalid_value.slice(0, invalid_value.size());
        ASSERT_TRUE(value_slice);
        EXPECT_FALSE(writer.write_rows(*value_slice));
        EXPECT_NE(writer.get_last_error().find("invalid name or value"), std::string::npos);
        EXPECT_FALSE(writer.close());
    }

}
}
