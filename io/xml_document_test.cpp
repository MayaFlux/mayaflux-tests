#include "file_contract_fixture.hpp"

#include "MayaFlux/IO/GlmTextCodec.hpp"

#include "MayaFlux/IO/Composite/DescribedRecords.hpp"
#include "MayaFlux/IO/Composite/XmlDocument.hpp"
#include "MayaFlux/IO/Composite/XmlRecordAccess.hpp"
#include "MayaFlux/Kakshya/NDData/VertexFormats.hpp"

namespace MayaFlux::Test {
namespace {

    class XmlDocumentFixture : public FileContractFixture {
    protected:
        [[nodiscard]] std::string path(const std::string& name) const
        {
            return (root() / name).string();
        }
    };

    TEST_F(XmlDocumentFixture, ViewsStayValidAfterTheLoadingReaderIsGone)
    {
        write("scene.xml", "<scene mode=\"fast\"><vertex position=\"0 1 2\"/><group><vertex position=\"9 9 9\"/></group></scene>");

        std::string error;
        const auto document = IO::XmlDocument::load(path("scene.xml"), error);
        ASSERT_TRUE(document) << error;

        const auto root = document->root_element();
        ASSERT_TRUE(root);
        EXPECT_EQ(root->name(), "scene");
        EXPECT_EQ(root->attribute("mode"), std::string_view("fast"));

        std::vector<std::string> names;
        root->for_each_field([&](const IO::XmlFieldView& field) {
            names.emplace_back(field.name);
        });
        EXPECT_EQ(names, (std::vector<std::string> { "mode", "vertex", "group" }));
    }

    TEST_F(XmlDocumentFixture, ElementAtFindsElementsByRowAndRefusesOtherRows)
    {
        write("rows.xml", "<a x=\"1\"><b/>text</a>");

        std::string error;
        const auto document = IO::XmlDocument::load(path("rows.xml"), error);
        ASSERT_TRUE(document) << error;

        ASSERT_TRUE(document->element_at(0));
        EXPECT_EQ(document->element_at(0)->name(), "a");
        EXPECT_FALSE(document->element_at(1));
        ASSERT_TRUE(document->element_at(2));
        EXPECT_EQ(document->element_at(2)->name(), "b");
        EXPECT_FALSE(document->element_at(3));
        EXPECT_FALSE(document->element_at(99));
    }

    TEST_F(XmlDocumentFixture, ForestExposesTheHierarchyAndABranchRegion)
    {
        write("tree.xml", "<a><b><c/></b><d/></a>");

        std::string error;
        const auto document = IO::XmlDocument::load(path("tree.xml"), error);
        ASSERT_TRUE(document) << error;

        const Kakshya::CompositeForest& forest = document->forest();
        EXPECT_TRUE(forest.index().preorder());
        const auto region = forest.subtree_region(1);
        ASSERT_TRUE(region);
        EXPECT_EQ(region->start_coordinates, (std::vector<uint64_t> { 1 }));
        EXPECT_EQ(region->end_coordinates, (std::vector<uint64_t> { 2 }));
    }

    TEST_F(XmlDocumentFixture, RecordsBindFromDocumentViews)
    {
        write("vertices.xml", "<scene><vertex position=\"0 1 2\" scalar=\"3\"/><vertex position=\"4 5 6\"/></scene>");

        std::string error;
        const auto document = IO::XmlDocument::load(path("vertices.xml"), error);
        ASSERT_TRUE(document) << error;

        Reflect::RecordBinding<Kakshya::Vertex, IO::XmlElementView> binding;
        binding.required("position");
        const auto rows = IO::collect_records(binding, *document->root_element(), "vertex", error);
        ASSERT_TRUE(rows) << error;
        ASSERT_EQ(rows->size(), 2U);
        EXPECT_EQ(rows->at(0)->get<glm::vec3>("position"), std::optional(glm::vec3(0.F, 1.F, 2.F)));
        EXPECT_EQ(rows->at(0)->get<float>("scalar"), std::optional(3.F));
        EXPECT_EQ(rows->at(1)->get<glm::vec3>("position"), std::optional(glm::vec3(4.F, 5.F, 6.F)));
    }

    TEST_F(XmlDocumentFixture, LoadReportsAMissingOrInvalidFile)
    {
        std::string error;
        EXPECT_FALSE(IO::XmlDocument::load(path("absent.xml"), error));
        EXPECT_FALSE(error.empty());

        write("broken.xml", "<a><b></a>");
        error.clear();
        EXPECT_FALSE(IO::XmlDocument::load(path("broken.xml"), error));
        EXPECT_FALSE(error.empty());
    }

    TEST_F(XmlDocumentFixture, FromRowsRejectsAForeignLayoutAndAcceptsReaderRows)
    {
        Kakshya::CompositeLayout layout;
        layout.add_field<float>("other");
        std::string error;
        EXPECT_FALSE(IO::XmlDocument::from_rows(Kakshya::CompositeArray(std::move(layout)), error));
        EXPECT_NE(error.find("XML node layout"), std::string::npos);

        write("ok.xml", "<a><b/></a>");
        IO::XmlReader reader;
        ASSERT_TRUE(reader.open(path("ok.xml")));
        auto rows = reader.take_rows();
        ASSERT_TRUE(rows);
        EXPECT_FALSE(reader.is_open());
        EXPECT_EQ(reader.rows_view(), nullptr);

        const auto document = IO::XmlDocument::from_rows(std::move(*rows), error);
        ASSERT_TRUE(document) << error;
        EXPECT_EQ(document->root_element()->name(), "a");
    }

    TEST_F(XmlDocumentFixture, TakeRowsOnAClosedReaderGivesNothing)
    {
        IO::XmlReader reader;
        EXPECT_FALSE(reader.take_rows());
    }

}
}
