#include "gtest/gtest.h"

#include "MayaFlux/Kakshya/NDData/DataAccess.hpp"

namespace MayaFlux::Test {
namespace {

    using Kakshya::DataAccess;
    using Kakshya::DataDimension;
    using Kakshya::DataModality;
    using Kakshya::DataVariant;
    using Kakshya::StructuredView;

    TEST(DataAccessMetadataTest, ReportsTheModalityItWasGiven)
    {
        DataVariant variant = std::vector<double>(4, 0.0);
        const std::vector<DataDimension> dims { DataDimension::time(4) };

        const DataAccess access(variant, dims, DataModality::AUDIO_1D);

        EXPECT_EQ(access.modality(), DataModality::AUDIO_1D);
    }

    TEST(DataAccessMetadataTest, UngroupedDimensionsAreScalar)
    {
        DataVariant variant = std::vector<double>(16, 0.0);
        const std::vector<DataDimension> dims { DataDimension::time(16) };

        const DataAccess access(variant, dims, DataModality::AUDIO_1D);

        EXPECT_FALSE(access.is_structured());
        EXPECT_EQ(access.element_count(), 16U);
        EXPECT_EQ(access.component_count(), 1U);
    }

    TEST(DataAccessMetadataTest, NoDimensionsIsNotStructuredAndCountsTheStorage)
    {
        DataVariant variant = std::vector<float>(7, 0.0F);
        const std::vector<DataDimension> dims;

        const DataAccess access(variant, dims, DataModality::UNKNOWN);

        EXPECT_FALSE(access.is_structured());
        EXPECT_EQ(access.element_count(), 7U);
        EXPECT_EQ(access.component_count(), 1U);
    }

    TEST(DataAccessMetadataTest, GroupedDimensionsAreStructuredAndCountElementsNotComponents)
    {
        DataVariant variant = std::vector<float>(12, 0.0F);
        const std::vector<DataDimension> dims { DataDimension::vertex_positions(4) };

        const DataAccess access(variant, dims, DataModality::VERTEX_POSITIONS_3D);

        EXPECT_TRUE(access.is_structured());
        EXPECT_EQ(access.element_count(), 4U);
        EXPECT_EQ(access.component_count(), 3U);
    }

    TEST(DataAccessMetadataTest, DescribesStructuredData)
    {
        DataVariant variant = std::vector<float>(12, 0.0F);
        const std::vector<DataDimension> dims { DataDimension::vertex_positions(4) };

        const DataAccess access(variant, dims, DataModality::VERTEX_POSITIONS_3D);
        const std::string description = access.type_description();

        EXPECT_NE(description.find('4'), std::string::npos);
        EXPECT_NE(description.find('3'), std::string::npos);
        EXPECT_NE(description.find("VERTEX_POSITIONS_3D"), std::string::npos);
    }

    TEST(DataAccessMetadataTest, DescribesScalarData)
    {
        DataVariant variant = std::vector<double>(16, 0.0);
        const std::vector<DataDimension> dims { DataDimension::time(16) };

        const DataAccess access(variant, dims, DataModality::AUDIO_1D);
        const std::string description = access.type_description();

        EXPECT_NE(description.find("scalar"), std::string::npos);
        EXPECT_NE(description.find("16"), std::string::npos);
        EXPECT_NE(description.find("AUDIO_1D"), std::string::npos);
    }

    struct SuggestionCase {
        DataModality modality;
        std::string_view expected;
    };

    class DataAccessSuggestionTest : public ::testing::TestWithParam<SuggestionCase> { };

    TEST_P(DataAccessSuggestionTest, NamesTheViewTypeForTheModality)
    {
        DataVariant variant = std::vector<float>();
        const std::vector<DataDimension> dims;

        const DataAccess access(variant, dims, GetParam().modality);

        EXPECT_EQ(access.suggested_view_type(), GetParam().expected);
    }

    INSTANTIATE_TEST_SUITE_P(Modalities, DataAccessSuggestionTest,
        ::testing::Values(
            SuggestionCase { DataModality::VERTEX_POSITIONS_3D, "glm::vec3" },
            SuggestionCase { DataModality::VERTEX_NORMALS_3D, "glm::vec3" },
            SuggestionCase { DataModality::VERTEX_TANGENTS_3D, "glm::vec3" },
            SuggestionCase { DataModality::VERTEX_COLORS_RGB, "glm::vec3" },
            SuggestionCase { DataModality::TEXTURE_COORDS_2D, "glm::vec2" },
            SuggestionCase { DataModality::VERTEX_TANGENTS_4D, "glm::vec4" },
            SuggestionCase { DataModality::VERTEX_COLORS_RGBA, "glm::vec4" },
            SuggestionCase { DataModality::TRANSFORMATION_MATRIX, "glm::mat4" },
            SuggestionCase { DataModality::AUDIO_1D, "double" },
            SuggestionCase { DataModality::AUDIO_MULTICHANNEL, "double" },
            SuggestionCase { DataModality::SPECTRAL_2D, "double" },
            SuggestionCase { DataModality::IMAGE_2D, "float" },
            SuggestionCase { DataModality::IMAGE_COLOR, "float" },
            SuggestionCase { DataModality::TEXTURE_2D, "float" },
            SuggestionCase { DataModality::VOLUMETRIC_3D, "unknown" },
            SuggestionCase { DataModality::UNKNOWN, "unknown" }));

    TEST(DataAccessGpuBufferTest, ExposesTheStoragePointerAndByteCount)
    {
        DataVariant variant = std::vector<float>(10, 1.0F);
        const std::vector<DataDimension> dims { DataDimension::time(10) };
        const DataAccess access(variant, dims, DataModality::AUDIO_1D);

        const auto [data, bytes, hint] = access.gpu_buffer();

        EXPECT_EQ(data, std::get<std::vector<float>>(variant).data());
        EXPECT_EQ(bytes, 10U * sizeof(float));
        (void)hint;
    }

    TEST(DataAccessGpuBufferTest, ByteCountFollowsTheElementType)
    {
        DataVariant variant = std::vector<glm::vec3>(5);
        const std::vector<DataDimension> dims { DataDimension::vertex_positions(5) };
        const DataAccess access(variant, dims, DataModality::VERTEX_POSITIONS_3D);

        const auto [data, bytes, hint] = access.gpu_buffer();

        EXPECT_EQ(data, std::get<std::vector<glm::vec3>>(variant).data());
        EXPECT_EQ(bytes, 5U * sizeof(glm::vec3));
        (void)hint;
    }

    TEST(DataAccessScalarViewTest, MatchingTypeAliasesTheStorage)
    {
        DataVariant variant = std::vector<double> { 0.5, -0.25, 1.0 };
        const std::vector<DataDimension> dims { DataDimension::time(3) };
        const DataAccess access(variant, dims, DataModality::AUDIO_1D);

        const auto view = access.view<double>();

        EXPECT_EQ(view.data(), std::get<std::vector<double>>(variant).data());
        ASSERT_EQ(view.size(), 3U);
        EXPECT_DOUBLE_EQ(view[1], -0.25);
    }

    TEST(DataAccessScalarViewTest, ViewObservesLaterWritesToTheStorage)
    {
        DataVariant variant = std::vector<float>(4, 0.0F);
        const std::vector<DataDimension> dims { DataDimension::time(4) };
        const DataAccess access(variant, dims, DataModality::AUDIO_1D);

        std::get<std::vector<float>>(variant)[2] = 9.0F;

        EXPECT_FLOAT_EQ(access.view<float>()[2], 9.0F);
    }

    TEST(DataAccessScalarViewTest, DifferentArithmeticTypeIsConverted)
    {
        DataVariant variant = std::vector<double> { 0.5, -0.25, 1.0 };
        const std::vector<DataDimension> dims { DataDimension::time(3) };
        const DataAccess access(variant, dims, DataModality::AUDIO_1D);

        const auto view = access.view<float>();

        ASSERT_EQ(view.size(), 3U);
        EXPECT_FLOAT_EQ(view[0], 0.5F);
        EXPECT_FLOAT_EQ(view[1], -0.25F);
        EXPECT_FLOAT_EQ(view[2], 1.0F);
        EXPECT_NE(static_cast<const void*>(view.data()), static_cast<const void*>(std::get<std::vector<double>>(variant).data()));
    }

    TEST(DataAccessScalarViewTest, IntegerStorageWidensToFloat)
    {
        DataVariant variant = std::vector<uint8_t> { 0, 128, 255 };
        const std::vector<DataDimension> dims { DataDimension::spatial_1d(3) };
        const DataAccess access(variant, dims, DataModality::IMAGE_2D);

        const auto view = access.view<float>();

        ASSERT_EQ(view.size(), 3U);
        EXPECT_FLOAT_EQ(view[0], 0.0F);
        EXPECT_FLOAT_EQ(view[1], 128.0F);
        EXPECT_FLOAT_EQ(view[2], 255.0F);
    }

    TEST(DataAccessScalarViewTest, ConvertedViewSurvivesRepeatedCallsOfTheSameType)
    {
        DataVariant variant = std::vector<double> { 1.0, 2.0, 3.0, 4.0 };
        const std::vector<DataDimension> dims { DataDimension::time(4) };
        const DataAccess access(variant, dims, DataModality::AUDIO_1D);

        const auto first = access.view<float>();
        const auto second = access.view<float>();

        EXPECT_EQ(first.data(), second.data());
        EXPECT_FLOAT_EQ(first[3], 4.0F);
        EXPECT_FLOAT_EQ(second[3], 4.0F);
    }

    TEST(DataAccessScalarViewTest, EmptyStorageGivesAnEmptyView)
    {
        DataVariant variant = std::vector<float>();
        const std::vector<DataDimension> dims;
        const DataAccess access(variant, dims, DataModality::UNKNOWN);

        EXPECT_TRUE(access.view<float>().empty());
        EXPECT_TRUE(access.view<double>().empty());
    }

    TEST(DataAccessScalarViewTest, StructuredStorageIsNotViewableAsAScalar)
    {
        DataVariant variant = std::vector<glm::vec3>(3);
        const std::vector<DataDimension> dims { DataDimension::vertex_positions(3) };
        const DataAccess access(variant, dims, DataModality::VERTEX_POSITIONS_3D);

        EXPECT_THROW((void)access.view<float>(), std::invalid_argument);
    }

    TEST(DataAccessStructuredViewTest, FlatComponentsReadAsElements)
    {
        DataVariant variant = std::vector<float> { 1, 2, 3, 4, 5, 6 };
        const std::vector<DataDimension> dims { DataDimension::vertex_positions(2) };
        const DataAccess access(variant, dims, DataModality::VERTEX_POSITIONS_3D);

        const auto view = access.view<glm::vec3>();

        ASSERT_EQ(view.size(), 2U);
        EXPECT_EQ(view[0], glm::vec3(1, 2, 3));
        EXPECT_EQ(view[1], glm::vec3(4, 5, 6));
    }

    TEST(DataAccessStructuredViewTest, MatchingComponentTypeAliasesTheStorage)
    {
        DataVariant variant = std::vector<float> { 1, 2, 3, 4, 5, 6 };
        const std::vector<DataDimension> dims { DataDimension::vertex_positions(2) };
        const DataAccess access(variant, dims, DataModality::VERTEX_POSITIONS_3D);

        const auto view = access.view<glm::vec3>();

        EXPECT_EQ(view.data(), static_cast<const void*>(std::get<std::vector<float>>(variant).data()));
        EXPECT_EQ(view.size_bytes(), 6U * sizeof(float));
    }

    TEST(DataAccessStructuredViewTest, WiderComponentStorageIsConverted)
    {
        DataVariant variant = std::vector<double> { 0.5, 1.5, 2.5, 3.5, 4.5, 5.5 };
        const std::vector<DataDimension> dims { DataDimension::vertex_positions(2) };
        const DataAccess access(variant, dims, DataModality::VERTEX_POSITIONS_3D);

        const auto view = access.view<glm::vec3>();

        ASSERT_EQ(view.size(), 2U);
        EXPECT_EQ(view[0], glm::vec3(0.5F, 1.5F, 2.5F));
        EXPECT_EQ(view[1], glm::vec3(3.5F, 4.5F, 5.5F));
    }

    TEST(DataAccessStructuredViewTest, ViewCoversOnlyTheDeclaredElements)
    {
        DataVariant variant = std::vector<float> { 1, 2, 3, 4, 5, 6, 7, 8, 9 };
        const std::vector<DataDimension> dims { DataDimension::vertex_positions(2) };
        const DataAccess access(variant, dims, DataModality::VERTEX_POSITIONS_3D);

        EXPECT_EQ(access.view<glm::vec3>().size(), 2U);
    }

    TEST(DataAccessStructuredViewTest, UvPairsReadAsVec2)
    {
        DataVariant variant = std::vector<float> { 0, 1, 0.5F, 0.5F };
        const std::vector<DataDimension> dims { DataDimension::texture_coords(2) };
        const DataAccess access(variant, dims, DataModality::TEXTURE_COORDS_2D);

        const auto view = access.view<glm::vec2>();

        ASSERT_EQ(view.size(), 2U);
        EXPECT_EQ(view[1], glm::vec2(0.5F, 0.5F));
    }

    TEST(DataAccessStructuredViewTest, ColorsWithAlphaReadAsVec4)
    {
        DataVariant variant = std::vector<float> { 1, 0, 0, 1, 0, 1, 0, 0.5F };
        const std::vector<DataDimension> dims { DataDimension::vertex_colors(2, true) };
        const DataAccess access(variant, dims, DataModality::VERTEX_COLORS_RGBA);

        const auto view = access.view<glm::vec4>();

        ASSERT_EQ(view.size(), 2U);
        EXPECT_EQ(view[1], glm::vec4(0, 1, 0, 0.5F));
    }

    TEST(DataAccessStructuredViewTest, MismatchedComponentCountThrows)
    {
        DataVariant variant = std::vector<float>(6, 0.0F);
        const std::vector<DataDimension> dims { DataDimension::vertex_positions(2) };
        const DataAccess access(variant, dims, DataModality::VERTEX_POSITIONS_3D);

        EXPECT_THROW((void)access.view<glm::vec2>(), std::runtime_error);
        EXPECT_THROW((void)access.view<glm::vec4>(), std::runtime_error);
    }

    TEST(DataAccessStructuredViewTest, MissingGroupingThrows)
    {
        DataVariant variant = std::vector<float>(6, 0.0F);
        const std::vector<DataDimension> dims { DataDimension::time(6) };
        const DataAccess access(variant, dims, DataModality::UNKNOWN);

        EXPECT_THROW((void)access.view<glm::vec3>(), std::runtime_error);
    }

    TEST(DataAccessStructuredViewTest, MissingDimensionsThrows)
    {
        DataVariant variant = std::vector<float>(6, 0.0F);
        const std::vector<DataDimension> dims;
        const DataAccess access(variant, dims, DataModality::UNKNOWN);

        EXPECT_THROW((void)access.view<glm::vec3>(), std::runtime_error);
    }

    TEST(DataAccessStructuredViewTest, TooLittleStorageForTheDeclaredElementsThrows)
    {
        DataVariant variant = std::vector<float>(5, 0.0F);
        const std::vector<DataDimension> dims { DataDimension::vertex_positions(2) };
        const DataAccess access(variant, dims, DataModality::VERTEX_POSITIONS_3D);

        EXPECT_THROW((void)access.view<glm::vec3>(), std::runtime_error);
    }

    TEST(DataAccessStructuredViewTest, ElementStorageReadsBackAsItsOwnType)
    {
        DataVariant variant = std::vector<glm::vec3> { { 1, 2, 3 }, { 4, 5, 6 } };
        const std::vector<DataDimension> dims { DataDimension::vertex_positions(2) };
        const DataAccess access(variant, dims, DataModality::VERTEX_POSITIONS_3D);

        const auto view = access.view<glm::vec3>();

        ASSERT_EQ(view.size(), 2U);
        EXPECT_EQ(view[0], glm::vec3(1, 2, 3));
        EXPECT_EQ(view[1], glm::vec3(4, 5, 6));
    }

    TEST(DataAccessStructuredViewTest, ElementStorageAliasesTheStorage)
    {
        DataVariant variant = std::vector<glm::vec3> { { 1, 2, 3 }, { 4, 5, 6 } };
        const std::vector<DataDimension> dims { DataDimension::vertex_positions(2) };
        const DataAccess access(variant, dims, DataModality::VERTEX_POSITIONS_3D);

        const auto view = access.view<glm::vec3>();

        EXPECT_EQ(view.data(), static_cast<const void*>(std::get<std::vector<glm::vec3>>(variant).data()));
        EXPECT_EQ(view.size_bytes(), 2U * sizeof(glm::vec3));
    }

    TEST(DataAccessStructuredViewTest, TooFewElementsInElementStorageThrows)
    {
        DataVariant variant = std::vector<glm::vec3>(1);
        const std::vector<DataDimension> dims { DataDimension::vertex_positions(2) };
        const DataAccess access(variant, dims, DataModality::VERTEX_POSITIONS_3D);

        EXPECT_THROW((void)access.view<glm::vec3>(), std::runtime_error);
    }

    TEST(DataAccessStructuredViewTest, MatrixElementStorageReadsBack)
    {
        DataVariant variant = std::vector<glm::mat4> { glm::mat4(1.0F), glm::mat4(2.0F) };
        const std::vector<DataDimension> dims { DataDimension::grouped("transforms", 2, 16) };
        const DataAccess access(variant, dims, DataModality::TRANSFORMATION_MATRIX);

        const auto view = access.view<glm::mat4>();

        ASSERT_EQ(view.size(), 2U);
        EXPECT_EQ(view[1], glm::mat4(2.0F));
    }

    TEST(StructuredViewTest, TightStrideIsTheComponentCount)
    {
        const std::vector<float> flat { 1, 2, 3, 4, 5, 6 };
        const StructuredView<glm::vec3> view(flat.data(), 2);

        EXPECT_EQ(view.size(), 2U);
        EXPECT_EQ(view.size_bytes(), 6U * sizeof(float));
        EXPECT_EQ(view.data(), static_cast<const void*>(flat.data()));
        EXPECT_EQ(view[1], glm::vec3(4, 5, 6));
    }

    TEST(StructuredViewTest, ByteStrideSkipsPadding)
    {
        const std::vector<float> padded { 1, 2, 99, 3, 4, 99, 5, 6, 99 };
        const StructuredView<glm::vec2> view(padded.data(), 3, 3 * sizeof(float));

        EXPECT_EQ(view[0], glm::vec2(1, 2));
        EXPECT_EQ(view[1], glm::vec2(3, 4));
        EXPECT_EQ(view[2], glm::vec2(5, 6));
        EXPECT_EQ(view.size_bytes(), 9U * sizeof(float));
    }

    TEST(StructuredViewTest, IterationVisitsEveryElementInOrder)
    {
        const std::vector<float> flat { 1, 2, 3, 4, 5, 6, 7, 8 };
        const StructuredView<glm::vec4> view(flat.data(), 2);

        std::vector<glm::vec4> seen;
        for (const glm::vec4 value : view) {
            seen.push_back(value);
        }

        ASSERT_EQ(seen.size(), 2U);
        EXPECT_EQ(seen[0], glm::vec4(1, 2, 3, 4));
        EXPECT_EQ(seen[1], glm::vec4(5, 6, 7, 8));
    }

    TEST(StructuredViewTest, EmptyViewHasEqualBeginAndEnd)
    {
        const std::vector<float> flat;
        const StructuredView<glm::vec3> view(flat.data(), 0);

        EXPECT_TRUE(view.begin() == view.end());
        EXPECT_TRUE(view.to_vector().empty());
    }

    TEST(StructuredViewTest, PostIncrementReturnsThePreviousPosition)
    {
        const std::vector<float> flat { 1, 2, 3, 4 };
        const StructuredView<glm::vec2> view(flat.data(), 2);

        auto it = view.begin();
        const auto previous = it++;

        EXPECT_EQ(*previous, glm::vec2(1, 2));
        EXPECT_EQ(*it, glm::vec2(3, 4));
        EXPECT_TRUE(previous != it);
    }

    TEST(StructuredViewTest, ToVectorCopiesEveryElement)
    {
        const std::vector<float> flat { 1, 2, 3, 4, 5, 6 };
        const StructuredView<glm::vec3> view(flat.data(), 2);

        const auto copy = view.to_vector();

        ASSERT_EQ(copy.size(), 2U);
        EXPECT_EQ(copy[0], glm::vec3(1, 2, 3));
        EXPECT_EQ(copy[1], glm::vec3(4, 5, 6));
    }

    TEST(StructuredViewTest, DoublePrecisionTypesUseDoubleComponents)
    {
        const std::vector<double> flat { 1, 2, 3, 4, 5, 6 };
        const StructuredView<glm::dvec3> view(flat.data(), 2);

        EXPECT_EQ(view.size_bytes(), 6U * sizeof(double));
        EXPECT_EQ(view[1], glm::dvec3(4, 5, 6));
    }

    TEST(StructuredViewTest, MatricesReadColumnMajor)
    {
        std::vector<float> flat(16);
        std::iota(flat.begin(), flat.end(), 0.0F);
        const StructuredView<glm::mat4> view(flat.data(), 1);

        const glm::mat4 m = view[0];

        EXPECT_FLOAT_EQ(m[0][0], 0.0F);
        EXPECT_FLOAT_EQ(m[0][3], 3.0F);
        EXPECT_FLOAT_EQ(m[1][0], 4.0F);
        EXPECT_FLOAT_EQ(m[3][3], 15.0F);
        EXPECT_EQ(view.size_bytes(), 16U * sizeof(float));
    }

} // namespace
} // namespace MayaFlux::Test
