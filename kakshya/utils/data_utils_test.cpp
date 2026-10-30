#include "gtest/gtest.h"

#include "MayaFlux/Kakshya/Utils/DataUtils.hpp"

namespace MayaFlux::Test {
namespace {

    using Kakshya::ComplexConversionStrategy;
    using Kakshya::DataDimension;
    using Kakshya::DataModality;
    using Kakshya::DataVariant;
    using Role = Kakshya::DataDimension::Role;

    template <typename T>
    std::vector<std::remove_const_t<T>> copy_of(std::span<T> values)
    {
        return std::vector<std::remove_const_t<T>>(values.begin(), values.end());
    }

    glm::mat4 counting_matrix()
    {
        glm::mat4 m(0.0F);
        for (int c = 0; c < 4; ++c) {
            for (int r = 0; r < 4; ++r) {
                m[c][r] = static_cast<float>(c * 4 + r);
            }
        }
        return m;
    }

    // ---------------------------------------------------------------------
    // Concepts the converters depend on
    // ---------------------------------------------------------------------

    TEST(DataConceptTest, ComplexDataIsExactlyTheStdComplexTypes)
    {
        EXPECT_TRUE(ComplexData<std::complex<float>>);
        EXPECT_TRUE(ComplexData<std::complex<double>>);
        EXPECT_FALSE(ComplexData<float>);
        EXPECT_FALSE(ComplexData<uint8_t>);
        EXPECT_FALSE(ComplexData<std::string>);
        EXPECT_FALSE(ComplexData<std::vector<float>>);
        EXPECT_FALSE(ComplexData<glm::vec3>);
        EXPECT_FALSE(ComplexData<glm::mat4>);
    }

    TEST(DataConceptTest, ProcessableDataIsNumericComplexOrGlm)
    {
        EXPECT_TRUE(ProcessableData<float>);
        EXPECT_TRUE(ProcessableData<uint16_t>);
        EXPECT_TRUE(ProcessableData<std::complex<double>>);
        EXPECT_TRUE(ProcessableData<glm::vec2>);
        EXPECT_TRUE(ProcessableData<glm::mat4>);
        EXPECT_FALSE(ProcessableData<std::string>);
        EXPECT_FALSE(ProcessableData<std::vector<float>>);
    }

    TEST(DataConceptTest, ConversionsExistBetweenCompatibleKinds)
    {
        EXPECT_TRUE((is_convertible_data_v<float, double>));
        EXPECT_TRUE((is_convertible_data_v<uint8_t, float>));
        EXPECT_TRUE((is_convertible_data_v<std::complex<float>, double>));
        EXPECT_TRUE((is_convertible_data_v<float, std::complex<double>>));
        EXPECT_TRUE((is_convertible_data_v<std::complex<float>, std::complex<double>>));
        EXPECT_TRUE((is_convertible_data_v<glm::vec3, glm::dvec3>));
        EXPECT_TRUE((is_convertible_data_v<glm::vec3, float>));
        EXPECT_TRUE((is_convertible_data_v<float, glm::vec3>));
    }

    TEST(DataConceptTest, NoConversionExistsBetweenIncompatibleKinds)
    {
        EXPECT_FALSE((is_convertible_data_v<glm::vec3, glm::vec4>));
        EXPECT_FALSE((is_convertible_data_v<glm::vec3, glm::vec2>));
        EXPECT_FALSE((is_convertible_data_v<glm::vec3, std::complex<float>>));
        EXPECT_FALSE((is_convertible_data_v<std::complex<float>, glm::vec3>));
        EXPECT_FALSE((is_convertible_data_v<std::string, float>));
        EXPECT_FALSE((is_convertible_data_v<std::vector<float>, double>));
    }

    // ---------------------------------------------------------------------
    // DataConverter via convert_data
    // ---------------------------------------------------------------------

    TEST(DataUtilsConvertTest, IdentityReturnsTheSourceSpanAndLeavesStorageAlone)
    {
        std::vector<float> source { 1, 2, 3 };
        std::vector<float> storage { 9, 9 };

        const auto result = Kakshya::convert_data(std::span<float>(source), storage);

        EXPECT_EQ(result.data(), source.data());
        EXPECT_EQ(result.size(), 3U);
        EXPECT_EQ(storage, (std::vector<float> { 9, 9 }));
    }

    TEST(DataUtilsConvertTest, ArithmeticTypesConvertValueForValue)
    {
        std::vector<double> source { 0.5, 1.5, 250.0 };
        std::vector<uint8_t> bytes;
        std::vector<float> floats;

        const auto as_bytes = Kakshya::convert_data(std::span<double>(source), bytes);
        const auto as_floats = Kakshya::convert_data(std::span<double>(source), floats);

        EXPECT_EQ(copy_of(as_bytes), (std::vector<uint8_t> { 0, 1, 250 }));
        EXPECT_EQ(copy_of(as_floats), (std::vector<float> { 0.5F, 1.5F, 250.0F }));
        EXPECT_EQ(as_bytes.data(), bytes.data());
    }

    TEST(DataUtilsConvertTest, ConversionReplacesWhateverStorageHeld)
    {
        std::vector<float> source { 1, 2 };
        std::vector<double> storage(10, 7.0);

        const auto result = Kakshya::convert_data(std::span<float>(source), storage);

        EXPECT_EQ(storage, (std::vector<double> { 1.0, 2.0 }));
        EXPECT_EQ(result.size(), 2U);
    }

    TEST(DataUtilsConvertTest, EmptySourcesGiveEmptyResults)
    {
        std::vector<float> source;
        std::vector<double> storage(3, 1.0);

        const auto result = Kakshya::convert_data(std::span<float>(source), storage);

        EXPECT_TRUE(result.empty());
        EXPECT_TRUE(storage.empty());
    }

    class ComplexStrategyTest : public ::testing::TestWithParam<std::pair<ComplexConversionStrategy, double>> { };

    TEST_P(ComplexStrategyTest, ComplexValuesReduceByTheStrategy)
    {
        std::vector<std::complex<double>> source { { 3.0, 4.0 } };
        std::vector<double> storage;

        const auto result = Kakshya::convert_data(std::span<std::complex<double>>(source), storage, GetParam().first);

        ASSERT_EQ(result.size(), 1U);
        EXPECT_DOUBLE_EQ(result[0], GetParam().second);
    }

    INSTANTIATE_TEST_SUITE_P(Strategies, ComplexStrategyTest,
        ::testing::Values(
            std::pair { ComplexConversionStrategy::MAGNITUDE, 5.0 },
            std::pair { ComplexConversionStrategy::REAL_PART, 3.0 },
            std::pair { ComplexConversionStrategy::IMAG_PART, 4.0 },
            std::pair { ComplexConversionStrategy::SQUARED_MAGNITUDE, 25.0 }));

    TEST(DataUtilsConvertTest, ComplexConversionDefaultsToMagnitude)
    {
        std::vector<std::complex<float>> source { { 0.0F, -2.0F } };
        std::vector<float> storage;

        const auto result = Kakshya::convert_data(std::span<std::complex<float>>(source), storage);

        EXPECT_FLOAT_EQ(result[0], 2.0F);
    }

    TEST(DataUtilsConvertTest, ArithmeticValuesBecomeComplexWithZeroImaginaryPart)
    {
        std::vector<float> source { 1.5F, -2.0F };
        std::vector<std::complex<double>> storage;

        const auto result = Kakshya::convert_data(std::span<float>(source), storage);

        EXPECT_EQ(copy_of(result), (std::vector<std::complex<double>> { { 1.5, 0.0 }, { -2.0, 0.0 } }));
    }

    TEST(DataUtilsConvertTest, ComplexPrecisionChangesComponentWise)
    {
        std::vector<std::complex<float>> singles { { 1.5F, -2.5F } };
        std::vector<std::complex<double>> doubles { { 0.25, 0.5 } };
        std::vector<std::complex<double>> widened;
        std::vector<std::complex<float>> narrowed;

        const auto up = Kakshya::convert_data(std::span<std::complex<float>>(singles), widened);
        const auto down = Kakshya::convert_data(std::span<std::complex<double>>(doubles), narrowed);

        EXPECT_EQ(copy_of(up), (std::vector<std::complex<double>> { { 1.5, -2.5 } }));
        EXPECT_EQ(copy_of(down), (std::vector<std::complex<float>> { { 0.25F, 0.5F } }));
    }

    TEST(DataUtilsConvertTest, VectorsFlattenComponentByComponent)
    {
        std::vector<glm::vec2> v2 { { 1, 2 }, { 3, 4 } };
        std::vector<glm::vec3> v3 { { 1, 2, 3 } };
        std::vector<glm::vec4> v4 { { 1, 2, 3, 4 } };
        std::vector<float> flat2;
        std::vector<double> flat3;
        std::vector<float> flat4;

        EXPECT_EQ(copy_of(Kakshya::convert_data(std::span<glm::vec2>(v2), flat2)), (std::vector<float> { 1, 2, 3, 4 }));
        EXPECT_EQ(copy_of(Kakshya::convert_data(std::span<glm::vec3>(v3), flat3)), (std::vector<double> { 1, 2, 3 }));
        EXPECT_EQ(copy_of(Kakshya::convert_data(std::span<glm::vec4>(v4), flat4)), (std::vector<float> { 1, 2, 3, 4 }));
    }

    TEST(DataUtilsConvertTest, MatricesFlattenColumnMajor)
    {
        std::vector<glm::mat4> source { counting_matrix() };
        std::vector<float> storage;

        const auto result = Kakshya::convert_data(std::span<glm::mat4>(source), storage);

        ASSERT_EQ(result.size(), 16U);
        for (size_t j = 0; j < 16; ++j) {
            EXPECT_FLOAT_EQ(result[j], static_cast<float>(j)) << j;
        }
    }

    TEST(DataUtilsConvertTest, DoublePrecisionVectorsFlattenToFloat)
    {
        std::vector<glm::dvec3> source { { 0.5, 1.5, 2.5 } };
        std::vector<float> storage;

        const auto result = Kakshya::convert_data(std::span<glm::dvec3>(source), storage);

        EXPECT_EQ(copy_of(result), (std::vector<float> { 0.5F, 1.5F, 2.5F }));
    }

    TEST(DataUtilsConvertTest, ScalarsStructureIntoVectors)
    {
        std::vector<float> six { 1, 2, 3, 4, 5, 6 };
        std::vector<glm::vec3> v3;
        std::vector<glm::vec2> v2;
        std::vector<glm::vec4> v4;
        std::vector<float> four { 1, 2, 3, 4 };

        EXPECT_EQ(copy_of(Kakshya::convert_data(std::span<float>(six), v3)), (std::vector<glm::vec3> { { 1, 2, 3 }, { 4, 5, 6 } }));
        EXPECT_EQ(copy_of(Kakshya::convert_data(std::span<float>(six), v2)), (std::vector<glm::vec2> { { 1, 2 }, { 3, 4 }, { 5, 6 } }));
        EXPECT_EQ(copy_of(Kakshya::convert_data(std::span<float>(four), v4)), (std::vector<glm::vec4> { { 1, 2, 3, 4 } }));
    }

    TEST(DataUtilsConvertTest, ScalarsStructureIntoMatricesColumnMajor)
    {
        std::vector<float> source(16);
        std::iota(source.begin(), source.end(), 0.0F);
        std::vector<glm::mat4> storage;

        const auto result = Kakshya::convert_data(std::span<float>(source), storage);

        ASSERT_EQ(result.size(), 1U);
        EXPECT_EQ(result[0], counting_matrix());
    }

    TEST(DataUtilsConvertTest, StructuringConvertsTheScalarType)
    {
        std::vector<double> source { 0.5, 1.5, 2.5 };
        std::vector<glm::vec3> storage;

        const auto result = Kakshya::convert_data(std::span<double>(source), storage);

        EXPECT_EQ(result[0], glm::vec3(0.5F, 1.5F, 2.5F));
    }

    TEST(DataUtilsConvertTest, StructuringNeedsWholeElements)
    {
        std::vector<float> source { 1, 2, 3, 4 };
        std::vector<glm::vec3> storage;

        EXPECT_THROW((void)Kakshya::convert_data(std::span<float>(source), storage), std::invalid_argument);
    }

    TEST(DataUtilsConvertTest, VectorsOfEqualWidthChangePrecision)
    {
        std::vector<glm::vec3> singles { { 1, 2, 3 } };
        std::vector<glm::dvec4> doubles { { 0.5, 1.5, 2.5, 3.5 } };
        std::vector<glm::dvec3> widened;
        std::vector<glm::vec4> narrowed;

        EXPECT_EQ(copy_of(Kakshya::convert_data(std::span<glm::vec3>(singles), widened)), (std::vector<glm::dvec3> { { 1, 2, 3 } }));
        EXPECT_EQ(copy_of(Kakshya::convert_data(std::span<glm::dvec4>(doubles), narrowed)), (std::vector<glm::vec4> { { 0.5F, 1.5F, 2.5F, 3.5F } }));
    }

    TEST(DataUtilsConvertTest, LegacyComplexConversionFillsTheDestinationSpan)
    {
        std::vector<std::complex<float>> source { { 3.0F, 4.0F }, { 6.0F, 8.0F } };
        std::vector<float> destination(4, -1.0F);

        Kakshya::convert_complex(std::span<std::complex<float>>(source), std::span<float>(destination), ComplexConversionStrategy::MAGNITUDE);

        EXPECT_EQ(destination, (std::vector<float> { 5.0F, 10.0F, -1.0F, -1.0F }));
    }

    TEST(DataUtilsConvertTest, LegacyComplexConversionTruncatesToTheDestination)
    {
        std::vector<std::complex<float>> source { { 1.0F, 0.0F }, { 2.0F, 0.0F }, { 3.0F, 0.0F } };
        std::vector<float> destination(2, 0.0F);

        Kakshya::convert_complex(std::span<std::complex<float>>(source), std::span<float>(destination), ComplexConversionStrategy::REAL_PART);

        EXPECT_EQ(destination, (std::vector<float> { 1.0F, 2.0F }));
    }

    // ---------------------------------------------------------------------
    // Dimensions arithmetic
    // ---------------------------------------------------------------------

    TEST(DataUtilsDimensionTest, TotalElementsIsTheProductOfExtents)
    {
        EXPECT_EQ(Kakshya::calculate_total_elements({ DataDimension::time(10), DataDimension::channel(3) }), 30U);
        EXPECT_EQ(Kakshya::calculate_total_elements({ DataDimension::time(7) }), 7U);
    }

    TEST(DataUtilsDimensionTest, NoDimensionsMeanNoElements)
    {
        EXPECT_EQ(Kakshya::calculate_total_elements({}), 0U);
        EXPECT_EQ(Kakshya::calculate_frame_size({}), 0U);
    }

    TEST(DataUtilsDimensionTest, AZeroExtentEmptiesTheTotal)
    {
        EXPECT_EQ(Kakshya::calculate_total_elements({ DataDimension::time(10), DataDimension::channel(0) }), 0U);
    }

    TEST(DataUtilsDimensionTest, FrameSizeIsTheProductOfAllButTheFirstExtent)
    {
        const std::vector<DataDimension> dims { DataDimension::time(100), DataDimension::spatial(4, 'y'), DataDimension::spatial(5, 'x') };

        EXPECT_EQ(Kakshya::calculate_frame_size(dims), 20U);
    }

    TEST(DataUtilsDimensionTest, ASingleDimensionHasFramesOfOneElement)
    {
        EXPECT_EQ(Kakshya::calculate_frame_size({ DataDimension::time(100) }), 1U);
    }

    TEST(DataUtilsDimensionTest, FindDimensionByRoleReturnsTheFirstMatch)
    {
        const std::vector<DataDimension> dims {
            DataDimension::time(10),
            DataDimension::channel(2),
            DataDimension::spatial(4, 'x'),
            DataDimension::spatial(4, 'y'),
        };

        EXPECT_EQ(Kakshya::find_dimension_by_role(dims, Role::TIME), 0);
        EXPECT_EQ(Kakshya::find_dimension_by_role(dims, Role::CHANNEL), 1);
        EXPECT_EQ(Kakshya::find_dimension_by_role(dims, Role::SPATIAL_Y), 3);
    }

    TEST(DataUtilsDimensionTest, FindDimensionByRoleReportsAbsenceAsMinusOne)
    {
        EXPECT_EQ(Kakshya::find_dimension_by_role({ DataDimension::time(10) }, Role::FREQUENCY), -1);
        EXPECT_EQ(Kakshya::find_dimension_by_role({}, Role::TIME), -1);
    }

    // ---------------------------------------------------------------------
    // Variant element type
    // ---------------------------------------------------------------------

    TEST(DataUtilsVariantTest, ElementTypeIsTheActiveAlternativesValueType)
    {
        EXPECT_EQ(Kakshya::get_variant_element_type(DataVariant { std::vector<double> {} }), std::type_index(typeid(double)));
        EXPECT_EQ(Kakshya::get_variant_element_type(DataVariant { std::vector<float> {} }), std::type_index(typeid(float)));
        EXPECT_EQ(Kakshya::get_variant_element_type(DataVariant { std::vector<uint8_t> {} }), std::type_index(typeid(uint8_t)));
        EXPECT_EQ(Kakshya::get_variant_element_type(DataVariant { std::vector<uint16_t> {} }), std::type_index(typeid(uint16_t)));
        EXPECT_EQ(Kakshya::get_variant_element_type(DataVariant { std::vector<uint32_t> {} }), std::type_index(typeid(uint32_t)));
        EXPECT_EQ(Kakshya::get_variant_element_type(DataVariant { std::vector<std::complex<float>> {} }), std::type_index(typeid(std::complex<float>)));
        EXPECT_EQ(Kakshya::get_variant_element_type(DataVariant { std::vector<std::complex<double>> {} }), std::type_index(typeid(std::complex<double>)));
        EXPECT_EQ(Kakshya::get_variant_element_type(DataVariant { std::vector<glm::vec2> {} }), std::type_index(typeid(glm::vec2)));
        EXPECT_EQ(Kakshya::get_variant_element_type(DataVariant { std::vector<glm::vec3> {} }), std::type_index(typeid(glm::vec3)));
        EXPECT_EQ(Kakshya::get_variant_element_type(DataVariant { std::vector<glm::vec4> {} }), std::type_index(typeid(glm::vec4)));
        EXPECT_EQ(Kakshya::get_variant_element_type(DataVariant { std::vector<glm::mat4> {} }), std::type_index(typeid(glm::mat4)));
    }

    // ---------------------------------------------------------------------
    // Frames
    // ---------------------------------------------------------------------

    TEST(DataUtilsFrameTest, InterleavedFramesAreFixedSizeSlices)
    {
        std::vector<float> data(10);
        std::iota(data.begin(), data.end(), 0.0F);

        const auto first = Kakshya::extract_frame(std::span<float>(data), 0, 4);
        const auto second = Kakshya::extract_frame(std::span<float>(data), 1, 4);

        EXPECT_EQ(copy_of(first), (std::vector<float> { 0, 1, 2, 3 }));
        EXPECT_EQ(copy_of(second), (std::vector<float> { 4, 5, 6, 7 }));
        EXPECT_EQ(first.data(), data.data());
    }

    TEST(DataUtilsFrameTest, ALastPartialFrameIsTruncated)
    {
        std::vector<float> data(10, 1.0F);

        EXPECT_EQ(Kakshya::extract_frame(std::span<float>(data), 2, 4).size(), 2U);
    }

    TEST(DataUtilsFrameTest, FramesPastTheEndAreEmpty)
    {
        std::vector<float> data(10, 1.0F);

        EXPECT_TRUE(Kakshya::extract_frame(std::span<float>(data), 3, 4).empty());
        EXPECT_TRUE(Kakshya::extract_frame(std::span<float>(data), 100, 4).empty());
        EXPECT_TRUE(Kakshya::extract_frame(std::span<float>(), 0, 4).empty());
    }

    TEST(DataUtilsFrameTest, PlanarFramesInterleaveOneSamplePerChannel)
    {
        std::vector<float> left { 1, 2, 3 };
        std::vector<float> right { 4, 5, 6 };
        const std::vector<std::span<float>> channels { std::span<float>(left), std::span<float>(right) };
        std::vector<float> out;

        const auto frame = Kakshya::extract_frame(channels, 1, out);

        EXPECT_EQ(copy_of(frame), (std::vector<float> { 2, 5 }));
        EXPECT_EQ(frame.data(), out.data());
    }

    TEST(DataUtilsFrameTest, ShortChannelsAreZeroPadded)
    {
        std::vector<float> left { 1, 2, 3 };
        std::vector<float> right { 4, 5 };
        const std::vector<std::span<float>> channels { std::span<float>(left), std::span<float>(right) };
        std::vector<float> out;

        EXPECT_EQ(copy_of(Kakshya::extract_frame(channels, 2, out)), (std::vector<float> { 3, 0 }));
        EXPECT_EQ(copy_of(Kakshya::extract_frame(channels, 9, out)), (std::vector<float> { 0, 0 }));
    }

    TEST(DataUtilsFrameTest, PlanarExtractionReplacesTheBufferContents)
    {
        std::vector<float> left { 1, 2 };
        const std::vector<std::span<float>> channels { std::span<float>(left) };
        std::vector<float> out { 9, 9, 9, 9 };

        const auto frame = Kakshya::extract_frame(channels, 0, out);

        EXPECT_EQ(copy_of(frame), (std::vector<float> { 1 }));
        EXPECT_EQ(out.size(), 1U);
    }

    TEST(DataUtilsFrameTest, NoChannelsGiveAnEmptyFrame)
    {
        std::vector<float> out { 1, 2 };

        EXPECT_TRUE(Kakshya::extract_frame(std::vector<std::span<float>> {}, 0, out).empty());
    }

    // ---------------------------------------------------------------------
    // convert_variant
    // ---------------------------------------------------------------------

    TEST(DataUtilsConvertVariantTest, MatchingTypeIsAZeroCopySpan)
    {
        DataVariant variant = std::vector<float> { 1, 2, 3 };

        const auto span = Kakshya::convert_variant<float>(variant);

        EXPECT_EQ(span.data(), std::get<std::vector<float>>(variant).data());
        EXPECT_EQ(span.size(), 3U);
    }

    TEST(DataUtilsConvertVariantTest, MatchingTypeSpanWritesThroughToTheVariant)
    {
        DataVariant variant = std::vector<float> { 1, 2, 3 };

        Kakshya::convert_variant<float>(variant)[1] = 9.0F;

        EXPECT_EQ(std::get<std::vector<float>>(variant)[1], 9.0F);
    }

    TEST(DataUtilsConvertVariantTest, ADifferentTypeReplacesTheVariantsStorage)
    {
        DataVariant variant = std::vector<float> { 1.5F, 2.5F };

        const auto span = Kakshya::convert_variant<double>(variant);

        ASSERT_TRUE(std::holds_alternative<std::vector<double>>(variant));
        EXPECT_EQ(span.data(), std::get<std::vector<double>>(variant).data());
        EXPECT_EQ(copy_of(span), (std::vector<double> { 1.5, 2.5 }));
    }

    TEST(DataUtilsConvertVariantTest, ComplexDataFollowsTheStrategy)
    {
        DataVariant magnitudes = std::vector<std::complex<double>> { { 3.0, 4.0 } };
        DataVariant reals = std::vector<std::complex<double>> { { 3.0, 4.0 } };
        DataVariant imaginaries = std::vector<std::complex<double>> { { 3.0, 4.0 } };

        EXPECT_DOUBLE_EQ(Kakshya::convert_variant<double>(magnitudes)[0], 5.0);
        EXPECT_DOUBLE_EQ(Kakshya::convert_variant<double>(reals, ComplexConversionStrategy::REAL_PART)[0], 3.0);
        EXPECT_DOUBLE_EQ(Kakshya::convert_variant<double>(imaginaries, ComplexConversionStrategy::IMAG_PART)[0], 4.0);
    }

    TEST(DataUtilsConvertVariantTest, VectorStorageFlattensToScalars)
    {
        DataVariant variant = std::vector<glm::vec3> { { 1, 2, 3 }, { 4, 5, 6 } };

        const auto span = Kakshya::convert_variant<float>(variant);

        EXPECT_EQ(copy_of(span), (std::vector<float> { 1, 2, 3, 4, 5, 6 }));
    }

    TEST(DataUtilsConvertVariantTest, IntegerStorageWidensToDouble)
    {
        DataVariant variant = std::vector<uint8_t> { 0, 128, 255 };

        EXPECT_EQ(copy_of(Kakshya::convert_variant<double>(variant)), (std::vector<double> { 0, 128, 255 }));
    }

    TEST(DataUtilsConvertVariantTest, IncompatibleTypesThrow)
    {
        DataVariant vec3 = std::vector<glm::vec3> { { 1, 2, 3 } };
        DataVariant complex = std::vector<std::complex<float>> { { 1.0F, 2.0F } };

        EXPECT_THROW((void)Kakshya::convert_variant<glm::vec2>(vec3), std::invalid_argument);
        EXPECT_THROW((void)Kakshya::convert_variant<glm::vec3>(complex), std::invalid_argument);
    }

    TEST(DataUtilsConvertVariantTest, ThrowingLeavesTheVariantUntouched)
    {
        DataVariant variant = std::vector<glm::vec3> { { 1, 2, 3 } };

        EXPECT_THROW((void)Kakshya::convert_variant<glm::vec2>(variant), std::invalid_argument);

        EXPECT_TRUE(std::holds_alternative<std::vector<glm::vec3>>(variant));
    }

    TEST(DataUtilsConvertVariantTest, ConstVariantsYieldTheSameValues)
    {
        const DataVariant variant = std::vector<float> { 1.5F, 2.5F };

        const auto span = Kakshya::convert_variant<double>(variant);

        EXPECT_EQ(copy_of(span), (std::vector<double> { 1.5, 2.5 }));
    }

    TEST(DataUtilsConvertVariantTest, ConvertVariantsYieldsOneSpanPerVariant)
    {
        const std::vector<DataVariant> variants {
            std::vector<double> { 1.0, 2.0 },
            std::vector<float> { 3.0F },
            std::vector<uint8_t> { 4, 5, 6 },
        };

        const auto spans = Kakshya::convert_variants<double>(variants);

        ASSERT_EQ(spans.size(), 3U);
        EXPECT_EQ(copy_of(spans[0]), (std::vector<double> { 1.0, 2.0 }));
        EXPECT_EQ(copy_of(spans[1]), (std::vector<double> { 3.0 }));
        EXPECT_EQ(copy_of(spans[2]), (std::vector<double> { 4.0, 5.0, 6.0 }));
    }

    TEST(DataUtilsConvertVariantTest, ConvertVariantsOfNothingIsNothing)
    {
        EXPECT_TRUE(Kakshya::convert_variants<double>(std::vector<DataVariant> {}).empty());
    }

    TEST(DataUtilsConvertVariantTest, DoubleConvenienceMatchesTheTemplate)
    {
        DataVariant variant = std::vector<std::complex<float>> { { 0.0F, 2.0F } };

        const auto span = Kakshya::convert_variant_to_double(variant);

        EXPECT_DOUBLE_EQ(span[0], 2.0);
    }

    // ---------------------------------------------------------------------
    // Extraction without mutation
    // ---------------------------------------------------------------------

    TEST(DataUtilsExtractTest, MatchingTypeIsCopiedIntoStorage)
    {
        const DataVariant variant = std::vector<float> { 1, 2, 3 };
        std::vector<float> storage;

        const auto span = Kakshya::extract_from_variant<float>(variant, storage);

        EXPECT_EQ(span.data(), storage.data());
        EXPECT_NE(span.data(), std::get<std::vector<float>>(variant).data());
        EXPECT_EQ(storage, (std::vector<float> { 1, 2, 3 }));
    }

    TEST(DataUtilsExtractTest, ADifferentTypeIsConvertedAndTheVariantKept)
    {
        const DataVariant variant = std::vector<uint8_t> { 1, 2, 3 };
        std::vector<float> storage;

        const auto span = Kakshya::extract_from_variant<float>(variant, storage);

        EXPECT_EQ(copy_of(span), (std::vector<float> { 1, 2, 3 }));
        EXPECT_TRUE(std::holds_alternative<std::vector<uint8_t>>(variant));
    }

    TEST(DataUtilsExtractTest, ComplexDataFollowsTheStrategy)
    {
        const DataVariant variant = std::vector<std::complex<double>> { { 3.0, 4.0 } };
        std::vector<double> storage;

        EXPECT_DOUBLE_EQ(Kakshya::extract_from_variant<double>(variant, storage)[0], 5.0);
        EXPECT_DOUBLE_EQ(Kakshya::extract_from_variant<double>(variant, storage, ComplexConversionStrategy::SQUARED_MAGNITUDE)[0], 25.0);
    }

    TEST(DataUtilsExtractTest, VectorsFlattenAndScalarsStructure)
    {
        const DataVariant vectors = std::vector<glm::vec2> { { 1, 2 }, { 3, 4 } };
        const DataVariant scalars = std::vector<float> { 1, 2, 3, 4, 5, 6 };
        std::vector<float> flat;
        std::vector<glm::vec3> structured;

        EXPECT_EQ(copy_of(Kakshya::extract_from_variant<float>(vectors, flat)), (std::vector<float> { 1, 2, 3, 4 }));
        EXPECT_EQ(copy_of(Kakshya::extract_from_variant<glm::vec3>(scalars, structured)),
            (std::vector<glm::vec3> { { 1, 2, 3 }, { 4, 5, 6 } }));
    }

    TEST(DataUtilsExtractTest, IncompatibleTypesThrow)
    {
        const DataVariant variant = std::vector<glm::vec3> { { 1, 2, 3 } };
        std::vector<glm::vec2> storage;

        EXPECT_THROW((void)Kakshya::extract_from_variant<glm::vec2>(variant, storage), std::invalid_argument);
    }

    TEST(DataUtilsExtractTest, SameTypeRawExtractionCopiesTheElements)
    {
        const std::vector<float> source { 1, 2, 3 };
        std::vector<float> destination;

        const auto span = Kakshya::extract_data<float, float>(std::span<const float>(source), destination);

        EXPECT_EQ(copy_of(span), source);
        EXPECT_EQ(destination, source);
        EXPECT_NE(span.data(), source.data());
    }

    TEST(DataUtilsExtractTest, SameWidthRawExtractionReinterpretsTheBits)
    {
        const std::vector<float> source { 1.0F, -2.0F };
        std::vector<uint32_t> destination;

        const auto span = Kakshya::extract_data<float, uint32_t>(std::span<const float>(source), destination);

        ASSERT_EQ(span.size(), 2U);
        EXPECT_EQ(span[0], 0x3F800000U);
        EXPECT_EQ(span[1], 0xC0000000U);
    }

    TEST(DataUtilsExtractAtTest, SameTypeReadsTheElement)
    {
        const DataVariant variant = std::vector<float> { 1.5F, 2.5F };

        EXPECT_EQ(Kakshya::extract_from_variant_at<float>(variant, 1), 2.5F);
    }

    TEST(DataUtilsExtractAtTest, ArithmeticTypesConvert)
    {
        const DataVariant variant = std::vector<uint8_t> { 7, 200 };

        EXPECT_EQ(Kakshya::extract_from_variant_at<double>(variant, 1), 200.0);
        EXPECT_EQ(Kakshya::extract_from_variant_at<float>(variant, 0), 7.0F);
    }

    TEST(DataUtilsExtractAtTest, ComplexValuesBecomeMagnitudes)
    {
        const DataVariant variant = std::vector<std::complex<double>> { { 3.0, 4.0 } };

        EXPECT_EQ(Kakshya::extract_from_variant_at<double>(variant, 0), 5.0);
    }

    TEST(DataUtilsExtractAtTest, GlmValuesReadAsTheirOwnTypeOnly)
    {
        const DataVariant variant = std::vector<glm::vec3> { { 1, 2, 3 }, { 4, 5, 6 } };

        EXPECT_EQ(Kakshya::extract_from_variant_at<glm::vec3>(variant, 1), glm::vec3(4, 5, 6));
        EXPECT_FALSE(Kakshya::extract_from_variant_at<double>(variant, 0).has_value());
        EXPECT_FALSE(Kakshya::extract_from_variant_at<glm::vec2>(variant, 0).has_value());
    }

    TEST(DataUtilsExtractAtTest, OutOfRangePositionsAreAbsent)
    {
        const DataVariant variant = std::vector<float> { 1.0F };

        EXPECT_FALSE(Kakshya::extract_from_variant_at<float>(variant, 1).has_value());
        EXPECT_FALSE(Kakshya::extract_from_variant_at<float>(variant, 1000).has_value());
        EXPECT_FALSE(Kakshya::extract_from_variant_at<float>(DataVariant { std::vector<float> {} }, 0).has_value());
    }

    TEST(DataUtilsExtractAtTest, ComplexTargetsFromOtherTypesAreAbsent)
    {
        const DataVariant real = std::vector<float> { 1.0F };
        const DataVariant complex = std::vector<std::complex<float>> { { 1.0F, 2.0F } };

        EXPECT_FALSE(Kakshya::extract_from_variant_at<std::complex<double>>(real, 0).has_value());
        EXPECT_FALSE(Kakshya::extract_from_variant_at<std::complex<double>>(complex, 0).has_value());
        EXPECT_TRUE(Kakshya::extract_from_variant_at<std::complex<float>>(complex, 0).has_value());
    }

    // ---------------------------------------------------------------------
    // safe_copy_data_variant
    // ---------------------------------------------------------------------

    TEST(DataUtilsSafeCopyTest, ScalarsConvertIntoTheOutputsType)
    {
        const DataVariant input = std::vector<float> { 1.5F, 2.5F };
        DataVariant output = std::vector<double> {};

        Kakshya::safe_copy_data_variant(input, output);

        ASSERT_TRUE(std::holds_alternative<std::vector<double>>(output));
        EXPECT_EQ(std::get<std::vector<double>>(output), (std::vector<double> { 1.5, 2.5 }));
    }

    TEST(DataUtilsSafeCopyTest, OutputIsResizedToTheConvertedInput)
    {
        const DataVariant input = std::vector<float> { 1.0F, 2.0F };
        DataVariant output = std::vector<float>(10, 7.0F);

        Kakshya::safe_copy_data_variant(input, output);

        EXPECT_EQ(std::get<std::vector<float>>(output), (std::vector<float> { 1.0F, 2.0F }));
    }

    TEST(DataUtilsSafeCopyTest, IntegersWidenToFloat)
    {
        const DataVariant input = std::vector<uint8_t> { 0, 255 };
        DataVariant output = std::vector<float> {};

        Kakshya::safe_copy_data_variant(input, output);

        EXPECT_EQ(std::get<std::vector<float>>(output), (std::vector<float> { 0.0F, 255.0F }));
    }

    TEST(DataUtilsSafeCopyTest, ComplexInputBecomesMagnitudes)
    {
        const DataVariant input = std::vector<std::complex<double>> { { 3.0, 4.0 }, { 0.0, 2.0 } };
        DataVariant output = std::vector<double> {};

        Kakshya::safe_copy_data_variant(input, output);

        EXPECT_EQ(std::get<std::vector<double>>(output), (std::vector<double> { 5.0, 2.0 }));
    }

    TEST(DataUtilsSafeCopyTest, VectorsFlattenAndScalarsStructure)
    {
        const DataVariant vectors = std::vector<glm::vec3> { { 1, 2, 3 } };
        const DataVariant scalars = std::vector<float> { 1, 2, 3, 4, 5, 6 };
        DataVariant flat = std::vector<float> {};
        DataVariant structured = std::vector<glm::vec3> {};

        Kakshya::safe_copy_data_variant(vectors, flat);
        Kakshya::safe_copy_data_variant(scalars, structured);

        EXPECT_EQ(std::get<std::vector<float>>(flat), (std::vector<float> { 1, 2, 3 }));
        EXPECT_EQ(std::get<std::vector<glm::vec3>>(structured), (std::vector<glm::vec3> { { 1, 2, 3 }, { 4, 5, 6 } }));
    }

    TEST(DataUtilsSafeCopyTest, IncompatibleTypesThrow)
    {
        const DataVariant vec3 = std::vector<glm::vec3> { { 1, 2, 3 } };
        const DataVariant scalars = std::vector<float> { 1, 2, 3, 4 };
        DataVariant vec2_out = std::vector<glm::vec2> {};
        DataVariant complex_out = std::vector<std::complex<float>> {};
        DataVariant vec3_out = std::vector<glm::vec3> {};

        EXPECT_THROW(Kakshya::safe_copy_data_variant(vec3, vec2_out), std::invalid_argument);
        EXPECT_THROW(Kakshya::safe_copy_data_variant(vec3, complex_out), std::invalid_argument);
        EXPECT_THROW(Kakshya::safe_copy_data_variant(scalars, vec3_out), std::invalid_argument);
    }

    // ---------------------------------------------------------------------
    // Pixel normalisation
    // ---------------------------------------------------------------------

    TEST(DataUtilsNormaliseTest, FloatsAreAZeroCopyViewAndStorageIsUntouched)
    {
        const DataVariant variant = std::vector<float> { 0.25F, 0.5F };
        std::vector<float> storage { 9.0F };

        const auto view = Kakshya::as_normalised_float(variant, storage);

        EXPECT_EQ(view.data(), std::get<std::vector<float>>(variant).data());
        EXPECT_EQ(view.size(), 2U);
        EXPECT_EQ(storage, (std::vector<float> { 9.0F }));
    }

    TEST(DataUtilsNormaliseTest, BytesScaleByTwoFiftyFive)
    {
        const DataVariant variant = std::vector<uint8_t> { 0, 51, 255 };
        std::vector<float> storage;

        const auto view = Kakshya::as_normalised_float(variant, storage);

        ASSERT_EQ(view.size(), 3U);
        EXPECT_FLOAT_EQ(view[0], 0.0F);
        EXPECT_NEAR(view[1], 0.2F, 1e-6);
        EXPECT_FLOAT_EQ(view[2], 1.0F);
        EXPECT_EQ(view.data(), storage.data());
    }

    TEST(DataUtilsNormaliseTest, HalfWordsScaleBySixtyFiveThousandFiveHundredThirtyFive)
    {
        const DataVariant variant = std::vector<uint16_t> { 0, 65535, 32768 };
        std::vector<float> storage;

        const auto view = Kakshya::as_normalised_float(variant, storage);

        ASSERT_EQ(view.size(), 3U);
        EXPECT_FLOAT_EQ(view[0], 0.0F);
        EXPECT_FLOAT_EQ(view[1], 1.0F);
        EXPECT_NEAR(view[2], 0.50000763F, 1e-6);
    }

    TEST(DataUtilsNormaliseTest, OtherTypesGiveAnEmptyView)
    {
        std::vector<float> storage;

        EXPECT_TRUE(Kakshya::as_normalised_float(DataVariant { std::vector<double>(4, 1.0) }, storage).empty());
        EXPECT_TRUE(Kakshya::as_normalised_float(DataVariant { std::vector<uint32_t>(4, 1) }, storage).empty());
        EXPECT_TRUE(Kakshya::as_normalised_float(DataVariant { std::vector<std::complex<float>>(4) }, storage).empty());
        EXPECT_TRUE(Kakshya::as_normalised_float(DataVariant { std::vector<glm::vec3>(4) }, storage).empty());
    }

    TEST(DataUtilsNormaliseTest, StorageIsReusedAcrossCalls)
    {
        const DataVariant variant = std::vector<uint8_t>(64, 255);
        std::vector<float> storage;
        const auto first = Kakshya::as_normalised_float(variant, storage);
        const float* first_data = first.data();

        const auto second = Kakshya::as_normalised_float(variant, storage);

        EXPECT_EQ(second.data(), first_data);
    }

    TEST(DataUtilsNormaliseTest, EmptyPixelDataGivesAnEmptyView)
    {
        std::vector<float> storage { 1.0F };

        EXPECT_TRUE(Kakshya::as_normalised_float(DataVariant { std::vector<uint8_t> {} }, storage).empty());
    }

    TEST(DataUtilsNormaliseTest, DenormalisingScalesAndClamps)
    {
        const std::vector<float> src { 0.0F, 0.5F, 1.0F, 1.5F, -0.5F };

        const auto bytes = Kakshya::denormalise_to_uint8(std::span<const float>(src));

        EXPECT_EQ(bytes, (std::vector<uint8_t> { 0, 127, 255, 255, 0 }));
    }

    TEST(DataUtilsNormaliseTest, DenormalisingIntoASpanWritesInPlace)
    {
        const std::vector<float> src { 0.0F, 1.0F };
        std::vector<uint8_t> dst(2, 9);

        Kakshya::denormalise_to_uint8(std::span<const float>(src), std::span<uint8_t>(dst));

        EXPECT_EQ(dst, (std::vector<uint8_t> { 0, 255 }));
    }

    TEST(DataUtilsNormaliseTest, DenormalisingNothingGivesNothing)
    {
        EXPECT_TRUE(Kakshya::denormalise_to_uint8(std::span<const float>()).empty());
    }

    TEST(DataUtilsNormaliseTest, EveryByteSurvivesANormaliseDenormaliseRoundTrip)
    {
        std::vector<uint8_t> original(256);
        std::iota(original.begin(), original.end(), uint8_t { 0 });
        const DataVariant variant = original;
        std::vector<float> storage;

        const auto normalised = Kakshya::as_normalised_float(variant, storage);
        const auto restored = Kakshya::denormalise_to_uint8(normalised);

        EXPECT_EQ(restored, original);
    }

    // ---------------------------------------------------------------------
    // Raw bytes
    // ---------------------------------------------------------------------

    TEST(DataUtilsBytesTest, BytesHalfWordsAndFloatsExposeTheirStorage)
    {
        const DataVariant bytes = std::vector<uint8_t>(5, 1);
        const DataVariant halves = std::vector<uint16_t>(5, 1);
        const DataVariant floats = std::vector<float>(5, 1.0F);

        const auto [byte_ptr, byte_size] = Kakshya::variant_bytes(bytes);
        const auto [half_ptr, half_size] = Kakshya::variant_bytes(halves);
        const auto [float_ptr, float_size] = Kakshya::variant_bytes(floats);

        EXPECT_EQ(byte_ptr, std::get<std::vector<uint8_t>>(bytes).data());
        EXPECT_EQ(byte_size, 5U);
        EXPECT_EQ(static_cast<const void*>(half_ptr), static_cast<const void*>(std::get<std::vector<uint16_t>>(halves).data()));
        EXPECT_EQ(half_size, 10U);
        EXPECT_EQ(static_cast<const void*>(float_ptr), static_cast<const void*>(std::get<std::vector<float>>(floats).data()));
        EXPECT_EQ(float_size, 20U);
    }

    TEST(DataUtilsBytesTest, OtherTypesHaveNoRawBytes)
    {
        for (const DataVariant& variant : {
                 DataVariant { std::vector<double>(4) },
                 DataVariant { std::vector<uint32_t>(4) },
                 DataVariant { std::vector<std::complex<float>>(4) },
                 DataVariant { std::vector<glm::vec3>(4) },
                 DataVariant { std::vector<glm::mat4>(4) } }) {
            const auto [ptr, size] = Kakshya::variant_bytes(variant);

            EXPECT_EQ(ptr, nullptr) << variant.index();
            EXPECT_EQ(size, 0U) << variant.index();
        }
    }

    TEST(DataUtilsBytesTest, MutableBytesWriteThroughToTheVariant)
    {
        DataVariant variant = std::vector<float>(2, 0.0F);

        const auto [ptr, size] = Kakshya::variant_bytes_mutable(variant);
        const float value = 3.5F;
        ASSERT_EQ(size, 8U);
        std::memcpy(ptr + sizeof(float), &value, sizeof(float));

        EXPECT_FLOAT_EQ(std::get<std::vector<float>>(variant)[1], 3.5F);
    }

    TEST(DataUtilsBytesTest, MutableBytesRefuseUnsupportedTypes)
    {
        DataVariant variant = std::vector<double>(4);

        const auto [ptr, size] = Kakshya::variant_bytes_mutable(variant);

        EXPECT_EQ(ptr, nullptr);
        EXPECT_EQ(size, 0U);
    }

    TEST(DataUtilsBytesTest, EmptyStorageHasZeroBytes)
    {
        const DataVariant variant = std::vector<uint8_t> {};

        EXPECT_EQ(Kakshya::variant_bytes(variant).second, 0U);
    }

    // ---------------------------------------------------------------------
    // Metadata
    // ---------------------------------------------------------------------

    TEST(DataUtilsMetadataTest, SetStoresAndOverwrites)
    {
        std::unordered_map<std::string, std::any> metadata;

        Kakshya::set_metadata_value(metadata, "rate", 48000);
        Kakshya::set_metadata_value(metadata, "rate", 44100);

        EXPECT_EQ(metadata.size(), 1U);
        EXPECT_EQ(std::any_cast<int>(metadata.at("rate")), 44100);
    }

    TEST(DataUtilsMetadataTest, GetReadsTheExactType)
    {
        std::unordered_map<std::string, std::any> metadata;
        Kakshya::set_metadata_value(metadata, "name", std::string("kick"));
        Kakshya::set_metadata_value(metadata, "gain", 0.5);

        EXPECT_EQ(Kakshya::get_metadata_value<std::string>(metadata, "name"), "kick");
        EXPECT_EQ(Kakshya::get_metadata_value<double>(metadata, "gain"), 0.5);
    }

    TEST(DataUtilsMetadataTest, GetConvertsBetweenNumericTypes)
    {
        std::unordered_map<std::string, std::any> metadata;
        Kakshya::set_metadata_value(metadata, "count", 3);
        Kakshya::set_metadata_value(metadata, "ratio", 0.25F);

        EXPECT_EQ(Kakshya::get_metadata_value<double>(metadata, "count"), 3.0);
        EXPECT_EQ(Kakshya::get_metadata_value<double>(metadata, "ratio"), 0.25);
        EXPECT_EQ(Kakshya::get_metadata_value<uint32_t>(metadata, "count"), 3U);
    }

    TEST(DataUtilsMetadataTest, GetReadsCStringsAsStrings)
    {
        std::unordered_map<std::string, std::any> metadata;
        Kakshya::set_metadata_value(metadata, "label", "snare");

        EXPECT_EQ(Kakshya::get_metadata_value<std::string>(metadata, "label"), "snare");
    }

    TEST(DataUtilsMetadataTest, MissingKeysAndMismatchedTypesAreAbsent)
    {
        std::unordered_map<std::string, std::any> metadata;
        Kakshya::set_metadata_value(metadata, "name", std::string("kick"));
        Kakshya::set_metadata_value(metadata, "count", 3);

        EXPECT_FALSE(Kakshya::get_metadata_value<int>(metadata, "missing").has_value());
        EXPECT_FALSE(Kakshya::get_metadata_value<int>(metadata, "name").has_value());
        EXPECT_FALSE(Kakshya::get_metadata_value<std::string>(metadata, "count").has_value());
    }

    TEST(DataUtilsMetadataTest, EmptyValuesAreAbsent)
    {
        std::unordered_map<std::string, std::any> metadata;
        Kakshya::set_metadata_value(metadata, "nothing", std::any {});

        EXPECT_FALSE(Kakshya::get_metadata_value<int>(metadata, "nothing").has_value());
    }

    // ---------------------------------------------------------------------
    // Modality detection from dimensions
    // ---------------------------------------------------------------------

    TEST(DataUtilsModalityTest, NoDimensionsAreUnknown)
    {
        EXPECT_EQ(Kakshya::detect_data_modality({}), DataModality::UNKNOWN);
    }

    struct DetectCase {
        std::string_view name;
        DataModality modality;
        std::vector<uint64_t> shape;
    };

    class DataUtilsDetectRoundTripTest : public ::testing::TestWithParam<DetectCase> { };

    TEST_P(DataUtilsDetectRoundTripTest, DimensionsOfAModalityDetectAsThatModality)
    {
        const auto dims = DataDimension::create_dimensions(GetParam().modality, GetParam().shape);

        EXPECT_EQ(Kakshya::detect_data_modality(dims), GetParam().modality);
    }

    INSTANTIATE_TEST_SUITE_P(Modalities, DataUtilsDetectRoundTripTest,
        ::testing::Values(
            DetectCase { "Audio1D", DataModality::AUDIO_1D, { 480 } },
            DetectCase { "AudioMultichannel", DataModality::AUDIO_MULTICHANNEL, { 480, 2 } },
            DetectCase { "Image2D", DataModality::IMAGE_2D, { 8, 8 } },
            DetectCase { "ImageColor", DataModality::IMAGE_COLOR, { 8, 8, 3 } },
            DetectCase { "ImageColorAlpha", DataModality::IMAGE_COLOR, { 8, 8, 4 } },
            DetectCase { "DepthMap", DataModality::DEPTH_MAP, { 8, 8, 1 } },
            DetectCase { "Spectral2D", DataModality::SPECTRAL_2D, { 10, 513 } },
            DetectCase { "Volumetric3D", DataModality::VOLUMETRIC_3D, { 4, 4, 4 } },
            DetectCase { "VideoGrayscale", DataModality::VIDEO_GRAYSCALE, { 5, 8, 8 } },
            DetectCase { "VideoColor", DataModality::VIDEO_COLOR, { 5, 8, 8, 3 } },
            DetectCase { "VideoDepth", DataModality::VIDEO_DEPTH, { 5, 8, 8, 1 } }),
        [](const ::testing::TestParamInfo<DetectCase>& info) {
            return std::string(info.param.name);
        });

    TEST(DataUtilsModalityTest, GroupedDimensionsDetectTheirVertexModality)
    {
        EXPECT_EQ(Kakshya::detect_data_modality({ DataDimension::vertex_positions(4) }), DataModality::VERTEX_POSITIONS_3D);
        EXPECT_EQ(Kakshya::detect_data_modality({ DataDimension::vertex_normals(4) }), DataModality::VERTEX_NORMALS_3D);
        EXPECT_EQ(Kakshya::detect_data_modality({ DataDimension::texture_coords(4) }), DataModality::TEXTURE_COORDS_2D);
        EXPECT_EQ(Kakshya::detect_data_modality({ DataDimension::vertex_colors(4, false) }), DataModality::VERTEX_COLORS_RGB);
        EXPECT_EQ(Kakshya::detect_data_modality({ DataDimension::vertex_colors(4, true) }), DataModality::VERTEX_COLORS_RGBA);
        EXPECT_EQ(Kakshya::detect_data_modality({ DataDimension::grouped("t", 4, 3, Role::TANGENT) }), DataModality::VERTEX_TANGENTS_3D);
        EXPECT_EQ(Kakshya::detect_data_modality({ DataDimension::grouped("b", 4, 3, Role::BITANGENT) }), DataModality::VERTEX_TANGENTS_3D);
    }

    TEST(DataUtilsModalityTest, SixteenComponentGroupsAreTransforms)
    {
        EXPECT_EQ(Kakshya::detect_data_modality({ DataDimension::grouped("m", 4, 16) }), DataModality::TRANSFORMATION_MATRIX);
    }

    TEST(DataUtilsModalityTest, GroupedColorsOfOtherWidthsAreNotVertexColors)
    {
        const auto modality = Kakshya::detect_data_modality({ DataDimension::grouped("c", 4, 2, Role::COLOR) });

        EXPECT_NE(modality, DataModality::VERTEX_COLORS_RGB);
        EXPECT_NE(modality, DataModality::VERTEX_COLORS_RGBA);
    }

    TEST(DataUtilsModalityTest, ASingleColorChannelImageIsGrayscale)
    {
        const std::vector<DataDimension> dims {
            DataDimension::spatial(8, 'y'),
            DataDimension::spatial(8, 'x'),
            DataDimension::channel(1),
        };

        EXPECT_EQ(Kakshya::detect_data_modality(dims), DataModality::IMAGE_2D);
    }

    TEST(DataUtilsModalityTest, TwoChannelImagesAreNotColor)
    {
        const std::vector<DataDimension> dims {
            DataDimension::spatial(8, 'y'),
            DataDimension::spatial(8, 'x'),
            DataDimension::channel(2),
        };

        EXPECT_EQ(Kakshya::detect_data_modality(dims), DataModality::IMAGE_2D);
    }

    TEST(DataUtilsModalityTest, SingleChannelAudioIsMono)
    {
        EXPECT_EQ(Kakshya::detect_data_modality({ DataDimension::time(480), DataDimension::channel(1) }), DataModality::AUDIO_1D);
    }

    TEST(DataUtilsModalityTest, SeveralChannelDimensionsAreMultichannel)
    {
        const std::vector<DataDimension> dims { DataDimension::time(480), DataDimension::channel(1), DataDimension::channel(1) };

        EXPECT_EQ(Kakshya::detect_data_modality(dims), DataModality::AUDIO_MULTICHANNEL);
    }

    TEST(DataUtilsModalityTest, SpectralDataWithSeveralChannelsIsATensor)
    {
        const std::vector<DataDimension> dims {
            DataDimension::time(10),
            DataDimension::frequency(513),
            DataDimension::channel(2),
            DataDimension::channel(2),
        };

        EXPECT_EQ(Kakshya::detect_data_modality(dims), DataModality::TENSOR_ND);
    }

    TEST(DataUtilsModalityTest, UnrecognisedStructuresAreTensors)
    {
        EXPECT_EQ(Kakshya::detect_data_modality({ DataDimension("a", 4, 1, Role::CUSTOM) }), DataModality::TENSOR_ND);
        EXPECT_EQ(Kakshya::detect_data_modality({ DataDimension::channel(2), DataDimension::channel(3) }), DataModality::TENSOR_ND);
        EXPECT_EQ(Kakshya::detect_data_modality({ DataDimension::time(4), DataDimension::spatial(4, 'x'), DataDimension::spatial(4, 'y'), DataDimension::spatial(4, 'z') }), DataModality::TENSOR_ND);
    }

    TEST(DataUtilsModalityTest, VolumesIgnoreChannels)
    {
        const std::vector<DataDimension> dims {
            DataDimension::spatial(4, 'x'),
            DataDimension::spatial(4, 'y'),
            DataDimension::spatial(4, 'z'),
            DataDimension::channel(3),
        };

        EXPECT_EQ(Kakshya::detect_data_modality(dims), DataModality::VOLUMETRIC_3D);
    }

    // ---------------------------------------------------------------------
    // Modality detection refined by the stored type
    // ---------------------------------------------------------------------

    TEST(DataUtilsModalityTest, DecimalAudioStaysAudio)
    {
        const auto dims = DataDimension::create_dimensions(DataModality::AUDIO_1D, { 8 });

        EXPECT_EQ(Kakshya::detect_data_modality(dims, DataVariant { std::vector<double>(8) }), DataModality::AUDIO_1D);
        EXPECT_EQ(Kakshya::detect_data_modality(dims, DataVariant { std::vector<float>(8) }), DataModality::AUDIO_1D);
    }

    TEST(DataUtilsModalityTest, ComplexDataOnATimeAxisIsSpectral)
    {
        const auto dims = DataDimension::create_dimensions(DataModality::AUDIO_1D, { 8 });

        EXPECT_EQ(Kakshya::detect_data_modality(dims, DataVariant { std::vector<std::complex<float>>(8) }), DataModality::SPECTRAL_2D);
        EXPECT_EQ(Kakshya::detect_data_modality(dims, DataVariant { std::vector<std::complex<double>>(8) }), DataModality::SPECTRAL_2D);
    }

    TEST(DataUtilsModalityTest, IntegerDataOnATimeAxisIsATensor)
    {
        const auto dims = DataDimension::create_dimensions(DataModality::AUDIO_1D, { 8 });

        EXPECT_EQ(Kakshya::detect_data_modality(dims, DataVariant { std::vector<uint8_t>(8) }), DataModality::TENSOR_ND);
        EXPECT_EQ(Kakshya::detect_data_modality(dims, DataVariant { std::vector<uint32_t>(8) }), DataModality::TENSOR_ND);
    }

    TEST(DataUtilsModalityTest, TensorsAreRefinedByTheirStorageToo)
    {
        const std::vector<DataDimension> dims { DataDimension("a", 4, 1, Role::CUSTOM) };

        EXPECT_EQ(Kakshya::detect_data_modality(dims, DataVariant { std::vector<std::complex<float>>(4) }), DataModality::SPECTRAL_2D);
        EXPECT_EQ(Kakshya::detect_data_modality(dims, DataVariant { std::vector<float>(4) }), DataModality::TENSOR_ND);
    }

    TEST(DataUtilsModalityTest, VectorStorageDoesNotMakeATensorSpectral)
    {
        const std::vector<DataDimension> dims { DataDimension("a", 4, 1, Role::CUSTOM) };

        EXPECT_EQ(Kakshya::detect_data_modality(dims, DataVariant { std::vector<glm::vec3>(4) }), DataModality::TENSOR_ND);
        EXPECT_EQ(Kakshya::detect_data_modality(dims, DataVariant { std::vector<glm::mat4>(4) }), DataModality::TENSOR_ND);
    }

    TEST(DataUtilsModalityTest, SpecificModalitiesIgnoreTheStorage)
    {
        const auto dims = DataDimension::create_dimensions(DataModality::IMAGE_2D, { 8, 8 });

        EXPECT_EQ(Kakshya::detect_data_modality(dims, DataVariant { std::vector<std::complex<float>>(64) }), DataModality::IMAGE_2D);
        EXPECT_EQ(Kakshya::detect_data_modality(dims, DataVariant { std::vector<uint8_t>(64) }), DataModality::IMAGE_2D);
    }

    // ---------------------------------------------------------------------
    // Dimension detection from one variant
    // ---------------------------------------------------------------------

    TEST(DataUtilsDetectDimensionsTest, DecimalVariantsAreATimeAxis)
    {
        const auto dims = Kakshya::detect_data_dimensions(DataVariant { std::vector<double>(100) });

        ASSERT_EQ(dims.size(), 1U);
        EXPECT_EQ(dims[0].role, Role::TIME);
        EXPECT_EQ(dims[0].size, 100U);
        EXPECT_EQ(Kakshya::detect_data_dimensions(DataVariant { std::vector<float>(10) })[0].role, Role::TIME);
    }

    TEST(DataUtilsDetectDimensionsTest, ComplexVariantsAreAFrequencyAxis)
    {
        const auto dims = Kakshya::detect_data_dimensions(DataVariant { std::vector<std::complex<float>>(64) });

        ASSERT_EQ(dims.size(), 1U);
        EXPECT_EQ(dims[0].role, Role::FREQUENCY);
        EXPECT_EQ(dims[0].size, 64U);
    }

    TEST(DataUtilsDetectDimensionsTest, IntegerVariantsAreASpatialAxis)
    {
        for (const DataVariant& variant : {
                 DataVariant { std::vector<uint8_t>(16) },
                 DataVariant { std::vector<uint16_t>(16) },
                 DataVariant { std::vector<uint32_t>(16) } }) {
            const auto dims = Kakshya::detect_data_dimensions(variant);

            ASSERT_EQ(dims.size(), 1U) << variant.index();
            EXPECT_EQ(dims[0].role, Role::SPATIAL_X) << variant.index();
            EXPECT_EQ(dims[0].size, 16U) << variant.index();
        }
    }

    TEST(DataUtilsDetectDimensionsTest, VectorVariantsAreGroupedByComponentCount)
    {
        const auto v2 = Kakshya::detect_data_dimensions(DataVariant { std::vector<glm::vec2>(5) });
        const auto v3 = Kakshya::detect_data_dimensions(DataVariant { std::vector<glm::vec3>(5) });
        const auto v4 = Kakshya::detect_data_dimensions(DataVariant { std::vector<glm::vec4>(5) });
        const auto m4 = Kakshya::detect_data_dimensions(DataVariant { std::vector<glm::mat4>(5) });

        ASSERT_EQ(v2.size(), 1U);
        ASSERT_TRUE(v2[0].grouping.has_value());
        EXPECT_EQ(v2[0].grouping->count, 2U);
        EXPECT_EQ(v2[0].role, Role::UV);
        EXPECT_EQ(v2[0].size, 5U);
        ASSERT_TRUE(v3[0].grouping.has_value());
        EXPECT_EQ(v3[0].grouping->count, 3U);
        EXPECT_EQ(v3[0].role, Role::POSITION);
        ASSERT_TRUE(v4[0].grouping.has_value());
        EXPECT_EQ(v4[0].grouping->count, 4U);
        EXPECT_EQ(v4[0].role, Role::COLOR);
        ASSERT_TRUE(m4[0].grouping.has_value());
        EXPECT_EQ(m4[0].grouping->count, 16U);
        EXPECT_EQ(m4[0].role, Role::CUSTOM);
    }

    TEST(DataUtilsDetectDimensionsTest, DetectedVectorDimensionsDetectTheirModality)
    {
        EXPECT_EQ(Kakshya::detect_data_modality(Kakshya::detect_data_dimensions(DataVariant { std::vector<glm::vec3>(5) })), DataModality::VERTEX_POSITIONS_3D);
        EXPECT_EQ(Kakshya::detect_data_modality(Kakshya::detect_data_dimensions(DataVariant { std::vector<glm::mat4>(5) })), DataModality::TRANSFORMATION_MATRIX);
    }

    // ---------------------------------------------------------------------
    // Dimension detection from several variants
    // ---------------------------------------------------------------------

    std::vector<DataVariant> repeated(const DataVariant& variant, size_t count)
    {
        return std::vector<DataVariant>(count, variant);
    }

    TEST(DataUtilsDetectVariantsTest, NoVariantsGiveOneEmptyCustomAxis)
    {
        const auto dims = Kakshya::detect_data_dimensions(std::vector<DataVariant> {});

        ASSERT_EQ(dims.size(), 1U);
        EXPECT_EQ(dims[0].name, "empty_variants");
        EXPECT_EQ(dims[0].size, 0U);
        EXPECT_EQ(dims[0].role, Role::CUSTOM);
    }

    TEST(DataUtilsDetectVariantsTest, OneDecimalVariantIsSamples)
    {
        const auto dims = Kakshya::detect_data_dimensions(repeated(std::vector<double>(100), 1));

        ASSERT_EQ(dims.size(), 1U);
        EXPECT_EQ(dims[0].name, "samples");
        EXPECT_EQ(dims[0].role, Role::TIME);
        EXPECT_EQ(dims[0].size, 100U);
    }

    TEST(DataUtilsDetectVariantsTest, OneComplexVariantIsFrequencyData)
    {
        const auto dims = Kakshya::detect_data_dimensions(repeated(std::vector<std::complex<float>>(64), 1));

        ASSERT_EQ(dims.size(), 1U);
        EXPECT_EQ(dims[0].name, "frequency_data");
        EXPECT_EQ(dims[0].role, Role::FREQUENCY);
    }

    TEST(DataUtilsDetectVariantsTest, OneIntegerVariantIsDataPoints)
    {
        const auto dims = Kakshya::detect_data_dimensions(repeated(std::vector<uint8_t>(64), 1));

        ASSERT_EQ(dims.size(), 1U);
        EXPECT_EQ(dims[0].name, "data_points");
        EXPECT_EQ(dims[0].role, Role::SPATIAL_X);
    }

    TEST(DataUtilsDetectVariantsTest, TwoDecimalVariantsAreStereoSamples)
    {
        const auto dims = Kakshya::detect_data_dimensions(repeated(std::vector<float>(100), 2));

        ASSERT_EQ(dims.size(), 2U);
        EXPECT_EQ(dims[0].role, Role::CHANNEL);
        EXPECT_EQ(dims[0].size, 2U);
        EXPECT_EQ(dims[1].name, "samples");
        EXPECT_EQ(dims[1].size, 100U);
    }

    TEST(DataUtilsDetectVariantsTest, SmallGroupsAreChannelsOfTheFirstVariantsLength)
    {
        const auto decimal = Kakshya::detect_data_dimensions(repeated(std::vector<float>(100), 8));
        const auto complex = Kakshya::detect_data_dimensions(repeated(std::vector<std::complex<float>>(64), 4));
        const auto integer = Kakshya::detect_data_dimensions(repeated(std::vector<uint8_t>(32), 3));

        ASSERT_EQ(decimal.size(), 2U);
        EXPECT_EQ(decimal[0].size, 8U);
        EXPECT_EQ(decimal[1].name, "samples");
        EXPECT_EQ(complex[1].name, "bins");
        EXPECT_EQ(complex[1].role, Role::FREQUENCY);
        EXPECT_EQ(integer[1].name, "pixels");
        EXPECT_EQ(integer[0].size, 3U);
    }

    TEST(DataUtilsDetectVariantsTest, SixteenVariantsStillCountAsChannels)
    {
        const auto dims = Kakshya::detect_data_dimensions(repeated(std::vector<float>(10), 16));

        ASSERT_EQ(dims.size(), 2U);
        EXPECT_EQ(dims[0].role, Role::CHANNEL);
        EXPECT_EQ(dims[0].size, 16U);
    }

    TEST(DataUtilsDetectVariantsTest, ManyVariantsAreBlocksOverTime)
    {
        const auto decimal = Kakshya::detect_data_dimensions(repeated(std::vector<float>(10), 17));
        const auto complex = Kakshya::detect_data_dimensions(repeated(std::vector<std::complex<float>>(10), 20));
        const auto integer = Kakshya::detect_data_dimensions(repeated(std::vector<uint8_t>(10), 30));

        ASSERT_EQ(decimal.size(), 2U);
        EXPECT_EQ(decimal[0].name, "time_blocks");
        EXPECT_EQ(decimal[0].role, Role::TIME);
        EXPECT_EQ(decimal[0].size, 17U);
        EXPECT_EQ(decimal[1].name, "block_samples");
        EXPECT_EQ(decimal[1].size, 10U);
        EXPECT_EQ(complex[0].name, "time_windows");
        EXPECT_EQ(complex[1].name, "frequency_bins");
        EXPECT_EQ(integer[0].name, "frames");
        EXPECT_EQ(integer[1].name, "frame_data");
    }

    TEST(DataUtilsDetectVariantsTest, VectorVariantsAreChannelsOfGroupedElements)
    {
        const auto dims = Kakshya::detect_data_dimensions(repeated(std::vector<glm::vec3>(12), 3));

        ASSERT_EQ(dims.size(), 2U);
        EXPECT_EQ(dims[0].role, Role::CHANNEL);
        EXPECT_EQ(dims[0].size, 3U);
        EXPECT_EQ(dims[1].name, "glm_elements");
        EXPECT_EQ(dims[1].size, 12U);
        ASSERT_TRUE(dims[1].grouping.has_value());
        EXPECT_EQ(dims[1].grouping->count, 3U);
        EXPECT_EQ(dims[1].role, Role::POSITION);
    }

    TEST(DataUtilsDetectVariantsTest, MixedKindsAreOpaqueVariants)
    {
        const std::vector<DataVariant> variants { std::vector<float>(10), std::vector<uint8_t>(10) };

        const auto dims = Kakshya::detect_data_dimensions(variants);

        ASSERT_EQ(dims.size(), 2U);
        EXPECT_EQ(dims[0].name, "mixed_variants");
        EXPECT_EQ(dims[0].size, 2U);
        EXPECT_EQ(dims[1].name, "variant_data");
        EXPECT_EQ(dims[1].size, 10U);
        EXPECT_EQ(dims[0].role, Role::CUSTOM);
    }

    TEST(DataUtilsDetectVariantsTest, MixedPrecisionsOfOneKindStayConsistent)
    {
        const std::vector<DataVariant> variants { std::vector<float>(10), std::vector<double>(10) };

        const auto dims = Kakshya::detect_data_dimensions(variants);

        ASSERT_EQ(dims.size(), 2U);
        EXPECT_EQ(dims[0].role, Role::CHANNEL);
        EXPECT_EQ(dims[1].name, "samples");
    }

    TEST(DataUtilsDetectVariantsTest, MixedVectorAndScalarVariantsAreNotConsistent)
    {
        const std::vector<DataVariant> variants { std::vector<glm::vec3>(10), std::vector<float>(10) };

        const auto dims = Kakshya::detect_data_dimensions(variants);

        EXPECT_EQ(dims[0].name, "mixed_variants");
    }

    TEST(DataUtilsDetectVariantsTest, DetectedDimensionsDetectTheirModality)
    {
        const auto stereo = Kakshya::detect_data_dimensions(repeated(std::vector<float>(100), 2));
        const auto spectral = Kakshya::detect_data_dimensions(repeated(std::vector<std::complex<float>>(64), 20));

        EXPECT_EQ(Kakshya::detect_data_modality(stereo), DataModality::AUDIO_MULTICHANNEL);
        EXPECT_EQ(Kakshya::detect_data_modality(spectral), DataModality::SPECTRAL_2D);
    }

} // namespace
} // namespace MayaFlux::Test
