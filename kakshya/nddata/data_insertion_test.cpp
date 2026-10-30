#include "gtest/gtest.h"

#include "MayaFlux/Kakshya/NDData/DataAccess.hpp"
#include "MayaFlux/Kakshya/NDData/DataInsertion.hpp"

namespace MayaFlux::Test {
namespace {

    using Kakshya::DataDimension;
    using Kakshya::DataInsertion;
    using Kakshya::DataModality;
    using Kakshya::DataVariant;
    using Role = Kakshya::DataDimension::Role;

    struct Target {
        DataVariant variant = std::vector<double>();
        std::vector<DataDimension> dims;
        DataModality modality = DataModality::UNKNOWN;
        DataInsertion insertion { variant, dims, modality };
    };

    TEST(DataInsertionScalarTest, ReplaceStoresTheDataAndTheModality)
    {
        Target t;

        t.insertion.insert_scalar<float>({ 1.0F, 2.0F, 3.0F }, DataModality::AUDIO_1D);

        ASSERT_TRUE(std::holds_alternative<std::vector<float>>(t.variant));
        EXPECT_EQ(std::get<std::vector<float>>(t.variant), (std::vector<float> { 1.0F, 2.0F, 3.0F }));
        EXPECT_EQ(t.modality, DataModality::AUDIO_1D);
        EXPECT_EQ(t.insertion.current_modality(), DataModality::AUDIO_1D);
    }

    TEST(DataInsertionScalarTest, ReplaceDescribesTheDataWithOneDimension)
    {
        Target t;

        t.insertion.insert_scalar<double>({ 1.0, 2.0, 3.0, 4.0 }, DataModality::AUDIO_1D);

        ASSERT_EQ(t.dims.size(), 1U);
        EXPECT_EQ(t.dims[0].name, "samples");
        EXPECT_EQ(t.dims[0].size, 4U);
        EXPECT_EQ(t.dims[0].stride, 1U);
        EXPECT_EQ(t.dims[0].role, Role::TIME);
    }

    TEST(DataInsertionScalarTest, ReplaceDropsPreviousDimensionsAndData)
    {
        Target t;
        t.insertion.insert_scalar<double>({ 1.0, 2.0, 3.0, 4.0 }, DataModality::AUDIO_1D);

        t.insertion.insert_scalar<float>({ 9.0F }, DataModality::SCALAR_F32);

        ASSERT_TRUE(std::holds_alternative<std::vector<float>>(t.variant));
        EXPECT_EQ(std::get<std::vector<float>>(t.variant).size(), 1U);
        ASSERT_EQ(t.dims.size(), 1U);
        EXPECT_EQ(t.dims[0].size, 1U);
        EXPECT_EQ(t.dims[0].name, "data");
        EXPECT_EQ(t.dims[0].role, Role::CUSTOM);
    }

    TEST(DataInsertionScalarTest, UnnamedModalitiesAreCalledData)
    {
        Target t;

        t.insertion.insert_scalar<float>({ 1.0F }, DataModality::IMAGE_2D);

        EXPECT_EQ(t.dims[0].name, "data");
        EXPECT_EQ(t.dims[0].role, Role::CUSTOM);
    }

    TEST(DataInsertionScalarTest, MultichannelAudioIsNamedChannelsOnTheTimeRole)
    {
        Target t;

        t.insertion.insert_scalar<double>({ 1.0, 2.0 }, DataModality::AUDIO_MULTICHANNEL);

        EXPECT_EQ(t.dims[0].name, "channels");
        EXPECT_EQ(t.dims[0].role, Role::TIME);
    }

    TEST(DataInsertionScalarTest, StructuredModalitiesRejectScalars)
    {
        Target t;

        EXPECT_THROW(
            t.insertion.insert_scalar<float>({ 1.0F, 2.0F, 3.0F }, DataModality::VERTEX_POSITIONS_3D),
            std::invalid_argument);
        EXPECT_TRUE(t.insertion.is_empty());
        EXPECT_TRUE(t.dims.empty());
    }

    TEST(DataInsertionScalarTest, AppendExtendsMatchingStorage)
    {
        Target t;
        t.insertion.insert_scalar<float>({ 1.0F, 2.0F }, DataModality::AUDIO_1D);

        t.insertion.insert_scalar<float>({ 3.0F, 4.0F, 5.0F }, DataModality::AUDIO_1D, false);

        EXPECT_EQ(std::get<std::vector<float>>(t.variant), (std::vector<float> { 1, 2, 3, 4, 5 }));
        ASSERT_EQ(t.dims.size(), 1U);
        EXPECT_EQ(t.dims[0].size, 5U);
    }

    TEST(DataInsertionScalarTest, AppendConvertsToTheExistingElementType)
    {
        Target t;
        t.insertion.insert_scalar<float>({ 1.0F }, DataModality::AUDIO_1D);

        t.insertion.insert_scalar<double>({ 2.5, 3.5 }, DataModality::AUDIO_1D, false);

        ASSERT_TRUE(std::holds_alternative<std::vector<float>>(t.variant));
        EXPECT_EQ(std::get<std::vector<float>>(t.variant), (std::vector<float> { 1.0F, 2.5F, 3.5F }));
    }

    TEST(DataInsertionScalarTest, AppendToEmptyDimensionsLeavesThemEmpty)
    {
        Target t;

        t.insertion.insert_scalar<double>({ 1.0, 2.0 }, DataModality::AUDIO_1D, false);

        EXPECT_EQ(std::get<std::vector<double>>(t.variant).size(), 2U);
        EXPECT_TRUE(t.dims.empty());
    }

    TEST(DataInsertionScalarTest, AppendDoesNotChangeTheModality)
    {
        Target t;
        t.insertion.insert_scalar<float>({ 1.0F }, DataModality::AUDIO_1D);

        t.insertion.insert_scalar<float>({ 2.0F }, DataModality::IMAGE_2D, false);

        EXPECT_EQ(t.modality, DataModality::AUDIO_1D);
    }

    TEST(DataInsertionScalarTest, AppendOntoStructuredStorageThrows)
    {
        Target t;
        t.insertion.insert_structured<glm::vec3>({ { 1, 2, 3 } }, DataModality::VERTEX_POSITIONS_3D);

        EXPECT_THROW(
            t.insertion.insert_scalar<float>({ 1.0F }, DataModality::AUDIO_1D, false),
            std::invalid_argument);
    }

    TEST(DataInsertionStructuredTest, ReplaceStoresTheElements)
    {
        Target t;

        t.insertion.insert_structured<glm::vec3>(
            { { 1, 2, 3 }, { 4, 5, 6 } }, DataModality::VERTEX_POSITIONS_3D);

        ASSERT_TRUE(std::holds_alternative<std::vector<glm::vec3>>(t.variant));
        const auto& stored = std::get<std::vector<glm::vec3>>(t.variant);
        ASSERT_EQ(stored.size(), 2U);
        EXPECT_EQ(stored[1], glm::vec3(4, 5, 6));
        EXPECT_EQ(t.modality, DataModality::VERTEX_POSITIONS_3D);
    }

    TEST(DataInsertionStructuredTest, PositionsGetAGroupedPositionDimension)
    {
        Target t;

        t.insertion.insert_structured<glm::vec3>(
            { { 1, 2, 3 }, { 4, 5, 6 } }, DataModality::VERTEX_POSITIONS_3D);

        ASSERT_EQ(t.dims.size(), 1U);
        EXPECT_EQ(t.dims[0].name, "positions");
        EXPECT_EQ(t.dims[0].size, 2U);
        EXPECT_EQ(t.dims[0].role, Role::POSITION);
        ASSERT_TRUE(t.dims[0].grouping.has_value());
        EXPECT_EQ(t.dims[0].grouping->count, 3U);
    }

    TEST(DataInsertionStructuredTest, NormalsGetANormalDimension)
    {
        Target t;

        t.insertion.insert_structured<glm::vec3>({ { 0, 1, 0 } }, DataModality::VERTEX_NORMALS_3D);

        EXPECT_EQ(t.dims[0].name, "normals");
        EXPECT_EQ(t.dims[0].role, Role::NORMAL);
        EXPECT_EQ(t.dims[0].grouping->count, 3U);
    }

    TEST(DataInsertionStructuredTest, UvCoordinatesGetAUvDimension)
    {
        Target t;

        t.insertion.insert_structured<glm::vec2>({ { 0, 1 }, { 1, 0 } }, DataModality::TEXTURE_COORDS_2D);

        EXPECT_EQ(t.dims[0].name, "uvs");
        EXPECT_EQ(t.dims[0].size, 2U);
        EXPECT_EQ(t.dims[0].role, Role::UV);
        EXPECT_EQ(t.dims[0].grouping->count, 2U);
    }

    TEST(DataInsertionStructuredTest, ColorsGroupByTheirComponentCount)
    {
        Target rgb;
        Target rgba;

        rgb.insertion.insert_structured<glm::vec3>({ { 1, 0, 0 } }, DataModality::VERTEX_COLORS_RGB);
        rgba.insertion.insert_structured<glm::vec4>({ { 1, 0, 0, 1 } }, DataModality::VERTEX_COLORS_RGBA);

        EXPECT_EQ(rgb.dims[0].name, "colors");
        EXPECT_EQ(rgb.dims[0].role, Role::COLOR);
        EXPECT_EQ(rgb.dims[0].grouping->count, 3U);
        EXPECT_EQ(rgba.dims[0].grouping->count, 4U);
    }

    TEST(DataInsertionStructuredTest, TangentsUseTheGenericGroupedDimension)
    {
        Target t3;
        Target t4;

        t3.insertion.insert_structured<glm::vec3>({ { 1, 0, 0 } }, DataModality::VERTEX_TANGENTS_3D);
        t4.insertion.insert_structured<glm::vec4>({ { 1, 0, 0, 1 } }, DataModality::VERTEX_TANGENTS_4D);

        EXPECT_EQ(t3.dims[0].name, "tangents");
        EXPECT_EQ(t3.dims[0].grouping->count, 3U);
        EXPECT_EQ(t4.dims[0].name, "tangents");
        EXPECT_EQ(t4.dims[0].grouping->count, 4U);
    }

    TEST(DataInsertionStructuredTest, MatricesGroupSixteenComponents)
    {
        Target t;

        t.insertion.insert_structured<glm::mat4>({ glm::mat4(1.0F), glm::mat4(2.0F) }, DataModality::TRANSFORMATION_MATRIX);

        ASSERT_TRUE(std::holds_alternative<std::vector<glm::mat4>>(t.variant));
        EXPECT_EQ(t.dims[0].name, "transforms");
        EXPECT_EQ(t.dims[0].size, 2U);
        EXPECT_EQ(t.dims[0].grouping->count, 16U);
    }

    TEST(DataInsertionStructuredTest, ComponentCountMustMatchTheModality)
    {
        Target t;

        EXPECT_THROW(
            t.insertion.insert_structured<glm::vec2>({ { 1, 2 } }, DataModality::VERTEX_POSITIONS_3D),
            std::invalid_argument);
        EXPECT_THROW(
            t.insertion.insert_structured<glm::vec3>({ { 1, 2, 3 } }, DataModality::VERTEX_COLORS_RGBA),
            std::invalid_argument);
        EXPECT_THROW(
            t.insertion.insert_structured<glm::vec4>({ { 1, 2, 3, 4 } }, DataModality::TEXTURE_COORDS_2D),
            std::invalid_argument);
        EXPECT_THROW(
            t.insertion.insert_structured<glm::vec4>({ { 1, 2, 3, 4 } }, DataModality::TRANSFORMATION_MATRIX),
            std::invalid_argument);
        EXPECT_TRUE(t.insertion.is_empty());
    }

    TEST(DataInsertionStructuredTest, NonStructuredModalitiesRejectGlmData)
    {
        Target t;

        EXPECT_THROW(
            t.insertion.insert_structured<glm::vec3>({ { 1, 2, 3 } }, DataModality::AUDIO_1D),
            std::invalid_argument);
        EXPECT_THROW(
            t.insertion.insert_structured<glm::vec3>({ { 1, 2, 3 } }, DataModality::UNKNOWN),
            std::invalid_argument);
    }

    TEST(DataInsertionStructuredTest, AppendExtendsMatchingStorageAndTheDimension)
    {
        Target t;
        t.insertion.insert_structured<glm::vec3>({ { 1, 2, 3 } }, DataModality::VERTEX_POSITIONS_3D);

        t.insertion.insert_structured<glm::vec3>({ { 4, 5, 6 }, { 7, 8, 9 } }, DataModality::VERTEX_POSITIONS_3D, false);

        const auto& stored = std::get<std::vector<glm::vec3>>(t.variant);
        ASSERT_EQ(stored.size(), 3U);
        EXPECT_EQ(stored[2], glm::vec3(7, 8, 9));
        EXPECT_EQ(t.dims[0].size, 3U);
    }

    TEST(DataInsertionStructuredTest, AppendOntoADifferentStorageTypeThrows)
    {
        Target t;
        t.insertion.insert_scalar<float>({ 1.0F }, DataModality::AUDIO_1D);

        EXPECT_THROW(
            t.insertion.insert_structured<glm::vec3>({ { 1, 2, 3 } }, DataModality::VERTEX_POSITIONS_3D, false),
            std::invalid_argument);
    }

    TEST(DataInsertionStructuredTest, AppendOntoADifferentGlmTypeThrows)
    {
        Target t;
        t.insertion.insert_structured<glm::vec3>({ { 1, 2, 3 } }, DataModality::VERTEX_POSITIONS_3D);

        EXPECT_THROW(
            t.insertion.insert_structured<glm::vec4>({ { 1, 2, 3, 4 } }, DataModality::VERTEX_COLORS_RGBA, false),
            std::invalid_argument);
    }

    TEST(DataInsertionStructuredTest, InsertedElementsReadBackThroughAnAccessOfTheSameDimensions)
    {
        Target t;
        t.insertion.insert_structured<glm::vec3>({ { 1, 2, 3 }, { 4, 5, 6 } }, DataModality::VERTEX_POSITIONS_3D);

        const Kakshya::DataAccess access(t.variant, t.dims, t.modality);
        const auto view = access.view<glm::vec3>();

        ASSERT_EQ(view.size(), 2U);
        EXPECT_EQ(view[1], glm::vec3(4, 5, 6));
    }

    TEST(DataInsertionSpanTest, ScalarSpansAreCopied)
    {
        Target t;
        std::vector<float> source { 1.0F, 2.0F, 3.0F };

        t.insertion.insert_from_span<float>(std::span<const float>(source), DataModality::AUDIO_1D);
        source[0] = 99.0F;

        const auto& stored = std::get<std::vector<float>>(t.variant);
        EXPECT_EQ(stored, (std::vector<float> { 1.0F, 2.0F, 3.0F }));
        EXPECT_NE(stored.data(), source.data());
        EXPECT_EQ(t.dims[0].size, 3U);
    }

    TEST(DataInsertionSpanTest, GlmSpansTakeTheStructuredPath)
    {
        Target t;
        const std::vector<glm::vec2> source { { 0, 0 }, { 1, 1 } };

        t.insertion.insert_from_span<glm::vec2>(std::span<const glm::vec2>(source), DataModality::TEXTURE_COORDS_2D);

        ASSERT_TRUE(std::holds_alternative<std::vector<glm::vec2>>(t.variant));
        EXPECT_EQ(t.dims[0].name, "uvs");
        EXPECT_EQ(t.dims[0].size, 2U);
    }

    TEST(DataInsertionConvertTest, ScalarsAreConvertedToTheTargetType)
    {
        Target t;
        const std::vector<double> source { 0.5, 1.5, 2.5 };

        t.insertion.insert_converted<double, float>(source, DataModality::AUDIO_1D);

        ASSERT_TRUE(std::holds_alternative<std::vector<float>>(t.variant));
        EXPECT_EQ(std::get<std::vector<float>>(t.variant), (std::vector<float> { 0.5F, 1.5F, 2.5F }));
        EXPECT_EQ(t.dims[0].size, 3U);
    }

    TEST(DataInsertionConvertTest, SourceIsLeftUntouched)
    {
        Target t;
        const std::vector<uint8_t> source { 1, 2, 3 };

        t.insertion.insert_converted<uint8_t, float>(source, DataModality::AUDIO_1D);

        EXPECT_EQ(source, (std::vector<uint8_t> { 1, 2, 3 }));
    }

    TEST(DataInsertionReserveTest, StructuredModalitiesSelectTheirGlmStorage)
    {
        const std::vector<std::pair<DataModality, size_t>> cases {
            { DataModality::VERTEX_POSITIONS_3D, 8 },
            { DataModality::VERTEX_NORMALS_3D, 8 },
            { DataModality::VERTEX_TANGENTS_3D, 8 },
            { DataModality::VERTEX_COLORS_RGB, 8 },
            { DataModality::TEXTURE_COORDS_2D, 7 },
            { DataModality::VERTEX_TANGENTS_4D, 9 },
            { DataModality::VERTEX_COLORS_RGBA, 9 },
            { DataModality::TRANSFORMATION_MATRIX, 10 },
        };

        for (const auto& [modality, index] : cases) {
            Target t;
            t.insertion.reserve_space(16, modality);

            EXPECT_EQ(t.variant.index(), index) << static_cast<int>(modality);
            EXPECT_EQ(t.modality, modality);
            EXPECT_TRUE(t.insertion.is_empty());
        }
    }

    TEST(DataInsertionReserveTest, ReservedCapacityCoversTheRequest)
    {
        Target t;

        t.insertion.reserve_space(64, DataModality::VERTEX_POSITIONS_3D);

        EXPECT_GE(std::get<std::vector<glm::vec3>>(t.variant).capacity(), 64U);
        EXPECT_EQ(std::get<std::vector<glm::vec3>>(t.variant).size(), 0U);
    }

    TEST(DataInsertionReserveTest, ScalarModalitiesReserveDoubles)
    {
        Target t;
        t.insertion.insert_scalar<float>({ 1.0F }, DataModality::IMAGE_2D);

        t.insertion.reserve_space(32, DataModality::AUDIO_1D);

        ASSERT_TRUE(std::holds_alternative<std::vector<double>>(t.variant));
        EXPECT_GE(std::get<std::vector<double>>(t.variant).capacity(), 32U);
        EXPECT_EQ(t.modality, DataModality::AUDIO_1D);
    }

    TEST(DataInsertionReserveTest, ReserveDiscardsExistingData)
    {
        Target t;
        t.insertion.insert_structured<glm::vec3>({ { 1, 2, 3 } }, DataModality::VERTEX_POSITIONS_3D);

        t.insertion.reserve_space(8, DataModality::VERTEX_NORMALS_3D);

        EXPECT_TRUE(t.insertion.is_empty());
    }

    TEST(DataInsertionStateTest, FreshTargetIsEmpty)
    {
        Target t;

        EXPECT_TRUE(t.insertion.is_empty());
        EXPECT_EQ(t.insertion.current_modality(), DataModality::UNKNOWN);
    }

    TEST(DataInsertionStateTest, InsertedDataIsNotEmpty)
    {
        Target t;

        t.insertion.insert_scalar<double>({ 1.0 }, DataModality::AUDIO_1D);

        EXPECT_FALSE(t.insertion.is_empty());
    }

    TEST(DataInsertionStateTest, ClearRemovesDataAndDimensionsButKeepsTheModality)
    {
        Target t;
        t.insertion.insert_scalar<double>({ 1.0, 2.0 }, DataModality::AUDIO_1D);

        t.insertion.clear_data();

        EXPECT_TRUE(t.insertion.is_empty());
        EXPECT_TRUE(t.dims.empty());
        EXPECT_EQ(t.insertion.current_modality(), DataModality::AUDIO_1D);
    }

    TEST(DataInsertionStateTest, ClearKeepsTheStorageType)
    {
        Target t;
        t.insertion.insert_structured<glm::vec3>({ { 1, 2, 3 } }, DataModality::VERTEX_POSITIONS_3D);

        t.insertion.clear_data();

        EXPECT_TRUE(std::holds_alternative<std::vector<glm::vec3>>(t.variant));
    }

} // namespace
} // namespace MayaFlux::Test
