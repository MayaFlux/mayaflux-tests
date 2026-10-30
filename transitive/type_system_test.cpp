#include "gtest/gtest.h"

#include "glm/glm.hpp"

namespace MayaFlux::Test {
namespace {

    enum class Flags : uint8_t {
        NONE = 0U,
        A = 1U << 0U,
        B = 1U << 1U,
        C = 1U << 2U,
    };
    MF_BITMASK_OPERATORS(Flags)

    TEST(BitmaskOperatorsTest, OrCombinesBitsAndAndIntersectsThem)
    {
        const Flags ab = Flags::A | Flags::B;

        EXPECT_EQ(static_cast<uint8_t>(ab), 3U);
        EXPECT_EQ(ab & Flags::B, Flags::B);
        EXPECT_EQ(ab & Flags::C, Flags::NONE);
        EXPECT_EQ(ab ^ Flags::A, Flags::B);
    }

    TEST(BitmaskOperatorsTest, ComplementIsMaskedByIntersection)
    {
        const Flags ab = Flags::A | Flags::B;

        EXPECT_EQ(ab & ~Flags::A, Flags::B);
    }

    TEST(BitmaskOperatorsTest, CompoundAssignmentsUpdateInPlace)
    {
        Flags flags = Flags::A;

        flags |= Flags::C;
        EXPECT_EQ(flags, Flags::A | Flags::C);
        flags &= Flags::C;
        EXPECT_EQ(flags, Flags::C);
        flags ^= Flags::C;
        EXPECT_EQ(flags, Flags::NONE);
    }

    TEST(BitmaskOperatorsTest, AnyFlagIsNonZero)
    {
        EXPECT_FALSE(any_flag(Flags::NONE));
        EXPECT_TRUE(any_flag(Flags::B));
    }

    TEST(BitmaskOperatorsTest, HasFlagRequiresEveryBit)
    {
        const Flags ab = Flags::A | Flags::B;

        EXPECT_TRUE(has_flag(ab, Flags::A));
        EXPECT_TRUE(has_flag(ab, Flags::A | Flags::B));
        EXPECT_FALSE(has_flag(ab, Flags::A | Flags::C));
        EXPECT_TRUE(has_flag(ab, Flags::NONE));
    }

    TEST(SafeConversionConceptTest, OnlySameDomainWideningIsSafe)
    {
        EXPECT_TRUE((SafeArithmeticConversion<int8_t, int16_t>));
        EXPECT_TRUE((SafeArithmeticConversion<int32_t, int64_t>));
        EXPECT_TRUE((SafeArithmeticConversion<uint8_t, uint32_t>));
        EXPECT_TRUE((SafeArithmeticConversion<float, double>));
        EXPECT_TRUE((SafeArithmeticConversion<float, float>));
    }

    TEST(SafeConversionConceptTest, NarrowingSignChangesAndCrossDomainAreNot)
    {
        EXPECT_FALSE((SafeArithmeticConversion<double, float>));
        EXPECT_FALSE((SafeArithmeticConversion<int64_t, int32_t>));
        EXPECT_FALSE((SafeArithmeticConversion<uint32_t, int64_t>));
        EXPECT_FALSE((SafeArithmeticConversion<int32_t, float>));
        EXPECT_FALSE((SafeArithmeticConversion<float, int32_t>));
    }

    TEST(GlmTraitTest, ComponentCountsFollowTheType)
    {
        EXPECT_EQ(glm_component_count<glm::vec2>(), 2U);
        EXPECT_EQ(glm_component_count<glm::dvec3>(), 3U);
        EXPECT_EQ(glm_component_count<glm::vec4>(), 4U);
        EXPECT_EQ(glm_component_count<glm::mat2>(), 4U);
        EXPECT_EQ(glm_component_count<glm::mat3>(), 9U);
        EXPECT_EQ(glm_component_count<glm::mat4>(), 16U);
        EXPECT_EQ(glm_component_count<glm::dmat4>(), 16U);
        EXPECT_EQ(glm_component_count<float>(), 0U);
    }

    TEST(GlmTraitTest, ConceptsClassifyVectorsAndMatrices)
    {
        EXPECT_TRUE(GlmType<glm::vec3>);
        EXPECT_TRUE(GlmType<glm::dmat3>);
        EXPECT_TRUE(GlmVectorType<glm::dvec4>);
        EXPECT_FALSE(GlmVectorType<glm::mat4>);
        EXPECT_TRUE(GlmMatrixType<glm::mat4>);
        EXPECT_FALSE(GlmType<float>);
        EXPECT_FALSE(GlmType<std::complex<float>>);
    }

    TEST(GlmTraitTest, ComponentTypeIsTheScalarOfTheType)
    {
        EXPECT_TRUE((std::is_same_v<glm_component_type<glm::vec3>, float>));
        EXPECT_TRUE((std::is_same_v<glm_component_type<glm::dvec2>, double>));
        EXPECT_TRUE((std::is_same_v<scalar_t<glm::vec4>, float>));
        EXPECT_TRUE((std::is_same_v<scalar_t<glm::dmat4>, double>));
        EXPECT_TRUE((std::is_same_v<scalar_t<float>, float>));
        EXPECT_TRUE((std::is_same_v<scalar_t<int>, int>));
    }

    TEST(DataConceptsTest, NumericAndComplexConceptsClassifyTheirTypes)
    {
        EXPECT_TRUE(IntegerData<uint8_t>);
        EXPECT_TRUE(IntegerData<bool>);
        EXPECT_FALSE(IntegerData<float>);
        EXPECT_TRUE(DecimalData<double>);
        EXPECT_FALSE(DecimalData<int>);
        EXPECT_TRUE(ArithmeticData<uint16_t>);
        EXPECT_FALSE(ArithmeticData<std::complex<float>>);
        EXPECT_TRUE(StringData<std::string>);
        EXPECT_TRUE(StringData<const char*>);
        EXPECT_FALSE(StringData<std::string_view>);
    }

    TEST(TypeHandlerTest, OnlyTheListedTypesAreSupported)
    {
        EXPECT_TRUE(SupportedDataType<float>);
        EXPECT_TRUE(SupportedDataType<double>);
        EXPECT_TRUE(SupportedDataType<int>);
        EXPECT_TRUE(SupportedDataType<uint32_t>);
        EXPECT_TRUE(SupportedDataType<size_t>);
        EXPECT_TRUE(SupportedDataType<std::complex<float>>);
        EXPECT_TRUE(SupportedDataType<std::complex<double>>);
        EXPECT_FALSE(SupportedDataType<uint8_t>);
        EXPECT_FALSE(SupportedDataType<glm::vec3>);
        EXPECT_STREQ(TypeHandler<float>::name, "float");
        EXPECT_STREQ(TypeHandler<std::string>::name, "unsupported");
    }

    TEST(SaturateCastTest, UnsignedTargetsClampAtBothEnds)
    {
        EXPECT_EQ(saturate_cast<uint8_t>(300.0), 255U);
        EXPECT_EQ(saturate_cast<uint8_t>(-5.0), 0U);
        EXPECT_EQ(saturate_cast<uint8_t>(127.9), 127U);
        EXPECT_EQ(saturate_cast<uint32_t>(4294967295.0), std::numeric_limits<uint32_t>::max());
        EXPECT_EQ(saturate_cast<uint32_t>(4294967296.0), std::numeric_limits<uint32_t>::max());
    }

    TEST(SaturateCastTest, SignedTargetsClampAtBothEnds)
    {
        EXPECT_EQ(saturate_cast<int8_t>(-200.0), std::numeric_limits<int8_t>::lowest());
        EXPECT_EQ(saturate_cast<int8_t>(-128.0), std::numeric_limits<int8_t>::lowest());
        EXPECT_EQ(saturate_cast<int32_t>(3.0e10), std::numeric_limits<int32_t>::max());
        EXPECT_EQ(saturate_cast<int32_t>(-3.0e10), std::numeric_limits<int32_t>::lowest());
        EXPECT_EQ(saturate_cast<int64_t>(9.3e18), std::numeric_limits<int64_t>::max());
        EXPECT_EQ(saturate_cast<int32_t>(-7.9), -7);
    }

    TEST(SaturateCastTest, NotANumberBecomesZeroForIntegers)
    {
        EXPECT_EQ(saturate_cast<uint8_t>(std::nan("")), 0U);
        EXPECT_EQ(saturate_cast<int32_t>(std::nan("")), 0);
    }

    TEST(SaturateCastTest, DecimalTargetsAreAPlainCast)
    {
        EXPECT_FLOAT_EQ(saturate_cast<float>(1.5), 1.5F);
        EXPECT_DOUBLE_EQ(saturate_cast<double>(1.0e300), 1.0e300);
        EXPECT_TRUE(std::isnan(saturate_cast<float>(std::nan(""))));
    }

    TEST(SafeAnyCastTest, ExactTypesPassThroughWithoutLoss)
    {
        const auto result = safe_any_cast<int>(std::any(42));

        ASSERT_TRUE(result.value.has_value());
        EXPECT_EQ(*result.value, 42);
        EXPECT_FALSE(result.precision_loss);
        EXPECT_TRUE(result.error.empty());
    }

    TEST(SafeAnyCastTest, AnEmptyAnyFails)
    {
        const auto result = safe_any_cast<int>(std::any {});

        EXPECT_FALSE(result.value.has_value());
        EXPECT_FALSE(result.error.empty());
        EXPECT_EQ(result.value_or(7), 7);
    }

    class SafeAnyCastSourceTest : public ::testing::TestWithParam<std::any> { };

    TEST_P(SafeAnyCastSourceTest, EveryIntegerAndDecimalSourceTypeReadsAsDouble)
    {
        const auto result = safe_any_cast<double>(GetParam());

        ASSERT_TRUE(result.value.has_value()) << GetParam().type().name();
        EXPECT_DOUBLE_EQ(*result.value, 3.0);
    }

    INSTANTIATE_TEST_SUITE_P(Sources, SafeAnyCastSourceTest,
        ::testing::Values(
            std::any(static_cast<signed char>(3)),
            std::any(static_cast<unsigned char>(3)),
            std::any(static_cast<short>(3)),
            std::any(static_cast<unsigned short>(3)),
            std::any(3),
            std::any(3U),
            std::any(3L),
            std::any(3UL),
            std::any(3LL),
            std::any(3ULL),
            std::any(3.0F),
            std::any(3.0),
            std::any(static_cast<uint8_t>(3)),
            std::any(static_cast<uint16_t>(3)),
            std::any(static_cast<int64_t>(3)),
            std::any(static_cast<uint64_t>(3)),
            std::any(static_cast<size_t>(3))));

    TEST(SafeAnyCastTest, BoolsAndCharsReadAsIntegers)
    {
        EXPECT_EQ(safe_any_cast<int>(std::any(true)).value, 1);
        EXPECT_EQ(safe_any_cast<int>(std::any('A')).value, 65);
    }

    TEST(SafeAnyCastTest, WideningIsLossless)
    {
        const auto result = safe_any_cast<double>(std::any(0.25F));

        EXPECT_DOUBLE_EQ(*result.value, 0.25);
        EXPECT_FALSE(result.precision_loss);
    }

    TEST(SafeAnyCastTest, NarrowingConvertsAndReportsLoss)
    {
        const auto truncated = safe_any_cast<int>(std::any(2.5));
        const auto exact = safe_any_cast<int>(std::any(3.0));
        const auto too_wide = safe_any_cast<float>(std::any(16777217));

        EXPECT_EQ(truncated.value, 2);
        EXPECT_TRUE(truncated.precision_loss);
        EXPECT_EQ(exact.value, 3);
        EXPECT_FALSE(exact.precision_loss);
        ASSERT_TRUE(too_wide.value.has_value());
        EXPECT_TRUE(too_wide.precision_loss);
    }

    TEST(SafeAnyCastTest, ComplexValuesReduceToTheirMagnitudeAndReportLoss)
    {
        const auto result = safe_any_cast<double>(std::any(std::complex<double> { 3.0, 4.0 }));
        const auto real_only = safe_any_cast<double>(std::any(std::complex<double> { 2.0, 0.0 }));

        ASSERT_TRUE(result.value.has_value());
        EXPECT_DOUBLE_EQ(*result.value, 5.0);
        EXPECT_TRUE(result.precision_loss);
        EXPECT_FALSE(real_only.precision_loss);
    }

    TEST(SafeAnyCastTest, ComplexTargetsAcceptRealAndComplexSources)
    {
        const auto from_float = safe_any_cast<std::complex<double>>(std::any(0.25F));
        const auto from_double = safe_any_cast<std::complex<float>>(std::any(1.5));
        const auto widened = safe_any_cast<std::complex<double>>(std::any(std::complex<float> { 1.0F, 2.0F }));
        const auto narrowed = safe_any_cast<std::complex<float>>(std::any(std::complex<double> { 1.0, 2.0 }));

        ASSERT_TRUE(from_float.value.has_value());
        EXPECT_EQ(*from_float.value, std::complex<double>(0.25, 0.0));
        ASSERT_TRUE(from_double.value.has_value());
        EXPECT_EQ(*from_double.value, std::complex<float>(1.5F, 0.0F));
        ASSERT_TRUE(widened.value.has_value());
        EXPECT_EQ(*widened.value, std::complex<double>(1.0, 2.0));
        EXPECT_FALSE(widened.precision_loss);
        ASSERT_TRUE(narrowed.value.has_value());
        EXPECT_EQ(*narrowed.value, std::complex<float>(1.0F, 2.0F));
    }

    TEST(SafeAnyCastTest, ComplexNarrowingReportsLoss)
    {
        const auto result = safe_any_cast<std::complex<float>>(std::any(std::complex<double> { 0.1, 0.2 }));

        ASSERT_TRUE(result.value.has_value());
        EXPECT_TRUE(result.precision_loss);
    }

    TEST(SafeAnyCastTest, StringsAcceptCStringsAndSucceedCleanly)
    {
        const auto from_literal = safe_any_cast<std::string>(std::any("snare"));
        const auto from_string = safe_any_cast<std::string>(std::any(std::string("kick")));

        ASSERT_TRUE(from_literal.value.has_value());
        EXPECT_EQ(*from_literal.value, "snare");
        EXPECT_TRUE(from_literal.error.empty());
        ASSERT_TRUE(from_string.value.has_value());
        EXPECT_EQ(*from_string.value, "kick");
        EXPECT_TRUE(from_string.error.empty());
    }

    TEST(SafeAnyCastTest, CStringTargetsAcceptCStrings)
    {
        const auto result = safe_any_cast<const char*>(std::any("hat"));

        ASSERT_TRUE(result.value.has_value());
        EXPECT_STREQ(*result.value, "hat");
    }

    TEST(SafeAnyCastTest, IncompatibleKindsFailWithAnExplanation)
    {
        const auto number_from_text = safe_any_cast<int>(std::any(std::string("x")));
        const auto text_from_number = safe_any_cast<std::string>(std::any(42));
        const auto complex_from_int = safe_any_cast<std::complex<float>>(std::any(std::string("x")));

        EXPECT_FALSE(number_from_text.value.has_value());
        EXPECT_FALSE(text_from_number.value.has_value());
        EXPECT_FALSE(complex_from_int.value.has_value());
        EXPECT_FALSE(number_from_text.error.empty());
        EXPECT_FALSE(text_from_number.error.empty());
    }

    TEST(SafeAnyCastTest, OtherTypesMatchExactlyOrNotAtAll)
    {
        const auto vector = safe_any_cast<glm::vec3>(std::any(glm::vec3(1, 2, 3)));
        const auto wrong = safe_any_cast<glm::vec3>(std::any(glm::vec4(1, 2, 3, 4)));
        const auto number = safe_any_cast<glm::vec3>(std::any(1.0F));
        const auto list = safe_any_cast<std::vector<int>>(std::any(std::vector<int> { 1, 2 }));

        ASSERT_TRUE(vector.value.has_value());
        EXPECT_EQ(*vector.value, glm::vec3(1, 2, 3));
        EXPECT_FALSE(wrong.value.has_value());
        EXPECT_FALSE(number.value.has_value());
        EXPECT_FALSE(wrong.error.empty());
        ASSERT_TRUE(list.value.has_value());
        EXPECT_EQ(list.value->size(), 2U);
    }

    TEST(SafeAnyCastTest, ThrowingFormReportsTheFailure)
    {
        EXPECT_EQ(safe_any_cast_or_throw<int>(std::any(5)), 5);
        EXPECT_THROW((void)safe_any_cast_or_throw<int>(std::any(std::string("x"))), std::runtime_error);
        EXPECT_THROW((void)safe_any_cast_or_throw<int>(std::any {}), std::runtime_error);
    }

    TEST(SafeAnyCastTest, DefaultingFormFallsBack)
    {
        EXPECT_EQ(safe_any_cast_or_default<int>(std::any(5), 9), 5);
        EXPECT_EQ(safe_any_cast_or_default<int>(std::any(std::string("x")), 9), 9);
        EXPECT_EQ(safe_any_cast_or_default<int>(std::any {}, 9), 9);
    }

    TEST(AnyEqualTest, EqualValuesCompareEqual)
    {
        EXPECT_TRUE(any_equal<int>(std::any(3), std::any(3)));
        EXPECT_FALSE(any_equal<int>(std::any(3), std::any(4)));
        EXPECT_TRUE(any_equal<std::string>(std::any(std::string("a")), std::any("a")));
        EXPECT_TRUE(any_equal<double>(std::any(2.0), std::any(2.0)));
    }

    TEST(AnyEqualTest, NumericTypesCompareAfterConversion)
    {
        EXPECT_TRUE(any_equal<double>(std::any(3), std::any(3.0)));
        EXPECT_TRUE(any_equal<int64_t>(std::any(static_cast<uint8_t>(3)), std::any(3)));
    }

    TEST(AnyEqualTest, AnUnconvertibleSideComparesUnequalInsteadOfThrowing)
    {
        EXPECT_FALSE(any_equal<int>(std::any(3), std::any(std::string("3"))));
        EXPECT_FALSE(any_equal<int>(std::any(std::string("3")), std::any(3)));
        EXPECT_FALSE(any_equal<int>(std::any(3), std::any {}));
        EXPECT_FALSE(any_equal<std::string>(std::any(std::string("3")), std::any(3)));
    }

    TEST(AnyEqualTest, OtherTypesNeedTheSameTypeOnBothSides)
    {
        EXPECT_TRUE(any_equal<glm::vec3>(std::any(glm::vec3(1, 2, 3)), std::any(glm::vec3(1, 2, 3))));
        EXPECT_FALSE(any_equal<glm::vec3>(std::any(glm::vec3(1, 2, 3)), std::any(glm::vec3(1, 2, 4))));
        EXPECT_FALSE(any_equal<glm::vec3>(std::any(glm::vec3(1, 2, 3)), std::any(glm::vec4(1, 2, 3, 4))));
    }

    TEST(SafeVariantGetTest, ReturnsTheActiveAlternative)
    {
        const std::variant<int, std::string> value = std::string("x");

        EXPECT_EQ(safe_variant_get_or_throw<std::string>(value, "ctx"), "x");
    }

    TEST(SafeVariantGetTest, ThrowsWithTheContextWhenInactive)
    {
        const std::variant<int, std::string> value = 3;

        try {
            (void)safe_variant_get_or_throw<std::string>(value, "ctx");
            FAIL() << "expected a throw";
        } catch (const std::runtime_error& error) {
            EXPECT_NE(std::string(error.what()).find("ctx"), std::string::npos);
        }
    }

} // namespace
} // namespace MayaFlux::Test
