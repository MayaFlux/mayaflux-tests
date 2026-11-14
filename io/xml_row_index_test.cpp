#include "file_contract_fixture.hpp"

#include "MayaFlux/IO/Composite/XmlReader.hpp"
#include "MayaFlux/IO/Composite/XmlRows.hpp"

namespace MayaFlux::Test {
namespace {

    class XmlRowIndexFixture : public FileContractFixture {
    protected:
        [[nodiscard]] std::string path(const std::string& name) const
        {
            return (root() / name).string();
        }
    };

    std::vector<uint64_t> to_vector(std::span<const uint64_t> values)
    {
        return { values.begin(), values.end() };
    }

    TEST_F(XmlRowIndexFixture, ChildrenAndRowsFollowTheParentLinks)
    {
        write("tree.xml", "<a x=\"1\"><b/><c><d/></c></a>");

        IO::XmlReader reader;
        ASSERT_TRUE(reader.open(path("tree.xml")));
        const auto* rows = reader.rows_view();
        ASSERT_NE(rows, nullptr);

        const IO::XmlRowIndex index(*rows);
        ASSERT_TRUE(index.valid());
        ASSERT_EQ(index.size(), 5U);

        EXPECT_EQ(to_vector(index.children(0)), (std::vector<uint64_t> { 1, 2, 3 }));
        EXPECT_EQ(to_vector(index.children(3)), (std::vector<uint64_t> { 4 }));
        EXPECT_TRUE(index.children(2).empty());

        const auto attribute = index.row(1);
        EXPECT_EQ(attribute.kind, "attribute");
        EXPECT_EQ(attribute.name, "x");
        EXPECT_EQ(attribute.value, "1");
        EXPECT_EQ(attribute.parent, std::optional<uint64_t> { 0 });
        EXPECT_EQ(index.row(0).kind, "element");
        EXPECT_FALSE(index.row(0).parent);
    }

    TEST_F(XmlRowIndexFixture, ReaderSubtreeExtentsStillBoundEachElement)
    {
        write("extent.xml", "<a><b><c/></b><d/></a>");

        IO::XmlReader reader;
        ASSERT_TRUE(reader.open(path("extent.xml")));

        std::vector<std::string> names;
        reader.element_at(1)->for_each_field([&](const IO::XmlFieldView& field) {
            names.emplace_back(field.name);
        });
        EXPECT_EQ(names, (std::vector<std::string> { "c" }));

        names.clear();
        reader.root_element()->for_each_field([&](const IO::XmlFieldView& field) {
            names.emplace_back(field.name);
        });
        EXPECT_EQ(names, (std::vector<std::string> { "b", "d" }));
    }

    TEST_F(XmlRowIndexFixture, ABadParentMakesTheIndexInvalidAtThatRow)
    {
        Kakshya::CompositeArray rows(IO::xml_composite_layout());
        Kakshya::CompositeInsertion insertion(rows);
        IO::XmlRowAppender appender(insertion);
        ASSERT_TRUE(appender.element("a"));

        const size_t bad = insertion.append();
        ASSERT_TRUE(insertion.set_text(bad, "kind", "element"));
        ASSERT_TRUE(insertion.set(bad, "parent", uint64_t { 5 }));

        const IO::XmlRowIndex index(rows);
        EXPECT_FALSE(index.valid());
        EXPECT_EQ(index.error_row(), 1U);
        EXPECT_FALSE(index.error().empty());
        EXPECT_TRUE(index.children(0).empty());
    }

}
}
