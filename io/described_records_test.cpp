#include "file_contract_fixture.hpp"

#include "MayaFlux/IO/GlmTextCodec.hpp"

#include "MayaFlux/IO/Composite/DelimitedTextReader.hpp"
#include "MayaFlux/IO/Composite/DescribedRecords.hpp"
#include "MayaFlux/IO/Composite/XmlRecordAccess.hpp"
#include "MayaFlux/Kakshya/NDData/VertexFormats.hpp"

namespace MayaFlux::Test {

struct DescribedRecordsRow {
    std::string name;
    double value {};
    int32_t count {};
};

}

namespace MayaFlux::Reflect::Generated {

template <>
struct Description<::MayaFlux::Test::DescribedRecordsRow> {
    static constexpr auto members()
    {
        return std::tuple {
            Member<&::MayaFlux::Test::DescribedRecordsRow::name> { "name" },
            Member<&::MayaFlux::Test::DescribedRecordsRow::value> { "value" },
            Member<&::MayaFlux::Test::DescribedRecordsRow::count> { "count" },
        };
    }
};

}

namespace MayaFlux::Test {
namespace {

    using Kakshya::Vertex;

    Reflect::RecordBinding<Vertex, IO::XmlElementView> vertex_binding()
    {
        Reflect::RecordBinding<Vertex, IO::XmlElementView> binding;
        binding.required("position");
        return binding;
    }

    class DescribedRecordsFixture : public FileContractFixture {
    protected:
        [[nodiscard]] std::string path(const std::string& name) const
        {
            return (root() / name).string();
        }
    };

    TEST(GlmTextCodecTest, VectorsDecodeOneNumberPerComponent)
    {
        Reflect::TextError error = Reflect::TextError::Unsupported;
        glm::vec3 position {};
        EXPECT_TRUE(Reflect::TextCodec<glm::vec3>::decode("1 2 3", position, error));
        EXPECT_EQ(position, glm::vec3(1.F, 2.F, 3.F));

        glm::dvec2 pair {};
        EXPECT_TRUE(Reflect::TextCodec<glm::dvec2>::decode("0.5\t-2", pair, error));
        EXPECT_EQ(pair, glm::dvec2(0.5, -2.0));

        EXPECT_EQ(Reflect::TextCodec<glm::vec4>::extent, 4U);
    }

    TEST(GlmTextCodecTest, VectorsRejectWrongCountAndBadTokens)
    {
        Reflect::TextError error = Reflect::TextError::Unsupported;
        glm::vec3 position(7.F);
        EXPECT_FALSE(Reflect::TextCodec<glm::vec3>::decode("1 2", position, error));
        EXPECT_EQ(error, Reflect::TextError::Cardinality);
        EXPECT_FALSE(Reflect::TextCodec<glm::vec3>::decode("1 2 x", position, error));
        EXPECT_EQ(error, Reflect::TextError::Invalid);
        EXPECT_EQ(position, glm::vec3(7.F));
    }

    TEST_F(DescribedRecordsFixture, XmlChildrenBecomeTypedRows)
    {
        write("scene.xml",
            "<scene><vertex position=\"0 1 2\" color=\"1 0 0 1\"/><group/>"
            "<vertex position=\"3 4 5\" scalar=\"2\"/></scene>");

        IO::XmlReader reader;
        ASSERT_TRUE(reader.open(path("scene.xml")));
        const auto root = reader.root_element();
        ASSERT_TRUE(root);

        std::string error;
        const auto rows = IO::collect_records(vertex_binding(), *root, "vertex", error);
        ASSERT_TRUE(rows) << error;
        ASSERT_EQ(rows->size(), 2U);

        EXPECT_EQ(rows->at(0)->get<glm::vec3>("position"), std::optional(glm::vec3(0.F, 1.F, 2.F)));
        EXPECT_EQ(rows->at(0)->get<glm::vec4>("color"), std::optional(glm::vec4(1.F, 0.F, 0.F, 1.F)));
        EXPECT_EQ(rows->at(1)->get<glm::vec3>("position"), std::optional(glm::vec3(3.F, 4.F, 5.F)));
        EXPECT_EQ(rows->at(1)->get<float>("scalar"), std::optional(2.F));
    }

    TEST_F(DescribedRecordsFixture, OmittedFieldsKeepTheirCppDefaults)
    {
        write("defaults.xml", "<scene><vertex position=\"1 1 1\"/></scene>");

        IO::XmlReader reader;
        ASSERT_TRUE(reader.open(path("defaults.xml")));
        std::string error;
        const auto rows = IO::collect_records(vertex_binding(), *reader.root_element(), "vertex", error);
        ASSERT_TRUE(rows) << error;
        ASSERT_EQ(rows->size(), 1U);
        EXPECT_EQ(rows->at(0)->get<glm::vec4>("color"), std::optional(Vertex {}.color));
        EXPECT_EQ(rows->at(0)->get<float>("scalar"), std::optional(Vertex {}.scalar));
        EXPECT_EQ(rows->at(0)->get<glm::vec3>("normal"), std::optional(Vertex {}.normal));
    }

    TEST_F(DescribedRecordsFixture, NoMatchingChildYieldsAnEmptyArray)
    {
        write("none.xml", "<scene><group/></scene>");

        IO::XmlReader reader;
        ASSERT_TRUE(reader.open(path("none.xml")));
        std::string error;
        const auto rows = IO::collect_records(vertex_binding(), *reader.root_element(), "vertex", error);
        ASSERT_TRUE(rows) << error;
        EXPECT_EQ(rows->size(), 0U);
    }

    TEST_F(DescribedRecordsFixture, FailureReportsTheElementRow)
    {
        write("bad.xml", "<scene><vertex position=\"0 1\"/></scene>");

        IO::XmlReader reader;
        ASSERT_TRUE(reader.open(path("bad.xml")));
        std::string error;
        uint64_t location = 0;
        EXPECT_FALSE(IO::collect_records(vertex_binding(), *reader.root_element(), "vertex",
            error, &location));
        EXPECT_NE(error.find("3 numbers in position"), std::string::npos);
        EXPECT_EQ(location, 1U);
    }

    TEST_F(DescribedRecordsFixture, MissingRequiredAttributeStopsCollection)
    {
        write("required.xml",
            "<scene><vertex position=\"0 0 0\"/><vertex color=\"1 1 1 1\"/></scene>");

        IO::XmlReader reader;
        ASSERT_TRUE(reader.open(path("required.xml")));
        std::string error;
        EXPECT_FALSE(IO::collect_records(vertex_binding(), *reader.root_element(), "vertex", error));
        EXPECT_NE(error.find("needs a position attribute"), std::string::npos);
    }

    TEST_F(DescribedRecordsFixture, RecordsOfRoundTripsCollectedRows)
    {
        write("round.xml",
            "<scene><vertex position=\"0 1 2\" scalar=\"4\"/><vertex position=\"3 4 5\"/></scene>");

        IO::XmlReader reader;
        ASSERT_TRUE(reader.open(path("round.xml")));
        std::string error;
        const auto rows = IO::collect_records(vertex_binding(), *reader.root_element(), "vertex", error);
        ASSERT_TRUE(rows) << error;

        const auto vertices = IO::records_of<Vertex>(*rows, error);
        ASSERT_TRUE(vertices) << error;
        ASSERT_EQ(vertices->size(), 2U);
        EXPECT_EQ((*vertices)[0].position, glm::vec3(0.F, 1.F, 2.F));
        EXPECT_EQ((*vertices)[0].scalar, 4.F);
        EXPECT_EQ((*vertices)[1].position, glm::vec3(3.F, 4.F, 5.F));
        EXPECT_EQ((*vertices)[1].scalar, Vertex {}.scalar);
    }

    TEST_F(DescribedRecordsFixture, RecordsOfRejectsAForeignLayout)
    {
        Kakshya::CompositeLayout layout;
        layout.add_field<float>("other");
        Kakshya::CompositeArray rows(std::move(layout));

        std::string error;
        EXPECT_FALSE(IO::records_of<Vertex>(rows, error));
        EXPECT_FALSE(error.empty());
    }

    TEST_F(DescribedRecordsFixture, DelimitedRowsDecodeThroughTheDescribedLayout)
    {
        write("rows.csv", "name,value,count\nalpha,1.5,2\nbeta,2.5,3\n");

        IO::DelimitedTextReader reader;
        ASSERT_TRUE(reader.set_layout(Kakshya::described_composite_layout<DescribedRecordsRow>()));
        ASSERT_TRUE(reader.open(path("rows.csv")));
        const auto rows = reader.read_composite();
        ASSERT_TRUE(rows);

        std::string error;
        const auto values = IO::records_of<DescribedRecordsRow>(*rows, error);
        ASSERT_TRUE(values) << error;
        ASSERT_EQ(values->size(), 2U);
        EXPECT_EQ((*values)[0].name, "alpha");
        EXPECT_EQ((*values)[0].value, 1.5);
        EXPECT_EQ((*values)[0].count, 2);
        EXPECT_EQ((*values)[1].name, "beta");
        EXPECT_EQ((*values)[1].value, 2.5);
        EXPECT_EQ((*values)[1].count, 3);
    }

}
}
