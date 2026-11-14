#include "gtest/gtest.h"
#include "../../support/test_metadata.hpp"

#include "MayaFlux/Kakshya/NDData/CompositeDescribe.hpp"
#include "MayaFlux/Kakshya/NDData/VertexFormats.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {

enum class DescribedCompositeMode : uint8_t { First, Second };

struct DescribedCompositeSample {
    std::string name;
    std::vector<Kakshya::Vertex> vertices;
    std::vector<float> weights;
    std::optional<float> opacity;
    DescribedCompositeMode mode { DescribedCompositeMode::First };
};

}

namespace MayaFlux::Reflect::Generated {

template <>
struct Description<::MayaFlux::Test::DescribedCompositeSample> {
    static constexpr auto members()
    {
        return std::tuple {
            Member<&::MayaFlux::Test::DescribedCompositeSample::name> { "name" },
            Member<&::MayaFlux::Test::DescribedCompositeSample::vertices> { "vertices" },
            Member<&::MayaFlux::Test::DescribedCompositeSample::weights> { "weights" },
            Member<&::MayaFlux::Test::DescribedCompositeSample::opacity> { "opacity" },
            Member<&::MayaFlux::Test::DescribedCompositeSample::mode> { "mode" },
        };
    }
};

}

namespace MayaFlux::Test {
namespace {

    TEST(CompositeDescribeTest, GeneratedDescriptionCreatesNestedLayout)
    {
        Kakshya::CompositeArray rows(Kakshya::described_composite_layout<DescribedCompositeSample>());
        const auto& fields = rows.layout().fields();

        ASSERT_EQ(fields.size(), 5U);
        EXPECT_EQ(fields[0].name, "name");
        EXPECT_EQ(fields[0].type, std::type_index(typeid(std::string)));
        EXPECT_EQ(fields[1].type, std::type_index(typeid(Kakshya::detail::CompositeList)));
        ASSERT_TRUE(fields[1].nested_layout);
        EXPECT_EQ(fields[1].nested_layout->fields()[0].name, "position");
        EXPECT_EQ(fields[1].nested_layout->fields()[0].type,
            std::type_index(typeid(glm::vec3)));
        EXPECT_EQ(fields[2].type, std::type_index(typeid(Kakshya::detail::CompositeList)));
        ASSERT_TRUE(fields[2].nested_layout);
        EXPECT_EQ(fields[2].nested_layout->fields()[0].name, "value");
        EXPECT_EQ(fields[2].nested_layout->fields()[0].type, std::type_index(typeid(float)));
        EXPECT_EQ(fields[3].type, std::type_index(typeid(float)));
        EXPECT_EQ(fields[4].type, std::type_index(typeid(uint8_t)));
    }

    TEST(CompositeDescribeTest, DescribedValuesRoundTripThroughNativeComposite)
    {
        DescribedCompositeSample input;
        input.name = "points";
        input.opacity = 0.5F;
        input.weights = { 0.25F, 0.75F };
        input.mode = DescribedCompositeMode::Second;
        Kakshya::Vertex first {};
        first.position = { 1.0F, 2.0F, 3.0F };
        first.color = { 0.2F, 0.3F, 0.4F, 1.0F };
        Kakshya::Vertex second {};
        second.position = { 4.0F, 5.0F, 6.0F };
        input.vertices = { first, second };

        Kakshya::CompositeArray rows(Kakshya::described_composite_layout<DescribedCompositeSample>());
        Kakshya::CompositeInsertion insertion(rows);
        const size_t row = insertion.append();
        std::string error;
        ASSERT_TRUE(Kakshya::write_described(insertion, row, input, error)) << error;
        const auto* encoded_vertices = rows.at(row)->list("vertices");
        ASSERT_NE(encoded_vertices, nullptr);
        ASSERT_EQ(encoded_vertices->size(), 2U);
        const auto encoded_position = encoded_vertices->at(0)->get<glm::vec3>("position");
        ASSERT_TRUE(encoded_position);
        EXPECT_EQ(*encoded_position, first.position);

        DescribedCompositeSample output;
        ASSERT_TRUE(Kakshya::overlay_described(*rows.at(row), output, error)) << error;
        EXPECT_EQ(output.name, "points");
        EXPECT_EQ(output.opacity, 0.5F);
        EXPECT_EQ(output.weights, input.weights);
        EXPECT_EQ(output.mode, DescribedCompositeMode::Second);
        ASSERT_EQ(output.vertices.size(), 2U);
        EXPECT_EQ(output.vertices[0].position, first.position);
        EXPECT_EQ(output.vertices[0].color, first.color);
        EXPECT_EQ(output.vertices[0].normal, first.normal);
        EXPECT_EQ(output.vertices[1].position, second.position);
    }

    TEST(CompositeDescribeTest, PresenceOverlaysDefaultsAndUnknownFieldsFail)
    {
        Kakshya::CompositeArray rows(Kakshya::described_composite_layout<Kakshya::Vertex>());
        Kakshya::CompositeInsertion insertion(rows);
        const size_t row = insertion.append();
        ASSERT_TRUE(insertion.set(row, "scalar", 3.0F));

        Kakshya::Vertex vertex {};
        vertex.position = { 7.0F, 8.0F, 9.0F };
        std::string error;
        std::vector<std::string> present;
        EXPECT_TRUE(rows.at(row)->for_each_present_field([&](const Kakshya::CompositeField& field) {
            present.push_back(field.name);
        }));
        EXPECT_EQ(present, (std::vector<std::string> { "scalar" }));
        size_t visited = 0;
        EXPECT_FALSE(rows.at(row)->for_each_present_field([&](const Kakshya::CompositeField&) {
            ++visited;
            return false;
        }));
        EXPECT_EQ(visited, 1U);
        ASSERT_TRUE(Kakshya::overlay_described(*rows.at(row), vertex, error)) << error;
        EXPECT_EQ(vertex.position, (glm::vec3 { 7.0F, 8.0F, 9.0F }));
        EXPECT_EQ(vertex.scalar, 3.0F);
        EXPECT_EQ(vertex.color, glm::vec4(1.0F));

        Kakshya::CompositeLayout foreign_layout;
        foreign_layout.add_field<float>("other");
        Kakshya::CompositeArray foreign(std::move(foreign_layout));
        ASSERT_TRUE(foreign.set(foreign.append(), "other", 1.0F));
        EXPECT_FALSE(Kakshya::overlay_described(*foreign.at(0), vertex, error));
        EXPECT_NE(error.find("other"), std::string::npos);
    }

    TEST(CompositeDescribeTest, ExactTypesAndAbsentOptionalFieldsKeepTheirMeaning)
    {
        Kakshya::CompositeArray rows(Kakshya::described_composite_layout<DescribedCompositeSample>());
        Kakshya::CompositeInsertion insertion(rows);
        const size_t row = insertion.append();
        std::string error;
        ASSERT_TRUE(Kakshya::write_described(insertion, row,
            DescribedCompositeSample { .name = "empty" }, error)) << error;
        EXPECT_FALSE(rows.at(row)->has("opacity"));
        EXPECT_TRUE(rows.at(row)->has("vertices"));
        const auto* vertices = rows.at(row)->list("vertices");
        ASSERT_NE(vertices, nullptr);
        EXPECT_EQ(vertices->size(), 0U);

        DescribedCompositeSample output;
        output.opacity = 0.75F;
        ASSERT_TRUE(Kakshya::overlay_described(*rows.at(row), output, error)) << error;
        EXPECT_EQ(output.opacity, 0.75F);

        Kakshya::CompositeLayout wrong_layout;
        wrong_layout.add_field<double>("scalar");
        Kakshya::CompositeArray wrong(std::move(wrong_layout));
        ASSERT_TRUE(wrong.set(wrong.append(), "scalar", 2.0));
        Kakshya::Vertex vertex {};
        EXPECT_FALSE(Kakshya::overlay_described(*wrong.at(0), vertex, error));
        EXPECT_NE(error.find("scalar"), std::string::npos);
    }

}
}
